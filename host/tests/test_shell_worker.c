/* Exercise worker.c's actual manual/background gates, not a copy of them.
 * Only Vita I/O is stubbed. Config, error rendering and relay lookup are real. */
#include "acutest.h"
#include "client.h"
#include "pb.h"
#include "replay.h"
#define VJO_TEST_MEMORY_STUBS 1
#define VJO_TEST_WORKER_LIFECYCLE 1
#ifdef VJO_MEMORY_DIAGNOSTICS
/* Keep startup simulation local to this diagnostic test translation unit. */
#define sceKernelCreateMutex vjo_test_default_create_mutex
#define sceKernelCreateEventFlag vjo_test_default_create_event
#define sceKernelCreateThread vjo_test_default_create_thread
#define sceKernelStartThread vjo_test_default_start_thread
#endif
#include <psp2/host_stubs.h>
#ifdef VJO_MEMORY_DIAGNOSTICS
#undef sceKernelCreateMutex
#undef sceKernelCreateEventFlag
#undef sceKernelCreateThread
#undef sceKernelStartThread
SceUID sceKernelCreateMutex(const char *, unsigned int, int, void *);
SceUID sceKernelCreateEventFlag(const char *, unsigned int, unsigned int, void *);
SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int,
                           unsigned int, unsigned int, int, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
#endif

static VjoState kernel_state;
static uint64_t test_now;
static unsigned int posted_events;
static VjoConfig loaded_config;
static int capture_reply, capture_waits, capture_pending;
static unsigned discard_requests;
static uint32_t capture_flags;
static int state_read_result;
static unsigned raw_calls, raw_rows_copied, raw_fault_row;
static unsigned raw_row_zero_reads;
static int raw_short_read, raw_sequential;

static void capture_pixels(uint32_t row, uint32_t n, uint8_t *dst)
{
    for (uint32_t y = 0; y < n; y++) {
        memset(dst + y * kernel_state.raw_stride, 0xCD, kernel_state.raw_stride);
        for (uint32_t x = 0; x < kernel_state.width; x++) {
            uint8_t *p = dst + y * kernel_state.raw_stride + x * 4;
            p[0] = (uint8_t)(x * 13 + (row+y) * 7);
            p[1] = (uint8_t)(x / 4);
            p[2] = (uint8_t)((row+y) * 5);
            p[3] = 255;
        }
    }
}
static unsigned control_polls_before_stop;
static unsigned power_ticks;
static void control_poll_hook(void);
#ifdef VJO_MEIKI_GAME_WORKER
int sceKernelDelayThread(unsigned int us) { test_now += us; return 0; }
#endif

uint64_t sceKernelGetProcessTimeWide(void) { return test_now; }
int sceKernelPowerTick(int type) { TEST_CHECK(type == SCE_KERNEL_POWER_TICK_DEFAULT); power_ticks++; return 0; }
int sceKernelSetEventFlag(SceUID uid, unsigned int bits) { posted_events |= bits; return 0; }
int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len) { return -1; }
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoRegisterShell(void) { return 0; }
int vjoWaitEvent(uint32_t mask, uint32_t *out, uint32_t timeout) {
    test_now += timeout;
    control_poll_hook();
    *out = 0;
    if (capture_pending > 0 && capture_waits > 0 && --capture_waits == 0) {
        kernel_state.done_seq = (uint32_t)capture_pending;
        *out = VJO_EV_CAPTURE_DONE;
        return 0;
    }
    return -1;
}
int vjoGetState(VjoState *out) {
    /* Even populated output must not be trusted when the syscall failed. */
    *out = kernel_state;
    return state_read_result;
}
int vjoSetRegion(const VjoRect *r) { return 0; }
int vjoSetTriggers(int toggle, int subtitle) { return 0; }
int vjoSetGameActive(int pid, int active) { return 0; }
int vjoSetInputBlock(int on) { return 0; }
static int capture_calls;
int vjoRequestCapture(uint32_t flags) {
    if (flags == VJO_CAPTURE_DISCARD) { discard_requests++; return 0; }
    capture_calls++;
    capture_flags = flags;
    capture_pending = capture_reply;
    if (capture_reply > 0 && !capture_waits) kernel_state.done_seq = (uint32_t)capture_reply;
    return capture_reply;
}
int vjoReadRaw(uint32_t row, uint32_t n, void *dst) {
    raw_calls++;
    if (row >= raw_fault_row) return raw_short_read ? (int)n - 1 : -1;
    TEST_CHECK((!raw_sequential || row == raw_rows_copied) && n && n <= 16);
    TEST_CHECK(row < kernel_state.height && n <= kernel_state.height-row);
    capture_pixels(row, n, dst);
    if (!row) raw_row_zero_reads++;
    raw_rows_copied += n;
    return (int)n;
}

#include "../../shell/worker.c"

enum { TEST_NET = 11, TEST_CTL, TEST_EVENT, TEST_CMD, TEST_CAPTURE, TEST_VIEW };
static unsigned thread_joins, thread_deletes, event_deletes, mutex_deletes, anki_starts, anki_stops;
static SceUID join_failure_uid;
static int anki_stop_error;
static char lifecycle_events[128];
static unsigned lifecycle_event_count;
#ifdef VJO_MEMORY_DIAGNOSTICS
static unsigned diagnostic_paf_calls, diagnostic_pool_calls, diagnostic_release_calls;
static int diagnostic_probe_error, diagnostic_release_error;
static int diagnostic_observe_idle, diagnostic_expect_no_results;
static unsigned diagnostic_order;
static int startup_succeeds;
static unsigned startup_threads, startup_mutexes, startup_events, startup_starts;

SceUID sceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt)
{
    (void)attr; (void)count; (void)opt;
    if (!startup_succeeds) return -1;
    startup_mutexes++;
    if (!strcmp(name, "VjoView")) return TEST_VIEW;
    if (!strcmp(name, "VjoCmd")) return TEST_CMD;
    TEST_CHECK(!strcmp(name, "VjoCapture"));
    return TEST_CAPTURE;
}
SceUID sceKernelCreateEventFlag(const char *name, unsigned int attr,
                              unsigned int bits, void *opt)
{
    (void)attr; (void)bits; (void)opt;
    if (!startup_succeeds) return -1;
    TEST_CHECK(!strcmp(name, "VjoNetEv"));
    startup_events++;
    return TEST_EVENT;
}
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *),
                            int priority, unsigned int stack,
                            unsigned int attr, int affinity, void *opt)
{
    (void)priority; (void)attr; (void)affinity; (void)opt;
    if (!startup_succeeds) return -1;
    startup_threads++;
    if (!strcmp(name, "VjoNet")) {
        TEST_CHECK(entry == net_main && stack == JOB_STACK_BYTES);
        return TEST_NET;
    }
    TEST_CHECK(!strcmp(name, "VjoControl") && entry == ctl_main && stack == 0x4000);
    return TEST_CTL;
}
int sceKernelStartThread(SceUID uid, SceSize size, void *args)
{
    (void)size; (void)args;
    if (!startup_succeeds) return -1;
    TEST_CHECK(uid == TEST_NET || uid == TEST_CTL);
    startup_starts++;
    return 0;
}
#endif

static void lifecycle_event(char value)
{
    TEST_ASSERT(lifecycle_event_count + 1 < sizeof(lifecycle_events));
    lifecycle_events[lifecycle_event_count++] = value;
    lifecycle_events[lifecycle_event_count] = 0;
}

int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout)
{
    TEST_CHECK(uid == TEST_NET || uid == TEST_CTL);
    TEST_CHECK(!running && (posted_events & NET_EV_QUIT));
    thread_joins++;
    lifecycle_event(uid == TEST_NET ? 'n' : 'c');
    return uid == join_failure_uid ? -1 : 0;
}
int sceKernelDeleteThread(SceUID uid)
{
    TEST_CHECK(uid == TEST_NET || uid == TEST_CTL);
    thread_deletes++;
    lifecycle_event(uid == TEST_NET ? 'N' : 'C');
    return 0;
}
int sceKernelDeleteEventFlag(SceUID uid)
{
    TEST_CHECK(uid == TEST_EVENT);
    event_deletes++;
    lifecycle_event('e');
    return 0;
}
int sceKernelDeleteMutex(SceUID uid)
{
    TEST_CHECK(uid == TEST_CMD || uid == TEST_CAPTURE || uid == TEST_VIEW);
    mutex_deletes++;
    lifecycle_event(uid == TEST_CMD ? 'm' : uid == TEST_CAPTURE ? 'p' : 'v');
    return 0;
}

#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
static unsigned bridge_finishes, bridge_starts;
static unsigned bridge_recognitions, bridge_detections;
static int bridge_start_error, bridge_inference_error, bridge_finish_error;
static unsigned char borrowed_metadata[64], borrowed_workspace[64], borrowed_input[64];
static float bridge_tensor[MEIKI_DETECT_ELEMENTS];
static uint64_t bridge_scratch[(MEIKI_PREPROCESS_SCRATCH_BYTES + 7) / 8];

int vjo_meiki_bridge_start_mode(VjoMeikiBridge *bridge, const VjoPlatform *platform,
                               const char *model_dir, int dialogue_box,
                               int (*cancelled)(void *), void *ud)
{
    (void)platform; (void)model_dir; (void)dialogue_box;
    (void)cancelled; (void)ud;
    bridge_starts++;
    if (bridge_start_error) return bridge_start_error;
    bridge->module = 77;
    bridge->metadata = borrowed_metadata;
    bridge->workspace = borrowed_workspace;
    bridge->preprocessing = bridge_tensor;
    bridge->input = bridge_tensor;
    bridge->input_elements = MEIKI_DETECT_ELEMENTS;
    bridge->scratch = bridge_scratch;
    return VJO_OK;
}
static int bridge_recognize(void *ud, const char *model_path, const float *input,
                            MeikiOutput *out, MeikiStats *stats)
{
    (void)ud;
    TEST_CHECK(strstr(model_path, VJO_MEIKI_MODEL_FILENAME) != NULL);
    TEST_CHECK(input == bridge_tensor);
    bridge_recognitions++;
    memset(out, 0, sizeof(*out));
    memset(stats, 0, sizeof(*stats));
    out->codes[0] = 0x732B; /* invented inference result: 猫 */
    out->scores[0] = .9f;
    out->boxes[2] = 10.0f;
    return bridge_inference_error;
}
static int bridge_detect(void *ud, const char *model_path, const float *input,
                         int32_t target_width, int32_t target_height,
                         MeikiDetectOutput *out, MeikiStats *stats)
{
    (void)ud; (void)target_width; (void)target_height;
    TEST_CHECK(strstr(model_path, VJO_MEIKI_DETECT_MODEL_FILENAME) != NULL);
    TEST_CHECK(input == bridge_tensor);
    bridge_detections++;
    memset(out, 0, sizeof(*out));
    memset(stats, 0, sizeof(*stats));
    out->scores[0] = .9f;
    out->boxes[0][2] = (float)kernel_state.width;
    out->boxes[0][3] = (float)kernel_state.height;
    return VJO_OK;
}
VjoMeikiEngine vjo_meiki_bridge_engine(VjoMeikiBridge *bridge)
{
    (void)bridge;
    return (VjoMeikiEngine){NULL, bridge_recognize, bridge_detect};
}
int vjo_meiki_bridge_finish(VjoMeikiBridge *bridge)
{
    bridge_finishes++;
    lifecycle_event('b');
    if (bridge_finish_error) return bridge_finish_error;
    memset(bridge, 0, sizeof(*bridge));
    bridge->module = -1;
    return 0;
}
#endif

#ifdef VJO_MEIKI_GAME_WORKER
static int game_submit_reply, game_engine_reply;
static const char *game_error_metadata;
static unsigned game_submits, game_reads, game_cancels;
static VjoGameOcrRequest game_request;
int vjoOcrSubmit(const VjoGameOcrRequest *request)
{
    TEST_CHECK(request->size == sizeof(*request));
    TEST_CHECK(request->done_seq == kernel_state.done_seq);
    TEST_CHECK(request->width == kernel_state.width && request->height == kernel_state.height);
    TEST_CHECK(request->stride == kernel_state.raw_stride);
    game_submits++; game_request = *request; return game_submit_reply;
}
int vjoOcrRead(uint32_t seq, VjoGameOcrResult *result)
{
    TEST_CHECK(seq == (uint32_t)game_submit_reply);
    game_reads++; memset(result, 0, sizeof(*result));
    result->size = sizeof(*result); result->seq = seq; result->rc = game_engine_reply;
    strcpy(result->text, game_error_metadata ? game_error_metadata : "猫"); return 0;
}
int vjoOcrCancel(uint32_t seq) { (void)seq; game_cancels++; return 0; }
#endif

void vjo_config_load(VjoConfig *out, VjoArena *a) { *out = loaded_config; }
void vjo_log_configure(const VjoConfig *c) {}
void vjo_log(const char *fmt, ...) {}
int vjo_region_load(const char *tid, VjoRect *out, VjoArena *a) { return 0; }
int vjo_region_save(const char *tid, const VjoRect *r, VjoArena *a) { return 0; }
int vjo_anki_start(void) { anki_starts++; return -1; }
int vjo_anki_stop(void) { anki_stops++; lifecycle_event('a'); return anki_stop_error; }
void vjo_anki_configure(const VjoConfig *c) {}
void vjo_anki_post_check(unsigned int seq) {}
void vjo_platform_vita(VjoPlatform *p) {}

static uint8_t result_mem[2][RESULT_ARENA_SIZE] __attribute__((aligned(64)));
static unsigned allocation_calls, allocation_frees;
static int allocation_failure;
static uint8_t heap_mem[MEM_SIZE];
static unsigned allocation_bytes, heap_bytes;
static unsigned heap_calls, heap_frees;
static int heap_available;
void *vjo_shell_heap_alloc(size_t bytes)
{
    heap_calls++;
    heap_bytes = bytes;
    TEST_CHECK(bytes <= MEM_SIZE);
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
#ifdef VJO_PAF_ALLOC
static unsigned paf_allocations, paf_frees;
static int paf_allocation_fail;
void *vjo_paf_alloc(size_t bytes)
{
    TEST_CHECK(bytes == sizeof(result_mem));
#ifdef VJO_MEMORY_DIAGNOSTICS
    if (diagnostic_observe_idle) {
        TEST_CHECK(diagnostic_order == 2 && !job_running && !capture_calls);
        diagnostic_order = 3;
    }
#endif
    paf_allocations++;
    return paf_allocation_fail ? NULL : result_mem;
}
void vjo_paf_free(void *pointer)
{
    if (pointer) {
        TEST_CHECK(pointer == result_mem);
        paf_frees++;
        lifecycle_event('r');
    }
}
void vjo_paf_memory_report(void) {}
#endif

#ifdef VJO_MEMORY_DIAGNOSTICS
static void check_diagnostic_idle(void)
{
    if (!diagnostic_observe_idle) return;
    TEST_CHECK(!job_running && !anki_started && !capture_calls);
    TEST_CHECK(!bridge_starts && !bridge_recognitions && !bridge_detections);
    TEST_CHECK(!(posted_events & NET_EV_JOB));
    if (diagnostic_expect_no_results)
        TEST_CHECK(!has_result_memory() && !paf_allocations && !allocation_calls);
}
void vjo_paf_probe_once(void)
{
    diagnostic_paf_calls++;
    check_diagnostic_idle();
    if (diagnostic_observe_idle) {
        TEST_CHECK(diagnostic_order == 0);
        diagnostic_order = 1;
    }
}
int vjo_memory_probe_once(void)
{
    diagnostic_pool_calls++;
    check_diagnostic_idle();
    if (diagnostic_observe_idle) {
        TEST_CHECK(diagnostic_order == 1);
        diagnostic_order = 2;
    }
    return diagnostic_probe_error;
}
int vjo_memory_probe_release(void)
{
    diagnostic_release_calls++;
    TEST_CHECK(!running && !threads_started);
    return diagnostic_release_error;
}
#endif

static void control_poll_hook(void)
{
    if (control_polls_before_stop && --control_polls_before_stop == 0)
        running = 0;
}

/* Let the real control loop handle one queued completion, then stop. */
static void poll_control_once(void)
{
    control_polls_before_stop = 2;
    ctl_main(0, NULL);
}
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
    vjo_config_parse(&loaded_config, "ocr_backend = lens\n", strlen("ocr_backend = lens\n"));
    vjo_config_parse(&loaded_config, ini, strlen(ini));
    cfg = loaded_config;
    allocation_bytes = heap_bytes = 0;
#ifdef VJO_MEIKI_GAME_WORKER
    game_submit_reply = 19; game_engine_reply = VJO_OK;
    game_error_metadata = NULL;
    game_submits = game_reads = game_cancels = 0;
#endif
    capture_calls = 0;
    discard_requests = 0;
    capture_reply = VJO_ERR_NO_GAME;
    capture_waits = capture_pending = 0;
    capture_flags = 0;
    state_read_result = 0;
    raw_calls = raw_rows_copied = 0;
    raw_row_zero_reads = 0;
    raw_fault_row = 0;
    raw_short_read = 0;
    raw_sequential = 1;
    allocation_calls = allocation_frees = 0;
    allocation_failure = 1;
    heap_calls = heap_frees = 0;
    heap_available = 0;
    mem_heap = NULL;
    running = 1;
    net_thread = ctl_thread = net_evf = cmd_lock = capture_lock = view_lock = -1;
    threads_started = 0;
    thread_joins = thread_deletes = event_deletes = mutex_deletes = anki_starts = anki_stops = 0;
    join_failure_uid = -1;
    anki_stop_error = 0;
    lifecycle_event_count = 0;
    lifecycle_events[0] = 0;
#ifdef VJO_MEMORY_DIAGNOSTICS
    diagnostic_failure = 0;
    diagnostic_paf_calls = diagnostic_pool_calls = diagnostic_release_calls = 0;
    diagnostic_probe_error = diagnostic_release_error = 0;
    diagnostic_observe_idle = diagnostic_expect_no_results = 0;
    diagnostic_order = 0;
    startup_succeeds = 0;
    startup_threads = startup_mutexes = startup_events = startup_starts = 0;
#endif
#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
    memset(&meiki_bridge, 0, sizeof(meiki_bridge));
    meiki_bridge.module = -1;
    bridge_finishes = bridge_starts = 0;
    bridge_recognitions = bridge_detections = 0;
    bridge_start_error = VJO_E_OCR_UNAVAILABLE;
    bridge_inference_error = 0;
    bridge_finish_error = 0;
#endif
    power_ticks = 0;
    region_selected = job_region_selected = 0;
    capture_generation = job_generation = 0;
    memset(&g_view, 0, sizeof(g_view));
    memset(cache_data, 0, sizeof(cache_data));
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
    memset(&mem_backoff, 0, sizeof(mem_backoff));
    memset(&plat, 0, sizeof(plat));
    plat.connect = connect_relay;
    plat.disconnect = disconnect_relay;
#ifdef VJO_PAF_ALLOC
    result_memory = result_mem;
    mem_uid = -1;
    paf_allocations = paf_frees = 0;
    paf_allocation_fail = 0;
#else
    mem_uid = 1; /* preallocated game arenas, no SDK allocator needed */
#endif
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
    control_polls_before_stop = 0;
    pending_cmd = VJO_CMD_NONE;
    pending_pid = 0;
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

static void test_legacy_source_settings_use_visual_ocr(void)
{
    const int legacy_sources[] = { VJO_SOURCE_AUTO, VJO_SOURCE_HOOKS };
    for (unsigned i = 0; i < sizeof(legacy_sources) / sizeof(legacy_sources[0]); i++) {
        setup(RELAY_CONFIG "ocr_backend = meiki\nocr_mode = auto\n");
        loaded_config.text_source = legacy_sources[i];
        apply_config();
        TEST_CHECK(cfg.text_source == VJO_SOURCE_OCR);
        TEST_CHECK(!auto_mode() && !background_wanted());
        region_selected = 1;
        open_overlay();
        TEST_CHECK(job_running && job_region_selected);
        TEST_CHECK(job_cfg.text_source == VJO_SOURCE_OCR && job_cfg.ocr_backend == VJO_OCR_MEIKI);
    }
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

static void game_setup(const char *mode)
{
    setup(mode);
}


/* A synthetic response with invented text. The real JPEG encoder, streamed
 * Lens request, HTTP receiver and protobuf parser run; no socket is opened. */
static VjoMemConn capture_conn;
static char capture_http[1024], capture_upload[128 * 1024];
static unsigned capture_connections;

static void capture_random(void *ud, void *dst, size_t n) { memset(dst, 0x5A, n); }
static int connect_capture(void *ud, const char *host, int port, int timeout, int io_timeout, VjoConn *out)
{
    static uint8_t response_mem[2048];
    static const unsigned wrapper_fields[] = {1, 2, 1, 1, 3, 2};
    VjoArena a;
    VjoBuf message;
    TEST_CHECK(!strcmp(host, VJO_LENS_HOST) && port == 443);
    vjo_arena_init(&a, response_mem, sizeof(response_mem));
    vjo_buf_init(&message, &a);
    pb_put_string(&message, 2, "猫を見ました");
    for (unsigned i = 0; i < sizeof(wrapper_fields)/sizeof(*wrapper_fields); i++) {
        VjoBuf outer;
        vjo_buf_init(&outer, &a);
        pb_put_bytes(&outer, wrapper_fields[i], message.data, message.len);
        TEST_CHECK(!outer.oom);
        message = outer;
    }
    int header = snprintf(capture_http, sizeof(capture_http),
        "HTTP/1.1 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)message.len);
    TEST_CHECK((size_t)header + message.len <= sizeof(capture_http));
    memcpy(capture_http + header, message.data, message.len);
    memset(&capture_conn, 0, sizeof(capture_conn));
    capture_conn.in = capture_http;
    capture_conn.len = (size_t)header + message.len;
    capture_conn.step = 11; /* exercise fragmented HTTP/protobuf receipt */
    capture_conn.out = capture_upload;
    capture_conn.out_cap = sizeof(capture_upload);
    vjo_memconn_init(&capture_conn, out);
    capture_connections++;
    return VJO_OK;
}
static void disconnect_capture(void *ud, VjoConn *c) {}

static void test_local_capture_keeps_multiple_passes(void)
{
    setup("ocr_backend = ncnn\n");
    capture_reply = 71;
    capture_waits = 2;
    VjoState st;
    /* run_local_ocr uses wait_capture(0), without the JPEG wrapper. Verify
     * the shared capture call preserves its multi-pass request even when
     * this host build omits the ncnn model runner. */
    TEST_CHECK(wait_capture(0, &st) == VJO_OK);
    TEST_CHECK(capture_calls == 1 && capture_flags == 0);
}

/* Complete OCR with invented text; Lens transport is covered separately. */

static void remove_game_arenas(void)
{
    mem_uid = -1;
#ifdef VJO_PAF_ALLOC
    result_memory = NULL;
#endif
    for (int i = 0; i < 2; i++) vjo_arena_init(&results[i], NULL, 0);
}

static void test_result_memory_reuse_and_job_gate(void)
{
    setup(RELAY_CONFIG);
    TEST_CHECK(has_result_memory());
    TEST_CHECK(mem_alloc() == 0); /* reuses both allocator kinds */
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_allocations == 0);
#endif
    start_job("preallocated memory", 1);
    TEST_CHECK(job_running && (posted_events & NET_EV_JOB));
    job_running = 0;
    mem_free();
    TEST_CHECK(!has_result_memory());
    posted_events = 0;
#ifdef VJO_PAF_ALLOC
    paf_allocation_fail = 1;
#endif
    start_job("without result memory", 1);
    TEST_CHECK(!job_running && !posted_events);
    mem_free(); /* repeated cleanup must not free twice */
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_allocations == 1 && paf_frees == 1);
#endif
}

static void test_game_exit_frees_idle_result_memory(void)
{
    setup(RELAY_CONFIG);
    g_view.list = &cache_data[0].list;
    on_game_exit();
    TEST_CHECK(!has_result_memory() && !title_id[0]);
    TEST_CHECK(!g_view.list && !results[0].base && !results[1].base);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_frees == 1);
#endif
}

static void test_game_exit_during_job_frees_after_cancelled_completion(void)
{
    setup(RELAY_CONFIG);
    open_overlay();
    TEST_ASSERT(job_running && has_result_memory());
    unsigned old_generation = job_generation;
    on_game_exit();
    TEST_CHECK(job_running && has_result_memory());
    TEST_CHECK(capture_generation != old_generation && job_cancelled(NULL));
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_frees == 0); /* worker still owns the allocation */
#endif
    cache_data[job_idx].sentence = "stale game";
    job_text_ready = job_done = 1;
    poll_control_once();
    TEST_CHECK(!job_running && !has_result_memory());
    TEST_CHECK(!cache_ok && active == -1 && !g_view.open && !g_view.list);
    TEST_CHECK(!results[0].base && !results[1].base);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_frees == 1);
#endif
}

static void test_live_job_prevents_idle_cleanup(void)
{
    setup(RELAY_CONFIG);
    open_overlay();
    TEST_ASSERT(job_running);
    on_game_exit();
    poll_control_once(); /* no completion: result memory is still in use */
    TEST_CHECK(job_running && has_result_memory());
    TEST_CHECK(power_ticks > 0);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_frees == 0);
#endif
}

static void setup_live_worker(void)
{
    setup(RELAY_CONFIG);
    net_thread = TEST_NET;
    ctl_thread = TEST_CTL;
    net_evf = TEST_EVENT;
    cmd_lock = TEST_CMD;
    capture_lock = TEST_CAPTURE;
    view_lock = TEST_VIEW;
    threads_started = anki_started = 1;
}

static void check_shell_resources_retained(void)
{
    TEST_CHECK(net_thread == TEST_NET && ctl_thread == TEST_CTL);
    TEST_CHECK(net_evf == TEST_EVENT && cmd_lock == TEST_CMD);
    TEST_CHECK(capture_lock == TEST_CAPTURE && view_lock == TEST_VIEW);
    TEST_CHECK(anki_started && has_result_memory());
    TEST_CHECK(thread_deletes == 0 && event_deletes == 0 && mutex_deletes == 0);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_frees == 0);
#endif
}

static void check_shell_resources_released(void)
{
    TEST_CHECK(net_thread == -1 && ctl_thread == -1 && net_evf == -1);
    TEST_CHECK(cmd_lock == -1 && capture_lock == -1 && view_lock == -1);
    TEST_CHECK(!threads_started && !anki_started && !has_result_memory());
    TEST_CHECK(!results[0].base && !results[1].base);
    TEST_CHECK(thread_deletes == 2 && event_deletes == 1 && mutex_deletes == 3);
#ifdef VJO_PAF_ALLOC
    TEST_CHECK(paf_frees == 1);
#endif
}

static void test_worker_stop_success_and_idempotence(void)
{
    setup_live_worker();
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(!running && (posted_events & NET_EV_QUIT));
    TEST_CHECK(thread_joins == 2 && anki_stops == 1);
    check_shell_resources_released();
#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
    TEST_CHECK(bridge_finishes == 1);
    TEST_CHECK(!strcmp(lifecycle_events, "ncbaNCrempv"));
#elif defined(VJO_PAF_ALLOC)
    TEST_CHECK(!strcmp(lifecycle_events, "ncaNCrempv"));
#else
    TEST_CHECK(!strcmp(lifecycle_events, "ncaNCempv"));
#endif
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(thread_joins == 2); /* no joins or deletions of expired handles */
    check_shell_resources_released();
}

static void test_worker_stop_join_failure_then_retry(void)
{
    const SceUID failed_threads[] = {TEST_NET, TEST_CTL};
    for (unsigned i = 0; i < sizeof(failed_threads) / sizeof(failed_threads[0]); ++i) {
        setup_live_worker();
        join_failure_uid = failed_threads[i];
        TEST_CHECK(vjo_worker_stop() < 0);
        TEST_CHECK(!running && (posted_events & NET_EV_QUIT));
        TEST_CHECK(threads_started && thread_joins == i + 1);
        TEST_CHECK(anki_stops == 0);
        check_shell_resources_retained();
#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
        TEST_CHECK(bridge_finishes == 0); /* engine cleanup waits for worker ownership */
#endif
        join_failure_uid = -1;
        TEST_CHECK(vjo_worker_stop() == 0);
        TEST_CHECK(thread_joins == i + 3 && anki_stops == 1);
        check_shell_resources_released();
        TEST_CHECK(vjo_worker_stop() == 0);
        TEST_CHECK(thread_joins == i + 3);
        check_shell_resources_released();
    }
}

static void test_worker_stop_anki_failure_then_retry(void)
{
    setup_live_worker();
    anki_stop_error = -1;
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(!running && !threads_started && thread_joins == 2);
    TEST_CHECK(anki_stops == 1);
    check_shell_resources_retained();
#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
    TEST_CHECK(bridge_finishes == 1);
#endif
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(thread_joins == 2 && anki_stops == 2);
    check_shell_resources_retained();
    anki_stop_error = 0;
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(thread_joins == 2 && anki_stops == 3);
    check_shell_resources_released();
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(thread_joins == 2);
    check_shell_resources_released();
}

#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
static void test_worker_stop_bridge_failure_then_retry(void)
{
    setup_live_worker();
    meiki_bridge.module = 77;
    meiki_bridge.metadata = borrowed_metadata;
    meiki_bridge.workspace = borrowed_workspace;
    meiki_bridge.preprocessing = borrowed_input;
    bridge_finish_error = VJO_E_SOURCE;
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(!running && !threads_started && thread_joins == 2);
    TEST_CHECK(bridge_finishes == 1 && !strcmp(lifecycle_events, "ncb"));
    TEST_CHECK(anki_stops == 0);
    check_shell_resources_retained();
    TEST_CHECK(meiki_bridge.module == 77 && meiki_bridge.metadata == borrowed_metadata);
    TEST_CHECK(meiki_bridge.workspace == borrowed_workspace);
    TEST_CHECK(meiki_bridge.preprocessing == borrowed_input);

    TEST_CHECK(vjo_worker_stop() < 0); /* persistent refusal keeps the same owner */
    TEST_CHECK(thread_joins == 2 && bridge_finishes == 2);
    TEST_CHECK(anki_stops == 0);
    check_shell_resources_retained();
    TEST_CHECK(meiki_bridge.module == 77 && meiki_bridge.metadata == borrowed_metadata);
    TEST_CHECK(meiki_bridge.workspace == borrowed_workspace);
    TEST_CHECK(meiki_bridge.preprocessing == borrowed_input);

    bridge_finish_error = 0;
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(thread_joins == 2 && bridge_finishes == 3 && anki_stops == 1);
    check_shell_resources_released();
    TEST_CHECK(meiki_bridge.module == -1 && !meiki_bridge.metadata);
    TEST_CHECK(!meiki_bridge.workspace && !meiki_bridge.preprocessing);
    TEST_CHECK(!strcmp(lifecycle_events, "ncbbbaNCrempv"));
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(thread_joins == 2);
    check_shell_resources_released();
}
#endif

#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
static void meiki_capture_setup(void)
{
    game_setup(RELAY_CONFIG "text_source = ocr\nocr_backend = meiki\n"
                 "meiki_layout = dialogue_box\nocr_mode = auto\n");
    /* A retail-game context uses the selected screenshot for OCR. */
    activate_game(7, "PCSG00001", VJO_GAME);
    region_selected = 1;
    bridge_start_error = VJO_OK;
    capture_reply = 71;
    capture_waits = 2;
    raw_fault_row = UINT32_MAX;
    raw_sequential = 0; /* detector and recognizer each read the same capture */
    kernel_state.width = 96;
    kernel_state.height = 32;
    kernel_state.raw_stride = kernel_state.width * 4;
    kernel_state.capture_checksum = 0x12345678;
    capture_connections = 0;
    plat.connect = connect_capture; /* Any unexpected Lens call is observable. */
    plat.disconnect = disconnect_capture;
    plat.random = capture_random;
    plat.plain_http = 1;
}

static void test_meiki_capture_without_lens(void)
{
    meiki_capture_setup();
    TEST_CHECK(VJO_API_VERSION == 9 && cfg.text_source == VJO_SOURCE_OCR);
    TEST_CHECK(!auto_mode() && !background_wanted() && !job_running);
    open_overlay();
    TEST_ASSERT(job_running && job_region_selected);
    job_lookup = 0; /* Exercise the OCR boundary without a dictionary socket. */
    uint32_t checksum = 0;
    TEST_ASSERT(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_OK);
    TEST_CHECK(capture_calls == 1 && capture_flags == 0 && !discard_requests);
    TEST_CHECK(raw_row_zero_reads >= 2); /* Actual detector + recognition passes. */
    TEST_CHECK(bridge_starts == 1 && bridge_detections == 1 && bridge_recognitions == 1);
    TEST_CHECK(bridge_finishes == 1 && meiki_bridge.module == -1);
    TEST_CHECK(checksum == kernel_state.capture_checksum);
    TEST_CHECK(!strcmp(cache_data[job_idx].sentence, "猫"));
    TEST_CHECK(!capture_connections && !relay_connections);
    on_job_text();
    job_done = 1; on_job_done();
    on_game_exit();
    TEST_CHECK(!has_result_memory() && !meiki_bridge.workspace && !g_view.list);
}

static void test_meiki_capture_errors_cleanup_without_fallback(void)
{
    static const struct {
        const char *name;
        int request, result, stride_extra, start_error, inference_error, finish_error, expected;
        unsigned starts, recognitions;
    } cases[] = {
        {"capture memory", VJO_ERR_NO_MEMORY, 0, 0, 0, 0, 0, VJO_E_OOM, 0, 0},
        {"capture copy", 71, VJO_ERR_COPY, 0, 0, 0, 0, VJO_E_SOURCE, 0, 0},
        {"invalid stride", 71, 0, 4, 0, 0, 0, VJO_E_SOURCE, 0, 0},
        {"module unavailable", 71, 0, 0, VJO_E_OCR_UNAVAILABLE, 0, 0, VJO_E_OCR_UNAVAILABLE, 1, 0},
        {"recognizer error", 71, 0, 0, 0, VJO_E_SOURCE, 0, VJO_E_SOURCE, 1, 1},
        {"module finish error", 71, 0, 0, 0, 0, VJO_E_SOURCE, VJO_E_SOURCE, 1, 1},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
        meiki_capture_setup();
        capture_reply = cases[i].request;
        kernel_state.capture_result = cases[i].result;
        kernel_state.raw_stride += (unsigned)cases[i].stride_extra;
        bridge_start_error = cases[i].start_error;
        bridge_inference_error = cases[i].inference_error;
        bridge_finish_error = cases[i].finish_error;
        open_overlay();
        TEST_ASSERT(job_running);
        uint32_t checksum = 0;
        int rc = run_job(&results[job_idx], &cache_data[job_idx], &checksum);
        TEST_CHECK_(rc == cases[i].expected, "%s: rc=%d", cases[i].name, rc);
        TEST_CHECK(cache_data[job_idx].failed_stage == VJO_STAGE_OCR && !job_text_ready);
        TEST_CHECK(bridge_starts == cases[i].starts && bridge_recognitions == cases[i].recognitions);
        TEST_CHECK(bridge_finishes == 1 && capture_flags == 0 && !discard_requests);
        TEST_CHECK(!capture_connections && !relay_connections);
        if (cases[i].finish_error) {
            TEST_CHECK(meiki_bridge.module == 77 && meiki_bridge.metadata == borrowed_metadata);
            TEST_CHECK(meiki_bridge.workspace == borrowed_workspace && meiki_bridge.input == bridge_tensor);
            bridge_finish_error = 0;
            TEST_CHECK(vjo_meiki_bridge_finish(&meiki_bridge) == VJO_OK);
        }
        TEST_CHECK(meiki_bridge.module == -1 && !meiki_bridge.workspace);
        job_done = 1; on_job_done();
        on_game_exit();
        TEST_CHECK(!has_result_memory() && !results[0].base && !results[1].base);
    }
}
#endif

#ifdef VJO_MEIKI_GAME_WORKER
static void test_game_meiki_dispatch_and_failure(void)
{
    const int expected[] = {VJO_OK, VJO_E_OOM, VJO_E_OCR_UNAVAILABLE};
    for (unsigned i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) {
        game_setup(RELAY_CONFIG "text_source = ocr\nocr_backend = meiki\n"
                     "meiki_layout = dialogue_box\nocr_mode = on_press\n");
        activate_game(7, "PCSG00001", VJO_GAME);
        region_selected = 1; capture_reply = 71; capture_waits = 2;
        kernel_state.width = 96; kernel_state.height = 32;
        kernel_state.raw_stride = kernel_state.width * 4;
        kernel_state.capture_checksum = 0x12345678;
        if (i == 1) game_engine_reply = VJO_E_OOM;
        if (i == 2) game_submit_reply = -1;
        open_overlay();
        TEST_ASSERT(job_running);
        TEST_CHECK(RESULT_ARENA_SIZE == 160 * 1024);
        job_lookup = 0;
        uint32_t checksum = 0;
        TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == expected[i]);
        TEST_CHECK(game_submits == 1 && game_reads == (i == 2 ? 0u : 1u));
        TEST_CHECK(game_request.layout == VJO_GAME_OCR_DIALOGUE_BOX && !game_cancels);
        TEST_CHECK(capture_flags == 0 && capture_calls == 1 && !raw_calls);
        TEST_CHECK(!relay_connections);
        if (!i) {
            TEST_CHECK(!strcmp(cache_data[job_idx].sentence, "猫"));
            TEST_CHECK(checksum == kernel_state.capture_checksum && job_text_ready);
        } else {
            TEST_CHECK(!job_text_ready && cache_data[job_idx].failed_stage == VJO_STAGE_OCR);
        }
        job_done = 1; on_job_done(); on_game_exit();
        TEST_CHECK(!has_result_memory());
    }
}
static void test_game_meiki_model_access_error(void)
{
    game_setup(RELAY_CONFIG "text_source = ocr\nocr_backend = meiki\nocr_mode = on_press\n");
    activate_game(7, "PCSG00001", VJO_GAME);
    region_selected = 1; capture_reply = 71;
    kernel_state.width = 96; kernel_state.height = 32;
    kernel_state.raw_stride = kernel_state.width*4;
    game_engine_reply = VJO_E_OCR_MODEL_IO;
    game_error_metadata = "MIO1:R:O:8001000D:P:00000000";
    open_overlay();
    TEST_ASSERT(job_running);
    job_lookup = 0;
    uint32_t checksum = 0;
    TEST_CHECK(run_job(&results[job_idx], &cache_data[job_idx], &checksum) == VJO_E_OCR_MODEL_IO);
    const VjoOverlayData *out = &cache_data[job_idx];
    TEST_CHECK(out->failed_stage == VJO_STAGE_OCR && out->err.rc == VJO_E_OCR_MODEL_IO);
    TEST_ASSERT(out->err.detail);
    TEST_CHECK(strstr(out->err.detail, "open the recognizer model (0x8001000D)") != NULL);
    TEST_CHECK(strstr(out->err.detail, "Keep the installed model files") != NULL);
    TEST_CHECK(!strstr(out->err.detail, "Install") && !strstr(out->err.detail, "MIO1"));
    TEST_CHECK(!job_text_ready && !relay_connections && !raw_calls);
    job_done = 1; on_job_done(); on_game_exit();
    TEST_CHECK(!has_result_memory());
    game_error_metadata = NULL;
}
#endif

static void test_meiki_manual_and_no_fallback(void)
{
    setup("ocr_backend = meiki\nocr_mode = auto\n" RELAY_CONFIG);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI);
    TEST_CHECK(!auto_mode() && !background_wanted());
    open_overlay();
    TEST_ASSERT(job_running);
    VjoOverlayData out;
    uint32_t checksum = 0;
#ifdef VJO_WITH_MEIKI
    TEST_CHECK(run_job(&results[job_idx], &out, &checksum) == VJO_E_OCR_REGION);
#ifndef VJO_MEIKI_GAME_WORKER
    TEST_CHECK(!bridge_starts && !bridge_finishes); /* selection is checked before capture/load */
#endif
#else
    TEST_CHECK(run_job(&results[job_idx], &out, &checksum) == VJO_E_OCR_UNAVAILABLE);
#endif
    TEST_CHECK(out.failed_stage == VJO_STAGE_OCR && !relay_connections);
}

static void test_meiki_layout_invalidates_cache(void)
{
    setup("dictionary = local\nocr_backend = meiki\n");
    VjoConfig previous = cfg;
    cache_ok = 1;
    loaded_config.meiki_layout = VJO_MEIKI_DIALOGUE_BOX;
    apply_config();
    TEST_CHECK(!cache_ok && !same_pipeline(&previous, &cfg));
    previous.ocr_backend = cfg.ocr_backend = VJO_OCR_LENS;
    TEST_CHECK(same_pipeline(&previous, &cfg));
}

#ifdef VJO_PAF_ALLOC
static void test_paf_allocation_failure_refuses_job(void)
{
    setup(RELAY_CONFIG);
    mem_free();
    paf_allocations = paf_frees = 0;
    paf_allocation_fail = 1;
    open_overlay();
    TEST_CHECK(paf_allocations == 1 && paf_frees == 0);
    TEST_CHECK(!has_result_memory() && !job_running && !posted_events);
    TEST_CHECK(g_view.open && g_view.status_is_error);
    TEST_CHECK(!strcmp(g_view.status, "Not enough memory for the overlay"));
}
#endif

#ifdef VJO_MEMORY_DIAGNOSTICS
#define DIAGNOSTIC_CONFIG "text_source = ocr\nocr_backend = meiki\nocr_mode = on_press\n" RELAY_CONFIG

static void test_diagnostic_probes_only_for_idle_pure_meiki(void)
{
    setup(DIAGNOSTIC_CONFIG);
    job_running = 1;
    start_job("already running", 0);
    TEST_CHECK(!diagnostic_paf_calls && !diagnostic_pool_calls);
    TEST_CHECK(!capture_calls && !posted_events);

    const char *other_backends[] = {"lens", "ncnn"};
    for (unsigned i = 0; i < sizeof(other_backends) / sizeof(*other_backends); ++i) {
        char ini[256];
        snprintf(ini, sizeof(ini), "text_source = ocr\nocr_backend = %s\n%s",
                 other_backends[i], RELAY_CONFIG);
        setup(ini);
        start_job("other backend", 0);
        TEST_CHECK(job_running && (posted_events & NET_EV_JOB));
        TEST_CHECK(!diagnostic_paf_calls && !diagnostic_pool_calls);
    }


}

static void test_diagnostic_probes_precede_result_allocation_and_job(void)
{
    const int allowed[] = {VJO_MEMORY_PROBE_OK, VJO_MEMORY_PROBE_UNAVAILABLE};
    for (unsigned i = 0; i < sizeof(allowed) / sizeof(*allowed); ++i) {
        setup(DIAGNOSTIC_CONFIG);
        remove_game_arenas();
        diagnostic_observe_idle = diagnostic_expect_no_results = 1;
        diagnostic_probe_error = allowed[i];
        /* Native-game context must still use pure OCR when configured so. */
        start_job("diagnostic ordering", 0);
        TEST_CHECK(diagnostic_paf_calls == 1 && diagnostic_pool_calls == 1);
        TEST_CHECK(diagnostic_order == 3 && paf_allocations == 1);
        TEST_CHECK(job_running && has_result_memory());
        TEST_CHECK(posted_events == NET_EV_JOB && !capture_calls);
        TEST_CHECK(!diagnostic_failure);
        start_job("running job refuses another probe", 0);
        TEST_CHECK(diagnostic_paf_calls == 1 && diagnostic_pool_calls == 1);
    }
}

static void test_diagnostic_failures_stick_without_allocation_or_ocr(void)
{
    const int errors[] = {VJO_MEMORY_PROBE_CLEANUP, VJO_MEMORY_PROBE_INVALID,
                          VJO_MEMORY_PROBE_CANARY, VJO_MEMORY_PROBE_BUSY};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); ++i) {
        setup(DIAGNOSTIC_CONFIG);
        remove_game_arenas();
        diagnostic_probe_error = errors[i];
        open_overlay();
        TEST_CHECK(diagnostic_failure == errors[i]);
        TEST_CHECK(diagnostic_paf_calls == 1 && diagnostic_pool_calls == 1);
        TEST_CHECK(strstr(g_view.status, "Memory diagnostic failed") != NULL);
        TEST_CHECK(!job_running && !posted_events && !has_result_memory());
        TEST_CHECK(!paf_allocations && !allocation_calls && !capture_calls && !bridge_starts);
        /* Even a later successful fake probe cannot clear the module's latch. */
        diagnostic_probe_error = VJO_MEMORY_PROBE_OK;
        close_overlay();
        open_overlay();
        TEST_CHECK(diagnostic_failure == errors[i]);
        TEST_CHECK(diagnostic_paf_calls == 1 && diagnostic_pool_calls == 1);
        TEST_CHECK(!job_running && !paf_allocations && !capture_calls && !bridge_starts);
        on_game_exit();
        strcpy(title_id, "PCSG00940");
        start_job("another game cannot clear diagnostic failure", 0);
        TEST_CHECK(diagnostic_failure == errors[i]);
        TEST_CHECK(diagnostic_pool_calls == 1 && !job_running && !paf_allocations);
    }
}

static void test_worker_stop_diagnostic_cleanup_failure_then_retry(void)
{
    setup_live_worker();
    diagnostic_release_error = VJO_MEMORY_PROBE_CLEANUP;
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(!running && !threads_started && thread_joins == 2);
    TEST_CHECK(diagnostic_release_calls == 1 && !anki_stops && bridge_finishes == 1);
    TEST_CHECK(!strcmp(lifecycle_events, "ncb"));
    check_shell_resources_retained();
    TEST_CHECK(vjo_worker_stop() < 0);
    TEST_CHECK(thread_joins == 2 && diagnostic_release_calls == 2 && !anki_stops);
    check_shell_resources_retained();
    diagnostic_release_error = VJO_MEMORY_PROBE_OK;
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(diagnostic_release_calls == 3 && thread_joins == 2 && anki_stops == 1);
    check_shell_resources_released();
    TEST_CHECK(!strcmp(lifecycle_events, "ncbbbaNCrempv"));
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(diagnostic_release_calls == 4 && thread_joins == 2);
    check_shell_resources_released();
}

static void test_diagnostic_startup_skips_anki_and_defers_probes(void)
{
    setup(DIAGNOSTIC_CONFIG);
    remove_game_arenas();
    startup_succeeds = 1;
    TEST_ASSERT(vjo_worker_start() == 0);
    TEST_CHECK(startup_mutexes == 3 && startup_events == 1);
    TEST_CHECK(startup_threads == 2 && startup_starts == 2 && threads_started);
    TEST_CHECK(!anki_starts && !anki_started);
    TEST_CHECK(!diagnostic_paf_calls && !diagnostic_pool_calls && !diagnostic_release_calls);
    TEST_CHECK(!job_running && !has_result_memory() && !paf_allocations);
    diagnostic_observe_idle = diagnostic_expect_no_results = 1;
    start_job("first manual Meiki action", 0);
    TEST_CHECK(diagnostic_order == 3 && diagnostic_paf_calls == 1 && diagnostic_pool_calls == 1);
    TEST_CHECK(job_running && !anki_starts && !anki_started);
    TEST_CHECK(vjo_worker_stop() == 0);
    TEST_CHECK(diagnostic_release_calls == 1 && thread_joins == 2);
    check_shell_resources_released();
}
#endif

TEST_LIST = {
#ifdef VJO_MEMORY_DIAGNOSTICS
    {"diagnostic_probes_only_for_idle_pure_meiki", test_diagnostic_probes_only_for_idle_pure_meiki},
    {"diagnostic_probes_precede_result_allocation_and_job", test_diagnostic_probes_precede_result_allocation_and_job},
    {"diagnostic_failures_stick_without_allocation_or_ocr", test_diagnostic_failures_stick_without_allocation_or_ocr},
    {"worker_stop_diagnostic_cleanup_failure_then_retry", test_worker_stop_diagnostic_cleanup_failure_then_retry},
    {"diagnostic_startup_skips_anki_and_defers_probes", test_diagnostic_startup_skips_anki_and_defers_probes},
#endif
    {"local_capture_keeps_multiple_passes", test_local_capture_keeps_multiple_passes},
    {"result_memory_reuse_and_job_gate", test_result_memory_reuse_and_job_gate},
    {"game_exit_frees_idle_result_memory", test_game_exit_frees_idle_result_memory},
    {"game_exit_during_job_frees_after_cancelled_completion", test_game_exit_during_job_frees_after_cancelled_completion},
    {"live_job_prevents_idle_cleanup", test_live_job_prevents_idle_cleanup},
    {"worker_stop_success_and_idempotence", test_worker_stop_success_and_idempotence},
    {"worker_stop_join_failure_then_retry", test_worker_stop_join_failure_then_retry},
    {"worker_stop_anki_failure_then_retry", test_worker_stop_anki_failure_then_retry},
#if defined(VJO_WITH_MEIKI) && !defined(VJO_MEIKI_GAME_WORKER)
    {"worker_stop_bridge_failure_then_retry", test_worker_stop_bridge_failure_then_retry},
    {"meiki_capture_without_lens", test_meiki_capture_without_lens},
    {"meiki_capture_errors_cleanup_without_fallback", test_meiki_capture_errors_cleanup_without_fallback},
#endif
#ifdef VJO_PAF_ALLOC
    {"paf_allocation_failure_refuses_job", test_paf_allocation_failure_refuses_job},
#endif
    {"ncnn_manual_and_no_fallback", test_ncnn_manual_and_no_fallback},
    {"meiki_manual_and_no_fallback", test_meiki_manual_and_no_fallback},
#ifdef VJO_MEIKI_GAME_WORKER
    {"game_meiki_dispatch_and_failure", test_game_meiki_dispatch_and_failure},
    {"game_meiki_model_access_error", test_game_meiki_model_access_error},
#endif
    {"meiki_layout_invalidates_cache", test_meiki_layout_invalidates_cache},
    {"ocr_context_changes", test_ocr_context_changes},
    {"legacy_source_settings_use_visual_ocr", test_legacy_source_settings_use_visual_ocr},
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
