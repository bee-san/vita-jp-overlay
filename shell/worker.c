/* Control thread (kernel events, overlay state, auto/on-press policy) and
 * worker thread (capture -> Lens or local ncnn -> dictionary). */
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#ifdef VJO_MEMORY_DIAGNOSTICS
#include "memory_probe.h"
extern void vjo_paf_probe_once(void);
#endif

#include "../core/jpegsw.h"
#include "../core/regions.h"
#include "../core/local_ocr.h"
#include "../core/text_source.h"
#include "../include/vjo_import.h"
#include "shell.h"
#ifdef VJO_WITH_MEIKI
#include "meiki_bridge.h"
#ifdef VJO_MEIKI_GAME_WORKER
#include "game_ocr_client.h"
#include "../include/vjo_game_ocr_diag.h"
#else
static VjoMeikiBridge meiki_bridge = {.module = -1};
#endif
#endif
#ifdef VJO_PAF_ALLOC
extern void *vjo_paf_alloc(size_t bytes);
extern void vjo_paf_free(void *pointer);
extern void vjo_paf_memory_report(void);
static void *result_memory;
#endif

/* Memory: two result arenas (the overlay shows one while the network thread
 * fills the other) in one memblock allocated only when a job needs it, plus a small
 * static scratch arena (control thread only) for config/region/messages. */
#ifdef VJO_MEIKI_GAME_WORKER
#define RESULT_ARENA_SIZE (160 * 1024)
#else
#define RESULT_ARENA_SIZE (384 * 1024)
#endif
#define NATIVE_ARENA_SIZE (128 * 1024)
#define SCRATCH_SIZE      (24 * 1024)
#define MEM_SIZE          (2 * RESULT_ARENA_SIZE)

#define POLL_TIMEOUT_US   50000
#define CAPTURE_TIMEOUT_US 3000000
#define BACKOFF_MIN_US    2000000LL
#define BACKOFF_MAX_US    60000000LL
#define JPEG_QUALITY      80
#define SUBTITLE_GAP_US   1000000LL /* between subtitle jobs */
#ifdef VJO_WITH_NCNN
#define JOB_STACK_BYTES  0x40000 /* recursive graph traversal and ARM kernels */
#else
#define JOB_STACK_BYTES  0x10000
#endif

VjoView g_view;

static SceUID view_lock = -1;
static SceUID cmd_lock = -1;
static SceUID capture_lock = -1; /* the kernel's one raw buffer: OCR job vs Anki screenshot */
static SceUID ctl_thread = -1, net_thread = -1;
static int threads_started;
static int anki_started; /* optional: the overlay runs without it */
static SceUID net_evf = -1;
static volatile int running = 1;

static SceUID mem_uid = -1;
static void *mem_heap; /* optional bounded Paf allocation, owned instead of mem_uid */
enum { MEM_RESULTS, MEM_NATIVE, MEM_ANCHOR };
static int mem_kind;
static VjoArena results[2];
static VjoArena scratch; /* static; control thread only */
static uint8_t scratch_mem[SCRATCH_SIZE];
static int active = -1;               /* results[] index shown/cached */
static uint32_t cache_checksum;
static int cache_ok;
static VjoOverlayData cache_data[2];
/* Raw native text shares the picker's resident text storage. The picker and
 * this list are mutually exclusive; neither depends on a result arena. */
static VjoEntryList native_list;

static VjoConfig cfg;      /* control thread */
static VjoConfig job_cfg;  /* snapshot used by the network thread */
static VjoPlatform plat;
static char title_id[12];   /* set while a game is active ("" otherwise) */
static int region_selected, job_region_selected;
static unsigned capture_generation, job_generation;
static VjoTextSnapshot text_state;
static int native_game, text_calibrated, text_started;
static int job_native, job_anchor;
static char job_hook_text[VJO_TEXT_BYTES], seen_hook_text[VJO_TEXT_BYTES];
static uint32_t seen_hook_id;
static int64_t hook_changed_us, hook_log_after;
static int hook_log_pending;
static uint32_t pending_hook_session, pending_hook_id;

/* Control-thread state. The overlay and the network job are independent: an
 * auto job runs while the overlay is closed, the overlay can open while a
 * job runs, and a result can be shown while the next job runs.
 *   overlay: OV_CLOSED, OV_OPEN, or OV_OPEN_OLD_JOB (opened while a job was
 *            already running: its result may be for an earlier screen, which
 *            is checked when it ends)
 *   job:     job_running (set here, finished via job_done from the network
 *            thread)
 *   subtitles: on/off (the strip shows each job's sentence while the
 *            overlay is closed)
 * plus inputs to the background policy: stable_pending (a REGION_STABLE
 * that has not been acted on yet) and the error backoffs, one per stage (a
 * failing dictionary does not stop the subtitles, which need only OCR).
 * Each event has one handler below (on_*). */
enum { OV_CLOSED, OV_OPEN, OV_OPEN_OLD_JOB };
static int ov = OV_CLOSED;
static int subtitles; /* written by set_subtitles only */
static int stable_pending;
typedef struct {
    int64_t until, us;
} Backoff;
static Backoff ocr_backoff, dict_backoff, mem_backoff;
static int64_t job_started_us;
static void backoff_note(Backoff *b, int failed);

/* network job */
#define NET_EV_JOB  1u
#define NET_EV_QUIT 2u
static volatile int job_running;
#ifdef VJO_MEMORY_DIAGNOSTICS
static int diagnostic_failure;
#endif
static volatile int job_done;
static volatile int job_idx;
static volatile int job_rc;
static volatile uint32_t job_checksum;
static volatile int job_lookup;     /* the job looks the words up (else OCR only) */
/* cache_data[job_idx].sentence is ready (the lookup may still run): the
 * net thread only appends to the arena, so the control thread may read it */
static volatile int job_text_ready;

static volatile int pending_cmd;
static VjoRect pending_rect;

void vjo_view_lock(void)
{
    sceKernelLockMutex(view_lock, 1, NULL);
}

void vjo_view_unlock(void)
{
    sceKernelUnlockMutex(view_lock, 1);
}

void vjo_post_command(int cmd, const VjoRect *rect)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    pending_cmd = cmd;
    if (rect)
        pending_rect = *rect;
    sceKernelUnlockMutex(cmd_lock, 1);
}

void vjo_post_hook(uint32_t session, uint32_t id)
{
    sceKernelLockMutex(cmd_lock, 1, NULL);
    pending_hook_session = session;
    pending_hook_id = id;
    pending_cmd = VJO_CMD_HOOK_SELECT;
    sceKernelUnlockMutex(cmd_lock, 1);
}

static int native_enabled(void)
{
    return native_game && cfg.text_source != VJO_SOURCE_OCR;
}

static void copy_utf8(char *dst, size_t cap, const char *src);

static void picker_match(int state, const char *reference)
{
    vjo_view_lock();
    g_view.hook_match_state = state;
    g_view.hook_ocr_available = cfg.text_source != VJO_SOURCE_HOOKS;
    copy_utf8(g_view.hook_reference, sizeof(g_view.hook_reference), reference ? reference : "");
    vjo_view_unlock();
}

/* Publish only on an explicit open/refresh or a completed screenshot. Live
 * hook traffic continues in the kernel without moving the rows under input. */
static void picker_publish(void)
{
    vjo_view_lock();
    g_view.hook_picker = 1;
    g_view.list = NULL;
    g_view.list_seq++;
    g_view.hook_session = text_state.session;
    g_view.hook_count = 0;
    for (unsigned i = 0; i < text_state.count && i < VJO_TEXT_CHOICES; i++) {
        if (cfg.text_source != VJO_SOURCE_HOOKS &&
            (g_view.hook_match_state != VJO_HOOK_MATCH_READY || text_state.candidates[i].score < 50))
            continue;
        g_view.hooks[g_view.hook_count++] = text_state.candidates[i];
    }
    g_view.version++;
    vjo_view_unlock();
}

static int picker_selection_current(void)
{
    int valid = 0;
    vjo_view_lock();
    if (g_view.hook_session == text_state.session)
        for (unsigned i = 0; i < g_view.hook_count; i++)
            if (g_view.hooks[i].id == text_state.current.id &&
                !sceClibStrcmp(g_view.hooks[i].text, text_state.current.text) &&
                (cfg.text_source == VJO_SOURCE_HOOKS ||
                 (g_view.hook_match_state == VJO_HOOK_MATCH_READY && text_state.current.score >= 50)))
                valid = 1;
    vjo_view_unlock();
    return valid;
}

static void picker_close(void)
{
    vjo_view_lock();
    g_view.hook_picker = 0;
    g_view.version++;
    vjo_view_unlock();
}

static int64_t now_us(void)
{
    return (int64_t)sceKernelGetProcessTimeWide();
}

static void log_hook_state(const char *why)
{
    vjo_log("hooks %s session=%u seq=%u candidates=%u scanning=%u address=%08X selected=%u kind=%u encoding=%u bytes=%u updates=%u",
            why, text_state.session, text_state.sequence, text_state.count, text_state.scanning,
            text_state.scan_address, text_state.selected, text_state.current.kind, text_state.current.encoding,
            (unsigned)sceClibStrnlen(text_state.current.text, sizeof(text_state.current.text)),
            text_state.current.updates);
    hook_log_after = now_us() + 1000000;
    hook_log_pending = 0;
}

/* Private diagnostics only for an explicit picker action. A pending OCR
 * action is logged once its displayed rows arrive, never on live updates. */
static const char *picker_log_pending;

static void picker_log_text(char out[161], const char *text)
{
    copy_utf8(out, 161, text);
    for (unsigned i = 0; out[i]; i++)
        if ((uint8_t)out[i] < 0x20 || out[i] == 0x7F) out[i] = ' ';
}

static void picker_log(const char *why)
{
    unsigned count, session, match;
    char text[161];
    vjo_view_lock();
    count = g_view.hook_picker ? g_view.hook_count : 0;
    if (count > VJO_TEXT_CHOICES) count = VJO_TEXT_CHOICES;
    session = g_view.hook_picker ? g_view.hook_session : text_state.session;
    match = g_view.hook_match_state;
    vjo_view_unlock();
    vjo_log("picker %s session=%u rows=%u match=%u selected=%u", why,
            session, count, match, text_state.current.id);
    for (unsigned i = 0; i < count; i++) {
        unsigned id, kind, encoding, address;
        vjo_view_lock();
        const VjoTextCandidate *c = &g_view.hooks[i];
        id = c->id; kind = c->kind; encoding = c->encoding; address = c->address;
        picker_log_text(text, c->text);
        vjo_view_unlock();
        vjo_log("picker row=%u id=%u kind=%u enc=%u addr=%08X text=%s",
                i + 1, id, kind, encoding, address, text);
    }
    if (text_state.current.id) {
        const VjoTextCandidate *c = &text_state.current;
        picker_log_text(text, c->text);
        vjo_log("picker selected id=%u kind=%u enc=%u addr=%08X text=%s",
                c->id, c->kind, c->encoding, c->address, text);
    }
}

static void picker_log_action(const char *why)
{
    picker_log_pending = job_running && job_anchor ? why : NULL;
    if (!picker_log_pending) picker_log(why);
}

/* ---------------- memory ---------------- */

static void mem_free(void);

static int has_result_memory(void)
{
#ifdef VJO_PAF_ALLOC
    if (result_memory) return 1;
#endif
    return mem_uid >= 0 || mem_heap != NULL;
}

static int mem_alloc(void)
{
    void *base = NULL;
    size_t arena_size = RESULT_ARENA_SIZE;
    size_t bytes = job_anchor ? RESULT_ARENA_SIZE : MEM_SIZE;
    /* Retire incompatible anchor/native arenas before a later OCR request. */
    if (has_result_memory() &&
        ((job_anchor && mem_kind != MEM_ANCHOR) ||
         (!job_anchor && mem_kind == MEM_ANCHOR) ||
         (mem_kind == MEM_NATIVE && (!native_enabled() || !text_state.current.id))))
        mem_free();
    if (has_result_memory()) return 0;
    if (now_us() < mem_backoff.until) return -1;
#ifdef VJO_PAF_ALLOC
    /* Native hooks retain their existing USER-first bounded fallback. Pure
     * OCR borrows Paf's reserved pages rather than another USER memblock. */
    if (!native_enabled()) {
        vjo_paf_memory_report();
        result_memory = vjo_paf_alloc(bytes);
        if (!result_memory) {
            vjo_log("result arenas: insufficient Paf heap (%u KiB)", (unsigned)(bytes >> 10));
            backoff_note(&mem_backoff, 1);
            return -1;
        }
        base = result_memory;
    } else
#endif
    {
        mem_uid = sceKernelAllocMemBlock("VjoShellMem", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                         (bytes + 0xFFF) & ~0xFFF, NULL);
        if (mem_uid < 0) {
            vjo_log("shell memblock (%d KiB) failed 0x%08X", (unsigned)(bytes >> 10), mem_uid);
            if (job_anchor) mem_heap = vjo_shell_heap_alloc(RESULT_ARENA_SIZE);
            else if (native_enabled() && text_state.current.id)
                mem_heap = vjo_shell_heap_alloc(2 * NATIVE_ARENA_SIZE);
            if (!mem_heap) {
                backoff_note(&mem_backoff, 1);
                return mem_uid;
            }
            base = mem_heap;
            if (!job_anchor) arena_size = NATIVE_ARENA_SIZE;
            vjo_log("%s workspace: Paf heap, %u KiB", job_anchor ? "OCR match" : "native dictionary",
                    (unsigned)((job_anchor ? arena_size : 2 * arena_size) >> 10));
        } else if (sceKernelGetMemBlockBase(mem_uid, &base) < 0 || !base) {
            sceKernelFreeMemBlock(mem_uid);
            mem_uid = -1;
            backoff_note(&mem_backoff, 1);
            return -1;
        }
    }
    backoff_note(&mem_backoff, 0);
    uint8_t *p = base;
    vjo_arena_init(&results[0], p, arena_size);
    vjo_arena_init(&results[1], job_anchor ? NULL : p + arena_size, job_anchor ? 0 : arena_size);
    mem_kind = job_anchor ? MEM_ANCHOR : mem_heap ? MEM_NATIVE : MEM_RESULTS;
    active = -1;
    cache_ok = 0;
    return 0;
}

static void mem_free(void)
{
    if (has_result_memory()) {
        vjo_view_lock();
        g_view.list = NULL;
        vjo_view_unlock();
#ifdef VJO_PAF_ALLOC
        if (result_memory) vjo_paf_free(result_memory);
        else
#endif
        if (mem_heap) vjo_shell_heap_free(mem_heap);
        else sceKernelFreeMemBlock(mem_uid);
    }
#ifdef VJO_PAF_ALLOC
    result_memory = NULL;
#endif
    mem_heap = NULL;
    mem_uid = -1;
    mem_kind = MEM_RESULTS;
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    active = -1;
    cache_ok = 0;
    backoff_note(&mem_backoff, 0);
}

/* ---------------- view ---------------- */

static void view_publish(int open, const VjoEntryList *list, const char *status, int is_error)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.open, open, __ATOMIC_RELEASE); /* also read unlocked (overlay frame) */
    g_view.list = list;
    g_view.list_seq++;
    g_view.anki_enabled = anki_started;
    g_view.anki_status[0] = '\0'; /* a new result clears the previous transient message */
    sceClibSnprintf(g_view.status, sizeof(g_view.status), "%s", status ? status : "");
    g_view.status_is_error = is_error;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.n_warnings = cfg.n_warnings;
    sceClibMemcpy(g_view.warnings, cfg.warnings, sizeof(g_view.warnings));
    g_view.version++;
    vjo_view_unlock();
}

static void view_show_cache(void)
{
    VjoOverlayData *d = &cache_data[active];
    size_t mark = vjo_arena_mark(&scratch);
    const char *err = d->err.rc ? vjo_err_text(&scratch, d->failed_stage, &d->err) : NULL;
    view_publish(1, &d->list, err, err != NULL);
    vjo_arena_release(&scratch, mark);
    if (anki_started) {
        unsigned seq;
        vjo_view_lock();
        seq = g_view.list_seq;
        vjo_view_unlock();
        vjo_anki_post_check(seq);
    }
}

/* Copies UTF-8 text, cut at a character boundary to fit. */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = sceClibStrnlen(src, cap - 1);
    if (n == cap - 1)
        while (n && ((uint8_t)src[n] & 0xC0) == 0x80)
            n--;
    sceClibMemcpy(dst, src, n);
    dst[n] = '\0';
}

/* The strip: text (NULL keeps it) of kind VJO_STRIP_*, and the busy mark. */
static void strip_publish(const char *text, int kind, int busy)
{
    vjo_view_lock();
    __atomic_store_n(&g_view.strip_on, subtitles, __ATOMIC_RELEASE); /* also read unlocked */
    if (text) {
        copy_utf8(g_view.strip_text, sizeof(g_view.strip_text), text);
        g_view.strip_kind = kind;
    }
    g_view.strip_busy = busy;
    g_view.font_size_ja = cfg.font_size_ja;
    g_view.font_size_en = cfg.font_size_en;
    g_view.strip_version++;
    vjo_view_unlock();
}

/* Hook text needs no capture, network job or result arena. Filtering uses the
 * control thread's existing scratch space; only resident copies escape it.
 * Keep job_hook_text and both result arenas untouched while a lookup runs. */
static void native_publish(const char *status, int is_error)
{
    VjoOverlayData d;
    size_t mark = vjo_arena_mark(&scratch);
    const char *text = text_state.current.text;
    if (!text_state.current.id || !text[0]) return;
    if (vjo_overlay_ocr_text(&scratch, &cfg, text, &d) == VJO_OK)
        text = d.sentence;
    if (ov != OV_CLOSED) {
        vjo_view_lock();
        g_view.hook_picker = 0;
        copy_utf8(g_view.hooks[0].text, sizeof(g_view.hooks[0].text), text);
        native_list.header = g_view.hooks[0].text;
        native_list.entries = NULL;
        native_list.n_entries = 0;
        vjo_view_unlock();
        view_publish(1, &native_list, status, is_error);
    }
    if (subtitles)
        strip_publish(text, VJO_STRIP_SENTENCE, 0);
    vjo_arena_release(&scratch, mark);
}

/* ---------------- network thread ---------------- */

typedef struct {
    const uint8_t *data;
} MemJpeg;

static int mem_jpeg_read(void *ud, uint32_t off, void *dst, uint32_t len)
{
    sceClibMemcpy(dst, ((MemJpeg *)ud)->data + off, len);
    return 0;
}

static int raw_rows(void *ud, uint32_t row, uint32_t n, uint8_t *dst)
{
    int rc;
    (void)ud;
    rc = vjoReadRaw(row, n, dst);
    if (rc != (int)n) vjo_log("capture rows %u+%u failed: %d (0x%08X)", row, n, rc, (unsigned)rc);
    return rc;
}

/* Caller holds capture_lock until it finishes reading the captured rows. */
static int wait_capture(uint32_t flags, VjoState *st)
{
    int seq, rc = VJO_OK;
    int64_t deadline;
    seq = vjoRequestCapture(flags);
    if (seq < 0) {
        /* Hardware returned C0000002 for an allocation failure, rather than
         * the kernel's -2. Read the public status instead of guessing at a
         * syscall error transformation; preserve both values in the log. */
        sceClibMemset(st, 0, sizeof(*st));
        st->size = sizeof(*st);
        int state_rc = vjoGetState(st);
        vjo_log("capture request failed %d (0x%08X), state rc=%d alloc=%u", seq,
                (unsigned)seq, state_rc, st->alloc_status);
        rc = seq == VJO_ERR_NO_MEMORY ||
             (!state_rc && st->alloc_status == VJO_ALLOC_FAIL) ? VJO_E_OOM : VJO_E_SOURCE;
        goto out;
    }
    /* CAPTURE_DONE may be left over from an earlier capture: the sequence
     * number decides which capture finished. */
    deadline = now_us() + CAPTURE_TIMEOUT_US;
    for (;;) {
        uint32_t bits = 0;
        st->size = sizeof(*st);
        vjoGetState(st);
        if (st->done_seq == (uint32_t)seq)
            break;
        if (now_us() >= deadline) {
            vjo_log("capture %d timed out", seq);
            rc = VJO_E_SOURCE;
            goto out;
        }
        vjoWaitEvent(VJO_EV_CAPTURE_DONE, &bits, 100000);
    }
    if (st->capture_result != 0) {
        vjo_log("capture failed %d", st->capture_result);
        rc = st->capture_result == VJO_ERR_NO_MEMORY ? VJO_E_OOM : VJO_E_SOURCE;
        goto out;
    }
out:
    return rc;
}

int vjo_capture_jpeg(VjoArena *a, uint32_t flags, int quality, VjoBuf *out, VjoState *st)
{
    int rc;
    int64_t t0;
    sceKernelLockMutex(capture_lock, 1, NULL);
    rc = wait_capture(flags | VJO_CAPTURE_ONCE, st);
    if (rc) goto out;
    t0 = now_us();
    if (vjo_jpeg_encode(a, st->width, st->height, st->raw_stride, raw_rows, NULL, quality, out) < 0) {
        rc = out->oom ? VJO_E_OOM : VJO_E_SOURCE;
        int release_rc = vjoRequestCapture(VJO_CAPTURE_DISCARD);
        if (release_rc < 0) vjo_log("discard failed JPEG capture: %d (0x%08X)", release_rc, (unsigned)release_rc);
        goto out;
    }
    vjo_log("JPEG %ux%u -> %u bytes in %d ms", st->width, st->height, (unsigned)out->len,
            (int)((now_us() - t0) / 1000));
out:
    sceKernelUnlockMutex(capture_lock, 1);
    return rc;
}

static int job_cancelled(void *ud)
{
    (void)ud;
    return !running || job_generation != __atomic_load_n(&capture_generation, __ATOMIC_ACQUIRE);
}

static int run_local_ocr(VjoArena *a, VjoOverlayData *out, uint32_t *checksum)
{
    int rc = VJO_E_OCR_UNAVAILABLE;
#ifdef VJO_WITH_NCNN
    VjoState st;
    void *workspace = NULL;
    SceUID uid;
    VjoOcrStats stats = {0};
    if (!job_region_selected) return VJO_E_OCR_REGION;
    if (job_cancelled(NULL)) return VJO_E_CANCELLED;
    char *text = vjo_arena_alloc(a, VJO_OCR_TEXT_CAP);
    if (!text) return VJO_E_OOM;
    uid = sceKernelAllocMemBlock("VjoOCR", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, VJO_OCR_HEAP_BYTES, NULL);
    if (uid < 0) {
        /* The main unknown on hardware: is this much free in SceShell? */
        vjo_log("ncnn workspace memblock (%u MiB) failed 0x%08X", VJO_OCR_HEAP_BYTES >> 20, uid);
        return VJO_E_OOM;
    }
    if (sceKernelGetMemBlockBase(uid, &workspace) < 0 || !workspace) {
        sceKernelFreeMemBlock(uid);
        return VJO_E_OOM;
    }
    sceKernelLockMutex(capture_lock, 1, NULL);
    rc = wait_capture(0, &st);
    if (!rc) {
        VjoOcrImage image = {NULL, st.width, st.height, raw_rows, job_cancelled};
        if (st.raw_stride != st.width * 4) rc = VJO_E_SOURCE;
        else rc = vjo_local_ocr(&plat, job_cfg.ocr_model_dir, &image, workspace,
                                VJO_OCR_HEAP_BYTES, text, VJO_OCR_TEXT_CAP, &stats);
        *checksum = st.capture_checksum;
    }
    sceKernelUnlockMutex(capture_lock, 1);
    sceKernelFreeMemBlock(uid); /* before dictionary lookup or publishing */
    vjo_log("ncnn rc=%d lines=%u tensor heap peak=%u KiB", rc, stats.lines, (unsigned)(stats.heap_peak >> 10));
    if (!rc) rc = vjo_overlay_ocr_text(a, &job_cfg, text, out);
#else
    (void)a; (void)out; (void)checksum;
#endif
    return rc;
}

#ifdef VJO_MEIKI_GAME_WORKER
static int game_ocr_submit(void *ud, const VjoGameOcrRequest *req)
{ (void)ud; return vjoOcrSubmit(req); }
static int game_ocr_read(void *ud, uint32_t seq, VjoGameOcrResult *out)
{ (void)ud; return vjoOcrRead(seq, out); }
static int game_ocr_cancel(void *ud, uint32_t seq)
{ (void)ud; return vjoOcrCancel(seq); }
static int64_t game_ocr_now(void *ud)
{ (void)ud; return now_us(); }
static void game_ocr_delay(void *ud, uint32_t us)
{ (void)ud; sceKernelDelayThread(us); }
#endif

static int run_meiki_ocr(VjoArena *a, VjoOverlayData *out, uint32_t *checksum)
{
#ifdef VJO_WITH_MEIKI
    VjoState st;
#ifndef VJO_MEIKI_GAME_WORKER
    VjoMeikiStats stats = {0};
#endif
    if (!job_region_selected) return VJO_E_OCR_REGION;
    if (job_cancelled(NULL)) return VJO_E_CANCELLED;
#ifdef VJO_MEIKI_GAME_WORKER
    VjoGameOcrResult *result = vjo_arena_alloc(a, sizeof(*result));
    if (!result) return VJO_E_OOM;
#else
    char *text = vjo_arena_alloc(a, VJO_OCR_TEXT_CAP);
    if (!text) return VJO_E_OOM;
#endif
    sceKernelLockMutex(capture_lock, 1, NULL);
    int rc = wait_capture(0, &st);
    if (!rc && st.raw_stride != st.width * 4) rc = VJO_E_SOURCE;
#ifdef VJO_MEIKI_GAME_WORKER
    if (!rc) {
        VjoGameOcrRequest req = {.size = sizeof(req), .done_seq = st.done_seq,
            .width = st.width, .height = st.height, .stride = st.raw_stride,
            .layout = job_cfg.meiki_layout == VJO_MEIKI_DIALOGUE_BOX
                ? VJO_GAME_OCR_DIALOGUE_BOX : VJO_GAME_OCR_SINGLE_LINE};
        sceClibSnprintf(req.model_dir, sizeof(req.model_dir), "%s", job_cfg.ocr_model_dir);
        const VjoGameOcrClient client = {NULL, game_ocr_submit, game_ocr_read,
                                       game_ocr_cancel, game_ocr_now, game_ocr_delay};
        rc = vjo_game_ocr_exchange(&client, &req, result, job_cancelled, NULL, 60000000);
        *checksum = st.capture_checksum;
        vjo_log("game Meiki rc=%d pool peak=%u KiB metadata peak=%u KiB cleanup=%d",
                rc, result->neural_peak >> 10, result->metadata_peak >> 10,
                result->cleanup_status);
        if (rc == VJO_E_OCR_MODEL_IO) {
            VjoGameOcrIoDiagnostic diagnostic;
            if (!vjo_game_ocr_io_parse(result->text, sizeof(result->text), &diagnostic)) {
                const char *operation = diagnostic.stage == 'O' ? "open" :
                    diagnostic.stage == 'S' ? "size" : diagnostic.stage == 'L' ? "seek" : "read";
                vjo_log("game Meiki model I/O asset=%c operation=%s code=0x%08X app0_stage=%c app0_code=0x%08X",
                        diagnostic.asset, operation, diagnostic.code,
                        diagnostic.control_stage, diagnostic.control_code);
                sceClibSnprintf(result->text, sizeof(result->text),
                    "Meiki could not %s the %s model (0x%08X). Keep the installed model files; this is a game file-access error.",
                    operation, diagnostic.asset == 'D' ? "detector" : "recognizer", diagnostic.code);
                out->err.detail = result->text;
            }
        }
    }
    sceKernelUnlockMutex(capture_lock, 1);
    if (!rc && job_cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (!rc) rc = vjo_overlay_ocr_text(a, &job_cfg, result->text, out);
#else
    if (!rc) rc = vjo_meiki_bridge_start_mode(&meiki_bridge, &plat, job_cfg.ocr_model_dir,
                      job_cfg.meiki_layout == VJO_MEIKI_DIALOGUE_BOX, job_cancelled, NULL);
    if (!rc) {
        VjoOcrImage image = {NULL, st.width, st.height, raw_rows, job_cancelled};
        VjoMeikiEngine engine = vjo_meiki_bridge_engine(&meiki_bridge);
        rc = (job_cfg.meiki_layout == VJO_MEIKI_DIALOGUE_BOX
               ? vjo_meiki_ocr_detected : vjo_meiki_ocr_single_line)(
                           job_cfg.ocr_model_dir, &image, &engine,
                           meiki_bridge.input, meiki_bridge.input_elements,
                           meiki_bridge.scratch, MEIKI_PREPROCESS_SCRATCH_BYTES,
                           text, VJO_OCR_TEXT_CAP, &stats);
        *checksum = st.capture_checksum;
    }
    /* Never release buffers while the module could still reference them. */
    int finish_rc = vjo_meiki_bridge_finish(&meiki_bridge);
    if (!rc) rc = finish_rc;
    sceKernelUnlockMutex(capture_lock, 1);
    vjo_log("Meiki rc=%d lines=%u/%u pool peak=%u KiB remaining=%u; metadata brk peak=%u KiB tainted=%u",
            rc, stats.completed_lines, stats.lines, (unsigned)(stats.heap_peak >> 10),
            (unsigned)stats.heap_remaining, meiki_bridge.module_stats.metadata_brk_peak >> 10,
            meiki_bridge.module_stats.tainted);
    if (!rc && job_cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (!rc) rc = vjo_overlay_ocr_text(a, &job_cfg, text, out);
#endif
    return rc;
#else
    (void)a; (void)out; (void)checksum;
    return VJO_E_OCR_UNAVAILABLE;
#endif
}

static int run_job(VjoArena *a, VjoOverlayData *out, uint32_t *checksum)
{
    VjoState st;
    VjoJpegSource src;
    VjoBuf jb;
    MemJpeg mem;

    sceClibMemset(out, 0, sizeof(*out));
    out->list.header = "";
    if (job_native) {
        if (vjo_overlay_ocr_text(a, &job_cfg, job_hook_text, out)) return out->err.rc;
        /* A text source is the pipeline input. No capture, coordinates, OCR
         * allocation, model load or Lens connection is needed. */
        *checksum = 0;
        goto recognized;
    }
    if (job_cfg.ocr_backend == VJO_OCR_NCNN || job_cfg.ocr_backend == VJO_OCR_MEIKI) {
        int rc = job_cfg.ocr_backend == VJO_OCR_MEIKI
            ? run_meiki_ocr(a, out, checksum) : run_local_ocr(a, out, checksum);
        if (rc) {
            out->failed_stage = VJO_STAGE_OCR;
            if (job_cfg.ocr_backend == VJO_OCR_MEIKI && rc == VJO_E_OCR_REGION)
                out->err.detail = job_cfg.meiki_layout == VJO_MEIKI_SINGLE_LINE
                    ? "Select one horizontal line with Square, then save with Cross."
                    : "Select a horizontal dialogue area with Square, then save with Cross.";
            if (job_cfg.ocr_backend == VJO_OCR_MEIKI && rc == VJO_E_OCR_MODEL)
                out->err.detail = "Install the pinned Meiki model files for the selected layout.";
#ifdef VJO_MEIKI_GAME_WORKER
            if (job_cfg.ocr_backend == VJO_OCR_MEIKI && rc == VJO_E_OCR_UNAVAILABLE)
                out->err.detail = "Install the Meiki OCR worker for this game, then restart it.";
#endif
            return out->err.rc = rc;
        }
        goto recognized;
    }
    vjo_buf_init(&jb, a);
    st.capture_checksum = 0;
    if ((out->err.rc = vjo_capture_jpeg(a, 0, JPEG_QUALITY, &jb, &st)) != VJO_OK) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc;
    }
    *checksum = st.capture_checksum;
    sceClibMemset(&src, 0, sizeof(src));
    src.width = st.width;
    src.height = st.height;
    mem.data = jb.data;
    src.ud = &mem;
    src.read = mem_jpeg_read;
    src.size = (uint32_t)jb.len;
    if (vjo_overlay_ocr(a, &plat, &job_cfg, &src, out))
        return out->err.rc;
recognized:
    if (job_cancelled(NULL)) {
        out->failed_stage = VJO_STAGE_OCR;
        return out->err.rc = VJO_E_CANCELLED;
    }
    __sync_synchronize(); /* the sentence is visible before job_text_ready */
    job_text_ready = 1;
    if (job_lookup) return vjo_overlay_lookup(a, &plat, &job_cfg, out);
    out->list.header = out->sentence ? out->sentence : "";
    return VJO_OK;
}

static int net_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (running) {
        unsigned int bits = 0;
        sceKernelWaitEventFlag(net_evf, NET_EV_JOB | NET_EV_QUIT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                               &bits, NULL);
        if (bits & NET_EV_QUIT)
            break;
        if (bits & NET_EV_JOB) {
            int idx = job_idx;
            uint32_t cs = 0;
            int64_t t0 = now_us();
            vjo_arena_reset(&results[idx]);
            job_rc = run_job(&results[idx], &cache_data[idx], &cs);
            job_checksum = cs;
            vjo_log("job %d done rc=%d in %d ms, arena peak %u", idx, job_rc,
                    (int)((now_us() - t0) / 1000), (unsigned)results[idx].peak);
            __sync_synchronize(); /* results are visible before job_done */
            job_done = 1;
        }
    }
    return 0;
}

/* lookup = 0: OCR only (the subtitles' sentence). */
static void start_job(const char *why, int lookup)
{
    int native = native_enabled() && text_state.current.id;
    if (native) {
        if (!text_state.current.text[0]) return;
        native_publish(NULL, 0);
        if (!lookup) return;
    }
    if (job_running) return;
#ifdef VJO_MEMORY_DIAGNOSTICS
    /* This opt-in build starts no Anki thread. The control thread is the
     * sole caller, before any capture or OCR job can own a workspace. */
    if (cfg.text_source == VJO_SOURCE_OCR && cfg.ocr_backend == VJO_OCR_MEIKI) {
        if (diagnostic_failure) {
            if (ov != OV_CLOSED)
                view_publish(1, NULL, "Memory diagnostic failed; restart the Vita before testing again", 1);
            return;
        }
        vjo_paf_probe_once();
        int probe_rc = vjo_memory_probe_once();
        if (probe_rc <= VJO_MEMORY_PROBE_CLEANUP) {
            diagnostic_failure = probe_rc;
            vjo_log("memory diagnostics failed rc=%d; OCR refused", probe_rc);
            if (ov != OV_CLOSED)
                view_publish(1, NULL, "Memory diagnostic failed; restart the Vita before testing again", 1);
            return;
        }
    }
#endif
    if (native_enabled() && !text_state.current.id &&
        (cfg.text_source == VJO_SOURCE_HOOKS || text_calibrated)) {
        if (ov != OV_CLOSED) picker_publish();
        return;
    }
    job_native = native;
    if (job_native) {
        if (!text_state.current.text[0]) return;
        copy_utf8(job_hook_text, sizeof(job_hook_text), text_state.current.text);
    }
    job_anchor = native_enabled() && !job_native && !text_calibrated;
    if (job_anchor) {
        text_calibrated = 1; /* retry only on an explicit open/read action */
        picker_match(VJO_HOOK_MATCH_MATCHING, NULL);
        if (ov != OV_CLOSED) {
            view_publish(1, NULL, "Reading the visible dialogue with OCR…", 0);
            picker_publish();
        }
        lookup = 0; /* the temporary anchor arena never holds dictionary results */
    }
    if (mem_alloc() < 0) {
        if (native) {
            native_publish("Text available; not enough memory for dictionary lookup", 1);
        } else if (native_enabled()) {
            if (ov != OV_CLOSED) {
                picker_match(VJO_HOOK_MATCH_FAILED, NULL);
                view_publish(1, NULL, "Not enough memory to read the screenshot. Press △ to retry.", 1);
                picker_publish();
            }
            if (subtitles) strip_publish("Open the overlay to choose a text hook", VJO_STRIP_STATUS, 0);
        } else {
            if (ov != OV_CLOSED) view_publish(1, NULL, "Not enough memory for the overlay", 1);
            if (subtitles) strip_publish("Not enough memory for subtitles", VJO_STRIP_ERROR, 0);
        }
        return;
    }
    vjo_log("job start (%s)%s", why, lookup ? "" : ", OCR only");
    job_lookup = lookup;
    job_idx = job_anchor ? 0 : active == 0 ? 1 : 0;
    job_cfg = cfg; /* the control thread may reload cfg while the job runs */
    job_region_selected = region_selected;
    job_generation = __atomic_load_n(&capture_generation, __ATOMIC_ACQUIRE);
    job_done = 0;
    job_text_ready = 0;
    job_running = 1;
    job_started_us = now_us();
    __sync_synchronize();
    sceKernelSetEventFlag(net_evf, NET_EV_JOB);
    if (subtitles && !job_native)
        strip_publish(NULL, 0, 1);
}

/* ---------------- control thread ---------------- */

static int same_dictionary(const VjoConfig *a, const VjoConfig *b)
{
    if (a->dictionary != b->dictionary) return 0;
    if (a->dictionary == VJO_DICT_LOCAL)
        return !sceClibStrcmp(a->local_dictionary_dir, b->local_dictionary_dir) &&
               !sceClibStrcmp(a->local_dictionaries, b->local_dictionaries);
    if (a->dictionary == VJO_DICT_HACHIDORI)
        return !sceClibStrcmp(a->hachidori_host, b->hachidori_host);
    return !sceClibStrcmp(vjo_config_api_key(a), vjo_config_api_key(b));
}

static int same_pipeline(const VjoConfig *a, const VjoConfig *b)
{
    return same_dictionary(a, b) && a->text_source == b->text_source && a->ocr_backend == b->ocr_backend &&
           (a->ocr_backend != VJO_OCR_MEIKI || a->meiki_layout == b->meiki_layout) &&
           a->non_japanese_filter == b->non_japanese_filter &&
           !sceClibStrcmp(a->ocr_model_dir, b->ocr_model_dir);
}

static int auto_mode(void)
{
    return !native_enabled() && cfg.ocr_backend == VJO_OCR_LENS && cfg.ocr_mode == VJO_OCR_AUTO;
}

static void apply_config(void)
{
    VjoConfig previous = cfg;
    vjo_config_load(&cfg, &scratch);
    if (!same_pipeline(&previous, &cfg)) cache_ok = 0;
    if (native_game && previous.text_source != cfg.text_source) {
        text_started = text_calibrated = 0;
        if (vjoTextRead(&text_state) == 0)
            vjoTextControl(text_state.session, VJO_TEXT_OFF, 0);
    }
    vjo_log_configure(&cfg);
    vjo_anki_configure(&cfg);
    if (vjoSetTriggers(cfg.toggle_button, cfg.subtitle_button) < 0)
        vjo_log("kernel refused the buttons: keeping the previous ones");
    for (int i = 0; i < cfg.n_warnings; i++)
        vjo_log("config warning: %s", cfg.warnings[i]);
}

static void push_region(void)
{
    VjoRect r;
    int found = vjo_region_load(title_id, &r, &scratch);
    region_selected = found && r.w && r.h;
    vjoSetRegion(found && r.w ? &r : NULL);
    vjo_log("region: %s%s", found == VJO_REGION_GAME ? "this game's" : found ? "all games'" : "none",
            found && r.w ? "" : ", full screen");
}

static int game_active(void)
{
    return title_id[0] != '\0';
}

static void open_overlay(void)
{
    VjoState st;
    ov = OV_OPEN;
    vjoSetInputBlock(1);
    apply_config(); /* settings are re-read on every open */
    if (cfg.text_source == VJO_SOURCE_HOOKS && !native_game) {
        view_publish(1, NULL, "Native text hooks are available for Vita games", 1);
        return;
    }
    if (native_enabled()) {
        if (vjoTextRead(&text_state) < 0) {
            view_publish(1, NULL, "Cannot read native text sources", 1);
            return;
        }
        log_hook_state("open");
        if (!text_started) {
            vjoTextControl(text_state.session, text_state.selected ? VJO_TEXT_FOLLOW : VJO_TEXT_LISTEN,
                           text_state.selected);
            text_started = 1;
        }
        if (!text_state.current.id) {
            if (!job_running) {
                text_calibrated = 0;
                picker_match(VJO_HOOK_MATCH_NONE, NULL);
                backoff_note(&mem_backoff, 0);
            }
            view_publish(1, NULL, cfg.text_source == VJO_SOURCE_HOOKS ? "Choose a text hook" : "Matching OCR to game text…", 0);
            picker_publish();
            start_job("one-time hook calibration", vjo_config_dict_ready(&cfg));
        } else {
            picker_close();
            view_publish(1, NULL, text_state.current.text[0] ? "Reading game text…" : "Waiting for the selected hook…", 0);
            start_job("native text", vjo_config_dict_ready(&cfg));
        }
        picker_log_action("open");
        return;
    }
    if (!vjo_config_dict_ready(&cfg)) {
        VjoErr e = {cfg.dictionary == VJO_DICT_HACHIDORI ? VJO_E_NO_HOST : VJO_E_NO_KEY,
                    0, 0, NULL, cfg.dictionary};
        size_t mark = vjo_arena_mark(&scratch);
        view_publish(1, NULL, vjo_err_text(&scratch, VJO_STAGE_DICT, &e), 1);
        vjo_arena_release(&scratch, mark);
        return;
    }
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        view_publish(1, NULL, "Not enough memory to capture the screen", 1);
        return;
    }
    if (auto_mode() && cache_ok && active >= 0 && cache_checksum == st.checksum) {
        vjo_log("open: background result");
        view_show_cache();
        return;
    }
    /* Why the background result can't be used (auto mode tuning). */
    if (auto_mode())
        vjo_log("open: %s (stable %u)",
                job_running ? "background job still running"
                : !cache_ok || active < 0 ? "no background result"
                                          : "screen changed since the background result",
                st.stable);
    view_publish(1, NULL, "Recognizing…", 0);
    if (job_running)
        ov = OV_OPEN_OLD_JOB;
    else
        start_job("overlay opened", 1);
}

static void close_overlay(void)
{
    picker_log_pending = NULL;
    if (ov == OV_CLOSED)
        return;
    ov = OV_CLOSED;
    if (job_running && job_anchor)
        __atomic_add_fetch(&capture_generation, 1, __ATOMIC_RELEASE);
    vjoSetInputBlock(0);
    vjo_view_lock();
    __atomic_store_n(&g_view.open, 0, __ATOMIC_RELEASE);
    /* The next job may reuse the shown arena; an open republishes it. */
    g_view.list = NULL;
    g_view.version++;
    vjo_view_unlock();
}

static void backoff_note(Backoff *b, int failed)
{
    if (!failed) {
        b->us = 0;
        b->until = 0;
        return;
    }
    b->us = b->us ? b->us * 2 : BACKOFF_MIN_US;
    if (b->us > BACKOFF_MAX_US)
        b->us = BACKOFF_MAX_US;
    b->until = now_us() + b->us;
}

/* The strip for a finished job: its sentence, else its error. */
static void strip_show_result(const VjoOverlayData *d)
{
    size_t mark = vjo_arena_mark(&scratch);
    if (d->sentence)
        strip_publish(d->sentence, VJO_STRIP_SENTENCE, 0);
    else
        strip_publish(vjo_err_text(&scratch, d->failed_stage, &d->err), VJO_STRIP_ERROR, 0);
    vjo_arena_release(&scratch, mark);
}

static int want_lookup(void);

/* The network thread finished the job. */
static void on_job_done(void)
{
    int idx = job_idx, ocr_failed;
    VjoOverlayData *d = &cache_data[idx];
    __sync_synchronize(); /* pairs with the network thread's barrier */
    job_running = 0;
    job_done = 0;
    job_text_ready = 0;
    if (job_native && sceClibStrcmp(job_hook_text, text_state.current.text)) {
        cache_ok = 0;
        if (game_active() && (ov != OV_CLOSED || subtitles))
            start_job("hook text changed during lookup", ov != OV_CLOSED && vjo_config_dict_ready(&cfg));
        return;
    }
    if (job_cancelled(NULL) || ((job_native || job_anchor) && !same_pipeline(&job_cfg, &cfg))) {
        /* The result is for an earlier region or game. Redo it for whoever
         * still waits: the overlay, or the subtitles (on_command's own job
         * was skipped while this one ran; the strip still shows busy). */
        cache_ok = 0;
        if (job_anchor) {
            mem_free();
            text_calibrated = 0;
            picker_match(VJO_HOOK_MATCH_NONE, NULL);
            if (subtitles && ov == OV_CLOSED)
                strip_publish("Open the overlay to choose game text", VJO_STRIP_STATUS, 0);
        }
        if (game_active() && (ov != OV_CLOSED || (subtitles && !job_anchor))) {
            int lookup = ov != OV_CLOSED || want_lookup();
            if (ov != OV_CLOSED)
                ov = OV_OPEN;
            start_job("capture context changed", lookup);
        }
        return;
    }
    if (job_anchor) {
        size_t mark = vjo_arena_mark(&scratch);
        const char *status;
        int matched = 0;
        if (d->err.rc == VJO_OK && d->sentence && d->sentence[0]) {
            /* The syscall accepts at most VJO_TEXT_BYTES, even if OCR read a
             * whole screen. This scratch copy and the view excerpt outlive
             * the arena only until their respective consumers copy them. */
            char *reference = vjo_arena_alloc(&scratch, VJO_TEXT_BYTES);
            if (reference) {
                copy_utf8(reference, VJO_TEXT_BYTES, d->sentence);
                if (vjoTextReference(text_state.session, reference) == 0) {
                    picker_match(VJO_HOOK_MATCH_READY, reference);
                    matched = 1;
                }
            }
        }
        /* vjoTextReference starts discovery for older clients. The normal
         * picker only listens to game calls; never crawl its entire memory. */
        vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
        vjoTextRead(&text_state);
        if (matched) status = "Choose the text that matches the screenshot.";
        else {
            picker_match(VJO_HOOK_MATCH_FAILED, NULL);
            status = d->err.rc != VJO_OK ? vjo_err_text(&scratch, d->failed_stage, &d->err) :
                     "No readable screenshot text. Check the dialogue region and press △ to retry.";
        }
        if (subtitles) strip_show_result(d);
        if (ov != OV_CLOSED) view_publish(1, NULL, status, !matched);
        vjo_arena_release(&scratch, mark);
        vjo_log("OCR match finished: %s, candidates=%u; releasing %u KiB workspace",
                matched ? "reference ready" : "failed", text_state.count, RESULT_ARENA_SIZE >> 10);
        /* on_job_text runs before the net thread returns: only here is the
         * anchor storage no longer in use. No view retains an arena pointer. */
        mem_free();
        if (ov != OV_CLOSED) picker_publish();
        if (ov != OV_CLOSED && picker_log_pending) picker_log(picker_log_pending);
        picker_log_pending = NULL;
        return;
    }
    ocr_failed = d->failed_stage == VJO_STAGE_OCR;
    backoff_note(&ocr_backoff, ocr_failed);
    if (!ocr_failed && job_lookup)
        backoff_note(&dict_backoff, d->err.rc != VJO_OK);
    if (job_native && d->err.rc == VJO_E_OOM) {
        cache_ok = 0;
        native_publish("Text available; not enough memory for dictionary lookup", 1);
        return;
    }
    if (!job_lookup && ov == OV_OPEN && !job_native) {
        /* OCR only, while the overlay shows a result or an error: that stays
         * (and its arena stays active); the strip takes the sentence */
        if (subtitles)
            strip_show_result(d);
        return;
    }
    /* Publish the new arena; the old one becomes the next job's target. */
    vjo_view_lock();
    active = idx;
    cache_ok = d->err.rc == VJO_OK && (job_lookup || job_native) && same_pipeline(&job_cfg, &cfg);
    cache_checksum = job_checksum;
    vjo_view_unlock();
    if (subtitles)
        strip_show_result(d);
    if (ov == OV_OPEN_OLD_JOB) {
        VjoState st;
        ov = OV_OPEN;
        st.size = sizeof(st);
        vjoGetState(&st);
        if (job_checksum != st.checksum || !job_lookup || !same_pipeline(&job_cfg, &cfg)) {
            start_job(job_lookup ? "result was for an earlier screen" : "the running job was OCR only", 1);
            return;
        }
    }
    if (ov != OV_CLOSED)
        view_show_cache();
    if (native_enabled() && !text_state.current.id && ov != OV_CLOSED)
        picker_publish();
}

/* Does a job started now look the words up? Yes when the overlay can show
 * them: it is open, or opens from the background result (auto). */
static int want_lookup(void)
{
    return vjo_config_dict_ready(&cfg) && (ov != OV_CLOSED || auto_mode()) &&
           now_us() >= dict_backoff.until;
}

/* The network thread posted the job's sentence (before its lookup). */
static void on_job_text(void)
{
    const char *sentence;
    __sync_synchronize(); /* pairs with the network thread's barrier */
    sentence = cache_data[job_idx].sentence;
    job_text_ready = 0;
    if (job_cancelled(NULL) || !same_pipeline(&job_cfg, &cfg) ||
        (job_native && sceClibStrcmp(job_hook_text, text_state.current.text))) return;
    if (job_anchor) return; /* published and released together at job completion */
    vjo_log("subtitle text ready in %d ms", (int)((now_us() - job_started_us) / 1000));
    if (subtitles && sentence && !job_native)
        strip_publish(sentence, VJO_STRIP_SENTENCE, 1);
}

static void set_subtitles(int on)
{
    VjoState st;
    const VjoOverlayData *d = active >= 0 ? &cache_data[active] : NULL;
    subtitles = on;
    vjo_log("subtitles %s", on ? "on" : "off");
    if (!on) {
        strip_publish("", VJO_STRIP_SENTENCE, 0);
        return;
    }
    apply_config(); /* settings are re-read, as when the overlay opens */
    if (native_enabled()) {
        if (!text_started && vjoTextRead(&text_state) == 0) {
            vjoTextControl(text_state.session, text_state.selected ? VJO_TEXT_FOLLOW : VJO_TEXT_LISTEN,
                           text_state.selected);
            text_started = 1;
        }
        strip_publish(text_state.current.id ? "Waiting for game text…" : "Open the overlay to choose a text hook",
                      VJO_STRIP_STATUS, 0);
        if (text_state.current.id) start_job("hook subtitles on", 0);
        return;
    }
    if (cfg.text_source == VJO_SOURCE_HOOKS) {
        strip_publish("Native text hooks require a Vita game", VJO_STRIP_ERROR, 0);
        return;
    }
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.alloc_status == VJO_ALLOC_FAIL) {
        strip_publish("Not enough memory to capture the screen", VJO_STRIP_ERROR, 0);
        return;
    }
    if (!job_running && cache_ok && d && d->sentence && cache_checksum == st.checksum) {
        strip_publish(d->sentence, VJO_STRIP_SENTENCE, 0);
        return;
    }
    strip_publish("Recognizing…", VJO_STRIP_STATUS, 1);
    start_job("subtitles on", want_lookup()); /* if one runs, its result comes */
}

/* subtitle_button: from the full overlay, everything closes. */
static void on_subtitle(void)
{
    if (!game_active())
        return;
    if (ov != OV_CLOSED) {
        close_overlay();
        if (subtitles)
            set_subtitles(0);
    } else {
        set_subtitles(!subtitles);
    }
}

/* A command from the overlay (paf main thread). */
static void on_command(void)
{
    int cmd;
    VjoRect r;
    uint32_t hook_session, hook_id;
    sceKernelLockMutex(cmd_lock, 1, NULL);
    cmd = pending_cmd;
    r = pending_rect;
    hook_session = pending_hook_session;
    hook_id = pending_hook_id;
    pending_cmd = VJO_CMD_NONE;
    sceKernelUnlockMutex(cmd_lock, 1);

    switch (cmd) {
    case VJO_CMD_HOOK_DISCOVER:
        if (!native_enabled() || job_running) break;
        __atomic_add_fetch(&capture_generation, 1, __ATOMIC_RELEASE);
        cache_ok = 0;
        /* Explicitly leaving the chosen source is not a lost-source event. */
        seen_hook_id = 0; seen_hook_text[0] = 0;
        vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
        vjoTextRead(&text_state);
        log_hook_state("refresh");
        if (cfg.text_source != VJO_SOURCE_HOOKS &&
            (!g_view.hook_picker || g_view.hook_match_state != VJO_HOOK_MATCH_READY)) {
            text_calibrated = 0;
            backoff_note(&mem_backoff, 0);
            start_job("choose another text source", 0);
        } else {
            view_publish(1, NULL, cfg.text_source == VJO_SOURCE_HOOKS ?
                "Choose game text." : "Choose the text that matches the screenshot.", 0);
            picker_publish();
        }
        picker_log_action("refresh");
        break;
    case VJO_CMD_HOOK_SELECT:
        if (!native_enabled() || job_running) break;
        if (hook_session != text_state.session ||
            vjoTextControl(hook_session, VJO_TEXT_FOLLOW, hook_id) < 0) {
            view_publish(1, NULL, "That text source is no longer available. Press □ to refresh.", 1);
            picker_publish();
            picker_log("select-missing");
            break;
        }
        __atomic_add_fetch(&capture_generation, 1, __ATOMIC_RELEASE);
        cache_ok = 0;
        vjoTextRead(&text_state);
        if (!picker_selection_current()) {
            picker_log("select-changed");
            vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
            vjoTextRead(&text_state);
            view_publish(1, NULL, "That text changed while you were choosing. Check the refreshed choices.", 1);
            picker_publish();
            break;
        }
        log_hook_state("select");
        picker_log("select");
        picker_close();
        view_publish(1, NULL, "Reading the selected text…", 0);
        start_job("manual hook selection", vjo_config_dict_ready(&cfg));
        break;
    case VJO_CMD_HOOK_OCR:
        if (!native_enabled() || cfg.text_source == VJO_SOURCE_HOOKS || job_running) break;
        seen_hook_id = 0; seen_hook_text[0] = 0;
        vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
        vjoTextRead(&text_state);
        text_calibrated = 0;
        backoff_note(&mem_backoff, 0);
        start_job("explicit OCR hook match", 0);
        picker_log_action("refresh-ocr");
        break;
    case VJO_CMD_CLOSED:
        close_overlay();
        break;
    case VJO_CMD_SET_REGION:
    case VJO_CMD_CLEAR_REGION:
        if (!game_active()) { /* the game exited meanwhile */
            close_overlay();
            break;
        }
        if (cmd == VJO_CMD_SET_REGION)
            vjo_log("region set for %s: %u,%u,%u,%u", title_id, r.x, r.y, r.w, r.h);
        else
            vjo_log("region for %s: full screen", title_id);
        if (vjo_region_save(title_id, cmd == VJO_CMD_SET_REGION ? &r : NULL, &scratch) < 0)
            vjo_log("region.ini: write failed");
        push_region();
        __atomic_add_fetch(&capture_generation, 1, __ATOMIC_RELEASE);
        cache_ok = 0;
        close_overlay();
        if (subtitles)
            start_job("region changed", want_lookup());
        break;
    }
}

/* ---- is the new foreground process a game? ----
 * Its title ID is often not readable right when the process starts, so the
 * lookup is retried for a few seconds. Unknown after that = a game. */
#define CLASSIFY_TIMEOUT_US 3000000

static SceUID pending_pid;
static int64_t pending_since;

int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len); /* SceProcessmgr, not in headers */

static int lookup_title(SceUID pid, char *tid, int size)
{
    int ret;
    sceClibMemset(tid, 0, size);
    ret = sceKernelGetProcessTitleId(pid, tid, size);
    if (ret >= 0 && tid[0])
        return 0;
    sceClibMemset(tid, 0, size);
    ret = sceAppMgrAppParamGetString(pid, 12, tid, size); /* 12 = title ID */
    if (ret >= 0 && tid[0])
        return 0;
    return ret < 0 ? ret : -1;
}

/* vjoSetGameActive's mode for a title: system apps, SceShell and VitaShell
 * (Select starts its FTP server) are not games. The PSP emulator, where
 * Adrenaline's games run, is one with a static framebuffer; all its games
 * share its title ID, so one region. */
static int title_game_mode(const char *tid)
{
    if (!sceClibStrcmp(tid, "NPXS10028"))
        return VJO_GAME_STATIC_FB;
    if (!sceClibStrncmp(tid, "NPXS", 4) || !sceClibStrncmp(tid, "main", 4) ||
        !sceClibStrncmp(tid, "VITASHELL", 9))
        return VJO_GAME_NONE;
    return VJO_GAME;
}

static void activate_game(SceUID pid, const char *tid, int mode)
{
    sceClibSnprintf(title_id, sizeof(title_id), "%s", tid);
    apply_config();
    push_region();
    backoff_note(&ocr_backoff, 0);
    backoff_note(&dict_backoff, 0);
    backoff_note(&mem_backoff, 0);
    vjoSetGameActive(pid, mode);
    native_game = mode == VJO_GAME;
    text_started = text_calibrated = 0;
    picker_match(VJO_HOOK_MATCH_NONE, NULL);
    seen_hook_id = 0; seen_hook_text[0] = 0;
    hook_log_after = 0;
    hook_log_pending = 0;
    picker_log_pending = NULL;
    sceClibMemset(&text_state, 0, sizeof(text_state));
    picker_close();
    if (native_enabled() && vjoTextRead(&text_state) == 0) {
        vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
    }
    vjo_log("game %s started: dictionary %s (key %s), trigger %s, subtitles %s, ocr %s, ocr_mode %s", title_id,
            vjo_dict_name(cfg.dictionary), vjo_config_api_key(&cfg)[0] ? "set" : "MISSING",
            vjo_trigger_name(cfg.toggle_button), vjo_trigger_name(cfg.subtitle_button),
            cfg.ocr_backend == VJO_OCR_NCNN ? "ncnn" : cfg.ocr_backend == VJO_OCR_MEIKI ? "meiki" : "lens",
            cfg.ocr_mode == VJO_OCR_AUTO ? "auto" : "on_press");
}

static void classify_pending(void)
{
    char tid[32];
    SceUID pid = pending_pid;
    int ret = lookup_title(pid, tid, sizeof(tid));
    if (ret == 0) {
        pending_pid = 0;
        int mode = title_game_mode(tid);
        if (mode == VJO_GAME_NONE) {
            vjo_log("%s: system app, not a game", tid);
            vjoSetGameActive(pid, VJO_GAME_NONE);
        } else {
            activate_game(pid, tid, mode);
        }
    } else if (now_us() - pending_since > CLASSIFY_TIMEOUT_US) {
        pending_pid = 0;
        vjo_log("no title ID for 0x%X (0x%08X): treating it as a game", pid, ret);
        activate_game(pid, "GAME", VJO_GAME);
    }
}

static void on_game_exit(void)
{
    __atomic_add_fetch(&capture_generation, 1, __ATOMIC_RELEASE);
    pending_pid = 0;
    close_overlay();
    if (subtitles)
        set_subtitles(0);
    /* Free memory unless the network thread still uses it. */
    if (!job_running)
        mem_free();
    title_id[0] = '\0';
    native_game = text_started = text_calibrated = 0;
    sceClibMemset(&text_state, 0, sizeof(text_state));
    picker_close();
}

/* A new foreground process: classified by classify_pending. */
static void on_game_start(void)
{
    __atomic_add_fetch(&capture_generation, 1, __ATOMIC_RELEASE);
    cache_ok = 0;
    VjoState st;
    stable_pending = 0;
    if (subtitles) /* over a game, without its exit: the strip goes */
        set_subtitles(0);
    st.size = sizeof(st);
    vjoGetState(&st);
    title_id[0] = '\0';
    pending_pid = st.game_pid;
    pending_since = now_us();
}

static void on_trigger(void)
{
    if (ov != OV_CLOSED)
        close_overlay();
    else if (game_active())
        open_overlay();
}

/* A background job for a settled screen: the auto prefetch (for an instant
 * overlay) and the subtitles' refresh (whatever ocr_mode says). */
static int background_wanted(void)
{
    return !native_enabled() && cfg.text_source != VJO_SOURCE_HOOKS &&
           cfg.ocr_backend == VJO_OCR_LENS && (subtitles || (auto_mode() && want_lookup()));
}

static void poll_hooks(void)
{
    uint32_t previous_count = text_state.count, previous_scan = text_state.scanning;
    int picker;
    if (!game_active() || !native_enabled() || vjoTextRead(&text_state) < 0) return;
    if (previous_count != text_state.count || previous_scan != text_state.scanning)
        hook_log_pending = 1;
    if (seen_hook_id && !text_state.selected) {
        /* The old screen no longer validates another source. Keep OCR on
         * demand, and clear its scores before showing fresh candidates. */
        text_calibrated = 1;
        vjoTextReference(text_state.session, "");
        vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
        picker_match(VJO_HOOK_MATCH_NONE, NULL);
        vjoTextRead(&text_state);
    }
    if (!text_started && (ov != OV_CLOSED || subtitles)) {
        vjoTextControl(text_state.session, text_state.selected ? VJO_TEXT_FOLLOW : VJO_TEXT_LISTEN,
                       text_state.selected);
        text_started = 1;
    }
    vjo_view_lock(); picker = g_view.hook_picker; vjo_view_unlock();
    /* Keep the published picker stable until an explicit refresh/reopen. */
    if (text_state.current.id != seen_hook_id || sceClibStrcmp(seen_hook_text, text_state.current.text)) {
        seen_hook_id = text_state.current.id;
        copy_utf8(seen_hook_text, sizeof(seen_hook_text), text_state.current.text);
        hook_changed_us = now_us(); cache_ok = 0;
        hook_log_pending = 1;
        if (!seen_hook_id || !seen_hook_text[0]) {
            if (subtitles) strip_publish("Waiting for a valid text source…", VJO_STRIP_STATUS, 0);
            if (ov != OV_CLOSED) {
                view_publish(1, NULL, "Choose a text hook", 0);
                picker_publish();
            }
            if (!text_state.selected) vjoTextControl(text_state.session, VJO_TEXT_LISTEN, 0);
        } else {
            /* A new native sentence is independent of the failed OCR anchor
             * or the previous sentence's text-filter error. */
            backoff_note(&ocr_backoff, 0);
            picker_close();
            native_publish(NULL, 0);
            picker = 0;
        }
    }
    if (!picker && seen_hook_id && seen_hook_text[0] && !job_running &&
        now_us() - hook_changed_us >= 300000 && ov != OV_CLOSED && vjo_config_dict_ready(&cfg) &&
        (!cache_ok || active < 0) && now_us() >= ocr_backoff.until &&
        now_us() >= dict_backoff.until && now_us() >= mem_backoff.until) {
        start_job("hook text settled", 1);
    }
    if (hook_log_pending && now_us() >= hook_log_after)
        log_hook_state("update");
}

static int background_throttled(void)
{
    int64_t t = now_us();
    return t < ocr_backoff.until || (subtitles && t < job_started_us + SUBTITLE_GAP_US);
}

/* A stable event that arrives while busy (or throttled) is kept, so the
 * newest screen is still recognized afterwards. */
static void auto_prefetch(void)
{
    VjoState st;
    if (!stable_pending || ov != OV_CLOSED || job_running || !game_active() || !background_wanted() ||
        background_throttled())
        return;
    stable_pending = 0;
    st.size = sizeof(st);
    vjoGetState(&st);
    if (st.stable && !(cache_ok && st.checksum == cache_checksum))
        start_job("auto: screen settled", want_lookup());
}

static int ctl_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    /* The order of the handlers within one wakeup is part of the policy
     * (e.g. a trigger and a game start in the same wakeup). */
    while (running) {
        uint32_t bits = 0;
        vjoWaitEvent(VJO_EV_TRIGGER | VJO_EV_SUBTITLE | VJO_EV_REGION_STABLE | VJO_EV_GAME_START |
                         VJO_EV_GAME_EXIT,
                     &bits, POLL_TIMEOUT_US);
        /* Keep the independent control thread awake while inference runs. */
        if (job_running) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        if (bits & VJO_EV_GAME_EXIT)
            on_game_exit();
        if (bits & VJO_EV_GAME_START)
            on_game_start();
        if (pending_pid > 0)
            classify_pending();
        if (bits & VJO_EV_TRIGGER)
            on_trigger();
        if (bits & VJO_EV_SUBTITLE)
            on_subtitle();
        if (job_running && job_text_ready)
            on_job_text();
        if (job_running && job_done)
            on_job_done();
        if (pending_cmd != VJO_CMD_NONE)
            on_command();
        poll_hooks();
        if (bits & VJO_EV_REGION_STABLE)
            stable_pending = 1;
        auto_prefetch();
        if (!game_active() && !job_running && has_result_memory() && ov == OV_CLOSED)
            mem_free();
    }
    return 0;
}

int vjo_worker_start(void)
{
    int ver, rc;
#if defined(__arm__)
    /* Presence in the module list does not mean its imports resolved or its
     * start entry succeeded. Check every bridge stub before the first call. */
#define REQUIRE_KERNEL_IMPORT(fn) do { if (!VJO_IMPORT_READY(fn)) { \
        vjo_log("kernel import unavailable: " #fn "; overlay not started"); \
        return -1; \
    } } while (0)
    REQUIRE_KERNEL_IMPORT(vjoGetVersion);
    REQUIRE_KERNEL_IMPORT(vjoRegisterShell);
    REQUIRE_KERNEL_IMPORT(vjoWaitEvent);
    REQUIRE_KERNEL_IMPORT(vjoGetState);
    REQUIRE_KERNEL_IMPORT(vjoSetRegion);
    REQUIRE_KERNEL_IMPORT(vjoSetTriggers);
    REQUIRE_KERNEL_IMPORT(vjoSetGameActive);
    REQUIRE_KERNEL_IMPORT(vjoSetInputBlock);
    REQUIRE_KERNEL_IMPORT(vjoPollInput);
    REQUIRE_KERNEL_IMPORT(vjoRequestCapture);
    REQUIRE_KERNEL_IMPORT(vjoReadRaw);
    REQUIRE_KERNEL_IMPORT(vjoTextControl);
    REQUIRE_KERNEL_IMPORT(vjoTextReference);
    REQUIRE_KERNEL_IMPORT(vjoTextRead);
#ifdef VJO_MEIKI_GAME_WORKER
    REQUIRE_KERNEL_IMPORT(vjoOcrSubmit);
    REQUIRE_KERNEL_IMPORT(vjoOcrRead);
    REQUIRE_KERNEL_IMPORT(vjoOcrCancel);
#endif
#undef REQUIRE_KERNEL_IMPORT
#endif
    vjo_log("worker startup: checking kernel API");
    ver = vjoGetVersion();
    vjo_log("worker startup: kernel API %d", ver);
    if (ver != VJO_API_VERSION) {
        vjo_log("kernel API version %d, expected %d: not starting", ver, VJO_API_VERSION);
        return -1;
    }
    vjo_log("worker startup: registering shell");
    rc = vjoRegisterShell();
    vjo_log("worker startup: shell registration %d", rc);
    if (rc < 0) {
        vjo_log("kernel refused the shell registration %d: not starting", rc);
        return -1;
    }
    vjo_log("worker startup: creating synchronization objects");
    view_lock = sceKernelCreateMutex("VjoView", 0, 0, NULL);
    cmd_lock = sceKernelCreateMutex("VjoCmd", 0, 0, NULL);
    capture_lock = sceKernelCreateMutex("VjoCapture", 0, 0, NULL);
    net_evf = sceKernelCreateEventFlag("VjoNetEv", 0, 0, NULL);
    if (view_lock < 0 || cmd_lock < 0 || capture_lock < 0 || net_evf < 0) {
        vjo_worker_stop();
        return -1;
    }
#ifdef VJO_MEMORY_DIAGNOSTICS
    anki_started = 0;
    vjo_log("memory diagnostics: Anki worker disabled; probes run once before first Meiki job");
#else
    vjo_log("worker startup: starting Anki worker");
    anki_started = vjo_anki_start() == 0;
    if (!anki_started)
        vjo_log("anki: thread failed to start: Anki is off");
#endif
    vjo_log("worker startup: initializing arenas");
    vjo_platform_vita(&plat);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    vjo_arena_init(&results[0], NULL, 0);
    vjo_arena_init(&results[1], NULL, 0);
    vjo_log("worker startup: applying config");
    apply_config(); /* logging, the trigger and Anki are set up before the first game */
    vjo_log("Vita JP Overlay shell started (kernel API %d)", ver);

    vjo_log("worker startup: creating control and network threads");
    net_thread = sceKernelCreateThread("VjoNet", net_main, 0x10000100, JOB_STACK_BYTES, 0, 0, NULL);
    ctl_thread = sceKernelCreateThread("VjoControl", ctl_main, 0x10000100, 0x4000, 0, 0, NULL);
    if (net_thread < 0 || ctl_thread < 0) {
        vjo_worker_stop();
        return -1;
    }
    vjo_log("worker startup: starting control and network threads");
    sceKernelStartThread(net_thread, 0, NULL);
    sceKernelStartThread(ctl_thread, 0, NULL);
    threads_started = 1;
    return 0;
}

/* Also undoes a partial vjo_worker_start. */
int vjo_worker_stop(void)
{
    running = 0;
    if (net_evf >= 0)
        sceKernelSetEventFlag(net_evf, NET_EV_QUIT);
    if (threads_started) {
        if (sceKernelWaitThreadEnd(net_thread, NULL, NULL) < 0 ||
            sceKernelWaitThreadEnd(ctl_thread, NULL, NULL) < 0) {
            vjo_log("worker join failed; keeping Shell resources for a stop retry");
            return -1;
        }
        threads_started = 0;
    }
#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
    if (vjo_meiki_bridge_finish(&meiki_bridge)) {
        vjo_log("Meiki cleanup pending; refusing Shell unload");
        return -1;
    }
#endif
#ifdef VJO_MEMORY_DIAGNOSTICS
    if (vjo_memory_probe_release()) {
        vjo_log("memory diagnostic cleanup pending; refusing Shell unload");
        return -1;
    }
#endif
    if (vjo_anki_stop() < 0) {
        vjo_log("Anki cleanup pending; refusing Shell unload");
        return -1;
    }
    if (net_thread >= 0)
        sceKernelDeleteThread(net_thread);
    if (ctl_thread >= 0)
        sceKernelDeleteThread(ctl_thread);
    net_thread = ctl_thread = -1;
    anki_started = 0;
    mem_free();
    if (net_evf >= 0)
        sceKernelDeleteEventFlag(net_evf);
    if (cmd_lock >= 0)
        sceKernelDeleteMutex(cmd_lock);
    if (capture_lock >= 0)
        sceKernelDeleteMutex(capture_lock);
    if (view_lock >= 0)
        sceKernelDeleteMutex(view_lock);
    net_evf = cmd_lock = capture_lock = view_lock = -1;
    return 0;
}
