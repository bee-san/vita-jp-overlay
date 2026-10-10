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
{ user_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? -1 : 7; }
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{ *base = fail_base ? NULL : storage; return fail_base ? -1 : 0; }
int sceKernelFreeMemBlock(SceUID uid) { user_frees++; return 0; }

#include "../../shell/worker.c"

void *vjo_shell_heap_alloc(size_t size)
{ heap_allocs++; TEST_CHECK(size <= sizeof(storage)); return fail_alloc ? NULL : storage; }
void vjo_shell_heap_free(void *p) { TEST_CHECK(p == storage); heap_frees++; }
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
{ return VJO_E_SOURCE; }
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

TEST_LIST = {
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
