/* Actual Lens control policy with only Vita scheduling and memory stubbed. */
#include "acutest.h"
#include "client.h"
#define VJO_TEST_MEMORY_STUBS 1
#include <psp2/host_stubs.h>

static VjoState state;
static VjoConfig loaded;
static uint64_t test_now;
static unsigned posted, user_allocs, user_frees, heap_allocs, heap_frees, cancels;
static int fail_alloc, fail_base, input_blocked;
static uint8_t storage[2 * 1024 * 1024];
/* The vita-vn-ocr job's own workspace (its memblock or Paf block). */
static uint8_t vocr_storage[2 * 1024 * 1024] __attribute__((aligned(64)));
static unsigned vocr_allocs, vocr_frees, vocr_paf_allocs, jpeg_captures;
static int fail_vocr_user, fail_vocr_paf;
static unsigned char raw_region[200 * 60 * 4];
static VjoState raw_state;
uint64_t sceKernelGetProcessTimeWide(void) { return test_now; }
int sceKernelSetEventFlag(SceUID id, unsigned bits) { posted |= bits; return 0; }
int vjoGetState(VjoState *out) { *out = state; return 0; }
int vjoSetRegion(const VjoRect *r) { return 0; }
int vjoSetTriggers(int a, int b) { return 0; }
int vjoSetGameActive(int pid, int mode) { return 0; }
int vjoSetInputBlock(int on) { input_blocked = on; return 0; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoRegisterShell(void) { return 0; }
int vjoWaitEvent(uint32_t mask, uint32_t *out, uint32_t timeout) { *out = 0; return -1; }
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned size, void *opt)
{
    if (!strcmp(name, "VjoVocr")) {
        vocr_allocs++;
        TEST_CHECK(size <= sizeof(vocr_storage) && !(size & 0xFFF));
        return fail_vocr_user ? -1 : 9;
    }
    user_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? -1 : 7;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    if (uid == 9) { *base = vocr_storage; return 0; }
    *base = fail_base ? NULL : storage; return fail_base ? -1 : 0;
}
int sceKernelFreeMemBlock(SceUID uid) { if (uid == 9) vocr_frees++; else user_frees++; return 0; }
int vjoReadRaw(uint32_t row, uint32_t n, void *dst)
{
    if (row >= raw_state.height) return -1;
    if (n > raw_state.height - row) n = raw_state.height - row;
    memcpy(dst, raw_region + (size_t)row * raw_state.raw_stride, (size_t)n * raw_state.raw_stride);
    return (int)n;
}

#include "../../shell/worker.c"

void *vjo_shell_heap_alloc(size_t size)
{ heap_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? NULL : storage; }
void *vjo_shell_heap_alloc_for(const char *what, size_t size)
{
    TEST_CHECK(!strcmp(what, "vocr") && size <= sizeof(vocr_storage));
    vocr_paf_allocs++;
    return fail_vocr_paf ? NULL : vocr_storage;
}
void vjo_shell_heap_free(void *p)
{
    TEST_CHECK(p == storage || p == vocr_storage);
    if (p == vocr_storage) vocr_frees++; else heap_frees++;
}
void vjo_config_load(VjoConfig *out, VjoArena *a) { *out = loaded; }
void vjo_log_configure(const VjoConfig *c) {}
void vjo_log(const char *fmt, ...) {}
int vjo_region_load(const char *id, VjoRect *out, VjoArena *a) { return 0; }
int vjo_region_save(const char *id, const VjoRect *r, VjoArena *a) { return 0; }
int vjo_anki_start(void) { return -1; }
void vjo_anki_stop(void) {}
void vjo_anki_configure(const VjoConfig *c) {}
void vjo_anki_post_check(unsigned seq) {}
void vjo_platform_vita(VjoPlatform *p, VjoNetCancel *c) {}
int vjo_net_cancel_init(VjoNetCancel *c) { return 0; }
void vjo_net_cancel(VjoNetCancel *c) { c->cancelled = 1; cancels++; }
void vjo_net_cancel_clear(VjoNetCancel *c) { c->cancelled = 0; }
int vjo_capture_init(void) { return 0; }
void vjo_capture_fini(void) {}
int vjo_capture_jpeg(VjoArena *a, uint32_t f, int q, VjoBuf *b, VjoState *s)
{ jpeg_captures++; return VJO_E_SOURCE; }
/* A finished region capture of raw_region, read through vjoReadRaw. */
int vjo_capture_raw(uint32_t f, int (*consume)(void *ud, const VjoState *st), void *ud, VjoState *s)
{
    *s = raw_state;
    return consume(ud, s);
}
int vjo_title_id(SceUID pid, char *out, int cap) { return -1; }
int vjo_title_game_mode(const char *id) { return VJO_GAME; }

static void reset(void)
{
    if (has_result_memory()) mem_free();
    memset(&g_view, 0, sizeof(g_view));
    memset(&state, 0, sizeof(state));
    memset(&plat, 0, sizeof(plat));
    plat.pool = &pool;
    vjo_config_defaults(&loaded);
    loaded.dictionary = VJO_DICT_LOCAL;
    cfg = loaded;
    memcpy(title_id, "PCSG00415", 10);
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    state.scene = state.capture_scene = state.region_scene = 2;
    state.region_seq = 4;
    state.stable = 1;
    test_now = 10000000;
    posted = user_allocs = user_frees = heap_allocs = heap_frees = cancels = 0;
    fail_alloc = fail_base = input_blocked = 0;
    vocr_allocs = vocr_frees = vocr_paf_allocs = jpeg_captures = 0;
    fail_vocr_user = fail_vocr_paf = 0;
    memset(&raw_state, 0, sizeof(raw_state));
    raw_state.width = 200;
    raw_state.height = 60;
    raw_state.raw_stride = 200 * 4;
    raw_state.capture_scene = 2;
    raw_state.region_seq = 4;
    memset(raw_region, 0, sizeof(raw_region));
    ov = OV_CLOSED;
    active = -1;
    game_generation = job_generation = 0;
    cache_ok = subtitles = stable_pending = warm_running = job_running = job_cancelled = 0;
    view_waiting = view_job_text = 0;
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
    memset(cache_data, 0, sizeof(cache_data));
}

static void test_local_open(void)
{
    reset();
    open_overlay();
    TEST_CHECK(job_running && job_lookup && input_blocked);
    TEST_CHECK(posted & NET_EV_JOB);
    TEST_CHECK(!g_view.status_is_error && want_lookup());
    TEST_CHECK(pool.n == POOL_SLOTS);
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory() && pool.n == 0);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(heap_allocs == 1 && heap_frees == 1 && user_allocs == 0);
#else
    TEST_CHECK(user_allocs == 1 && user_frees == 1);
#endif
}

static void test_config_cache(void)
{
    reset();
    cache_ok = 1;
    apply_config();
    TEST_CHECK(cache_ok);
    strcpy(loaded.local_dictionaries, "daijirin.vjdict,main.vjdict");
    apply_config();
    TEST_CHECK(!cache_ok);
    cache_ok = 1;
    strcpy(loaded.local_dictionary_dir, "ux0:data/other");
    apply_config();
    TEST_CHECK(!cache_ok);
    loaded.dictionary = VJO_DICT_HACHIDORI;
    strcpy(loaded.hachidori_host, "192.168.1.2");
    apply_config();
    TEST_CHECK(want_lookup());
}

static void test_cached_scene(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    cache_data[0].list.header = "日本語";
    open_overlay();
    TEST_CHECK(!job_running && g_view.list == &cache_data[0].list);
    TEST_CHECK(input_blocked && !g_view.status_is_error);
    close_overlay();
    TEST_CHECK(!g_view.list && !input_blocked);
    mem_free();
}

static void test_stale_cancel(void)
{
    reset();
    job_running = job_captured = 1;
    job_capture_scene = 1;
    job_capture_seq = 1;
    TEST_CHECK(cancel_outdated_job(&state));
    TEST_CHECK(job_cancelled && cancels == 1);
    TEST_CHECK(!cancel_outdated_job(&state) && cancels == 1);
    job_running = 0;
}

static void test_background_local(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    stable_pending = 1;
    auto_prefetch();
    TEST_CHECK(job_running && job_lookup && (posted & NET_EV_JOB));
    job_running = 0;
    mem_free();
}

static void test_alloc_failure(void)
{
    reset();
    fail_alloc = 1;
    open_overlay();
    TEST_CHECK(!has_result_memory() && !job_running && g_view.status_is_error);
    TEST_CHECK(strstr(g_view.status, "memory") != NULL);
    fail_alloc = 0;
    open_overlay();
    TEST_CHECK(has_result_memory() && job_running);
    job_running = 0;
    mem_free();
#ifndef VJO_PAF_ALLOC
    fail_base = 1;
    TEST_CHECK(mem_alloc() < 0 && !has_result_memory());
    TEST_CHECK(user_frees == 2);
#endif
}

static void test_ocr_config_migration(void)
{
    VjoConfig c;
    const char old[] = "text_source = hooks\nocr_backend = meiki\n";
    vjo_config_defaults(&c);
    vjo_config_parse(&c, old, sizeof(old)-1);
    TEST_CHECK(c.text_source == VJO_SOURCE_OCR && c.ocr_backend == VJO_OCR_LENS);
    TEST_CHECK(c.n_warnings == 2);
}

static void test_changed_dictionary_drops_running_result(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_dictionary = cfg.dictionary;
    cache_data[0].filtered = "日本語";
    start_job("test", 1);
    ov = OV_OPEN_OLD_JOB;
    job_same_text = 1;
    strcpy(loaded.local_dictionaries, "new.vjdict");
    apply_config();
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    on_job_done();
    TEST_CHECK(job_running && !job_cached_text && !job_same_text && !cache_ok);
    TEST_CHECK(strcmp(job_cfg.local_dictionaries, "new.vjdict") == 0);
    TEST_CHECK(g_view.list != &cache_data[0].list);
    job_running = 0;
    mem_free();
}

static void test_game_exit_drains_previous_job(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_dictionary = cfg.dictionary;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    job_same_text = 1;
    ov = OV_OPEN;
    input_blocked = 1;

    on_game_exit();
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    TEST_CHECK(!game_active() && ov == OV_CLOSED && !input_blocked);
    TEST_CHECK(has_result_memory()); /* the running job still owns its arena */
    TEST_CHECK(job_generation != game_generation);
    on_job_done();
    TEST_CHECK(!job_running && !cache_ok && !stable_pending);
    TEST_CHECK(!g_view.list);
    mem_free();
    TEST_CHECK(!has_result_memory() && pool.n == 0);
}

static void test_game_start_rejects_previous_generation(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    active = 0;
    cache_ok = 1;
    cache_dictionary = cfg.dictionary;
    cache_data[0].filtered = "日本語";
    cache_scene = state.scene;
    cache_seq = state.region_seq;
    start_job("old game", 1);
    cache_data[job_idx].sentence = "old game text";
    job_capture_scene = state.scene;
    job_capture_seq = state.region_seq;
    job_same_text = 1;
    ov = OV_OPEN;
    input_blocked = 1;
    state.game_pid = 101;

    on_game_start(); /* a foreground switch need not include GAME_EXIT */
    TEST_CHECK(job_cancelled && cancels == 1 && !cache_ok);
    TEST_CHECK(ov == OV_CLOSED && !input_blocked && pending_pid == 101);
    activate_game(state.game_pid, "PCSG00202", VJO_GAME);
    open_overlay(); /* the kernel has not sampled a new scene ID yet */
    TEST_CHECK(ov == OV_OPEN_OLD_JOB && !g_view.list && !cache_ok);

    /* The generation check is independent of the socket cancellation flag. */
    job_cancelled = 0;
    job_text_ready = 1;
    on_job_text();
    TEST_CHECK(!g_view.list && !view_job_text);
    on_job_done();
    TEST_CHECK(job_running && job_generation == game_generation);
    TEST_CHECK(!job_cached_text && !job_same_text && !cache_ok);
    TEST_CHECK(!g_view.list);
    job_running = 0;
    mem_free();
}

/* ---- ocr_backend = vocr ---- */

static int run_vocr_job(void)
{
    TEST_ASSERT(mem_alloc() == 0);
    job_cfg = cfg;
    job_cancelled = 0;
    job_captured = 0;
    vjo_arena_reset(&results[0]);
    return run_job(&results[0], &cache_data[0]);
}

static void test_vocr_never_uses_lens(void)
{
    reset();
    cfg.ocr_backend = loaded.ocr_backend = VJO_OCR_VOCR;
    int rc = run_vocr_job();
    TEST_CHECK(jpeg_captures == 0);
#ifdef VJO_WITH_VOCR
    /* An empty region: no line, no model read, the workspace is released. */
    TEST_CHECK(rc == VJO_OK && cache_data[0].failed_stage == VJO_STAGE_NONE);
    TEST_CHECK(cache_data[0].ocr_text && !cache_data[0].ocr_text[0]);
    TEST_CHECK(job_captured && job_capture_scene == 2 && job_capture_seq == 4);
    TEST_CHECK(vocr_allocs == 1 && vocr_frees == 1 && vocr_paf_allocs == 0);
#else
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(cache_data[0].err.detail && strstr(cache_data[0].err.detail, "vita-vn-ocr"));
    TEST_CHECK(vocr_allocs == 0);
#endif
    mem_free();
}

static void test_vocr_config(void)
{
    VjoConfig c;
    const char ini[] = "ocr_backend = VOCR\nvocr_model = FL10_w8.vocr\n";
    const char bad[] = "vocr_model = ../H15_w8.vocr\nvocr_model = H15_w8.bin\nvocr_model =\n";
    vjo_config_defaults(&c);
    TEST_CHECK(c.ocr_backend == VJO_OCR_LENS && !strcmp(c.vocr_model, "H15_w8.vocr"));
    vjo_config_parse(&c, ini, sizeof(ini) - 1);
    TEST_CHECK(c.ocr_backend == VJO_OCR_VOCR && !strcmp(c.vocr_model, "FL10_w8.vocr") && c.n_warnings == 0);
    vjo_config_defaults(&c);
    vjo_config_parse(&c, bad, sizeof(bad) - 1);
    TEST_CHECK(!strcmp(c.vocr_model, "H15_w8.vocr") && c.n_warnings == 3); /* each kept the default */
}

static void test_vocr_warms_no_lens_connection(void)
{
    reset();
    TEST_ASSERT(mem_alloc() == 0);
    cfg.ocr_backend = VJO_OCR_VOCR;
    state.unsettled_ms = 100;
    warm_connections();
    TEST_CHECK(!(posted & NET_EV_WARM) && !warm_running);
    cfg.ocr_backend = VJO_OCR_LENS;
    warm_connections();
    TEST_CHECK((posted & NET_EV_WARM) && warm_running);
    warm_running = 0;
    mem_free();
}

static void test_vocr_model_change_invalidates_cache(void)
{
    reset();
    loaded.ocr_backend = VJO_OCR_VOCR;
    apply_config();
    cache_ok = 1;
    strcpy(loaded.vocr_model, "FL10_w8.vocr");
    apply_config();
    TEST_CHECK(!cache_ok);
}

#ifdef VJO_WITH_VOCR
static void strokes(unsigned y0, unsigned y1)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = 10; x < 190; x += 6)
            memset(raw_region + ((size_t)y * 200 + x) * 4, 235, 6); /* 1.5 px of R, G, B */
}

static void test_vocr_missing_model_releases_workspace(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    strokes(15, 39); /* one light line on black */
    int rc = run_vocr_job();
    TEST_CHECK(rc == VJO_E_OCR_MODEL && cache_data[0].failed_stage == VJO_STAGE_OCR);
    TEST_CHECK(!cache_data[0].sentence && vocr_allocs == 1 && vocr_frees == 1);
    mem_free();
}

static void test_vocr_allocation_fallback_and_failure(void)
{
    reset();
    cfg.ocr_backend = VJO_OCR_VOCR;
    fail_vocr_user = 1;
    int rc = run_vocr_job();
#ifdef VJO_PAF_ALLOC
    /* No free USER pages: the guarded Paf heap is the fallback. */
    TEST_CHECK(rc == VJO_OK && vocr_paf_allocs == 1 && vocr_frees == 1);
    fail_vocr_paf = 1;
    rc = run_job(&results[0], &cache_data[0]);
    TEST_CHECK(vocr_paf_allocs == 2 && vocr_frees == 1);
#endif
    TEST_CHECK(rc == VJO_E_OOM && cache_data[0].failed_stage == VJO_STAGE_OCR);
    mem_free();
}
#endif

TEST_LIST = {
    {"vocr_never_uploads_to_lens", test_vocr_never_uses_lens},
    {"vocr_config_parses_and_validates", test_vocr_config},
    {"vocr_opens_no_lens_connection", test_vocr_warms_no_lens_connection},
    {"vocr_model_change_invalidates_cache", test_vocr_model_change_invalidates_cache},
#ifdef VJO_WITH_VOCR
    {"vocr_missing_model_releases_workspace", test_vocr_missing_model_releases_workspace},
    {"vocr_allocation_fallback_and_failure", test_vocr_allocation_fallback_and_failure},
#endif
    {"local_dictionary_opens_without_key", test_local_open},
    {"ordered_dictionary_config_invalidates_cache", test_config_cache},
    {"scene_cache_is_immediate", test_cached_scene},
    {"outdated_scene_cancels_once", test_stale_cancel},
    {"local_dictionary_background_prefetch", test_background_local},
    {"allocation_failure_recovers_and_releases", test_alloc_failure},
    {"experimental_config_migrates_to_ocr", test_ocr_config_migration},
    {"changed_dictionary_drops_inflight_cached_result", test_changed_dictionary_drops_running_result},
    {"game_exit_drains_previous_job_without_reviving_cache", test_game_exit_drains_previous_job},
    {"game_start_rejects_previous_generation_and_restarts", test_game_start_rejects_previous_generation},
    {NULL, NULL}
};
