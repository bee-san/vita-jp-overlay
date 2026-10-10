/* Actual C adapter/line finder/preprocessing/decoder, with inference injected
 * at the engine boundary. Real MNN is exercised separately by vjo-meiki-ocr. */
#include "acutest.h"
#include "meiki_ocr.h"
#include "client.h"
#include "config.h"
#include <math.h>
#include <string.h>

static unsigned char pixels[100 * 100 * 4];
static float input[MEIKI_DETECT_ELEMENTS];
static uint16_t scratch[(MEIKI_PREPROCESS_SCRATCH_BYTES + 1) / 2];
static unsigned engine_calls, row_reads;
static int cancel, cancel_in_engine, cancel_after_rows, fail_after_rows, fail_engine_call, engine_rc;
static int allocation_failed;
static size_t retained;
static char engine_path[256];
static MeikiDetectOutput detections;
static unsigned detector_calls;
static int detector_rc, cancel_in_detection, detector_allocation_failed;
static size_t detector_retained;

static int rows(void *ud, unsigned y, unsigned n, unsigned char *out)
{
    (void)ud;
    row_reads++;
    if ((fail_after_rows && row_reads > (unsigned)fail_after_rows) ||
        y > 100 || n > 100 - y) return -1;
    memcpy(out, pixels + (size_t)y * 100 * 4, (size_t)n * 100 * 4);
    if (cancel_after_rows && row_reads >= (unsigned)cancel_after_rows) cancel = 1;
    return (int)n;
}
static int cancelled(void *ud) { (void)ud; return cancel; }
static VjoOcrImage image = {NULL, 100, 100, rows, cancelled};

static int infer(void *ud, const char *path, const float *tensor,
                 MeikiOutput *out, MeikiStats *stats)
{
    (void)ud;
    engine_calls++;
    TEST_CHECK(tensor == input);
    TEST_CHECK(!strcmp(path, "fixtures/ocr/" VJO_MEIKI_MODEL_FILENAME));
    strcpy(engine_path, path);
    memset(out, 0, sizeof(*out));
    out->codes[0] = engine_calls == 1 ? 0x732b : 0x4eba; /* 猫, 人 */
    out->boxes[2] = 10.0f;
    out->scores[0] = .9f;
    memset(stats, 0, sizeof(*stats));
    stats->heap_peak = engine_calls * 100;
    stats->heap_remaining = retained;
    stats->load_us = engine_calls * 10;
    stats->inference_us = engine_calls * 20;
    stats->allocation_failed = allocation_failed;
    if (cancel_in_engine) cancel = 1;
    return engine_calls == (unsigned)fail_engine_call ? engine_rc : VJO_OK;
}
static int detect(void *ud, const char *path, const float *tensor,
                   int32_t target_width, int32_t target_height,
                   MeikiDetectOutput *out, MeikiStats *stats)
{
    (void)ud;
    detector_calls++;
    TEST_CHECK(tensor == input);
    TEST_CHECK(!strcmp(path, "fixtures/ocr/" VJO_MEIKI_DETECT_MODEL_FILENAME));
    TEST_CHECK(target_width == 166 && target_height == 100);
    *out = detections;
    memset(stats, 0, sizeof(*stats));
    stats->heap_peak = 400;
    stats->heap_remaining = detector_retained;
    stats->load_us = 7; stats->inference_us = 11;
    stats->allocation_failed = detector_allocation_failed;
    if (cancel_in_detection) cancel = 1;
    return detector_rc;
}
static VjoMeikiEngine engine = {NULL, infer, detect};

static void setup(void)
{
    memset(pixels, 0, sizeof(pixels));
    engine_calls = row_reads = 0;
    cancel = cancel_in_engine = cancel_after_rows = fail_after_rows = fail_engine_call = engine_rc = 0;
    allocation_failed = 0;
    retained = 0;
    engine_path[0] = 0;
    memset(&detections, 0, sizeof(detections));
    detector_calls = 0;
    detector_rc = cancel_in_detection = detector_allocation_failed = 0;
    detector_retained = 0;
}
static void ink(unsigned y)
{
    for (unsigned yy = y; yy < y + 12; ++yy)
        for (unsigned x = 10; x < 60; x += 4)
            memset(pixels + (yy * 100 + x) * 4, 255, 3);
}
static int run(char *text, size_t cap, VjoMeikiStats *stats)
{
    return vjo_meiki_ocr("fixtures/ocr", &image, &engine,
                         input, MEIKI_PREPROCESS_ELEMENTS,
                         scratch, sizeof(scratch), text, cap, stats);
}
static int run_single(char *text, size_t cap, VjoMeikiStats *stats)
{
    return vjo_meiki_ocr_single_line("fixtures/ocr", &image, &engine,
                                    input, MEIKI_PREPROCESS_ELEMENTS,
                                    scratch, sizeof(scratch), text, cap, stats);
}
static int run_detected(char *text, size_t cap, VjoMeikiStats *stats)
{
    return vjo_meiki_ocr_detected("fixtures/ocr", &image, &engine,
                                 input, MEIKI_DETECT_ELEMENTS,
                                 scratch, sizeof(scratch), text, cap, stats);
}
static void detection(unsigned i, float x1, float y1, float x2, float y2, float score)
{
    detections.boxes[i][0] = x1; detections.boxes[i][1] = y1;
    detections.boxes[i][2] = x2; detections.boxes[i][3] = y2;
    detections.scores[i] = score;
}

static void test_config_backends_and_no_jpeg_upload(void)
{
    const char *names[] = {"lens", "ncnn", "meiki", "MeIkI"};
    const int values[] = {VJO_OCR_LENS, VJO_OCR_NCNN, VJO_OCR_MEIKI, VJO_OCR_MEIKI};
    VjoConfig cfg;
    vjo_config_defaults(&cfg);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.text_source == VJO_SOURCE_OCR);
    TEST_CHECK(cfg.meiki_layout == VJO_MEIKI_SINGLE_LINE);
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        char config[80];
        int n = snprintf(config, sizeof(config), "ocr_backend = %s\nocr_mode = auto\n", names[i]);
        vjo_config_defaults(&cfg);
        vjo_config_parse(&cfg, config, (size_t)n);
        TEST_CHECK(cfg.ocr_backend == values[i] && cfg.n_warnings == 0);
        if (cfg.ocr_backend != VJO_OCR_LENS) {
            VjoOverlayData out;
            TEST_CHECK(vjo_overlay_ocr(NULL, NULL, &cfg, NULL, &out) == VJO_E_OCR_UNAVAILABLE);
            TEST_CHECK(out.failed_stage == VJO_STAGE_OCR);
            TEST_CHECK(strstr(out.err.detail, "raw pixels") != NULL);
            if (cfg.ocr_backend == VJO_OCR_MEIKI)
                TEST_CHECK(strstr(out.err.detail, "vjo-meiki-ocr") != NULL);
        }
    }
    vjo_config_defaults(&cfg);
    const char invalid[] = "ocr_backend = unknown\n";
    vjo_config_parse(&cfg, invalid, sizeof(invalid) - 1);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.n_warnings == 1);
    const char box[] = "meiki_layout = DiAlOgUe_BoX\n";
    vjo_config_defaults(&cfg); vjo_config_parse(&cfg, box, sizeof(box) - 1);
    TEST_CHECK(cfg.meiki_layout == VJO_MEIKI_DIALOGUE_BOX && !cfg.n_warnings);
    TEST_CHECK(cfg.ocr_backend == VJO_OCR_MEIKI && cfg.ocr_mode == VJO_OCR_AUTO);
    const char single[] = "meiki_layout = SINGLE_LINE\n";
    vjo_config_parse(&cfg, single, sizeof(single) - 1);
    TEST_CHECK(cfg.meiki_layout == VJO_MEIKI_SINGLE_LINE && !cfg.n_warnings);
    const char bad_layout[] = "meiki_layout = unsupported\n";
    vjo_config_parse(&cfg, bad_layout, sizeof(bad_layout) - 1);
    TEST_CHECK(cfg.meiki_layout == VJO_MEIKI_SINGLE_LINE && cfg.n_warnings == 1);
}

static void test_multiline_decode_and_aggregate_stats(void)
{
    setup(); ink(10); ink(40);
    char text[4096]; VjoMeikiStats stats;
    TEST_ASSERT(run(text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫\n人"));
    TEST_CHECK(engine_calls == 2 && stats.lines == 2 && stats.completed_lines == 2);
    TEST_CHECK(stats.heap_peak == 200 && stats.heap_remaining == 0);
    TEST_CHECK(stats.load_us == 30 && stats.inference_us == 60 && !stats.allocation_failed);
}

static void test_blank_and_unsupported_regions(void)
{
    setup(); char text[20] = "old"; VjoMeikiStats stats;
    TEST_CHECK(vjo_meiki_ocr("fixtures/ocr", &image, &engine,
                              NULL, 0, NULL, 0, text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!text[0] && !engine_calls && !stats.lines);
    memset(pixels, 255, sizeof(pixels));
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_OCR_REGION);
    TEST_CHECK(!text[0] && !engine_calls);
}

static void test_cancel_before_and_after_inference(void)
{
    setup(); ink(10); ink(40); char text[20]; VjoMeikiStats stats;
    cancel = 1;
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && !engine_calls);
    cancel = 0; cancel_in_engine = 1;
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && engine_calls == 1 && stats.completed_lines == 0);
    TEST_CHECK(stats.heap_peak == 100 && stats.heap_remaining == 0);
}

static void test_source_failure_during_preprocessing(void)
{
    setup(); ink(10); char text[20]; VjoMeikiStats stats;
    fail_after_rows = 100; /* segmentation succeeds; first resize read fails */
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_SOURCE);
    TEST_CHECK(!text[0] && !engine_calls && stats.lines == 1);
}

static void test_engine_failure_clears_partial_text(void)
{
    setup(); ink(10); ink(40); char text[20]; VjoMeikiStats stats;
    fail_engine_call = 2; engine_rc = VJO_E_OCR_MODEL;
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_OCR_MODEL);
    TEST_CHECK(!text[0] && engine_calls == 2 && stats.completed_lines == 1);
}

static void test_memory_failure_or_retained_pool_refuses_result(void)
{
    setup(); ink(10); char text[20]; VjoMeikiStats stats;
    allocation_failed = 1;
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && stats.allocation_failed);
    setup(); ink(10); retained = 64;
    TEST_CHECK(run(text, sizeof(text), &stats) == VJO_E_OCR_INFERENCE);
    TEST_CHECK(!text[0] && stats.heap_remaining == 64);
}

static void test_utf8_capacity_clears_partial_text(void)
{
    setup(); ink(10); ink(40); char text[10]; VjoMeikiStats stats;
    memset(text, 0x5a, sizeof(text));
    TEST_CHECK(run(text, 4, &stats) == VJO_E_TOO_LARGE);
    TEST_CHECK(!text[0] && engine_calls == 2 && stats.completed_lines == 1);
    TEST_CHECK((unsigned char)text[4] == 0x5a);
    setup(); ink(10);
    TEST_CHECK(run(text, 3, &stats) == VJO_E_TOO_LARGE);
    TEST_CHECK(!text[0] && stats.completed_lines == 0);
}

static void test_missing_engine_and_short_buffers(void)
{
    setup(); ink(10); char text[20] = "old"; VjoMeikiStats stats;
    TEST_CHECK(vjo_meiki_ocr("fixtures/ocr", &image, NULL, input, MEIKI_PREPROCESS_ELEMENTS,
                              scratch, sizeof(scratch), text, sizeof(text), &stats) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!text[0] && !engine_calls);
    TEST_CHECK(vjo_meiki_ocr("fixtures/ocr", &image, &engine, input, MEIKI_PREPROCESS_ELEMENTS - 1,
                              scratch, sizeof(scratch), text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && !engine_calls);
    TEST_CHECK(vjo_meiki_ocr("fixtures/ocr", &image, &engine, input, MEIKI_PREPROCESS_ELEMENTS,
                              scratch, sizeof(scratch) - 1, text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && !engine_calls);
}

static void test_single_line_accepts_dim_and_light_background(void)
{
    char text[20]; VjoMeikiStats stats;
    setup();
    memset(pixels, 60, sizeof(pixels)); /* below projection's white threshold */
    TEST_ASSERT(run_single(text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫") && engine_calls == 1);
    TEST_CHECK(stats.lines == 1 && stats.completed_lines == 1);
    TEST_CHECK(input[0] > .23f && input[0] < .24f);
    setup();
    memset(pixels, 255, sizeof(pixels));
    for (unsigned y = 10; y < 22; ++y)
        for (unsigned x = 10; x < 60; x += 4)
            memset(pixels + (y * 100 + x) * 4, 0, 3);
    TEST_ASSERT(run_single(text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫") && engine_calls == 1);
    TEST_CHECK(input[0] == 1.0f);
}

static void test_single_line_source_and_cancellation(void)
{
    char text[20] = "old"; VjoMeikiStats stats;
    setup(); fail_after_rows = 1;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_SOURCE);
    TEST_CHECK(!text[0] && !engine_calls && stats.lines == 1);
    setup(); cancel = 1;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && !row_reads && !engine_calls);
    setup(); cancel_after_rows = 1;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && row_reads && !engine_calls);
    setup(); cancel_in_engine = 1;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && engine_calls == 1 && !stats.completed_lines);
    TEST_CHECK(stats.heap_peak == 100 && !stats.heap_remaining);
}

static void test_single_line_utf8_and_retained_pool(void)
{
    char text[10]; VjoMeikiStats stats;
    setup(); memset(text, 0x5a, sizeof(text));
    TEST_CHECK(run_single(text, 4, &stats) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫") && (unsigned char)text[4] == 0x5a);
    setup(); memset(text, 0x5a, sizeof(text));
    TEST_CHECK(run_single(text, 3, &stats) == VJO_E_TOO_LARGE);
    TEST_CHECK(!text[0] && !stats.completed_lines);
    TEST_CHECK((unsigned char)text[3] == 0x5a);
    setup(); retained = 64;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_OCR_INFERENCE);
    TEST_CHECK(!text[0] && stats.heap_remaining == 64);
    setup(); retained = 64; fail_engine_call = 1; engine_rc = VJO_E_OOM;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && stats.heap_remaining == 64);
    setup(); allocation_failed = 1;
    TEST_CHECK(run_single(text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && stats.allocation_failed);
}

static void test_single_line_invalid_image(void)
{
    char text[20] = "old"; VjoMeikiStats stats;
    setup(); VjoOcrImage invalid = image;
    invalid.width = VJO_OCR_MAX_WIDTH + 1;
    TEST_CHECK(vjo_meiki_ocr_single_line("fixtures/ocr", &invalid, &engine,
        input, MEIKI_PREPROCESS_ELEMENTS, scratch, sizeof(scratch),
        text, sizeof(text), &stats) == VJO_E_OCR_REGION);
    TEST_CHECK(!text[0] && !row_reads && !engine_calls);
    invalid = image; invalid.rows = NULL;
    TEST_CHECK(vjo_meiki_ocr_single_line("fixtures/ocr", &invalid, &engine,
        input, MEIKI_PREPROCESS_ELEMENTS, scratch, sizeof(scratch),
        text, sizeof(text), &stats) == VJO_E_OCR_REGION);
    TEST_CHECK(!text[0] && !row_reads && !engine_calls);
}

static void test_detector_filters_clamps_sorts_and_reuses_input(void)
{
    setup(); char text[30]; VjoMeikiStats stats;
    detection(0, 10.8f, 50.7f, 60.9f, 70.9f, .9f);
    detection(1, -4.0f, 5.2f, 120.0f, 25.6f, .8f);
    detection(2, 20, 30, 80, 40, .5f); /* strict > .5 */
    detection(3, 80, 30, 70, 40, .9f); /* nonpositive crop */
    TEST_ASSERT(run_detected(text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫\n人") && detector_calls == 1 && engine_calls == 2);
    TEST_CHECK(stats.lines == 2 && stats.completed_lines == 2);
    TEST_CHECK(stats.heap_peak == 400 && !stats.heap_remaining);
    TEST_CHECK(stats.load_us == 37 && stats.inference_us == 71);
    TEST_CHECK(stats.line_boxes.count == 2);
    TEST_CHECK(stats.line_boxes.lines[0].x == 0 && stats.line_boxes.lines[0].y == 5);
    TEST_CHECK(stats.line_boxes.lines[0].w == 100 && stats.line_boxes.lines[0].h == 20);
    TEST_CHECK(stats.line_boxes.lines[1].x == 10 && stats.line_boxes.lines[1].w == 50);
}

static void test_detector_empty_tall_and_line_limit(void)
{
    setup(); char text[30] = "old"; VjoMeikiStats stats;
    TEST_ASSERT(run_detected(text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!text[0] && detector_calls == 1 && !engine_calls && !stats.lines);
    TEST_CHECK(stats.heap_peak == 400);
    setup(); detection(0, 10, 10, 20, 30, .9f); /* tall single glyph */
    TEST_ASSERT(run_detected(text, sizeof(text), &stats) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫") && engine_calls == 1);
    setup();
    for (unsigned i = 0; i < 9; ++i) detection(i, 0, i * 10, 100, i * 10 + 8, .9f);
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OCR_REGION);
    TEST_CHECK(!text[0] && detector_calls == 1 && !engine_calls && !stats.completed_lines);
}

static void test_detector_source_cancel_and_short_buffers(void)
{
    setup(); char text[30]; VjoMeikiStats stats;
    fail_after_rows = 1;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_SOURCE);
    TEST_CHECK(!text[0] && !detector_calls && !engine_calls);
    setup(); cancel_after_rows = 1;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && !detector_calls);
    setup(); cancel_in_detection = 1;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_CANCELLED);
    TEST_CHECK(!text[0] && detector_calls == 1 && !engine_calls && stats.heap_peak == 400);
    setup();
    TEST_CHECK(vjo_meiki_ocr_detected("fixtures/ocr", &image, &engine, input,
        MEIKI_DETECT_ELEMENTS - 1, scratch, sizeof(scratch), text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && !row_reads && !detector_calls);
    VjoMeikiEngine missing = {NULL, infer, NULL};
    TEST_CHECK(vjo_meiki_ocr_detected("fixtures/ocr", &image, &missing, input,
        MEIKI_DETECT_ELEMENTS, scratch, sizeof(scratch), text, sizeof(text), &stats) == VJO_E_OCR_UNAVAILABLE);
    TEST_CHECK(!text[0] && !detector_calls);
}

static void test_detector_failure_retention_and_invalid_coordinates(void)
{
    setup(); char text[30]; VjoMeikiStats stats;
    detector_rc = VJO_E_OCR_MODEL;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OCR_MODEL);
    TEST_CHECK(!text[0] && !engine_calls);
    setup(); detector_retained = 64;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OCR_INFERENCE);
    TEST_CHECK(!text[0] && stats.heap_remaining == 64 && !engine_calls);
    setup(); detector_allocation_failed = 1;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0] && stats.allocation_failed && !engine_calls);
    setup(); detection(0, 0, 0, NAN, 20, .9f);
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OCR_INFERENCE);
    TEST_CHECK(!text[0] && !engine_calls);
    setup(); detections.scores[0] = INFINITY;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OCR_INFERENCE);
    TEST_CHECK(!text[0] && !engine_calls);
}

static void test_detector_recognition_failure_clears_partial_utf8(void)
{
    setup(); char text[20]; VjoMeikiStats stats;
    detection(0, 0, 0, 100, 20, .9f); detection(1, 0, 30, 100, 50, .9f);
    fail_engine_call = 2; engine_rc = VJO_E_OCR_MODEL;
    TEST_CHECK(run_detected(text, sizeof(text), &stats) == VJO_E_OCR_MODEL);
    TEST_CHECK(!text[0] && stats.completed_lines == 1 && engine_calls == 2);
    setup(); detection(0, 0, 0, 100, 20, .9f); detection(1, 0, 30, 100, 50, .9f);
    memset(text, 0x5a, sizeof(text));
    TEST_CHECK(run_detected(text, 4, &stats) == VJO_E_TOO_LARGE);
    TEST_CHECK(!text[0] && (unsigned char)text[4] == 0x5a && stats.completed_lines == 1);
}

TEST_LIST = {
    {"config_backends_and_no_jpeg_upload", test_config_backends_and_no_jpeg_upload},
    {"multiline_decode_and_aggregate_stats", test_multiline_decode_and_aggregate_stats},
    {"blank_and_unsupported_regions", test_blank_and_unsupported_regions},
    {"cancel_before_and_after_inference", test_cancel_before_and_after_inference},
    {"source_failure_during_preprocessing", test_source_failure_during_preprocessing},
    {"engine_failure_clears_partial_text", test_engine_failure_clears_partial_text},
    {"memory_failure_or_retained_pool_refuses_result", test_memory_failure_or_retained_pool_refuses_result},
    {"utf8_capacity_clears_partial_text", test_utf8_capacity_clears_partial_text},
    {"missing_engine_and_short_buffers", test_missing_engine_and_short_buffers},
    {"single_line_accepts_dim_and_light_background", test_single_line_accepts_dim_and_light_background},
    {"single_line_source_and_cancellation", test_single_line_source_and_cancellation},
    {"single_line_utf8_and_retained_pool", test_single_line_utf8_and_retained_pool},
    {"single_line_invalid_image", test_single_line_invalid_image},
    {"detector_filters_clamps_sorts_and_reuses_input", test_detector_filters_clamps_sorts_and_reuses_input},
    {"detector_empty_tall_and_line_limit", test_detector_empty_tall_and_line_limit},
    {"detector_source_cancel_and_short_buffers", test_detector_source_cancel_and_short_buffers},
    {"detector_failure_retention_and_invalid_coordinates", test_detector_failure_retention_and_invalid_coordinates},
    {"detector_recognition_failure_clears_partial_utf8", test_detector_recognition_failure_clears_partial_utf8},
    {NULL, NULL}
};
