/* Host diagnostic: actual overlay adapter with the pinned bounded MNN engine.
 * Inputs are private packed RGBA fixtures; no image decoder is required.
 * MODEL_DIR alone accepts stdin WIDTH<TAB>HEIGHT<TAB>INPUT.rgba records.
 * JSON text_hex keeps arbitrary recognized UTF-8 inside one output record. */
#include "meiki_ocr.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char *pixels; unsigned width, height; } Image;
enum { PROJECTED_LINES, SINGLE_LINE, DIALOGUE_BOX };
typedef struct { void *workspace; int layout; } HostEngine;

static int runtime_error(int rc)
{
    switch (rc) {
    case 0: return VJO_OK;
    case -1: return VJO_E_SOURCE;
    case -2: case -4: return VJO_E_OCR_MODEL;
    case -10: case -12: return VJO_E_OOM;
    case -13: return VJO_E_OCR_UNAVAILABLE;
    default: return VJO_E_OCR_INFERENCE;
    }
}

static int rows(void *ud, unsigned y, unsigned n, unsigned char *dst)
{
    Image *image = ud;
    if (y > image->height || n > image->height - y) return -1;
    memcpy(dst, image->pixels + (size_t)y * image->width * 4,
            (size_t)n * image->width * 4);
    return (int)n;
}

static int infer(void *ud, const char *path, const float *input,
                 MeikiOutput *out, MeikiStats *stats)
{
    HostEngine *engine = ud;
    int rc = meiki_run(path, input, engine->workspace, MEIKI_WORKSPACE_BYTES, out, stats);
    return runtime_error(rc);
}

static int detect(void *ud, const char *path, const float *input,
                   int32_t target_width, int32_t target_height,
                   MeikiDetectOutput *out, MeikiStats *stats)
{
    HostEngine *engine = ud;
    return runtime_error(meiki_detect_run(path, input, target_width, target_height,
                         engine->workspace, MEIKI_WORKSPACE_BYTES, out, stats));
}

static int dimension(const char *value, unsigned maximum, unsigned *out)
{
    char *end;
    if (!value || !*value || *value == '-') return -1;
    errno = 0;
    unsigned long n = strtoul(value, &end, 10);
    if (errno || *end || !n || n > maximum) return -1;
    *out = (unsigned)n;
    return 0;
}

static int recognize(const char *dir, const char *width, const char *height,
                     const char *path, VjoMeikiEngine *engine,
                     float *input, void *scratch)
{
    Image image = {0};
    if (dimension(width, VJO_OCR_MAX_WIDTH, &image.width) ||
        dimension(height, VJO_OCR_MAX_HEIGHT, &image.height)) return 2;
    size_t bytes = (size_t)image.width * image.height * 4;
    image.pixels = malloc(bytes);
    FILE *file = fopen(path, "rb");
    if (!image.pixels || !file) {
        if (file) fclose(file);
        free(image.pixels);
        return 2;
    }
    int valid = fread(image.pixels, 1, bytes, file) == bytes && fgetc(file) == EOF;
    fclose(file);
    if (!valid) { free(image.pixels); return 2; }
    VjoOcrImage pixels = {&image, image.width, image.height, rows, NULL};
    VjoMeikiStats stats;
    char text[VJO_OCR_TEXT_CAP];
    HostEngine *host = engine->ud;
    size_t input_elements = host->layout == DIALOGUE_BOX
        ? MEIKI_DETECT_ELEMENTS : MEIKI_PREPROCESS_ELEMENTS;
    int rc = (host->layout == DIALOGUE_BOX ? vjo_meiki_ocr_detected :
              host->layout == SINGLE_LINE ? vjo_meiki_ocr_single_line : vjo_meiki_ocr)(
        dir, &pixels, engine, input, input_elements,
        scratch, MEIKI_PREPROCESS_SCRATCH_BYTES, text, sizeof(text), &stats);
    printf("{\"rc\":%d,\"lines\":%u,\"completed_lines\":%u,"
            "\"heap_peak\":%zu,\"heap_remaining\":%zu,\"allocation_failed\":%d,"
            "\"load_us\":%llu,\"inference_us\":%llu,\"text_hex\":\"",
            rc, stats.lines, stats.completed_lines, stats.heap_peak,
            stats.heap_remaining, stats.allocation_failed,
            (unsigned long long)stats.load_us, (unsigned long long)stats.inference_us);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        printf("%02x", *p);
    printf("\",\"boxes\":[");
    for (unsigned i = 0; i < stats.line_boxes.count; ++i) {
        const VjoOcrLine *box = &stats.line_boxes.lines[i];
        printf("%s[%u,%u,%u,%u]", i ? "," : "", box->x, box->y, box->w, box->h);
    }
    puts("]}");
    fflush(stdout);
    free(image.pixels);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    int layout = PROJECTED_LINES;
    if (argc > first && !strcmp(argv[first], "--single-line")) { layout = SINGLE_LINE; first++; }
    else if (argc > first && !strcmp(argv[first], "--dialogue-box")) { layout = DIALOGUE_BOX; first++; }
    int arguments = argc - first;
    if (arguments != 1 && arguments != 4) {
        fprintf(stderr, "Usage: vjo-meiki-ocr [--single-line|--dialogue-box] MODEL_DIR [WIDTH HEIGHT INPUT.rgba]\n");
        return 2;
    }
    HostEngine host = {0};
    host.layout = layout;
    VjoMeikiEngine engine = {&host, infer, detect};
    size_t input_elements = layout == DIALOGUE_BOX ? MEIKI_DETECT_ELEMENTS : MEIKI_PREPROCESS_ELEMENTS;
    float *input = malloc(input_elements * sizeof(*input));
    void *scratch = malloc(MEIKI_PREPROCESS_SCRATCH_BYTES);
    if (!input || !scratch || posix_memalign(&host.workspace, 64, MEIKI_WORKSPACE_BYTES)) {
        free(input); free(scratch);
        return 2;
    }
    int rc = 0;
    if (arguments == 4)
        rc = recognize(argv[first], argv[first + 1], argv[first + 2], argv[first + 3],
                       &engine, input, scratch);
    else {
        char line[4096];
        while (fgets(line, sizeof(line), stdin)) {
            size_t n = strlen(line);
            if (!n || line[n - 1] != '\n') { rc = 2; break; }
            line[--n] = 0;
            if (n && line[n - 1] == '\r') line[--n] = 0;
            char *height = strchr(line, '\t');
            char *path = height ? strchr(height + 1, '\t') : NULL;
            if (!height || !path || !path[1] || strchr(path + 1, '\t')) { rc = 2; break; }
            *height++ = 0; *path++ = 0;
            if (recognize(argv[first], line, height, path, &engine, input, scratch) == 2) {
                rc = 2;
                break;
            }
            /* OCR errors have a JSON record; continue the remaining fixtures. */
        }
        if (ferror(stdin)) rc = 2;
    }
    free(host.workspace); free(scratch); free(input);
    return rc;
}
