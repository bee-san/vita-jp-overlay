/* Horizontal bright-text dialogue OCR. No JPEG, GPU or network dependency. */
#ifndef VJO_LOCAL_OCR_H
#define VJO_LOCAL_OCR_H
#include "arena.h"
#include "net.h"
#ifdef __cplusplus
extern "C" {
#endif

#define VJO_OCR_MAX_WIDTH 960
#define VJO_OCR_MAX_HEIGHT 544
#define VJO_OCR_MAX_LINES 8
#define VJO_OCR_MAX_LINE_HEIGHT 96
#define VJO_OCR_TEXT_CAP 4096
#define VJO_OCR_HEAP_BYTES (64u * 1024u * 1024u)

typedef struct {
    void *ud;
    unsigned width, height;
    /* Packed RGBA rows; return the number of rows copied, or <0. */
    int (*rows)(void *ud, unsigned y, unsigned n, unsigned char *rgba);
    int (*cancelled)(void *ud); /* optional; checked between reads/lines */
} VjoOcrImage;
typedef struct { unsigned x, y, w, h; } VjoOcrLine;
typedef struct {
    VjoOcrLine lines[VJO_OCR_MAX_LINES];
    unsigned count;
} VjoOcrLines;
typedef struct { size_t heap_peak; unsigned lines; } VjoOcrStats;

/* Bounded projection segmentation of white/near-white text. Caller selects
 * the dialogue region first; this is not a general scene-text detector. */
int vjo_ocr_find_lines(const VjoOcrImage *image, VjoOcrLines *out);
/* Greedy CTC (blank 0); last is reset to 0 for each independent text line. */
int vjo_ocr_ctc_append(unsigned token, unsigned *last, const char *const *vocab,
                      unsigned vocab_count, char *text, size_t cap, size_t *used);
/* workspace is an aligned, private buffer valid for the call. Model/tensor
 * allocations are confined to it and released before return. Single caller. */
int vjo_local_ocr(const VjoPlatform *platform, const char *model_dir,
                  const VjoOcrImage *image, void *workspace, size_t workspace_size,
                  char *text, size_t text_cap, VjoOcrStats *stats);
#ifdef __cplusplus
}
#endif
#endif
