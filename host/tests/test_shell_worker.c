/* Exercise worker.c's actual manual/background gates, not a copy of them.
 * Only Vita I/O is stubbed. Config, error rendering and relay lookup are real. */
#include "acutest.h"
#include "client.h"
#include "replay.h"
#define VJO_TEST_MEMORY_STUBS 1
#include <psp2/host_stubs.h>
#include "../../include/vjo_text.h"

static VjoState kernel_state;
static uint64_t test_now;
static unsigned int posted_events;
static VjoConfig loaded_config;

uint64_t sceKernelGetProcessTimeWide(void) { return test_now; }
int sceKernelSetEventFlag(SceUID uid, unsigned int bits) { posted_events |= bits; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoRegisterShell(void) { return 0; }
int vjoWaitEvent(uint32_t mask, uint32_t *out, uint32_t timeout) { return -1; }
int vjoGetState(VjoState *out) { *out = kernel_state; return 0; }
int vjoSetRegion(const VjoRect *r) { return 0; }
int vjoSetTriggers(int toggle, int subtitle) { return 0; }
int vjoSetGameActive(int pid, int active) { return 0; }
int vjoSetInputBlock(int on) { return 0; }
static int capture_calls;
int vjoRequestCapture(uint32_t flags) { capture_calls++; return VJO_ERR_NO_GAME; }
int vjoReadRaw(uint32_t row, uint32_t n, void *dst) { return -1; }
static VjoTextSnapshot hook_snapshot;
int vjoTextRead(VjoTextSnapshot *out) { *out = hook_snapshot; return 0; }
static int last_hook_mode, discover_calls;
int vjoTextControl(uint32_t session, int mode, uint32_t selected) {
    last_hook_mode = mode;
    if (mode == VJO_TEXT_DISCOVER) discover_calls++;
    if (session != hook_snapshot.session) return -1;
    hook_snapshot.selected = selected;
    memset(&hook_snapshot.current, 0, sizeof(hook_snapshot.current));
    for (unsigned i = 0; i < hook_snapshot.count; i++)
        if (hook_snapshot.candidates[i].id == selected) hook_snapshot.current = hook_snapshot.candidates[i];
    hook_snapshot.sequence++;
    return 0;
}
static int anchor_calls;
static char last_reference[VJO_TEXT_BYTES];
int vjoTextReference(uint32_t session, const char *text) {
    anchor_calls++;
    snprintf(last_reference, sizeof(last_reference), "%s", text);
    return session == hook_snapshot.session ? 0 : -1;
}

#include "../../shell/worker.c"

void vjo_config_load(VjoConfig *out, VjoArena *a) { *out = loaded_config; }
void vjo_log_configure(const VjoConfig *c) {}
void vjo_log(const char *fmt, ...) {}
int vjo_region_load(const char *tid, VjoRect *out, VjoArena *a) { return 0; }
int vjo_region_save(const char *tid, const VjoRect *r, VjoArena *a) { return 0; }
int vjo_anki_start(void) { return -1; }
void vjo_anki_stop(void) {}
void vjo_anki_configure(const VjoConfig *c) {}
void vjo_anki_post_check(unsigned int seq) {}
void vjo_platform_vita(VjoPlatform *p) {}

static uint8_t result_mem[2][RESULT_ARENA_SIZE];
static unsigned allocation_calls, allocation_frees;
static int allocation_failure;
static uint8_t heap_mem[RESULT_ARENA_SIZE];
static unsigned allocation_bytes, heap_bytes;
static unsigned heap_calls, heap_frees;
static int heap_available;
void *vjo_shell_heap_alloc(size_t bytes)
{
    heap_calls++;
    heap_bytes = bytes;
    TEST_CHECK(bytes == RESULT_ARENA_SIZE || bytes == 2 * NATIVE_ARENA_SIZE);
    return heap_available && bytes <= sizeof(heap_mem) ? heap_mem : NULL;
}
void vjo_shell_heap_free(void *ptr)
{
    TEST_CHECK(ptr == heap_mem);
    heap_frees++;
}
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned int size, void *opt)
{
    allocation_calls++;
    allocation_bytes = size;
    if (allocation_failure || size > sizeof(result_mem)) return (int)0x80024302u;
    return 1;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    *base = result_mem;
    return 0;
}
int sceKernelFreeMemBlock(SceUID uid) { allocation_frees++; return 0; }
static VjoMemConn relay_conn;
static char relay_reply[1024], relay_request[4096];
static int relay_connections;
static int connect_relay(void *ud, const char *host, int port, int timeout, int io_timeout, VjoConn *out)
{
    const char *json = "{\"index\":0,\"originalTextLength\":1,\"dictionaryEntries\":[{"
        "\"headwords\":[{\"term\":\"猫\",\"reading\":\"ねこ\"}],"
        "\"definitions\":[{\"entries\":[\"cat\"]}]}]}";
    TEST_CHECK(!strcmp(host, "anki.local") && port == 19634);
    snprintf(relay_reply, sizeof(relay_reply), "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n%s",
             (unsigned long)strlen(json), json);
    memset(&relay_conn, 0, sizeof(relay_conn));
    relay_conn.in = relay_reply;
    relay_conn.len = strlen(relay_reply);
    relay_conn.out = relay_request;
    relay_conn.out_cap = sizeof(relay_request) - 1;
    vjo_memconn_init(&relay_conn, out);
    relay_connections++;
    return VJO_OK;
}
static void disconnect_relay(void *ud, VjoConn *c)
{
    relay_request[relay_conn.out_len] = '\0';
}

static void setup(const char *ini)
{
    vjo_config_defaults(&loaded_config);
    vjo_config_parse(&loaded_config, ini, strlen(ini));
    cfg = loaded_config;
    native_game = text_calibrated = text_started = job_native = job_anchor = 0;
    memset(&hook_snapshot, 0, sizeof(hook_snapshot));
    hook_snapshot.session = 42;
    memset(&text_state, 0, sizeof(text_state));
    seen_hook_id = 0; seen_hook_text[0] = 0;
    hook_log_after = 0;
    hook_log_pending = 0;
    anchor_calls = discover_calls = 0;
    last_hook_mode = VJO_TEXT_OFF;
    last_reference[0] = 0;
    allocation_bytes = heap_bytes = 0;
    mem_kind = MEM_RESULTS;
    capture_calls = 0;
    allocation_calls = allocation_frees = 0;
    allocation_failure = 1;
    heap_calls = heap_frees = 0;
    heap_available = 0;
    mem_heap = NULL;
    running = 1;
    region_selected = job_region_selected = 0;
    capture_generation = job_generation = 0;
    memset(&g_view, 0, sizeof(g_view));
    memset(cache_data, 0, sizeof(cache_data));
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
    memset(&mem_backoff, 0, sizeof(mem_backoff));
    memset(&native_list, 0, sizeof(native_list));
    memset(&plat, 0, sizeof(plat));
    plat.connect = connect_relay;
    plat.disconnect = disconnect_relay;
    mem_uid = 1; /* preallocated game arenas, no SDK allocator needed */
    for (int i = 0; i < 2; i++)
        vjo_arena_init(&results[i], result_mem[i], sizeof(result_mem[i]));
    vjo_arena_init(&scratch, scratch_mem, sizeof(scratch_mem));
    active = -1;
    cache_ok = 0;
    cache_checksum = 0;
    ov = OV_CLOSED;
    subtitles = stable_pending = 0;
    job_running = job_done = job_lookup = job_text_ready = 0;
    job_idx = 0;
    job_started_us = 0;
    anki_started = 0;
    strcpy(title_id, "PCSG00001");
    memset(&kernel_state, 0, sizeof(kernel_state));
    kernel_state.alloc_status = VJO_ALLOC_OK;
    kernel_state.stable = 1;
    kernel_state.checksum = 7;
    test_now = 10000000;
    posted_events = 0;
    relay_connections = 0;
    relay_request[0] = '\0';
}

#define RELAY_CONFIG "dictionary = hachidori\nhachidori_host = anki.local:19634\n"

/* Complete the scheduled job using known OCR text and the real relay adapter.
 * Capturing/Lens and actual threads are intentionally outside this gate test. */
static void complete_lookup(void)
{
    TEST_ASSERT(job_running && job_lookup && (posted_events & NET_EV_JOB));
    VjoOverlayData *out = &cache_data[job_idx];
    TEST_ASSERT(vjo_overlay_from_text(&results[job_idx], &plat, &job_cfg, "猫", out) == VJO_OK);
    TEST_ASSERT(out->list.n_entries == 1);
    TEST_CHECK(!strcmp(out->list.entries[0].vocab->spelling, "猫"));
    TEST_CHECK(!strcmp(out->list.entries[0].vocab->meanings[0], "cat"));
    TEST_CHECK(relay_connections == 1);
    TEST_CHECK(strstr(relay_request, "POST /termEntries HTTP/1.1\r\n") != NULL);
    TEST_CHECK(!strstr(relay_request, "Authorization") && !strstr(relay_request, "Api-Key"));
    job_checksum = kernel_state.checksum;
    job_done = 1;
    on_job_done();
    TEST_CHECK(cache_ok && !job_running);
}

static void test_manual_relay_without_keys(void)
{
    setup(RELAY_CONFIG "ocr_mode = on_press\n");
    TEST_CHECK(!*vjo_config_api_key(&cfg));
    open_overlay();
    TEST_CHECK(g_view.open && !g_view.status_is_error);
    complete_lookup();
    TEST_CHECK(g_view.list == &cache_data[active].list && !g_view.status_is_error);
}

static void test_background_relay_without_keys(void)
{
    setup(RELAY_CONFIG);
    stable_pending = 1;
    auto_prefetch();
    complete_lookup();
    TEST_CHECK(!g_view.open);
    /* Opening the same settled screen uses the prefetched lookup. */
    posted_events = 0;
    open_overlay();
    TEST_CHECK(!job_running && posted_events == 0);
    TEST_CHECK(g_view.list == &cache_data[active].list && !g_view.status_is_error);
}

static void test_subtitles_auto_relay_lookup(void)
{
    setup(RELAY_CONFIG);
    set_subtitles(1);
    complete_lookup();
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !strcmp(g_view.strip_text, "猫"));
}

static void test_relay_missing_host_message(void)
{
    setup("dictionary = hachidori\n");
    open_overlay();
    TEST_CHECK(g_view.status_is_error);
    TEST_CHECK(strstr(g_view.status, "Hachidori relay host is not set or invalid") != NULL);
    TEST_CHECK(strstr(g_view.status, "hachidori_host = HOST[:PORT]") != NULL);
    TEST_CHECK(!strstr(g_view.status, "API key") && !strstr(g_view.status, "(null)"));
    TEST_CHECK(!job_running && posted_events == 0 && relay_connections == 0);
}

static void test_relay_invalid_host_message(void)
{
    setup("dictionary = hachidori\nhachidori_host = http://anki.local\n");
    TEST_CHECK(loaded_config.n_warnings > 0 && !loaded_config.hachidori_host[0]);
    open_overlay();
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "hachidori_host = HOST[:PORT]"));
    TEST_CHECK(!strstr(g_view.status, "API key"));
    TEST_CHECK(!job_running && posted_events == 0);
}

static void test_unconfigured_relay_skips_background_lookup(void)
{
    const char *hosts[] = {"", "http://anki.local", "auto", "anki.local:0"};
    for (unsigned int i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        setup(RELAY_CONFIG);
        /* Also protect snapshots that have not passed through the INI parser. */
        strcpy(cfg.hachidori_host, hosts[i]);
        stable_pending = 1;
        TEST_CHECK(!want_lookup());
        auto_prefetch();
        TEST_CHECK(!job_running && posted_events == 0);
    }
}

static void test_cloud_missing_keys_still_block(void)
{
    const char *clouds[] = {"jpdb", "jiten"};
    for (int i = 0; i < 2; i++) {
        char ini[128];
        snprintf(ini, sizeof(ini), "dictionary = %s\nhachidori_host = anki.local\n", clouds[i]);
        setup(ini);
        /* A key for another backend must not authorise this one. */
        strcpy(loaded_config.api_key[i == VJO_DICT_JPDB ? VJO_DICT_JITEN : VJO_DICT_JPDB], "other-key");
        cfg = loaded_config;
        open_overlay();
        TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "API key is not set"));
        TEST_CHECK(strstr(g_view.status, vjo_dict_info(i)->key_setting) != NULL);
        TEST_CHECK(!job_running && posted_events == 0);
        ov = OV_CLOSED;
        stable_pending = 1;
        auto_prefetch();
        TEST_CHECK(!job_running && !want_lookup() && posted_events == 0);
        /* Subtitles remain available without a dictionary key. */
        set_subtitles(1);
        TEST_CHECK(job_running && !job_lookup);
    }
}

static void test_cloud_configured_keys_still_schedule(void)
{
    for (int dict = VJO_DICT_JPDB; dict <= VJO_DICT_JITEN; dict++) {
        setup("");
        loaded_config.dictionary = dict;
        strcpy(loaded_config.api_key[dict], "configured-key");
        cfg = loaded_config;
        open_overlay();
        TEST_CHECK(job_running && job_lookup && !g_view.status_is_error);
        job_running = 0;
        ov = OV_CLOSED;
        posted_events = 0;
        stable_pending = 1;
        auto_prefetch();
        TEST_CHECK(job_running && job_lookup && (posted_events & NET_EV_JOB));
    }
}

static void test_relay_lookup_respects_mode_and_backoff(void)
{
    setup(RELAY_CONFIG);
    TEST_CHECK(want_lookup());
    dict_backoff.until = (int64_t)test_now + 1;
    TEST_CHECK(!want_lookup());
    test_now++;
    TEST_CHECK(want_lookup());
    cfg.ocr_mode = VJO_OCR_ON_PRESS;
    TEST_CHECK(!want_lookup());
    ov = OV_OPEN;
    TEST_CHECK(want_lookup());
}

static void test_subtitles_on_press_stay_ocr_only(void)
{
    setup(RELAY_CONFIG "ocr_mode = on_press\n");
    set_subtitles(1);
    TEST_CHECK(job_running && !job_lookup && (posted_events & NET_EV_JOB));
}

static void test_local_worker_and_config_changes(void)
{
    setup("dictionary = local\nlocal_dictionaries = main.vjdict\n");
    TEST_CHECK(vjo_config_dict_ready(&cfg));
    TEST_CHECK(!vjo_config_api_key(&cfg)[0]);
    open_overlay();
    TEST_CHECK(job_running && job_lookup && !g_view.status_is_error);
    job_running = 0;
    ov = OV_CLOSED;
    posted_events = 0;
    stable_pending = 1;
    auto_prefetch();
    TEST_CHECK(job_running && job_lookup && (posted_events & NET_EV_JOB));

    cache_ok = 1;
    strcpy(loaded_config.local_dictionaries, "new.vjdict");
    apply_config();
    TEST_CHECK(!cache_ok);
    TEST_CHECK(!same_dictionary(&job_cfg, &cfg));

    /* Reopening during an old lookup must schedule a fresh lookup. */
    ov = OV_OPEN_OLD_JOB;
    job_checksum = kernel_state.checksum;
    cache_data[job_idx].err.rc = VJO_OK;
    cache_data[job_idx].failed_stage = VJO_STAGE_NONE;
    on_job_done();
    TEST_CHECK(job_running && job_lookup && same_dictionary(&job_cfg, &cfg));
}

static void test_ncnn_manual_and_no_fallback(void)
{
    setup("dictionary = local\nocr_backend = ncnn\nocr_mode = auto\n");
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_NCNN);
    stable_pending = 1;
    auto_prefetch();
    TEST_CHECK(!job_running && !posted_events);
    subtitles = 1;
    TEST_CHECK(!background_wanted());
    subtitles = 0;
    open_overlay();
    TEST_ASSERT(job_running && job_lookup);
    uint32_t checksum = 0;
    int rc = run_job(&results[job_idx], &cache_data[job_idx], &checksum);
#ifdef VJO_WITH_NCNN
    TEST_CHECK(rc == VJO_E_OCR_REGION);
    job_region_selected = 1; /* stubbed SDK refuses the workspace */
    rc = run_job(&results[job_idx], &cache_data[job_idx], &checksum);
    TEST_CHECK(rc == VJO_E_OOM);
#else
    TEST_CHECK(rc == VJO_E_OCR_UNAVAILABLE);
#endif
    TEST_CHECK(!relay_connections && !job_text_ready);
    TEST_CHECK(cache_data[job_idx].failed_stage == VJO_STAGE_OCR);
    /* The public JPEG API must also refuse Lens for a local configuration. */
    VjoOverlayData out;
    TEST_CHECK(vjo_overlay_ocr(&results[job_idx], &plat, &cfg, NULL, &out) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!relay_connections && out.failed_stage == VJO_STAGE_OCR);
}

static void test_ocr_context_changes(void)
{
    setup("dictionary = local\n");
    cache_ok = 1;
    loaded_config.ocr_backend = VJO_OCR_NCNN;
    apply_config();
    TEST_CHECK(!cache_ok);
    open_overlay();
    TEST_ASSERT(job_running);
    unsigned old_generation = job_generation;
    on_game_exit();
    TEST_CHECK(job_cancelled(NULL) && capture_generation != old_generation);
    cache_data[job_idx].sentence = "old game";
    on_job_text(); on_job_done();
    TEST_CHECK(!job_running && !cache_ok && active == -1 && !g_view.open);
}

static void test_region_change_redoes_subtitle_job(void)
{
    setup("dictionary = local\nocr_backend = ncnn\n");
    set_subtitles(1);
    TEST_ASSERT(job_running && !job_lookup);
    /* A new region while that job runs: on_command cannot start its own. */
    pending_cmd = VJO_CMD_SET_REGION;
    pending_rect = (VjoRect){0, 0, 0x8000, 0x4000};
    on_command();
    TEST_CHECK(job_running && job_cancelled(NULL));
    posted_events = 0;
    on_job_done();
    /* Not left busy with the old region's text: the job is redone. */
    TEST_CHECK(job_running && !job_lookup && !job_cancelled(NULL) && (posted_events & NET_EV_JOB));
    TEST_CHECK(g_view.strip_on && g_view.strip_busy && !g_view.open);
}

static void native_setup(const char *mode)
{
    setup(mode);
    native_game = 1;
    hook_snapshot.pid = 7;
    hook_snapshot.count = 1;
    VjoTextCandidate *c = &hook_snapshot.candidates[0];
    c->id = 10; c->kind = VJO_TEXT_CALL; c->encoding = VJO_TEXT_UTF8;
    c->japanese = c->length = 6; c->updates = 1;
    strcpy(c->text, "猫を見ました");
}

static void test_manual_hook_bypasses_capture_and_ocr(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\nocr_backend = ncnn\n");
    open_overlay();
    TEST_CHECK(g_view.hook_picker && g_view.hook_count == 1 && !job_running);
    vjo_post_hook(hook_snapshot.session, 10);
    on_command();
    TEST_ASSERT(job_running && job_native && !g_view.hook_picker);
    uint32_t checksum = 123;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    TEST_CHECK(!capture_calls && !anchor_calls && relay_connections > 0 && checksum == 0);
    TEST_CHECK(!strcmp(cache_data[job_idx].sentence, "猫を見ました"));
    on_job_text(); on_job_done();
    TEST_CHECK(!job_running && cache_ok && g_view.list && !g_view.hook_picker);
}

static void test_automatic_hooks_match_once_and_handle_ocr_failure(void)
{
    native_setup(RELAY_CONFIG "text_source = auto\n");
    allocation_failure = 0;
    stable_pending = 1; auto_prefetch();
    TEST_CHECK(!job_running); /* never continuous OCR while finding a hook */
    open_overlay();
    TEST_ASSERT(job_running && job_anchor && !job_native && !job_lookup);
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_MATCHING && !g_view.hook_count);
    TEST_CHECK(allocation_bytes == RESULT_ARENA_SIZE && !results[1].base);
    uint32_t checksum = 0;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) != VJO_OK);
    on_job_done();
    TEST_CHECK(g_view.hook_picker && text_calibrated && capture_calls == 1);
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_FAILED && g_view.status_is_error);
    TEST_CHECK(!g_view.hook_count && !g_view.hook_reference[0] && !mem_heap && mem_uid < 0);
    hook_snapshot.candidates[0].score = 80; /* stale score cannot validate failed OCR */
    for (int i = 0; i < 20; i++) { hook_snapshot.sequence++; poll_hooks(); }
    TEST_CHECK(!job_running && !hook_snapshot.selected && !g_view.hook_count);
    close_overlay(); open_overlay(); /* explicit reopen is a fresh screenshot */
    TEST_CHECK(job_running && job_anchor && !job_lookup && capture_calls == 1);
    TEST_CHECK(!discover_calls && last_hook_mode == VJO_TEXT_LISTEN);
}

/* Complete OCR with invented text; Lens transport is covered separately. */
static void complete_anchor(const char *sentence)
{
    TEST_ASSERT(job_running && job_anchor && !job_lookup && job_idx == 0);
    TEST_ASSERT(vjo_overlay_ocr_text(&results[0], &job_cfg, sentence, &cache_data[0]) == VJO_OK);
    job_text_ready = 1;
    on_job_text();
    TEST_CHECK(job_running && !anchor_calls && (mem_uid >= 0 || mem_heap));
    job_done = 1;
    on_job_done();
}

static void test_anchor_heap_lifecycle_and_stable_matches(void)
{
    native_setup(RELAY_CONFIG "text_source = auto\n");
    mem_free(); allocation_frees = 0;
    heap_available = 1;
    hook_snapshot.count = 2;
    hook_snapshot.candidates[0].score = 95;
    hook_snapshot.candidates[1] = hook_snapshot.candidates[0];
    hook_snapshot.candidates[1].id = 11;
    hook_snapshot.candidates[1].score = 0;
    open_overlay();
    TEST_ASSERT(job_running && mem_heap == heap_mem && mem_kind == MEM_ANCHOR);
    TEST_CHECK(allocation_bytes == RESULT_ARENA_SIZE && heap_bytes == RESULT_ARENA_SIZE);
    TEST_CHECK(!results[1].base && results[0].size == RESULT_ARENA_SIZE && !heap_frees);
    complete_anchor("猫を見ました");
    TEST_CHECK(!job_running && !mem_heap && heap_frees == 1 && !g_view.list);
    TEST_CHECK(!results[0].base && !results[1].base && !relay_connections);
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_READY && g_view.hook_count == 1);
    TEST_CHECK(!strcmp(g_view.hook_reference, "猫を見ました") && !strcmp(last_reference, "猫を見ました"));
    TEST_CHECK(!hook_snapshot.selected && !discover_calls && last_hook_mode == VJO_TEXT_LISTEN);
    unsigned version = g_view.version;
    for (int i = 0; i < 50; i++) {
        hook_snapshot.sequence++;
        strcpy(hook_snapshot.candidates[0].text, "別の文章");
        poll_hooks();
    }
    TEST_CHECK(g_view.version == version && !strcmp(g_view.hooks[0].text, "猫を見ました"));
    TEST_CHECK(!job_running && !hook_snapshot.selected);
    pending_cmd = VJO_CMD_HOOK_DISCOVER; on_command();
    TEST_CHECK(!strcmp(g_view.hooks[0].text, "別の文章") && !job_running && !discover_calls);
    vjo_post_hook(hook_snapshot.session, 10); on_command();
    TEST_ASSERT(job_running && job_native && !job_anchor && mem_kind == MEM_NATIVE);
    TEST_CHECK(heap_bytes == 2 * NATIVE_ARENA_SIZE && results[1].base);
}

static void test_anchor_oom_is_explicit_and_retryable(void)
{
    native_setup(RELAY_CONFIG "text_source = auto\n");
    mem_free();
    open_overlay();
    TEST_CHECK(!job_running && g_view.hook_match_state == VJO_HOOK_MATCH_FAILED);
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "Not enough memory"));
    TEST_CHECK(!g_view.hook_count && allocation_bytes == RESULT_ARENA_SIZE && heap_bytes == RESULT_ARENA_SIZE);
    for (int i = 0; i < 30; i++) { test_now += 100000; poll_hooks(); }
    TEST_CHECK(allocation_calls == 1 && !discover_calls);
    heap_available = 1;
    pending_cmd = VJO_CMD_HOOK_OCR; on_command();
    TEST_CHECK(job_running && g_view.hook_match_state == VJO_HOOK_MATCH_MATCHING);
    complete_anchor("犬を見ました");
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_READY && !g_view.hook_count);
}

static void test_anchor_cancel_frees_only_after_done(void)
{
    native_setup(RELAY_CONFIG "text_source = auto\n");
    mem_free(); allocation_frees = 0;
    heap_available = 1;
    open_overlay();
    TEST_ASSERT(job_running && mem_heap && !heap_frees);
    on_game_exit();
    TEST_CHECK(job_cancelled(NULL) && mem_heap && !heap_frees);
    cache_data[0].sentence = "old screen";
    on_job_text(); on_job_done();
    TEST_CHECK(!job_running && !mem_heap && heap_frees == 1 && !anchor_calls);
    TEST_CHECK(!g_view.open && !g_view.hook_reference[0] && !g_view.list);
}

static void test_reopen_during_anchor_discards_old_screenshot(void)
{
    native_setup(RELAY_CONFIG "text_source = auto\n");
    allocation_failure = 0;
    open_overlay();
    TEST_ASSERT(job_running && job_anchor);
    unsigned generation = job_generation;
    close_overlay(); open_overlay();
    TEST_CHECK(job_cancelled(NULL));
    cache_data[0].sentence = "old screenshot";
    on_job_text(); on_job_done();
    TEST_CHECK(job_running && job_anchor && job_generation != generation);
    TEST_CHECK(!anchor_calls && !g_view.hook_reference[0]);
    complete_anchor("今の画面です");
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_READY);
    TEST_CHECK(!strcmp(g_view.hook_reference, "今の画面です"));
}

static void test_dictionary_to_picker_reads_new_screenshot(void)
{
    native_setup("text_source = auto\n");
    allocation_failure = 0;
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    open_overlay(); poll_hooks();
    TEST_ASSERT(seen_hook_id == 10 && !g_view.hook_picker && !job_running);
    pending_cmd = VJO_CMD_HOOK_DISCOVER; on_command(); poll_hooks();
    TEST_CHECK(job_running && job_anchor && g_view.hook_picker);
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_MATCHING && !seen_hook_id);
    TEST_CHECK(!anchor_calls && !discover_calls);
    complete_anchor("今の画面です");
    TEST_CHECK(g_view.hook_match_state == VJO_HOOK_MATCH_READY && anchor_calls == 1);
}

static void test_changed_picker_choice_is_not_silently_selected(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    open_overlay();
    strcpy(hook_snapshot.candidates[0].text, "別の文章");
    hook_snapshot.sequence++;
    poll_hooks();
    TEST_CHECK(!strcmp(g_view.hooks[0].text, "猫を見ました"));
    vjo_post_hook(hook_snapshot.session, 10); on_command();
    TEST_CHECK(!job_running && !hook_snapshot.selected && g_view.hook_picker);
    TEST_CHECK(g_view.status_is_error && strstr(g_view.status, "changed"));
    TEST_CHECK(!strcmp(g_view.hooks[0].text, "別の文章"));
}

static void test_ambiguous_and_stale_hook_choices(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    open_overlay();
    hook_snapshot.count = 2;
    hook_snapshot.candidates[1] = hook_snapshot.candidates[0];
    hook_snapshot.candidates[1].id = 11;
    hook_snapshot.candidates[0].score = hook_snapshot.candidates[1].score = 80;
    text_calibrated = 1;
    poll_hooks();
    TEST_CHECK(!hook_snapshot.selected && g_view.hook_picker && !job_running);
    vjo_post_hook(hook_snapshot.session-1, 10);
    on_command();
    TEST_CHECK(!hook_snapshot.selected && !job_running);
    TEST_CHECK(!capture_calls && !relay_connections);
}

static void test_hook_picker_without_dictionary_settings(void)
{
    native_setup("text_source = hooks\n");
    open_overlay();
    TEST_CHECK(g_view.hook_picker && !g_view.status_is_error && !job_running);
    vjo_post_hook(hook_snapshot.session, 10); on_command();
    TEST_ASSERT(!job_running && g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, "猫を見ました"));
    TEST_CHECK(!capture_calls && !relay_connections && !allocation_calls);
}

static void test_changed_hook_discards_old_lookup(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    open_overlay();
    TEST_ASSERT(job_running && job_native);
    uint32_t checksum;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    strcpy(hook_snapshot.current.text, "犬を見ました");
    poll_hooks();
    on_job_done();
    TEST_CHECK(job_running && job_native && !strcmp(job_hook_text, "犬を見ました"));
    TEST_ASSERT(g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, "犬を見ました") && !capture_calls);
}

static void test_lost_hook_stays_manual(void)
{
    native_setup(RELAY_CONFIG "text_source = auto\n");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    text_calibrated = 1;
    open_overlay();
    poll_hooks();
    TEST_ASSERT(seen_hook_id == 10);
    uint32_t checksum;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    hook_snapshot.selected = 0; memset(&hook_snapshot.current, 0, sizeof(hook_snapshot.current));
    /* The transport clears the old OCR reference when the source is lost. */
    hook_snapshot.candidates[0].score = 0;
    poll_hooks(); on_job_done();
    TEST_CHECK(text_calibrated && g_view.hook_picker && !job_running && anchor_calls == 1);
    TEST_CHECK(!capture_calls);
}

static void test_hook_cache_uses_unfiltered_source(void)
{
    native_setup("text_source = hooks\n");
    strcpy(hook_snapshot.candidates[0].text, "English speaker\n猫を見ました");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    open_overlay(); poll_hooks();
    TEST_ASSERT(!job_running && g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, "猫を見ました"));
    TEST_CHECK(strcmp(g_view.list->header, seen_hook_text) != 0);
    test_now += 400000; poll_hooks();
    TEST_CHECK(!job_running && !capture_calls && !allocation_calls);
}

static void test_blank_selected_hook_recovers(void)
{
    native_setup("text_source = hooks\n");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    hook_snapshot.current.text[0] = 0;
    hook_snapshot.candidates[0].text[0] = 0;
    open_overlay(); poll_hooks();
    TEST_ASSERT(g_view.hook_picker && !job_running);
    strcpy(hook_snapshot.current.text, "猫を見ました");
    poll_hooks(); test_now += 400000; poll_hooks();
    TEST_ASSERT(!g_view.hook_picker && !job_running && g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, "猫を見ました") && !capture_calls);
}

static void remove_game_arenas(void)
{
    mem_uid = -1;
    for (int i = 0; i < 2; i++) vjo_arena_init(&results[i], NULL, 0);
}

static void check_native_sentence(const char *sentence)
{
    TEST_ASSERT(g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, sentence));
    TEST_CHECK(g_view.list->n_entries == 0);
    TEST_CHECK(!g_view.hook_picker && !capture_calls && !anchor_calls);
    if (subtitles) {
        TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy);
        TEST_CHECK(!strcmp(g_view.strip_text, sentence));
    }
}

static void test_low_memory_picker_and_raw_updates(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\nocr_backend = ncnn\n");
    remove_game_arenas();
    activate_game(7, "PCSG00415", VJO_GAME);
    TEST_CHECK(mem_uid < 0 && allocation_calls == 0);
    open_overlay();
    TEST_CHECK(g_view.open && g_view.hook_picker && g_view.hook_count == 1);
    TEST_CHECK(!g_view.status_is_error && !job_running && allocation_calls == 0);
    set_subtitles(1);
    TEST_CHECK(allocation_calls == 0 && !capture_calls && !anchor_calls);
    vjo_post_hook(hook_snapshot.session, 10); on_command();
    TEST_CHECK(allocation_calls == 1 && mem_uid < 0 && !job_running);
    check_native_sentence("猫を見ました");

    /* Kernel polling must neither require arenas nor retry allocation per tick. */
    for (int i = 0; i < 20; i++) { test_now += 50000; poll_hooks(); }
    TEST_CHECK(allocation_calls == 1 && !job_running);
    strcpy(hook_snapshot.current.text, "犬を見ました");
    hook_snapshot.sequence++;
    poll_hooks();
    check_native_sentence("犬を見ました");
    TEST_CHECK(allocation_calls == 1 && !relay_connections && !posted_events);
    test_now += BACKOFF_MIN_US;
    poll_hooks();
    TEST_CHECK(allocation_calls == 2 && !job_running);
    check_native_sentence("犬を見ました");
}

static void test_native_subtitles_need_no_arena(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    remove_game_arenas();
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    set_subtitles(1);
    TEST_CHECK(!g_view.open && subtitles && !job_running && allocation_calls == 0);
    TEST_CHECK(g_view.strip_kind == VJO_STRIP_SENTENCE && !g_view.strip_busy);
    TEST_CHECK(!strcmp(g_view.strip_text, "猫を見ました"));
    strcpy(hook_snapshot.current.text, "次の台詞を読みます");
    poll_hooks(); test_now += 500000; poll_hooks();
    TEST_CHECK(!strcmp(g_view.strip_text, "次の台詞を読みます"));
    TEST_CHECK(!job_running && !allocation_calls && !capture_calls && !anchor_calls);
}

static void test_stale_dictionary_events_preserve_new_native_text(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    set_subtitles(1);
    open_overlay();
    TEST_ASSERT(job_running && job_native && job_lookup);
    uint32_t checksum;
    TEST_ASSERT(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    strcpy(hook_snapshot.current.text, "犬を見ました");
    hook_snapshot.sequence++;
    poll_hooks();
    check_native_sentence("犬を見ました");
    on_job_text();
    check_native_sentence("犬を見ました");
    on_job_done();
    TEST_ASSERT(g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, "犬を見ました"));
    TEST_CHECK(!strcmp(g_view.strip_text, "犬を見ました"));
    TEST_CHECK(!cache_ok && !capture_calls);
}

static void test_low_memory_exit_and_reopen(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    remove_game_arenas();
    open_overlay();
    vjo_post_hook(hook_snapshot.session, 10); on_command();
    TEST_CHECK(allocation_calls == 1 && !job_running);
    close_overlay();
    open_overlay();
    check_native_sentence("猫を見ました");
    TEST_CHECK(allocation_calls == 1); /* same failed allocation remains throttled */
    on_game_exit();
    TEST_CHECK(!g_view.open && !g_view.list && !subtitles && !native_game && !title_id[0]);
    hook_snapshot.session++;
    hook_snapshot.selected = 0;
    memset(&hook_snapshot.current, 0, sizeof(hook_snapshot.current));
    strcpy(hook_snapshot.candidates[0].text, "新しいゲームです");
    activate_game(8, "PCSG00940", VJO_GAME);
    open_overlay();
    TEST_CHECK(g_view.hook_picker && !g_view.list && !job_running);
    allocation_failure = 0;
    vjo_post_hook(hook_snapshot.session, 10); on_command();
    TEST_CHECK(allocation_calls == 2 && mem_uid >= 0 && job_running && job_native);
    TEST_ASSERT(g_view.list && g_view.list->header);
    TEST_CHECK(!strcmp(g_view.list->header, "新しいゲームです"));
}

static void test_native_arena_exhaustion_preserves_raw_text(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    set_subtitles(1); open_overlay(); poll_hooks();
    TEST_ASSERT(job_running && job_native);
    vjo_arena_init(&results[job_idx], result_mem[job_idx], 1);
    uint32_t checksum;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_E_OOM);
    on_job_text(); on_job_done();
    TEST_CHECK(!job_running && !cache_ok && g_view.status_is_error);
    check_native_sentence("猫を見ました");
    int64_t until = ocr_backoff.until > dict_backoff.until ? ocr_backoff.until : dict_backoff.until;
    TEST_ASSERT(until > (int64_t)test_now);
    test_now = (uint64_t)(until-1); poll_hooks();
    TEST_CHECK(!job_running);
    check_native_sentence("猫を見ました");
    test_now++; poll_hooks();
    TEST_CHECK(job_running && job_native && !allocation_calls && !relay_connections);
}

static void test_native_pipeline_change_discards_old_dictionary(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    set_subtitles(1); open_overlay();
    TEST_ASSERT(job_running && job_native);
    uint32_t checksum;
    TEST_ASSERT(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    TEST_ASSERT(cache_data[job_idx].list.n_entries == 1);
    loaded_config.dictionary = VJO_DICT_LOCAL;
    strcpy(loaded_config.local_dictionaries, "replacement.vjdict");
    apply_config();
    TEST_ASSERT(!same_pipeline(&job_cfg, &cfg));
    on_job_text();
    check_native_sentence("猫を見ました");
    on_job_done();
    TEST_CHECK(job_running && job_native && same_pipeline(&job_cfg, &cfg));
    TEST_CHECK(!cache_ok);
    check_native_sentence("猫を見ました");
}

static void test_native_heap_lookup_and_cleanup(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    remove_game_arenas();
    heap_available = 1;
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    open_overlay();
    TEST_ASSERT(job_running && job_native && mem_heap == heap_mem && mem_uid < 0);
    TEST_CHECK(heap_calls == 1 && allocation_calls == 1);
    TEST_CHECK(results[0].size == NATIVE_ARENA_SIZE && results[1].size == NATIVE_ARENA_SIZE);
    TEST_CHECK(results[1].base == heap_mem + NATIVE_ARENA_SIZE);
    uint32_t checksum;
    TEST_ASSERT(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    on_job_text(); on_job_done();
    TEST_ASSERT(g_view.list && g_view.list->n_entries == 1);
    TEST_CHECK(!strcmp(g_view.list->header, "猫を見ました"));
    TEST_CHECK(results[active].peak <= NATIVE_ARENA_SIZE);
    on_game_exit();
    TEST_CHECK(heap_frees == 1 && allocation_frees == 0 && !mem_heap && !g_view.list);
    TEST_CHECK(!results[0].base && !results[1].base);
    mem_free();
    TEST_CHECK(heap_frees == 1);
}

static void test_native_heap_retires_for_ocr(void)
{
    native_setup(RELAY_CONFIG "text_source = hooks\n");
    remove_game_arenas();
    heap_available = 1;
    vjoTextControl(hook_snapshot.session, VJO_TEXT_FOLLOW, 10);
    vjoTextRead(&text_state);
    TEST_ASSERT(mem_alloc() == 0 && mem_heap);
    cfg.text_source = VJO_SOURCE_OCR;
    allocation_failure = 0;
    TEST_ASSERT(mem_alloc() == 0);
    TEST_CHECK(heap_frees == 1 && !mem_heap && mem_uid >= 0);
    TEST_CHECK(results[0].size == RESULT_ARENA_SIZE && results[1].size == RESULT_ARENA_SIZE);
    mem_free();
    TEST_CHECK(allocation_frees == 1 && heap_frees == 1);
}

TEST_LIST = {
    {"changed_picker_choice_is_not_silently_selected", test_changed_picker_choice_is_not_silently_selected},
    {"reopen_during_anchor_discards_old_screenshot", test_reopen_during_anchor_discards_old_screenshot},
    {"dictionary_to_picker_reads_new_screenshot", test_dictionary_to_picker_reads_new_screenshot},
    {"anchor_heap_lifecycle_and_stable_matches", test_anchor_heap_lifecycle_and_stable_matches},
    {"anchor_oom_is_explicit_and_retryable", test_anchor_oom_is_explicit_and_retryable},
    {"anchor_cancel_frees_only_after_done", test_anchor_cancel_frees_only_after_done},
    {"native_heap_lookup_and_cleanup", test_native_heap_lookup_and_cleanup},
    {"native_heap_retires_for_ocr", test_native_heap_retires_for_ocr},
    {"low_memory_picker_and_raw_updates", test_low_memory_picker_and_raw_updates},
    {"native_subtitles_need_no_arena", test_native_subtitles_need_no_arena},
    {"stale_dictionary_events_preserve_new_native_text", test_stale_dictionary_events_preserve_new_native_text},
    {"low_memory_exit_and_reopen", test_low_memory_exit_and_reopen},
    {"native_arena_exhaustion_preserves_raw_text", test_native_arena_exhaustion_preserves_raw_text},
    {"native_pipeline_change_discards_old_dictionary", test_native_pipeline_change_discards_old_dictionary},
    {"manual_hook_bypasses_capture_and_ocr", test_manual_hook_bypasses_capture_and_ocr},
    {"automatic_hooks_match_once_and_handle_ocr_failure", test_automatic_hooks_match_once_and_handle_ocr_failure},
    {"ambiguous_and_stale_hook_choices", test_ambiguous_and_stale_hook_choices},
    {"hook_picker_without_dictionary_settings", test_hook_picker_without_dictionary_settings},
    {"changed_hook_discards_old_lookup", test_changed_hook_discards_old_lookup},
    {"lost_hook_stays_manual", test_lost_hook_stays_manual},
    {"hook_cache_uses_unfiltered_source", test_hook_cache_uses_unfiltered_source},
    {"blank_selected_hook_recovers", test_blank_selected_hook_recovers},
    {"ncnn_manual_and_no_fallback", test_ncnn_manual_and_no_fallback},
    {"ocr_context_changes", test_ocr_context_changes},
    {"region_change_redoes_subtitle_job", test_region_change_redoes_subtitle_job},
    {"local_worker_and_config_changes", test_local_worker_and_config_changes},
    {"manual_relay_without_keys", test_manual_relay_without_keys},
    {"background_relay_without_keys", test_background_relay_without_keys},
    {"subtitles_auto_relay_lookup", test_subtitles_auto_relay_lookup},
    {"relay_missing_host_message", test_relay_missing_host_message},
    {"relay_invalid_host_message", test_relay_invalid_host_message},
    {"unconfigured_relay_skips_background_lookup", test_unconfigured_relay_skips_background_lookup},
    {"cloud_missing_keys_still_block", test_cloud_missing_keys_still_block},
    {"cloud_configured_keys_still_schedule", test_cloud_configured_keys_still_schedule},
    {"relay_lookup_respects_mode_and_backoff", test_relay_lookup_respects_mode_and_backoff},
    {"subtitles_on_press_stay_ocr_only", test_subtitles_on_press_stay_ocr_only},
    {NULL, NULL}
};
