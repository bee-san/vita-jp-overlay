/* vita-vn-ocr backend (core/vocr_ocr.c) without its weights: sizing, the
 * single pass over the rows, the line finder on both polarities, and every
 * refusal before or while the model loads. With VJO_VOCR_MODEL_DIR set, the
 * pinned table is also checked against the real release files. */
#include "acutest.h"
#include "net_posix.h"
#include "vocr.h"
#include "vocr_ocr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 400
#define H 100
static unsigned char pixels[W * H * 4];
static unsigned reads[H];
static int last_row, fail_rows, cancel_now, opens;

static int rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    (void)ud;
    if (fail_rows || y + n > H || (int)y != last_row + 1) return -1;
    for (unsigned r = 0; r < n; r++) reads[y + r]++;
    last_row = (int)(y + n - 1);
    memcpy(dst, pixels + (size_t)y * W * 4, (size_t)n * W * 4);
    return (int)n;
}
static int cancelled(void *ud) { (void)ud; return cancel_now; }
static VjoOcrImage im = {NULL, W, H, rows, cancelled};

static void fill(unsigned char v)
{
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
        pixels[i] = pixels[i+1] = pixels[i+2] = v;
        pixels[i+3] = 255;
    }
}

/* Glyph-like vertical strokes (2 px wide every 6 px) across one text line. */
static void line_of_strokes(unsigned y0, unsigned y1, unsigned char v)
{
    for (unsigned y = y0; y < y1; y++)
        for (unsigned x = 20; x < W - 20; x += 6)
            for (unsigned k = 0; k < 2; k++) {
                unsigned char *p = pixels + ((size_t)y * W + x + k) * 4;
                p[0] = p[1] = p[2] = v;
            }
}

static void reset(void)
{
    memset(reads, 0, sizeof(reads));
    last_row = -1;
    fail_rows = cancel_now = opens = 0;
}

static int no_file(void *ud, const char *path, VjoFile *out)
{
    (void)ud; (void)path; (void)out;
    opens++;
    return -1;
}

static void *workspace(const char *model, size_t *bytes)
{
    void *p = NULL;
    *bytes = vjo_vocr_workspace_bytes(model, W, H);
    TEST_ASSERT(*bytes > 0);
    TEST_ASSERT(posix_memalign(&p, 64, *bytes) == 0);
    return p;
}

static void test_models_and_sizes(void)
{
    unsigned n = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n);
    TEST_CHECK(n == 3 && all != NULL);
    const VjoVocrModel *h15 = vjo_vocr_model(VJO_VOCR_DEFAULT_MODEL);
    TEST_ASSERT(h15 != NULL);
    TEST_CHECK(h15->bytes == 1155968 && h15->arena_bytes == 182944);
    TEST_CHECK(vjo_vocr_model("FL10_w8.vocr")->bytes == 657664);
    TEST_CHECK(!vjo_vocr_model("H15_f32.vocr") && !vjo_vocr_model("") && !vjo_vocr_model(NULL));
    /* weights + max(finder, recognizer arena) + grey region + the runtime's two structs */
    size_t full = vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 960, 544);
    size_t parts = 1155968 + 182944 + 960 * 544;
    TEST_CHECK(full >= parts && full - parts < sizeof(vocr_model) + sizeof(vocr_ctx) + 64);
    TEST_MSG("960x544 workspace %zu, parts %zu", full, parts);
    TEST_CHECK(vjo_vocr_workspace_bytes("FL10_w8.vocr", 960, 544) < full);
    TEST_CHECK(vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 880, 144) < full);
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 961, 10));
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 10, 545));
    TEST_CHECK(!vjo_vocr_workspace_bytes(VJO_VOCR_DEFAULT_MODEL, 0, 10));
    TEST_CHECK(!vjo_vocr_workspace_bytes("other.vocr", 100, 10));
}

static void test_empty_region_reads_no_model(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64] = "old";
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;
    reset();
    fill(16);
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) == VJO_OK);
    TEST_CHECK(!text[0] && st.lines == 0 && !st.model_loaded && opens == 0);
    TEST_CHECK(st.workspace == bytes && st.region_bytes == (size_t)W * H);
    for (unsigned y = 0; y < H; y++) TEST_CHECK(reads[y] == 1); /* each row once, in order */
    free(ws);
}

static void test_lines_both_polarities(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64] = "old";
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;

    reset(); /* light text on a dark box */
    fill(20);
    line_of_strokes(20, 44, 235);
    line_of_strokes(60, 84, 235);
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 2 && st.dark == 0 && opens == 1 && !text[0]);
    TEST_MSG("lines %u dark %u", st.lines, st.dark);

    reset(); /* dark text on a light box: the overlay's bright-pixel finder rejects it */
    fill(235);
    line_of_strokes(20, 44, 30);
    line_of_strokes(60, 84, 30);
    TEST_CHECK(vjo_vocr_ocr(&p, "missing", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 2 && st.dark == 2);
    TEST_MSG("lines %u dark %u", st.lines, st.dark);
    free(ws);
}

static void write_file(const char *path, size_t n, unsigned char v)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT(f != NULL);
    for (size_t i = 0; i < n; i++) fputc(v, f);
    fclose(f);
}

static void test_wrong_or_corrupt_weights(void)
{
    VjoPlatform p;
    PosixPlatform pp = {0};
    VjoVocrStats st;
    char dir[] = "/tmp/vjo-vocr-XXXXXX", path[128], text[64];
    size_t bytes;
    void *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    posix_platform_init(&pp, &p);
    TEST_ASSERT(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/%s", dir, VJO_VOCR_DEFAULT_MODEL);
    fill(20);
    line_of_strokes(30, 54, 235);
    write_file(path, 1000, 0); /* wrong size */
    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, dir, VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    write_file(path, 1155968, 0x5A); /* right size, wrong SHA-256 */
    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, dir, VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_OCR_MODEL);
    TEST_CHECK(st.lines == 1 && !st.model_loaded && !text[0]);
    unlink(path);
    rmdir(dir);
    free(ws);
}

static void test_refusals(void)
{
    VjoPlatform p = {0};
    VjoVocrStats st;
    char text[64] = "old";
    size_t bytes;
    unsigned char *ws = workspace(VJO_VOCR_DEFAULT_MODEL, &bytes);
    p.file_open = no_file;
    fill(20);
    line_of_strokes(30, 54, 235);

    reset();
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes - 1, text, sizeof(text), &st) ==
               VJO_E_OOM);
    TEST_CHECK(!text[0] && st.workspace == bytes && reads[0] == 0); /* refused before any read */
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws + 8, bytes - 8, text, sizeof(text), &st) ==
               VJO_E_OOM); /* unaligned */
    TEST_CHECK(vjo_vocr_ocr(&p, "d", "other.vocr", &im, ws, bytes, text, sizeof(text), &st) == VJO_E_OCR_MODEL);
    VjoOcrImage big = im;
    big.width = VJO_OCR_MAX_WIDTH + 1;
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &big, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_OCR_REGION);
    reset();
    cancel_now = 1;
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_CANCELLED);
    reset();
    fail_rows = 1;
    TEST_CHECK(vjo_vocr_ocr(&p, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_SOURCE);
    TEST_CHECK(vjo_vocr_ocr(NULL, "d", VJO_VOCR_DEFAULT_MODEL, &im, ws, bytes, text, sizeof(text), &st) ==
               VJO_E_SOURCE);
    free(ws);
}

/* The pinned sizes and arenas match the release files (when they are here). */
static void test_release_files(void)
{
    const char *dir = VJO_VOCR_MODEL_DIR;
    unsigned n = 0, checked = 0;
    const VjoVocrModel *all = vjo_vocr_models(&n);
    if (!dir[0]) {
        TEST_SKIP("VJO_VOCR_MODEL_DIR not set");
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        char path[512];
        void *blob = NULL;
        FILE *f;
        snprintf(path, sizeof(path), "%s/%s", dir, all[i].name);
        f = fopen(path, "rb");
        if (!f) continue;
        TEST_ASSERT(posix_memalign(&blob, 16, all[i].bytes + 1) == 0);
        TEST_CHECK(fread(blob, 1, all[i].bytes + 1, f) == all[i].bytes);
        fclose(f);
        vocr_model m;
        vocr_opts o;
        memset(&o, 0, sizeof(o));
        o.mode = VOCR_MODE_W8I;
        TEST_CHECK(vocr_load(&m, blob, all[i].bytes) == VOCR_OK);
        TEST_CHECK(vocr_check_crc(blob, all[i].bytes) == VOCR_OK);
        TEST_CHECK(vocr_arena_bytes(&m, &o) == all[i].arena_bytes);
        TEST_MSG("%s: arena %zu, pinned %u", all[i].name, vocr_arena_bytes(&m, &o), all[i].arena_bytes);
        free(blob);
        checked++;
    }
    TEST_CHECK(checked > 0);
    TEST_MSG("no release file in %s", dir);
}

TEST_LIST = {
    {"models_and_workspace_sizes", test_models_and_sizes},
    {"empty_region_reads_rows_once_and_no_model", test_empty_region_reads_no_model},
    {"finds_light_and_dark_text_lines", test_lines_both_polarities},
    {"wrong_size_or_hash_is_refused", test_wrong_or_corrupt_weights},
    {"refusals_publish_no_text", test_refusals},
    {"pinned_table_matches_release_files", test_release_files},
    {NULL, NULL}
};
