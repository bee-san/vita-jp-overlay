/* Real bridge, including BearSSL model SHA validation. Only file/Paf/module
 * boundaries are replaced. Set VJO_MEIKI_TEST_MODEL to the pinned MNN file
 * for successful-load cases; it is deliberately never a committed fixture. */
#include "acutest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define sceClibMemcmp memcmp
#include "../../shell/meiki_bridge.c"

static VjoMeikiBridge bridge;
static unsigned opens, reads, closes, allocations, frees, loads, runs, stops, unloads;
static unsigned detects, detector_reads;
static int open_failed, read_failed, use_model, cancel_requested, cancel_after_reads;
static int allocation_fail_at, load_result, start_status, stop_result, unload_result, unload_status;
static int invalid_api, missing_stop, inference_result;
static int dialogue_mode, current_detector, detector_use_model, detector_open_failed;
static int detector_read_failed, cancel_after_detector_reads, missing_detect;
static uint64_t advertised_size;
static uint64_t advertised_detector_size;
static FILE *model_file;
static void *allocated[3];
static char events[64];
static unsigned n_events;

static void event(char value)
{
    TEST_ASSERT(n_events + 1 < sizeof(events));
    events[n_events++] = value;
    events[n_events] = 0;
}

void vjo_log(const char *fmt, ...) { (void)fmt; }
void vjo_paf_memory_report(void) {}
void *vjo_paf_alloc(size_t bytes)
{
    const size_t expected[] = {
        MEIKI_MODULE_METADATA_BYTES, MEIKI_WORKSPACE_BYTES,
        (dialogue_mode ? MEIKI_DETECT_ELEMENTS : MEIKI_PREPROCESS_ELEMENTS) * sizeof(float)
            + MEIKI_PREPROCESS_SCRATCH_BYTES
    };
    unsigned slot = allocations++;
    TEST_ASSERT(slot < 3);
    TEST_CHECK(bytes == expected[slot]);
    TEST_CHECK(closes == (dialogue_mode ? 2u : 1u)); /* all hashes precede allocation */
    event("MWP"[slot]);
    if ((int)allocations == allocation_fail_at) return NULL;
    TEST_ASSERT(posix_memalign(&allocated[slot], 64, bytes) == 0);
    return allocated[slot];
}
void vjo_paf_free(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 3; ++i) {
        if (pointer == allocated[i]) {
            event("mwp"[i]);
            free(pointer);
            allocated[i] = NULL;
            frees++;
            return;
        }
    }
    TEST_CHECK(0); /* unknown pointer or repeated free */
}

static int file_read(void *ctx, uint64_t offset, void *out, size_t size)
{
    (void)ctx;
    reads++;
    if (current_detector) detector_reads++;
    if (read_failed || (current_detector && detector_read_failed)) return -1;
    if (!(current_detector ? detector_use_model : use_model)) { memset(out, 0, size); return 0; }
    if (fseek(model_file, (long)offset, SEEK_SET)) return -1;
    return fread(out, 1, size, model_file) == size ? 0 : -1;
}
static void file_close(void *ctx)
{
    (void)ctx;
    closes++;
    if (model_file) { fclose(model_file); model_file = NULL; }
}
static int file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    opens++;
    current_detector = !strcmp(path, "ocr/" VJO_MEIKI_DETECT_MODEL_FILENAME);
    TEST_CHECK(current_detector || !strcmp(path, "ocr/" VJO_MEIKI_MODEL_FILENAME));
    if (current_detector) TEST_CHECK(dialogue_mode);
    if (open_failed || (current_detector && detector_open_failed)) return -1;
    if (current_detector ? detector_use_model : use_model) {
        model_file = fopen(getenv(current_detector ? "VJO_MEIKI_TEST_DETECT_MODEL" : "VJO_MEIKI_TEST_MODEL"), "rb");
        TEST_ASSERT(model_file != NULL);
    }
    *out = (VjoFile){NULL, current_detector ? advertised_detector_size : advertised_size,
                    file_read, file_close};
    return 0;
}
static VjoPlatform platform = {.file_open = file_open};
static int cancelled(void *ud)
{
    (void)ud;
    return cancel_requested || (cancel_after_reads && reads >= (unsigned)cancel_after_reads) ||
        (cancel_after_detector_reads && detector_reads >= (unsigned)cancel_after_detector_reads);
}

static int module_run(const MeikiModuleRequest *request, MeikiModuleStats *stats)
{
    event('R'); runs++;
    TEST_CHECK(request->size == sizeof(*request));
    TEST_CHECK(request->workspace == bridge.workspace && request->workspace_bytes == MEIKI_WORKSPACE_BYTES);
    TEST_CHECK(request->input == bridge.input && !strcmp(request->model_path, bridge.model_path));
    memset(request->output, 0, sizeof(*request->output));
    request->output->codes[0] = 0x732b;
    stats->runtime.heap_peak = 123456;
    stats->runtime.heap_remaining = 0;
    stats->runtime.allocation_failed = 0;
    return inference_result;
}
static int module_stop(MeikiModuleStats *stats)
{
    (void)stats;
    event('S'); stops++;
    TEST_CHECK(bridge.metadata && bridge.workspace && bridge.preprocessing);
    return stop_result;
}
static int module_detect(const MeikiModuleDetectRequest *request, MeikiModuleStats *stats)
{
    event('D'); detects++;
    TEST_CHECK(request->size == sizeof(*request));
    TEST_CHECK(request->workspace == bridge.workspace && request->workspace_bytes == MEIKI_WORKSPACE_BYTES);
    TEST_CHECK(request->input == bridge.input && !strcmp(request->model_path, bridge.detect_model_path));
    TEST_CHECK(request->target_width == 320 && request->target_height == 192);
    memset(request->output, 0, sizeof(*request->output));
    request->output->boxes[0][2] = 20; request->output->scores[0] = .9f;
    stats->runtime.heap_peak = 654321;
    stats->runtime.heap_remaining = 0; stats->runtime.allocation_failed = 0;
    return inference_result;
}
SceUID sceKernelLoadStartModule(const char *path, SceSize bytes, void *args,
                               int flags, SceKernelLMOption *option, int *status)
{
    (void)flags; (void)option;
    event('L'); loads++;
    TEST_CHECK(!strcmp(path, VJO_MEIKI_MODULE_PATH));
    TEST_CHECK(bytes == sizeof(MeikiModuleStart));
    MeikiModuleStart *start = args;
    TEST_CHECK(start->size == sizeof(*start) && start->magic == MEIKI_MODULE_MAGIC);
    TEST_CHECK(start->abi == MEIKI_MODULE_ABI);
    TEST_CHECK(start->metadata_heap == bridge.metadata && start->metadata_bytes == MEIKI_MODULE_METADATA_BYTES);
    TEST_CHECK(start->api_out == &bridge.api && start->initial_stats == &bridge.module_stats);
    TEST_CHECK(closes == (dialogue_mode ? 2u : 1u)); /* hashes finished before loading */
    *start->api_out = (MeikiModuleApi){sizeof(MeikiModuleApi),
                                     invalid_api ? MEIKI_MODULE_ABI + 1 : MEIKI_MODULE_ABI,
                                     module_run, missing_stop ? NULL : module_stop,
                                     missing_detect ? NULL : module_detect};
    *status = start_status;
    return load_result;
}
int sceKernelStopUnloadModule(SceUID uid, SceSize bytes, void *args,
                              int flags, SceKernelULMOption *option, int *status)
{
    (void)bytes; (void)args; (void)flags; (void)option;
    event('U'); unloads++;
    TEST_CHECK(uid == load_result && bridge.stopped);
    TEST_CHECK(bridge.metadata && bridge.workspace && bridge.preprocessing);
    *status = unload_status;
    return unload_result;
}

static int have_model(void)
{
    if (!getenv("VJO_MEIKI_TEST_MODEL")) {
        TEST_SKIP("Set VJO_MEIKI_TEST_MODEL to run pinned-model success/lifetime checks");
        return 0;
    }
    return 1;
}
static void setup(int valid_model)
{
    memset(&bridge, 0, sizeof(bridge));
    bridge.module = -1;
    opens = reads = closes = allocations = frees = loads = runs = stops = unloads = 0;
    open_failed = read_failed = cancel_requested = cancel_after_reads = 0;
    allocation_fail_at = start_status = stop_result = unload_result = unload_status = 0;
    invalid_api = missing_stop = inference_result = 0;
    load_result = 42;
    advertised_size = MODEL_BYTES;
    use_model = valid_model;
    model_file = NULL;
    dialogue_mode = current_detector = detector_open_failed = detector_read_failed = 0;
    cancel_after_detector_reads = missing_detect = 0;
    detects = detector_reads = 0;
    detector_use_model = valid_model;
    advertised_detector_size = DETECT_MODEL_BYTES;
    memset(allocated, 0, sizeof(allocated));
    events[0] = 0; n_events = 0;
}
static int start(void)
{
    return vjo_meiki_bridge_start(&bridge, &platform, "ocr", cancelled, NULL);
}
static int start_dialogue(void)
{
    dialogue_mode = 1;
    return vjo_meiki_bridge_start_mode(&bridge, &platform, "ocr", 1, cancelled, NULL);
}
static int have_detector(void)
{
    if (!have_model()) return 0;
    if (!getenv("VJO_MEIKI_TEST_DETECT_MODEL")) {
        TEST_SKIP("Set VJO_MEIKI_TEST_DETECT_MODEL to run pinned-detector bridge checks");
        return 0;
    }
    return 1;
}
static void check_empty(void)
{
    TEST_CHECK(!bridge.metadata && !bridge.workspace && !bridge.preprocessing);
    TEST_CHECK(!bridge.input && !bridge.scratch && bridge.module < 0);
}

static void test_bad_size_hash_and_io_allocate_nothing(void)
{
    setup(0); advertised_size--;
    TEST_CHECK(start() == VJO_E_OCR_MODEL);
    TEST_CHECK(opens == 1 && closes == 1 && reads == 0 && allocations == 0 && loads == 0);
    check_empty();
    setup(0); /* correct length, deliberately wrong real SHA-256 */
    TEST_CHECK(start() == VJO_E_OCR_MODEL);
    TEST_CHECK(reads > 0 && closes == 1 && allocations == 0 && loads == 0);
    check_empty();
    setup(0); read_failed = 1;
    TEST_CHECK(start() == VJO_E_OCR_MODEL);
    TEST_CHECK(reads == 1 && closes == 1 && allocations == 0);
    setup(0); open_failed = 1;
    TEST_CHECK(start() == VJO_E_OCR_MODEL);
    TEST_CHECK(opens == 1 && closes == 0 && allocations == 0);
}

static void test_cancelled_validation_allocates_nothing(void)
{
    setup(0); cancel_requested = 1;
    TEST_CHECK(start() == VJO_E_CANCELLED);
    TEST_CHECK(reads == 0 && closes == 1 && allocations == 0 && loads == 0);
    setup(0); cancel_after_reads = 1;
    TEST_CHECK(start() == VJO_E_CANCELLED);
    TEST_CHECK(reads == 1 && closes == 1 && allocations == 0 && loads == 0);
}

static void test_partial_allocation_failure_releases_every_buffer(void)
{
    if (!have_model()) return;
    for (int fail = 1; fail <= 3; ++fail) {
        setup(1); allocation_fail_at = fail;
        TEST_CHECK(start() == VJO_E_OOM);
        TEST_CHECK(allocations >= (unsigned)fail && frees == allocations - 1 && loads == 0);
        check_empty();
    }
}

static void test_run_stop_unload_free_order(void)
{
    if (!have_model()) return;
    setup(1);
    TEST_ASSERT(start() == VJO_OK);
    TEST_CHECK(!strcmp(events, "MWPL"));
    TEST_CHECK(bridge.scratch == (unsigned char *)bridge.input + MEIKI_INPUT_ELEMENTS * sizeof(float));
    VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
    MeikiOutput output; MeikiStats stats = {0};
    TEST_CHECK(engine.run(engine.ud, bridge.model_path, bridge.input, &output, &stats) == VJO_OK);
    TEST_CHECK(output.codes[0] == 0x732b && stats.heap_peak == 123456 && runs == 1);
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
    TEST_CHECK(!strcmp(events, "MWPLRSUpwm"));
    TEST_CHECK(stops == 1 && unloads == 1 && frees == 3);
    check_empty();
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK && frees == 3);
}

static void test_stop_refusal_keeps_buffers_and_prevents_reload(void)
{
    if (!have_model()) return;
    setup(1); TEST_ASSERT(start() == VJO_OK);
    stop_result = MEIKI_MODULE_BUSY;
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(stops == 1 && !unloads && !frees && bridge.module == 42 && !bridge.stopped);
    TEST_CHECK(bridge.metadata && bridge.workspace && bridge.preprocessing);
    TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE && loads == 1 && allocations == 3 && opens == 1);
    stop_result = 0;
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
    TEST_CHECK(stops == 2 && unloads == 1 && frees == 3);
    check_empty();
}

static void test_unload_refusal_keeps_stopped_engine_buffers(void)
{
    if (!have_model()) return;
    for (int failure_kind = 0; failure_kind < 3; ++failure_kind) {
        setup(1); TEST_ASSERT(start() == VJO_OK);
        if (failure_kind == 0) unload_result = -1;
        else unload_status = failure_kind == 1 ? -1 : SCE_KERNEL_STOP_FAIL;
        TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_E_OCR_UNAVAILABLE);
        TEST_CHECK(stops == 1 && unloads == 1 && !frees && bridge.stopped && bridge.module == 42);
        TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE && loads == 1 && allocations == 3);
        VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
        MeikiOutput output; MeikiStats stats = {0};
        TEST_CHECK(engine.run(engine.ud, bridge.model_path, bridge.input, &output, &stats) == VJO_E_OCR_UNAVAILABLE);
        TEST_CHECK(runs == 0);
        unload_result = unload_status = 0;
        TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
        TEST_CHECK(stops == 1 && unloads == 2 && frees == 3); /* no second stop */
        check_empty();
    }
}

static void test_load_failure_and_incomplete_api(void)
{
    if (!have_model()) return;
    setup(1); load_result = -1;
    TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE && frees == 3);
    TEST_CHECK(!stops && !unloads); check_empty();
    const int failures[] = {-1, SCE_KERNEL_START_NO_RESIDENT, SCE_KERNEL_START_FAILED};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        setup(1); start_status = failures[i];
        TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE);
        /* Pinned module start failures have already joined their worker. */
        TEST_CHECK(stops == 0 && unloads == 1 && frees == 3); check_empty();
    }
    setup(1); missing_stop = 1;
    TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(bridge.module == 42 && bridge.metadata && !stops && !unloads && !frees);
    TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE && loads == 1);
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_E_OCR_UNAVAILABLE && !frees);
    /* Supply the fake's known stop only to let this host test reclaim memory. */
    bridge.api.stop = module_stop;
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
    setup(1); invalid_api = 1;
    TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(bridge.module == 42 && bridge.metadata && !stops && !unloads && !frees);
    TEST_CHECK(start() == VJO_E_OCR_UNAVAILABLE && loads == 1);
    /* The fake supplies its known ABI/stop only to reclaim this test's memory. */
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!stops && !unloads && !frees && bridge.metadata);
    bridge.api.abi = MEIKI_MODULE_ABI;
    bridge.api.stop = module_stop;
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
}

static void test_model_path_mismatch_never_enters_engine(void)
{
    if (!have_model()) return;
    setup(1); TEST_ASSERT(start() == VJO_OK);
    VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
    MeikiOutput output; MeikiStats stats = {0};
    TEST_CHECK(engine.run(engine.ud, "another/model.mnn", bridge.input, &output, &stats) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!runs);
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
}

static void test_dialogue_second_model_is_validated_before_allocation(void)
{
    if (!have_model()) return;
    for (int failure = 0; failure < 4; ++failure) {
        setup(1); detector_use_model = 0;
        if (failure == 0) advertised_detector_size--;
        else if (failure == 2) detector_read_failed = 1;
        else if (failure == 3) detector_open_failed = 1;
        TEST_CHECK(start_dialogue() == VJO_E_OCR_MODEL);
        TEST_CHECK(opens == 2 && !allocations && !loads);
        TEST_CHECK(closes == (failure == 3 ? 1u : 2u));
        if (failure == 0) TEST_CHECK(!detector_reads);
        else if (failure == 1) TEST_CHECK(detector_reads > 1); /* real SHA rejection */
        else if (failure == 2) TEST_CHECK(detector_reads == 1);
        check_empty();
    }
}

static void test_dialogue_cancelled_detector_validation_allocates_nothing(void)
{
    if (!have_model()) return;
    setup(1); detector_use_model = 0; cancel_after_detector_reads = 1;
    TEST_CHECK(start_dialogue() == VJO_E_CANCELLED);
    TEST_CHECK(opens == 2 && closes == 2 && detector_reads == 1 && !allocations && !loads);
    check_empty();
}

static void test_dialogue_partial_allocation_failure_releases_every_buffer(void)
{
    if (!have_detector()) return;
    for (int fail = 1; fail <= 3; ++fail) {
        setup(1); allocation_fail_at = fail;
        TEST_CHECK(start_dialogue() == VJO_E_OOM);
        TEST_CHECK(closes == 2 && frees == allocations - 1 && !loads);
        check_empty();
    }
}

static void test_dialogue_detect_run_and_cleanup_share_workspace(void)
{
    if (!have_detector()) return;
    setup(1); TEST_ASSERT(start_dialogue() == VJO_OK);
    TEST_CHECK(bridge.input_elements == MEIKI_DETECT_ELEMENTS);
    TEST_CHECK(bridge.input_elements * sizeof(float) + MEIKI_PREPROCESS_SCRATCH_BYTES == 750720);
    TEST_CHECK(bridge.scratch == (unsigned char *)bridge.input + 737280);
    VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
    MeikiDetectOutput detected; MeikiOutput output; MeikiStats stats = {0};
    TEST_CHECK(engine.detect(engine.ud, bridge.detect_model_path, bridge.input, 320, 192,
                             &detected, &stats) == VJO_OK);
    TEST_CHECK(detects == 1 && stats.heap_peak == 654321 && detected.scores[0] == .9f);
    TEST_CHECK(engine.run(engine.ud, bridge.model_path, bridge.input, &output, &stats) == VJO_OK);
    TEST_CHECK(runs == 1 && stats.heap_peak == 123456);
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
    TEST_CHECK(!strcmp(events, "MWPLDRSUpwm") && frees == 3);
    check_empty();
}

static void test_dialogue_missing_detect_api_and_wrong_path_refuse_calls(void)
{
    if (!have_detector()) return;
    setup(1); missing_detect = 1;
    TEST_CHECK(start_dialogue() == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(stops == 1 && unloads == 1 && frees == 3 && !detects && !runs);
    check_empty();
    setup(1); TEST_ASSERT(start_dialogue() == VJO_OK);
    VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
    MeikiDetectOutput output; MeikiStats stats = {0};
    TEST_CHECK(engine.detect(engine.ud, "other/detector.mnn", bridge.input, 320, 192,
                             &output, &stats) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!detects);
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
    setup(1); TEST_ASSERT(start() == VJO_OK); /* single-line wrapper never loads detector */
    engine = vjo_meiki_bridge_engine(&bridge);
    TEST_CHECK(engine.detect(engine.ud, "ocr/" VJO_MEIKI_DETECT_MODEL_FILENAME, bridge.input,
                             320, 192, &output, &stats) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(opens == 1 && !detects);
    TEST_CHECK(vjo_meiki_bridge_finish(&bridge) == VJO_OK);
}

TEST_LIST = {
    {"bad_size_hash_and_io_allocate_nothing", test_bad_size_hash_and_io_allocate_nothing},
    {"cancelled_validation_allocates_nothing", test_cancelled_validation_allocates_nothing},
    {"partial_allocation_failure_releases_every_buffer", test_partial_allocation_failure_releases_every_buffer},
    {"run_stop_unload_free_order", test_run_stop_unload_free_order},
    {"stop_refusal_keeps_buffers_and_prevents_reload", test_stop_refusal_keeps_buffers_and_prevents_reload},
    {"unload_refusal_keeps_stopped_engine_buffers", test_unload_refusal_keeps_stopped_engine_buffers},
    {"load_failure_and_incomplete_api", test_load_failure_and_incomplete_api},
    {"model_path_mismatch_never_enters_engine", test_model_path_mismatch_never_enters_engine},
    {"dialogue_second_model_is_validated_before_allocation", test_dialogue_second_model_is_validated_before_allocation},
    {"dialogue_cancelled_detector_validation_allocates_nothing", test_dialogue_cancelled_detector_validation_allocates_nothing},
    {"dialogue_partial_allocation_failure_releases_every_buffer", test_dialogue_partial_allocation_failure_releases_every_buffer},
    {"dialogue_detect_run_and_cleanup_share_workspace", test_dialogue_detect_run_and_cleanup_share_workspace},
    {"dialogue_missing_detect_api_and_wrong_path_refuse_calls", test_dialogue_missing_detect_api_and_wrong_path_refuse_calls},
    {NULL, NULL}
};
