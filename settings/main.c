/* Standalone LiveArea settings app: no network, no plugin imports. */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>
#include "catalog.h"
#include "../vita/games_storage.h"

#define ROWS 5
#define BG RGBA8(13, 18, 31, 255)
#define CARD RGBA8(24, 33, 52, 255)
#define SELECTED RGBA8(32, 51, 77, 255)
#define WHITE RGBA8(238, 244, 255, 255)
#define DIM RGBA8(149, 169, 194, 255)
#define ACCENT RGBA8(91, 220, 207, 255)
#define OFF RGBA8(244, 160, 167, 255)
static VjoInstalledGame library[VJO_GAMES_MAX];
static VjoGames policy, saved;
static unsigned char scratch_mem[40 * 1024];
static VjoArena scratch;
static vita2d_pvf *font;
static vita2d_texture *icons[ROWS];
static int visible[VJO_GAMES_MAX], total, count, selected, offset, filter, truncated;
static int icons_offset = -1, writable = 1;
static char status[160];

static void text(int x, int y, unsigned color, float size, const char *s)
{ vita2d_pvf_draw_text(font, x, y, color, size, s); }

static void free_icons(void)
{
    for (int i = 0; i < ROWS; i++) {
        if (icons[i]) vita2d_free_texture(icons[i]);
        icons[i] = NULL;
    }
    icons_offset = -1;
}

static void rebuild(void)
{
    count = 0;
    for (int i = 0; i < total; i++) {
        int on = vjo_games_enabled(&policy, library[i].id);
        if (!filter || (filter == 1 && on) || (filter == 2 && !on)) visible[count++] = i;
    }
    if (selected >= count) selected = count ? count - 1 : 0;
    if (selected < offset) offset = selected;
    if (selected >= offset + ROWS) offset = selected - ROWS + 1;
    if (!count) offset = 0;
    free_icons();
}

static void save(void)
{
    if (!writable) {
        policy = saved;
        snprintf(status, sizeof(status), "Cannot read settings. Repair games.ini first.");
    } else if (vjo_games_file_save(&policy, &scratch) < 0) {
        policy = saved;
        /* A final sync can fail after publication; show the actual disk state. */
        if (vjo_games_file_load(&policy, &scratch) >= 0) saved = policy;
        snprintf(status, sizeof(status), "Save could not be confirmed. Check your choices and retry.");
    } else {
        saved = policy;
        snprintf(status, sizeof(status), "Saved. Close this app, then restart or resume your game.");
    }
    rebuild();
}

static void toggle(void)
{
    if (!count) return;
    const char *id = library[visible[selected]].id;
    if (vjo_games_set(&policy, id, !vjo_games_enabled(&policy, id)) < 0) {
        snprintf(status, sizeof(status), "Settings are full (512 title overrides). No change saved.");
        return;
    }
    save();
}

static void toggle_default(void)
{
    policy.default_enabled = !policy.default_enabled;
    save();
}

static void change_filter(void)
{
    filter = (filter + 1) % 3;
    selected = offset = 0;
    rebuild();
}

static void scan(void)
{
    total = vjo_catalog_scan(library, VJO_GAMES_MAX, &truncated);
    rebuild();
    if (truncated) snprintf(status, sizeof(status), "Showing the first 512 installed titles.");
}

static void move(int delta)
{
    if (!count) return;
    selected += delta;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    int prev = offset;
    if (selected < offset) offset = selected;
    if (selected >= offset + ROWS) offset = selected - ROWS + 1;
    if (prev != offset) free_icons();
}

static void draw(void)
{
    const char *filters[] = {"All games", "Enabled", "Disabled"};
    if (icons_offset != offset) {
        free_icons();
        for (int r = 0; r < ROWS && offset + r < count; r++) {
            const char *path = library[visible[offset + r]].icon;
            if (path[0]) icons[r] = vita2d_load_PNG_file(path);
        }
        icons_offset = offset;
    }
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, 960, 4, ACCENT);
    text(30, 46, WHITE, 1.45f, "JP Overlay Settings");
    char label[80];
    snprintf(label, sizeof(label), "%d installed titles  /  %s", total, filters[filter]);
    text(30, 74, DIM, 0.85f, label);
    text(757, 45, ACCENT, 0.85f, "[Square] Filter");
    vita2d_draw_rectangle(24, 91, 912, 61, CARD);
    text(42, 117, WHITE, 1.0f, policy.default_enabled ? "All games by default" : "Selected games only");
    text(42, 140, DIM, 0.75f, "Per-game choices override this default. [Triangle] Change");
    text(790, 125, ACCENT, 0.9f, policy.default_enabled ? "DEFAULT ON" : "DEFAULT OFF");
    for (int r = 0; r < ROWS && offset + r < count; r++) {
        int index = offset + r, y = 162 + r * 60;
        const VjoInstalledGame *g = &library[visible[index]];
        int on = vjo_games_enabled(&policy, g->id);
        vita2d_draw_rectangle(24, y, 912, 56, index == selected ? SELECTED : CARD);
        if (index == selected) vita2d_draw_rectangle(24, y, 4, 56, ACCENT);
        if (icons[r]) vita2d_draw_texture_scale(icons[r], 38, y + 6,
            44.0f / vita2d_texture_get_width(icons[r]), 44.0f / vita2d_texture_get_height(icons[r]));
        else {
            vita2d_draw_rectangle(38, y + 6, 44, 44, SELECTED);
            text(49, y + 33, ACCENT, 0.9f, "JP");
        }
        /* Scissor long/Japanese names without cutting their UTF-8 bytes. */
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(98, y, 767, y + 56);
        text(98, y + 24, WHITE, 0.98f, g->title);
        text(98, y + 45, DIM, 0.72f, g->id);
        vita2d_disable_clipping();
        text(783, y + 33, on ? ACCENT : OFF, 0.9f, on ? "ON" : "OFF");
        vita2d_draw_rectangle(841, y + 18, 68, 22, on ? ACCENT : DIM);
        vita2d_draw_fill_circle(on ? 895 : 855, y + 29, 8, BG);
    }
    if (!count) text(42, 211, DIM, 1.0f, "No games in this filter. Press Square to show all.");
    text(30, 483, writable ? DIM : OFF, 0.78f, status);
    text(30, 519, WHITE, 0.78f, "D-pad / L-R: Navigate    X / Touch: Toggle    Start: Refresh    Circle: Exit");
    if (count) {
        snprintf(label, sizeof(label), "%d/%d", selected + 1, count);
        text(851, 483, ACCENT, 0.8f, label);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

int main(void)
{
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    int load = vjo_games_file_load(&policy, &scratch);
    writable = load >= 0;
    saved = policy;
    snprintf(status, sizeof(status), "%s", load < 0 ? "Cannot read settings. Repair games.ini first." :
        load > 0 ? "Recovered saved choices from backup. Changes save automatically." :
        "Turn OFF Uncharted to keep L+R free. Changes save automatically.");
    if (vita2d_init() < 0) { sceKernelExitProcess(1); return 1; }
    font = vita2d_load_default_pvf();
    if (!font) { vita2d_fini(); sceKernelExitProcess(1); return 1; }
    vita2d_set_clear_color(BG);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    scan();
    uint32_t previous = 0;
    uint64_t repeat_at = 0;
    int touched = 0;
    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~previous;
        uint32_t direction = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
        uint64_t now = sceKernelGetProcessTimeWide();
        if (pressed & SCE_CTRL_CIRCLE) break;
        if (pressed & SCE_CTRL_CROSS) toggle();
        if (pressed & SCE_CTRL_TRIANGLE) toggle_default();
        if (pressed & SCE_CTRL_SQUARE) change_filter();
        if (pressed & SCE_CTRL_START) scan();
        if (pressed & SCE_CTRL_LTRIGGER) move(-ROWS);
        if (pressed & SCE_CTRL_RTRIGGER) move(ROWS);
        if (direction && ((pressed & direction) || now >= repeat_at)) {
            move(direction & SCE_CTRL_UP ? -1 : 1);
            repeat_at = now + ((pressed & direction) ? 350000 : 90000);
        }
        previous = pad.buttons;
        SceTouchData touch;
        memset(&touch, 0, sizeof(touch));
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
        if (touch.reportNum && !touched) {
            int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
            if (x >= 24 && x <= 936 && y >= 91 && y < 152) toggle_default();
            else if (x >= 750 && y < 80) change_filter();
            else if (x >= 24 && x <= 936 && y >= 162 && y < 462) {
                int row = (y - 162) / 60;
                if ((y - 162) % 60 < 56 && offset + row < count) {
                    selected = offset + row;
                    toggle();
                }
            }
        }
        touched = touch.reportNum != 0;
        draw();
        sceDisplayWaitVblankStart();
    }
    vita2d_wait_rendering_done();
    free_icons();
    vita2d_free_pvf(font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
