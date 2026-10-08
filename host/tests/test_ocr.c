#include "acutest.h"
#include "local_ocr.h"
#include "ocr_heap.h"
#include <stdlib.h>
#include <string.h>

static unsigned char pixels[100 * 100 * 4];
static int cancel_read, fail_read;
static int rows(void *ud, unsigned y, unsigned n, unsigned char *out)
{
    (void)ud;
    if (fail_read || y+n > 100) return -1;
    memcpy(out, pixels+y*100*4, n*100*4);
    return (int)n;
}
static int cancelled(void *ud) { (void)ud; return cancel_read; }
static VjoOcrImage im = {NULL, 100, 100, rows, cancelled};
static void ink(unsigned y)
{
    for (unsigned yy = y; yy < y+12; yy++)
        for (unsigned x = 10; x < 60; x += 4)
            memset(pixels+(yy*100+x)*4, 255, 3);
}
static void test_regions(void)
{
    VjoOcrLines lines;
    memset(pixels, 0, sizeof(pixels)); cancel_read = fail_read = 0;
    TEST_CHECK(vjo_ocr_find_lines(&im, &lines) == VJO_OK && !lines.count);
    ink(10); ink(40);
    TEST_ASSERT(vjo_ocr_find_lines(&im, &lines) == VJO_OK);
    TEST_CHECK(lines.count == 2 && lines.lines[0].y < lines.lines[1].y);
    for (unsigned i = 0; i < lines.count; i++) {
        TEST_CHECK(lines.lines[i].x + lines.lines[i].w <= im.width);
        TEST_CHECK(lines.lines[i].y + lines.lines[i].h <= im.height);
    }
    fail_read = 1;
    TEST_CHECK(vjo_ocr_find_lines(&im, &lines) == VJO_E_SOURCE);
    fail_read = 0; cancel_read = 1;
    TEST_CHECK(vjo_ocr_find_lines(&im, &lines) == VJO_E_CANCELLED);
    cancel_read = 0;
    memset(pixels, 255, sizeof(pixels));
    TEST_CHECK(vjo_ocr_find_lines(&im, &lines) == VJO_E_OCR_REGION);
    VjoOcrImage huge = im; huge.width = VJO_OCR_MAX_WIDTH+1;
    TEST_CHECK(vjo_ocr_find_lines(&huge, &lines) == VJO_E_OCR_REGION);
}
static void test_ctc(void)
{
    const char *vocab[] = {"猫", "に", "🇯🇵"};
    unsigned tokens[] = {1,1,0,1,2,2,0,3};
    unsigned last = 0; size_t used = 0; char text[32] = "";
    for (unsigned i = 0; i < sizeof(tokens)/sizeof(tokens[0]); i++)
        TEST_CHECK(vjo_ocr_ctc_append(tokens[i], &last, vocab, 3, text, sizeof(text), &used) == VJO_OK);
    TEST_CHECK(!strcmp(text, "猫猫に🇯🇵"));
    TEST_CHECK(vjo_ocr_ctc_append(4, &last, vocab, 3, text, sizeof(text), &used) == VJO_E_PARSE);
    last = 0; used = 0; text[0] = 0;
    TEST_CHECK(vjo_ocr_ctc_append(1, &last, vocab, 3, text, 3, &used) == VJO_E_TOO_LARGE);
    TEST_CHECK(used == 0 && text[0] == 0); /* no partial UTF-8 */
}
static void test_heap(void)
{
    void *memory = NULL;
    TEST_ASSERT(posix_memalign(&memory, 64, 4096) == 0);
    TEST_ASSERT(vjo_ocr_heap_begin(memory, 4096) == 0);
    TEST_CHECK(vjo_ocr_heap_begin(memory, 4096) < 0); /* non-reentrant */
    void *a = vjo_ncnn_malloc(900), *b = vjo_ncnn_malloc(900), *c = vjo_ncnn_malloc(900);
    TEST_ASSERT(a && b && c);
    TEST_CHECK(!((uintptr_t)a & 63));
    vjo_ncnn_free(b); vjo_ncnn_free(a); vjo_ncnn_free(c);
    a = vjo_ncnn_malloc(3900); /* adjacent free blocks coalesce */
    TEST_ASSERT(a != NULL);
    TEST_CHECK(vjo_ncnn_malloc(4096) == NULL && vjo_ocr_heap_failed());
    vjo_ncnn_free(a);
    TEST_CHECK(vjo_ocr_heap_end() <= 4096);
    free(memory);
}
static void test_bounded_entry(void)
{
    VjoPlatform platform = {0}; VjoOcrStats stats; char text[20] = "old";
    memset(pixels, 0, sizeof(pixels)); ink(10);
    cancel_read = fail_read = 0;
    TEST_CHECK(vjo_local_ocr(&platform, "missing", &im, NULL, 0, text, sizeof(text), &stats) == VJO_E_OOM);
    TEST_CHECK(!text[0]);
    cancel_read = 1;
    TEST_CHECK(vjo_local_ocr(&platform, "missing", &im, NULL, 0, text, sizeof(text), &stats) == VJO_E_CANCELLED);
    cancel_read = 0;
    void *memory = NULL;
    TEST_ASSERT(posix_memalign(&memory, 64, VJO_OCR_HEAP_BYTES) == 0);
    TEST_CHECK(vjo_local_ocr(&platform, "missing", &im, memory, VJO_OCR_HEAP_BYTES, text, sizeof(text), &stats) == VJO_E_OCR_MODEL);
    TEST_CHECK(!text[0]);
    free(memory);
}
TEST_LIST = {
    {"regions_and_capture_errors", test_regions}, {"ctc_utf8", test_ctc},
    {"bounded_heap", test_heap}, {"bounded_entry", test_bounded_entry}, {NULL, NULL}
};
