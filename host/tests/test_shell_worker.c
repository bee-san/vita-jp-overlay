/* Exercise worker.c's actual manual/background gates, not a copy of them.
 * Only Vita I/O is stubbed. Config, error rendering and relay lookup are real. */
#include "acutest.h"
#include "client.h"
#include "replay.h"
#include <psp2/host_stubs.h>

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
int vjoRequestCapture(uint32_t flags) { return VJO_ERR_NO_GAME; }
int vjoReadRaw(uint32_t row, uint32_t n, void *dst) { return -1; }

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
    running = 1;
    region_selected = job_region_selected = 0;
    capture_generation = job_generation = 0;
    memset(&g_view, 0, sizeof(g_view));
    memset(cache_data, 0, sizeof(cache_data));
    memset(&ocr_backoff, 0, sizeof(ocr_backoff));
    memset(&dict_backoff, 0, sizeof(dict_backoff));
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

TEST_LIST = {
    {"ncnn_manual_and_no_fallback", test_ncnn_manual_and_no_fallback},
    {"ocr_context_changes", test_ocr_context_changes},
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
