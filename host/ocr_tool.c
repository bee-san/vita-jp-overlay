/* Diagnostic runner for the same bounded recognizer used by SceShell.
 * Input: tightly packed RGBA8 (no image-decoder dependency). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local_ocr.h"
#include "net_posix.h"

typedef struct { unsigned char *pixels; unsigned width, height; } Image;
static int rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    Image *im = ud;
    if (y > im->height || n > im->height-y) return -1;
    memcpy(dst, im->pixels+(size_t)y*im->width*4, (size_t)n*im->width*4);
    return (int)n;
}
int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "Usage: vjo-ocr MODEL_DIR WIDTH HEIGHT INPUT.rgba\n");
        return 2;
    }
    Image im = {0};
    im.width = (unsigned)atoi(argv[2]); im.height = (unsigned)atoi(argv[3]);
    if (!im.width || im.width > VJO_OCR_MAX_WIDTH || !im.height || im.height > VJO_OCR_MAX_HEIGHT)
        return 2;
    size_t bytes = (size_t)im.width*im.height*4;
    im.pixels = malloc(bytes);
    void *workspace = NULL;
    if (!im.pixels || posix_memalign(&workspace, 64, VJO_OCR_HEAP_BYTES)) return 2;
    FILE *f = fopen(argv[4], "rb");
    if (!f || fread(im.pixels, 1, bytes, f) != bytes || fgetc(f) != EOF) {
        fprintf(stderr, "Invalid RGBA input\n"); return 2;
    }
    fclose(f);
    VjoPlatform platform;
    PosixPlatform pp = {0};
    posix_platform_init(&pp, &platform);
    VjoOcrImage input = {&im, im.width, im.height, rows, NULL};
    VjoOcrStats stats;
    char text[VJO_OCR_TEXT_CAP];
    int rc = vjo_local_ocr(&platform, argv[1], &input, workspace, VJO_OCR_HEAP_BYTES,
                         text, sizeof(text), &stats);
    fprintf(stderr, "rc=%d lines=%u tensor_heap_peak=%zu\n", rc, stats.lines, stats.heap_peak);
    if (!rc) puts(text);
    free(workspace); free(im.pixels);
    return rc ? 1 : 0;
}
