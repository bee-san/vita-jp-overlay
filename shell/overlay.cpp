/* Full-screen ScePaf overlay: header (OCR text, selected word highlighted),
 * separator, the selected word's definition, and the touch region selector.
 * Runs entirely on the paf main thread via MainThreadCallList.
 *
 * Header and body are ui::Text widgets in a fixed-size box inside a
 * ui::ScrollView (as in GrapheneCt's NetStream, the text takes the box's
 * width and grows to fit). The view only clips: its content is the box, which
 * never changes size, so the view never re-centres or eases the text (it did
 * both when the Text was its direct child). Sizes and colors are set per
 * range with SetStyleAttribute (in pixels), and the Text reports where text
 * is (GetCharInfo/GetLineInfo), so nothing is estimated. Moving the
 * selection only recolors two ranges of the header. Each Text is placed
 * inside its box every frame from its laid-out size: paf positions are
 * centre-based and y-up, so the text's top sits at the box's top when
 * pos.y = (box height - text height) / 2, plus the scroll offset. ScrollView
 * DoSnap and Text GetNextDownHit are not used: pressing ▼ with them crashed
 * the console.
 *
 * Subtitles: a second page, the strip (strip.cpp), shows while the overlay
 * is closed.
 *
 * Touch: SceTouch routes the front panel by region, and while a game is in
 * front SceShell's regions are inactive. The region selector activates
 * SceShell's default touch region while it is open (as SceShell's own menus
 * over a game must) and reads the touches through paf's input device. */
#include <psp2/kernel/modulemgr.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>

#include "paf_ui.h"
extern "C" {
#include "../core/utf.h"
}

using namespace paf;

/* The paf text info classes have virtual destructors, whose deleting
 * variants reference sized delete (no C++ runtime is linked; these objects
 * live on the stack). */
void operator delete(void *p, unsigned int)
{
    sce_paf_free(p);
}

/* Layout (paf coordinates: origin at the screen centre, y up). The header
 * fits its text, 1..HEADER_MAX_LINES lines; the separator and the body follow
 * it, and the body keeps its bottom (shell/rco/vitajpoverlay.xml holds the
 * initial sizes: header 128, body 320). */
#define HEADER_TOP 260.0f
#define HEADER_MAX_LINES 3
#define HEADER_SEP_GAP 19.0f /* header bottom -> separator */
#define SEP_BODY_GAP 17.0f   /* separator -> body top */
#define BODY_BOTTOM -224.0f
#define HEADER_H_INITIAL 128.0f
#define BODY_H_INITIAL 320.0f
#define HEADER_UNITS 4096 /* UTF-16 units */
#define BODY_UNITS 4096
#define MAX_SPANS 96
#define STICK_DEADZONE 40
#define STICK_SCROLL_PX 0.12f /* per frame per unit of stick deflection */
#define LAYOUT_RETRY_FRAMES 6 /* layout may land a frame or two after SetString */
#define CLEAR_HOLD_US 1000000
#define MAX_RANGES VJO_MAX_ENTRIES /* entries whose header highlight range is cached */

enum { MODE_OVERLAY = 0, MODE_REGION = 1 };

static Plugin *s_plugin;
static ui::Scene *s_page;
/* A Text in a fixed-size box inside a clipping ScrollView (see the top). */
struct Pane {
    ui::Widget *view, *box;
    ui::Text *text;
    float h;       /* box height */
    float scroll;  /* content y at the box top */
    float applied; /* last pos.y given to the Text; NOT_PLACED = place afresh */
};
#define NOT_PLACED (-1e9f)
static Pane s_header = {NULL, NULL, NULL, HEADER_H_INITIAL, 0.0f, NOT_PLACED};
static Pane s_body = {NULL, NULL, NULL, BODY_H_INITIAL, 0.0f, NOT_PLACED};
static ui::Widget *s_separator, *s_band_mid;
static ui::RichText *s_hint, *s_region_hint;
static ui::Widget *s_panel, *s_region_layer, *s_region_rect;

static unsigned s_seen_version, s_seen_anki_version;
static unsigned s_list_seq; /* the shown list (Anki requests name it) */
static int s_anki_enabled, s_anki_syncing;
static int s_selected;
static uint32_t s_hook_session, s_hook_ids[VJO_TEXT_CHOICES];
static int s_hook_count, s_hook_selected, s_hook_picker;
static int s_hook_can_ocr, s_hook_matching;
/* Copied from the view under its lock, so nothing outside render() reads
 * the result memory (the control thread frees it when a game exits) and no
 * paf call runs while the lock is held. */
static int s_n_entries;
static uint32_t s_rng_start[MAX_RANGES], s_rng_len[MAX_RANGES]; /* len 0 = no highlight */

/* logged when the overlay closes */
static int64_t s_frame_max_us;
static int s_frames, s_moves, s_stick_max; /* largest stick deflection from centre */
static int s_mode;
static uint32_t s_prev_buttons;
static int s_prev_touch;

/* header state */
static uint32_t s_hl_start, s_hl_len; /* highlighted range now drawn */
static int s_follow_frames;           /* frames left to scroll the header to the word */

/* region selection (screen pixels) */
static int s_touch_region_on; /* SceShell's default front touch region activated by us */
static int s_drag, s_has_rect;
static int s_x0, s_y0, s_x1, s_y1;
static int64_t s_square_down_us;
static int s_square_clear_armed;

static uint8_t s_ui_mem[96 * 1024];
static VjoArena s_ui;

static void set_rich(ui::RichText *w, const char *s)
{
    if (w)
        w->SetCText(s, sce_paf_strlen(s));
}

/* Places the pane's Text so that content y = scroll is at the box's top.
 * The widget takes the box's width (adjust 1) with the text left-aligned in
 * it, so x stays 0; its height is the laid-out height (adjust 2) and paf
 * positions are centre-based, so y comes from that height. Runs every frame
 * (cheap) and only calls SetPos on a change. */
static void pane_place(Pane *p)
{
    float h = text_height(p->text), y;
    if (!p->text || h <= 0.0f)
        return;
    y = (p->h - h) / 2.0f + p->scroll;
    if (y != p->applied) {
        p->applied = y;
        p->text->SetPos(0.0f, y, 0.0f);
    }
}

/* Scrolls to content y, kept within the laid-out text. */
static void pane_scroll_to(Pane *p, float y)
{
    float max = text_height(p->text) - p->h;
    if (y > max)
        y = max;
    if (y < 0.0f)
        y = 0.0f;
    p->scroll = y;
}

/* New text or a new box: scrolled to the top and placed afresh. */
static void pane_reset(Pane *p)
{
    p->scroll = 0.0f;
    p->applied = NOT_PLACED;
}

/* Sizes the view and its box (same size: the view only clips) with the
 * given top edge; the Text is placed afresh in the new box. */
static void pane_size(Pane *p, float h, float top)
{
    p->h = h;
    if (p->view) {
        p->view->SetSize(BOX_W, h, 0.0f);
        p->view->SetPos(0.0f, top - h / 2.0f, 0.0f);
    }
    if (p->box)
        p->box->SetSize(BOX_W, h, 0.0f);
    p->applied = NOT_PLACED;
}

/* Recolors the highlighted word: the old range back to white, the new one
 * yellow. No relayout. */
static void header_highlight(void)
{
    if (!s_header.text)
        return;
    if (s_hl_len)
        s_header.text->SetStyleAttribute(graph::TextStyleAttribute_Color, s_hl_start, s_hl_len, rgba(VJO_RGB_TEXT));
    s_hl_len = 0;
    if (s_selected < s_n_entries && s_selected < MAX_RANGES && s_rng_len[s_selected]) {
        s_hl_start = s_rng_start[s_selected];
        s_hl_len = s_rng_len[s_selected];
        s_header.text->SetStyleAttribute(graph::TextStyleAttribute_Color, s_hl_start, s_hl_len, rgba(VJO_RGB_HIGHLIGHT));
    }
    s_follow_frames = LAYOUT_RETRY_FRAMES;
}

/* Scrolls the header just enough to show the highlighted word's whole line.
 * The line comes from the Text (GetCharInfo); the line height is the laid
 * out height over the line count (the line frame's fields turned out not to
 * be x, y, w, h: on the device line 0 was "0, 0, 0, 746" for a 746 px line).
 * Returns 1 when done. */
static int header_follow(void)
{
    graph::TextLayoutCharInfo ci;
    uint32_t lines = 0;
    float height, lh, top, scroll = s_header.scroll;
    ui::Text *t = s_header.text;
    if (!t || !s_hl_len)
        return 1;
    if (t->GetCharInfo(s_hl_start, ci) < 0 || t->GetLineCount(lines) < 0 || !lines)
        return 0; /* not laid out yet */
    height = text_height(t);
    if (height <= 0.0f)
        return 0;
    lh = height / (float)lines;
    top = (float)ci.line * lh;
    if (top + lh > scroll + s_header.h)
        scroll = top + lh - s_header.h;
    if (top < scroll)
        scroll = top;
    pane_scroll_to(&s_header, scroll);
    return 1;
}

/* new_content: a new result (header text rebuilt and scrolled to the top);
 * otherwise only the selection moved. The text is built from the view under
 * its lock; the paf calls happen after it is released. */
/* Keep previews readable without changing the captured source. Collapse
 * line/control whitespace and omit invisible formatting; bound by complete
 * codepoints, rather than cutting a Japanese sentence after 80 bytes. */
static void hook_preview(char *out, size_t cap, const char *text, size_t bytes, unsigned limit)
{
    size_t len = sceClibStrnlen(text, bytes), i = 0, w = 0;
    unsigned points = 0;
    int space = 0, truncated = 0;
    if (!cap) return;
    while (i < len) {
        uint32_t cp = vjo_utf8_next(text, len, &i);
        if (cp == '\\' && i < len && (text[i] == 'n' || text[i] == 'r' || text[i] == 't')) {
            i++;
            cp = ' ';
        }
        if (cp <= 0x20 || cp == 0x7F || cp == 0x2028 || cp == 0x2029 || cp == 0x3000) {
            space = w != 0;
            continue;
        }
        if (cp == 0xFFFD || cp == 0xFEFF || (cp >= 0x200B && cp <= 0x200F) ||
            (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2069)) continue;
        char encoded[4];
        int n = vjo_utf8_encode(cp, encoded);
        if (points == limit || w + (size_t)space + (size_t)n + 4 > cap) {
            truncated = 1;
            break;
        }
        if (space) out[w++] = ' ';
        sceClibMemcpy(out + w, encoded, (size_t)n);
        w += (size_t)n;
        points++;
        space = 0;
    }
    if (truncated && w + 4 <= cap) {
        sceClibMemcpy(out + w, "…", 3);
        w += 3;
    }
    out[w] = 0;
}

static void render_hooks(void)
{
    VjoStyled hdr, body;
    int ja, en, match_ready;
    char label[96], preview[4 * 160 + 4];
    uint32_t selected = s_hook_count ? s_hook_ids[s_hook_selected] : 0;
    font_px(&ja, &en);
    vjo_arena_reset(&s_ui);
    if (vjo_styled_init(&hdr, &s_ui, HEADER_UNITS, 3) < 0 ||
        vjo_styled_init(&body, &s_ui, BODY_UNITS, MAX_SPANS) < 0) return;
    vjo_view_lock();
    if (s_hook_session != g_view.hook_session) selected = 0;
    s_hook_session = g_view.hook_session;
    if (s_hook_matching && g_view.hook_match_state != VJO_HOOK_MATCH_MATCHING) selected = 0;
    s_hook_matching = g_view.hook_match_state == VJO_HOOK_MATCH_MATCHING;
    s_hook_can_ocr = g_view.hook_ocr_available && !s_hook_matching;
    match_ready = g_view.hook_match_state == VJO_HOOK_MATCH_READY && g_view.hook_reference[0];
    if (match_ready) {
        vjo_styled_puts(&hdr, "Read from screenshot\n", VJO_RGB_READING, en);
        hook_preview(preview, sizeof(preview), g_view.hook_reference, sizeof(g_view.hook_reference), 160);
        vjo_styled_puts(&hdr, preview, VJO_RGB_TEXT, ja);
    } else {
        vjo_styled_puts(&hdr, s_hook_matching ? "Reading screenshot…" :
            g_view.hook_match_state == VJO_HOOK_MATCH_FAILED ? "Screenshot could not be read" :
            "Choose game text", VJO_RGB_TEXT, ja);
    }
    s_hook_count = (int)(g_view.hook_count > VJO_TEXT_CHOICES ? VJO_TEXT_CHOICES : g_view.hook_count);
    s_hook_selected = 0;
    for (int i = 0; i < s_hook_count; i++) {
        s_hook_ids[i] = g_view.hooks[i].id;
        if (s_hook_ids[i] == selected) s_hook_selected = i;
    }
    if (g_view.status[0]) {
        vjo_styled_puts(&body, g_view.status,
            g_view.status_is_error ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        vjo_styled_puts(&body, "\n\n", VJO_RGB_DIM, en);
    }
    if (s_hook_matching) {
        vjo_styled_puts(&body, "The screenshot text and its closest matches will appear here.", VJO_RGB_DIM, en);
    } else if (!s_hook_count) {
        if (match_ready)
            vjo_styled_puts(&body, s_hook_can_ocr ?
                "No matching game text found.\nAdvance the dialogue and press □ to refresh, or △ to read the screenshot again." :
                "No matching game text found.\nAdvance the dialogue, then press □ to refresh.", VJO_RGB_DIM, en);
        else
            vjo_styled_puts(&body, "No game text found yet.\nAdvance the dialogue, then press □ to refresh.", VJO_RGB_DIM, en);
    } else {
        const VjoTextCandidate *c = &g_view.hooks[s_hook_selected];
        if (match_ready && c->score) {
            if (!s_hook_selected)
                sceClibSnprintf(label, sizeof(label), "Best match · %u%%\n", c->score);
            else
                sceClibSnprintf(label, sizeof(label), "Choice %d of %d · %u%% match\n",
                    s_hook_selected + 1, s_hook_count, c->score);
        } else {
            sceClibSnprintf(label, sizeof(label), "Choice %d of %d%s\n",
                s_hook_selected + 1, s_hook_count, match_ready ? " · no text match" : "");
        }
        vjo_styled_puts(&body, label, VJO_RGB_HIGHLIGHT, en);
        hook_preview(preview, sizeof(preview), c->text, sizeof(c->text), 160);
        vjo_styled_puts(&body, preview[0] ? preview : "No readable text", VJO_RGB_HIGHLIGHT, ja);
        if (s_hook_count > 1) {
            vjo_styled_puts(&body, "\n\nOther choices\n", VJO_RGB_DIM, en);
            for (int i = 0; i < s_hook_count; i++) {
                if (i == s_hook_selected) continue;
                c = &g_view.hooks[i];
                if (match_ready && c->score)
                    sceClibSnprintf(label, sizeof(label), "%d · %u%% match\n", i + 1, c->score);
                else
                    sceClibSnprintf(label, sizeof(label), "%d%s\n", i + 1,
                        match_ready ? " · no text match" : "");
                vjo_styled_puts(&body, label, VJO_RGB_DIM, en);
                hook_preview(preview, sizeof(preview), c->text, sizeof(c->text), 48);
                vjo_styled_puts(&body, preview[0] ? preview : "No readable text", VJO_RGB_TEXT, ja);
                vjo_styled_puts(&body, "\n\n", VJO_RGB_DIM, en);
            }
        }
    }
    vjo_view_unlock();
    text_set(s_header.text, &hdr);
    text_set(s_body.text, &body);
    s_n_entries = 0; s_hl_len = 0;
    pane_reset(&s_header); pane_reset(&s_body);
    const char *hint;
    if (s_hook_matching)
        hint = "<font color=\"#8a94a6\">Reading screenshot… · Select+□ region · ○ close</font>";
    else if (!s_hook_count)
        hint = s_hook_can_ocr ?
            "<font color=\"#8a94a6\">△ read again · □ refresh · Select+□ region · ○ close</font>" :
            "<font color=\"#8a94a6\">□ refresh · Select+□ region · ○ close</font>";
    else
        hint = s_hook_can_ocr ?
            "<font color=\"#8a94a6\">▲ ▼ choose · × use text · △ read again · □ refresh · Select+□ region · stick scroll · ○ close</font>" :
            "<font color=\"#8a94a6\">▲ ▼ choose · × use text · □ refresh · Select+□ region · stick scroll · ○ close</font>";
    set_rich(s_hint, hint);
}

static void render(int new_content)
{
    VjoStyled hdr, body;
    int ja, en, have_hdr = 0, have_body = 0;
    const VjoEntryList *l;

    vjo_view_lock(); s_hook_picker = g_view.hook_picker; vjo_view_unlock();
    if (s_hook_picker) { render_hooks(); return; }
    set_rich(s_hint, "<font color=\"#8a94a6\">◀ ▶ ▲ ▼ word · × queue · △ send · stick scroll · □ region · Select+□ text · ○ close</font>");

    font_px(&ja, &en);
    vjo_view_lock();
    l = g_view.list;
    s_n_entries = l ? l->n_entries : 0;
    s_list_seq = g_view.list_seq;
    s_anki_enabled = g_view.anki_enabled;
    s_anki_syncing = g_view.anki_syncing;
    if (s_selected >= s_n_entries)
        s_selected = s_n_entries ? s_n_entries - 1 : 0;
    vjo_arena_reset(&s_ui);

    if (new_content) {
        if (vjo_styled_init(&hdr, &s_ui, HEADER_UNITS, 1) == 0) {
            vjo_styled_header(&hdr, l, ja);
            have_hdr = 1;
        }
        for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++)
            if (!vjo_entry_header_range(l, i, &s_rng_start[i], &s_rng_len[i]))
                s_rng_len[i] = 0;
    }

    /* body: status, the selected entry (or "no entries"), config warnings */
    if (vjo_styled_init(&body, &s_ui, BODY_UNITS, MAX_SPANS) == 0) {
        have_body = 1;
        if (g_view.status[0])
            vjo_styled_puts(&body, g_view.status, g_view.status_is_error ? VJO_RGB_ERROR : VJO_RGB_DIM, en);
        if (s_anki_enabled) {
            char queue[80];
            if (g_view.anki_pending < 0)
                sceClibSnprintf(queue, sizeof(queue), "Anki queue unavailable: check ux0 storage");
            else
                sceClibSnprintf(queue, sizeof(queue), "Anki queue: %d · %s", g_view.anki_pending,
                                g_view.anki_syncing ? "syncing…" : "△ send");
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            vjo_styled_puts(&body, queue, VJO_RGB_DIM, en);
        }
        if (g_view.anki_status[0]) {
            /* by VJO_ANKI_STATUS_* */
            static const uint32_t rgb[] = {VJO_RGB_DIM, VJO_RGB_ERROR};
            int k = g_view.anki_status_kind;
            if (k < 0 || k >= (int)(sizeof(rgb) / sizeof(rgb[0])))
                k = VJO_ANKI_STATUS_DIM;
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            vjo_styled_puts(&body, g_view.anki_status, rgb[k], en);
        }
        if (s_n_entries) {
            int marked = g_view.anki_marks_seq == s_list_seq && s_selected < VJO_MAX_ENTRIES &&
                         g_view.anki_mark[s_selected] == 1;
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_DIM, en);
            if (g_view.anki_marks_seq == s_list_seq && s_selected < VJO_MAX_ENTRIES &&
                g_view.anki_mark[s_selected] == 2)
                vjo_styled_puts(&body, "Queued offline\n", VJO_RGB_DIM, en);
            vjo_styled_entry(&body, l, s_selected, ja, en, marked);
        } else if (l && !g_view.status[0]) {
            vjo_styled_puts(&body, "No dictionary entries for this text.", VJO_RGB_DIM, en);
        }
        for (int i = 0; i < g_view.n_warnings; i++) {
            if (body.len)
                vjo_styled_puts(&body, "\n", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, "config.ini: ", VJO_RGB_ERROR, en);
            vjo_styled_puts(&body, g_view.warnings[i], VJO_RGB_ERROR, en);
        }
    }
    vjo_view_unlock();

    if (new_content) {
        if (have_hdr)
            text_set(s_header.text, &hdr);
        pane_reset(&s_header);
        s_hl_len = 0;
    }
    header_highlight();
    if (have_body)
        text_set(s_body.text, &body);
    pane_reset(&s_body);
}

static void layout_apply(float header_h)
{
    float sep = HEADER_TOP - header_h - HEADER_SEP_GAP;
    float body_top = sep - SEP_BODY_GAP;
    pane_size(&s_header, header_h, HEADER_TOP);
    pane_size(&s_body, body_top - BODY_BOTTOM, body_top);
    /* the mid band covers the gap between the boxes, centred on the separator */
    if (s_separator)
        s_separator->SetPos(0.0f, sep, 0.0f);
    if (s_band_mid)
        s_band_mid->SetPos(0.0f, sep, 0.0f);
    if (s_hl_len)
        s_follow_frames = LAYOUT_RETRY_FRAMES;
}

/* Fits the header to its text: 1..HEADER_MAX_LINES laid-out lines. Cheap
 * (two queries); applies only on a change. */
static void header_fit(void)
{
    uint32_t lines = 0;
    float h = text_height(s_header.text), want;
    if (!s_header.text || h <= 0.0f || s_header.text->GetLineCount(lines) < 0 || !lines)
        return;
    want = h / (float)lines * (float)(lines < HEADER_MAX_LINES ? lines : HEADER_MAX_LINES);
    if (want > HEADER_H_INITIAL)
        want = HEADER_H_INITIAL;
    want = (float)(int)(want + 0.5f);
    if (want != s_header.h) {
        layout_apply(want);
        pane_scroll_to(&s_body, s_body.scroll); /* keep the body scroll in range */
    }
}

static void show_region_rect(void)
{
    int x = s_x0 < s_x1 ? s_x0 : s_x1, y = s_y0 < s_y1 ? s_y0 : s_y1;
    int w = s_x0 < s_x1 ? s_x1 - s_x0 : s_x0 - s_x1, hgt = s_y0 < s_y1 ? s_y1 - s_y0 : s_y0 - s_y1;
    if (!s_region_rect)
        return;
    if (w < 2 || hgt < 2) {
        s_region_rect->Hide();
        return;
    }
    /* paf layout: origin at the screen centre, y up */
    s_region_rect->SetSize((float)w, (float)hgt, 0.0f);
    s_region_rect->SetPos((float)(x + w / 2 - SCREEN_W / 2), (float)(SCREEN_H / 2 - (y + hgt / 2)), 0.0f);
    s_region_rect->Show();
}

/* Front touches go to SceShell while on (and not to the game). */
static void touch_region(int on)
{
    int rc;
    if (on == s_touch_region_on)
        return;
    rc = inputdevice::touchscreen::ActivateDefaultRegion(inputdevice::touchscreen::PORT_FRONT, on != 0);
    s_touch_region_on = on;
    vjo_log("touch region %s: 0x%X", on ? "on" : "off", rc);
}

static void enter_region_mode(void)
{
    s_mode = MODE_REGION;
    touch_region(1);
    s_drag = 0;
    s_has_rect = 0;
    s_square_down_us = sceKernelGetProcessTimeWide();
    s_square_clear_armed = 1;
    if (s_panel)
        s_panel->Hide();
    if (s_region_layer)
        s_region_layer->Show();
    if (s_region_rect)
        s_region_rect->Hide();
    set_rich(s_region_hint, "<font color=\"#ffffff\">Drag on the screen to select the text area · "
                            "× confirm · ○ cancel · hold □ for full screen</font>");
}

static void leave_region_mode(void)
{
    touch_region(0);
    s_mode = MODE_OVERLAY;
    if (s_region_layer)
        s_region_layer->Hide();
    if (s_panel)
        s_panel->Show();
}

static void open_page(void)
{
    Plugin::PageOpenParam param;
    param.overwrite_draw_priority = 6; /* above the game, like PSVshellPlus' HUD */
    s_page = s_plugin->PageOpen("vjo_page_overlay", param);
    if (!s_page) {
        vjo_log("overlay page failed to open");
        return;
    }
    s_panel = s_page->FindChild("vjo_panel");
    s_header.text = (ui::Text *)s_page->FindChild("vjo_header");
    s_body.text = (ui::Text *)s_page->FindChild("vjo_body");
    s_header.view = s_page->FindChild("vjo_header_view");
    s_header.box = s_page->FindChild("vjo_header_box");
    s_body.view = s_page->FindChild("vjo_body_view");
    s_body.box = s_page->FindChild("vjo_body_box");
    s_separator = s_page->FindChild("vjo_separator");
    s_band_mid = s_page->FindChild("vjo_band_mid");
    s_header.h = HEADER_H_INITIAL; /* the page opens with the RCO layout */
    s_body.h = BODY_H_INITIAL;
    pane_reset(&s_header);
    pane_reset(&s_body);
    s_hint = (ui::RichText *)s_page->FindChild("vjo_hint");
    {
        /* The views get these empty scrollbars instead of their default
         * ones, which flashed whenever the text moved. */
        static const char *const sbars[] = {"vjo_header_sbar_v", "vjo_header_sbar_h", "vjo_body_sbar_v",
                                            "vjo_body_sbar_h"};
        for (unsigned i = 0; i < sizeof(sbars) / sizeof(sbars[0]); i++) {
            ui::Widget *w = s_page->FindChild(sbars[i]);
            if (w)
                w->Hide();
        }
    }
    s_region_layer = s_page->FindChild("vjo_region_layer");
    s_region_rect = s_page->FindChild("vjo_region_rect");
    s_region_hint = (ui::RichText *)s_page->FindChild("vjo_region_hint");
    if (s_region_layer)
        s_region_layer->Hide();
    /* Wrapped text with Japanese line-breaking rules. */
    {
        ui::Text *texts[2] = {s_header.text, s_body.text};
        for (int i = 0; i < 2; i++) {
            if (!texts[i])
                continue;
            texts[i]->SetLayoutAttribute(graph::TextLayoutAttribute_WordWrap, true);
            texts[i]->SetLayoutAttribute(graph::TextLayoutAttribute_Kinsoku, true);
        }
    }
    {
        char hint[160];
        int anki;
        vjo_view_lock();
        anki = g_view.anki_enabled;
        vjo_view_unlock();
        sceClibSnprintf(hint, sizeof(hint),
                        "<font color=\"#8a94a6\">◀ ▶ ▲ ▼ word · %sstick scroll · □ region · ○ close</font>",
                        anki ? "× queue · △ send · " : "");
        set_rich(s_hint, hint);
    }
    s_mode = MODE_OVERLAY;
    s_selected = 0;
    s_hl_len = 0;
    s_n_entries = 0;
    s_frames = s_moves = 0;
    s_frame_max_us = 0;
    s_stick_max = 0;
    s_prev_buttons = ~0u; /* ignore buttons already held when opening */
    s_prev_touch = 1;
}

static void close_page(void)
{
    Plugin::PageCloseParam param;
    if (s_mode == MODE_REGION)
        leave_region_mode();
    param.fade = false;
    s_plugin->PageClose("vjo_page_overlay", param);
    s_page = NULL;
    s_panel = s_region_layer = s_region_rect = NULL;
    vjo_log("overlay closed: %d frames, %d word moves, stick %d, slowest frame %d ms", s_frames, s_moves,
            s_stick_max, (int)(s_frame_max_us / 1000));
    s_header.view = s_header.box = s_body.view = s_body.box = NULL;
    s_header.text = s_body.text = NULL;
    s_separator = s_band_mid = NULL;
    s_hint = s_region_hint = NULL;
    s_n_entries = 0;
}

/* ▲/▼: the word on the nearest line above/below that has one, closest in x
 * to the selected word; from the header's own layout (GetCharInfo). */
static int entry_vertical(int up)
{
    graph::TextLayoutCharInfo ci;
    uint32_t cur_line;
    float cur_x;
    int best = s_selected;
    uint32_t best_line = 0;
    float best_dx = 0.0f;
    if (!s_header.text || !s_hl_len || s_header.text->GetCharInfo(s_hl_start, ci) < 0)
        return s_selected;
    cur_line = ci.line;
    cur_x = ci.x;
    for (int i = 0; i < s_n_entries && i < MAX_RANGES; i++) {
        float dx;
        if (i == s_selected || !s_rng_len[i] || s_header.text->GetCharInfo(s_rng_start[i], ci) < 0)
            continue;
        if (up ? ci.line >= cur_line : ci.line <= cur_line)
            continue;
        dx = ci.x > cur_x ? ci.x - cur_x : cur_x - ci.x;
        /* nearest line first, then nearest x */
        if (best == s_selected || (up ? ci.line > best_line : ci.line < best_line) ||
            (ci.line == best_line && dx < best_dx)) {
            best = i;
            best_line = ci.line;
            best_dx = dx;
        }
    }
    return best;
}

static void input_overlay(const VjoInput *in, uint32_t pressed)
{
    if (s_hook_picker) {
        if (!s_hook_matching && (pressed & SCE_CTRL_UP) && s_hook_selected > 0) s_hook_selected--;
        if (!s_hook_matching && (pressed & SCE_CTRL_DOWN) && s_hook_selected + 1 < s_hook_count) s_hook_selected++;
        if (!s_hook_matching && (pressed & (SCE_CTRL_UP | SCE_CTRL_DOWN))) render_hooks();
        if (!s_hook_matching && (pressed & SCE_CTRL_CROSS) && s_hook_count)
            vjo_post_hook(s_hook_session, s_hook_ids[s_hook_selected]);
        if (pressed & SCE_CTRL_SQUARE) {
            if (in->buttons & SCE_CTRL_SELECT) enter_region_mode();
            else if (!s_hook_matching) vjo_post_command(VJO_CMD_HOOK_DISCOVER, NULL);
        }
        if (s_hook_can_ocr && (pressed & SCE_CTRL_TRIANGLE)) vjo_post_command(VJO_CMD_HOOK_OCR, NULL);
        if (pressed & SCE_CTRL_CIRCLE) vjo_post_command(VJO_CMD_CLOSED, NULL);
        int dy = (int)in->ly - 128;
        if (dy <= -STICK_DEADZONE || dy >= STICK_DEADZONE)
            pane_scroll_to(&s_body, s_body.scroll + (float)dy * STICK_SCROLL_PX);
        return;
    }
    int n = s_n_entries;
    int next = s_selected;
    int dy;
    if ((pressed & SCE_CTRL_RIGHT) && s_selected + 1 < n)
        next = s_selected + 1;
    if ((pressed & SCE_CTRL_LEFT) && s_selected > 0)
        next = s_selected - 1;
    if (pressed & SCE_CTRL_UP)
        next = entry_vertical(1);
    if (pressed & SCE_CTRL_DOWN)
        next = entry_vertical(0);
    if (next != s_selected) {
        s_selected = next;
        s_moves++;
        render(0);
    }
    /* either stick scrolls a long definition */
    dy = (int)in->ly - 128;
    if (dy > -STICK_DEADZONE && dy < STICK_DEADZONE)
        dy = (int)in->ry - 128;
    if ((dy < 0 ? -dy : dy) > s_stick_max)
        s_stick_max = dy < 0 ? -dy : dy;
    if (dy <= -STICK_DEADZONE || dy >= STICK_DEADZONE)
        pane_scroll_to(&s_body, s_body.scroll + (float)dy * STICK_SCROLL_PX);
    if ((pressed & SCE_CTRL_CROSS) && s_anki_enabled && !s_anki_syncing && s_selected < n)
        vjo_anki_post_add(s_list_seq, s_selected);
    if ((pressed & SCE_CTRL_TRIANGLE) && s_anki_enabled)
        vjo_anki_post_sync();
    if (pressed & SCE_CTRL_CIRCLE)
        vjo_post_command(VJO_CMD_CLOSED, NULL);
    if (pressed & SCE_CTRL_SQUARE) {
        if (in->buttons & SCE_CTRL_SELECT) vjo_post_command(VJO_CMD_HOOK_DISCOVER, NULL);
        else enter_region_mode();
    }
}

static void input_region(uint32_t held, uint32_t pressed)
{
    const inputdevice::touchscreen::Data *pd = NULL;
    int touching = 0, x = 0, y = 0;
    /* paf: screen pixels from the top left */
    inputdevice::touchscreen::GetData(inputdevice::touchscreen::PORT_FRONT, 0, &pd);
    if (pd && pd->state != inputdevice::touchscreen::Data::STATE_NONE &&
        pd->state != inputdevice::touchscreen::Data::STATE_RELEASE) {
        float fx = 0.0f, fy = 0.0f;
        int rc = inputdevice::touchscreen::GetPointFromTopleft(inputdevice::touchscreen::PORT_FRONT, 0, fx, fy);
        touching = 1;
        x = rc >= 0 ? (int)fx : pd->x;
        y = rc >= 0 ? (int)fy : pd->y;
    }
    if (touching) {
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        if (x >= SCREEN_W)
            x = SCREEN_W - 1;
        if (y >= SCREEN_H)
            y = SCREEN_H - 1;
        if (!s_prev_touch || !s_drag) {
            s_drag = 1;
            s_x0 = s_x1 = x;
            s_y0 = s_y1 = y;
            vjo_log("region drag from %d,%d", x, y);
        } else {
            s_x1 = x;
            s_y1 = y;
        }
        s_has_rect = (s_x0 != s_x1) && (s_y0 != s_y1);
        show_region_rect();
    } else {
        if (s_drag)
            vjo_log("region drag to %d,%d", s_x1, s_y1);
        s_drag = 0;
    }
    s_prev_touch = touching;

    /* hold □ (from entering region mode) for 1 s -> full screen */
    if (!(held & SCE_CTRL_SQUARE))
        s_square_clear_armed = 0;
    if (pressed & SCE_CTRL_SQUARE) {
        s_square_clear_armed = 1;
        s_square_down_us = sceKernelGetProcessTimeWide();
    }
    if (s_square_clear_armed && sceKernelGetProcessTimeWide() - s_square_down_us >= CLEAR_HOLD_US) {
        s_square_clear_armed = 0;
        leave_region_mode();
        vjo_post_command(VJO_CMD_CLEAR_REGION, NULL);
        return;
    }
    if ((pressed & SCE_CTRL_CROSS) && s_has_rect) {
        VjoRect r;
        int x = s_x0 < s_x1 ? s_x0 : s_x1, y = s_y0 < s_y1 ? s_y0 : s_y1;
        int w = s_x0 < s_x1 ? s_x1 - s_x0 : s_x0 - s_x1, h = s_y0 < s_y1 ? s_y1 - s_y0 : s_y0 - s_y1;
        r.x = (uint16_t)(x * 65536 / SCREEN_W);
        r.y = (uint16_t)(y * 65536 / SCREEN_H);
        r.w = (uint16_t)((w * 65536 / SCREEN_W) > 65535 ? 65535 : w * 65536 / SCREEN_W);
        r.h = (uint16_t)((h * 65536 / SCREEN_H) > 65535 ? 65535 : h * 65536 / SCREEN_H);
        leave_region_mode();
        vjo_post_command(VJO_CMD_SET_REGION, &r);
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE)
        leave_region_mode();
}

/* ---- frame ---- */

static void frame_body(unsigned version, unsigned anki_version);

static void frame(void *arg)
{
    int want_open, want_strip;
    unsigned version, anki_version, strip_version;
    (void)arg;

    /* Runs on SceShell's paf main thread every frame: while closed, skip the
     * lock unless an open is published (a store under the lock; a stale read
     * only delays the open by a frame). */
    if (!s_page && !vjo_strip_is_open() && !__atomic_load_n(&g_view.open, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_view.strip_on, __ATOMIC_ACQUIRE))
        return;
    vjo_view_lock();
    want_open = g_view.open;
    version = g_view.version;
    anki_version = g_view.anki_version;
    want_strip = g_view.strip_on && g_view.strip_text[0] && !want_open;
    strip_version = g_view.strip_version;
    vjo_view_unlock();

    /* the strip first: it closes before the overlay opens */
    vjo_strip_frame(s_plugin, want_strip, strip_version);

    if (want_open && !s_page) {
        open_page();
        s_seen_version = version - 1;
        s_seen_anki_version = anki_version;
    } else if (!want_open && s_page) {
        close_page();
        return;
    }
    if (!s_page)
        return;
    {
        int64_t t0 = sceKernelGetProcessTimeWide();
        frame_body(version, anki_version);
        t0 = sceKernelGetProcessTimeWide() - t0;
        if (t0 > s_frame_max_us)
            s_frame_max_us = t0;
        s_frames++;
    }
}

/* One frame with the overlay page open. */
static void frame_body(unsigned version, unsigned anki_version)
{
    if (version != s_seen_version) {
        s_seen_version = version;
        s_seen_anki_version = anki_version;
        render(1);
    } else if (anki_version != s_seen_anki_version) {
        /* Anki status or marks: the body only, keeping the selection and
         * the definition's scroll */
        float scroll = s_body.scroll;
        s_seen_anki_version = anki_version;
        render(0);
        s_body.scroll = scroll;
    }
    if (s_follow_frames > 0 && (header_follow() || --s_follow_frames == 0))
        s_follow_frames = 0;
    header_fit();
    pane_place(&s_header);
    pane_place(&s_body);

    {
        VjoInput in;
        uint32_t held, pressed;
        sceClibMemset(&in, 0, sizeof(in));
        in.ly = in.ry = 128;
        vjoPollInput(&in);
        held = in.buttons;
        pressed = held & ~s_prev_buttons;
        s_prev_buttons = held;
        if (s_mode == MODE_OVERLAY)
            input_overlay(&in, pressed);
        else
            input_region(held, pressed);
    }
}

extern "C" void vjo_overlay_init(void *plugin)
{
    s_plugin = (Plugin *)plugin;
    vjo_arena_init(&s_ui, s_ui_mem, sizeof(s_ui_mem));
    common::MainThreadCallList::Register(frame, NULL);
}
