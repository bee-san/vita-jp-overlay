/* vita-vn-ocr backend: bee-san/vita-vn-ocr's polarity-agnostic dialogue line
 * finder and tiny CTC recognizer (plain C99, no malloc), with the int8 weights
 * published in its v0.2.0 release. Built with -DVJO_WITH_VOCR=ON; see
 * docs/vita-vn-ocr.md.
 *
 * One job reads the selected region once, converts it to grey, finds its lines
 * (any text/background polarity), then loads and checks the weights file and
 * recognizes each line, inverting dark text as the model was trained. All of
 * its memory is one caller-provided workspace whose exact size
 * vjo_vocr_workspace_bytes() reports; nothing else is allocated. */
#ifndef VJO_VOCR_OCR_H
#define VJO_VOCR_OCR_H

#include <stddef.h>
#include <stdint.h>

#include "local_ocr.h" /* VjoOcrImage, VJO_OCR_MAX_WIDTH/HEIGHT, VJO_OCR_TEXT_CAP */
#include "net.h"       /* VjoPlatform.file_open, now_us */

#ifdef __cplusplus
extern "C" {
#endif

#define VJO_VOCR_DEFAULT_MODEL "H15_w8.vocr"

/* A weights file this build accepts: exact size and SHA-256 of the release
 * asset, and the recognizer's arena (W8I mode, default tiling). */
typedef struct {
    const char *name;
    uint32_t bytes;
    unsigned char sha256[32];
    uint32_t arena_bytes;
} VjoVocrModel;

/* The model named `name` (a file name in ocr_model_dir), or NULL. */
const VjoVocrModel *vjo_vocr_model(const char *name);
/* The supported models, for messages and tools; *count is set. */
const VjoVocrModel *vjo_vocr_models(unsigned *count);

typedef struct {
    size_t workspace;        /* bytes the job needs (vjo_vocr_workspace_bytes) */
    size_t model_bytes;      /* weights, read in place */
    size_t scratch_bytes;    /* the finder's arena, then the recognizer's */
    size_t region_bytes;     /* the region in grey, one byte per pixel */
    unsigned width, height;  /* region */
    unsigned lines;          /* lines found */
    unsigned dark;           /* of those, dark text (inverted before recognition) */
    unsigned truncated;      /* more lines than the finder returns (16) */
    unsigned frames;         /* CTC frames recognized */
    unsigned model_loaded;   /* 0 when no line was found: the file was not read */
    uint32_t find_us, load_us, recognize_us; /* with VjoPlatform.now_us, else 0 */
} VjoVocrStats;

/* Exact workspace bytes for a job on a width x height region with `model`;
 * 0 for an unsupported model or region. */
size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height);

/* Recognizes the region: lines top to bottom, separated by '\n', in text (at
 * most text_cap bytes with the NUL). workspace: 16-byte aligned, private to
 * the call. image->rows returns RGBA rows (A8B8G8R8 bytes R, G, B, A) and is
 * read exactly once per row, in order. Returns VJO_OK (also with no line:
 * text "" and no file read), VJO_E_OCR_MODEL (file missing, wrong size or
 * hash, or refused by the runtime), VJO_E_OOM (workspace too small),
 * VJO_E_OCR_REGION, VJO_E_SOURCE (row read), VJO_E_CANCELLED,
 * VJO_E_TOO_LARGE (text) or VJO_E_OCR_INFERENCE. text is "" on error. */
int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoOcrImage *image, void *workspace, size_t workspace_size,
                 char *text, size_t text_cap, VjoVocrStats *stats);

#ifdef __cplusplus
}
#endif
#endif
