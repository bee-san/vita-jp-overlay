/* Bounded line/region adapter. Meiki preprocessing and decoding are C;
 * the caller supplies an isolated inference engine through this interface. */
#ifndef VJO_MEIKI_OCR_H
#define VJO_MEIKI_OCR_H
#include "local_ocr.h"
#include "preprocess.h"
#include "runtime.h"
#include "detect_preprocess.h"
#ifdef __cplusplus
extern "C" {
#endif

#define VJO_MEIKI_MODEL_FILENAME "meiki-stream-int8.mnn"
#define VJO_MEIKI_DETECT_MODEL_FILENAME "meiki-detect-int8.mnn"
typedef struct {
    void *ud;
    /* Return VJO_OK or a VJO_E_* error. Validate the pinned model before
     * loading. Release per-call model/tensor allocations before returning.
     * The engine owns its private workspace; input/scratch are separate. */
    int (*run)(void *ud, const char *model_path, const float *input,
               MeikiOutput *output, MeikiStats *stats);
    /* Optional existing Meiki detector; same serialized workspace/lifetime
     * contract as run. Coordinates use the supplied original-size targets. */
    int (*detect)(void *ud, const char *model_path, const float *input,
                  int32_t target_width, int32_t target_height,
                  MeikiDetectOutput *output, MeikiStats *stats);
} VjoMeikiEngine;

typedef struct {
    unsigned lines, completed_lines;
    size_t heap_peak, heap_remaining;
    float session_memory_mib;
    uint64_t load_us, inference_us;
    int allocation_failed;
    VjoOcrLines line_boxes; /* selected/cropped input coordinates */
} VjoMeikiStats;

/* The image is the already selected dialogue region. The existing white-text
 * line finder supports horizontal text, at most eight lines, at most 960x544.
 * Single caller. Reuses one 368640-byte input and 13440-byte preprocessing
 * scratch for every line, without allocating or retaining model/tensor RAM.
 * text is never published partially on error, and is limited to 4096 bytes
 * including NUL even if a larger destination is supplied. Stats retain the
 * maximum engine pool use and remaining allocations, and summed durations.
 */
int vjo_meiki_ocr(const char *model_dir, const VjoOcrImage *image,
                  const VjoMeikiEngine *engine,
                  float *input, size_t input_elements,
                  void *scratch, size_t scratch_bytes,
                  char *text, size_t text_cap, VjoMeikiStats *stats);
/* Recognize the entire selected image as one horizontal text line. Bypasses
 * white-text projection, preserving dim, coloured and dark-on-light text and
 * attached ruby. Select a narrow line crop without adjacent text. Uses the
 * same buffers, engine lifetime, cancellation and output bounds as above. */
int vjo_meiki_ocr_single_line(const char *model_dir, const VjoOcrImage *image,
                             const VjoMeikiEngine *engine,
                             float *input, size_t input_elements,
                             void *scratch, size_t scratch_bytes,
                             char *text, size_t text_cap, VjoMeikiStats *stats);
/* Run the existing 320x192 Meiki detector inside the selected horizontal
 * dialogue region, then recognize each valid score>.5 crop in y order.
 * At most eight crops; no partial result on overflow or failure. Requires
 * MEIKI_DETECT_ELEMENTS floats (737280 bytes), reusable by recognition, and
 * the same 13440-byte scratch as the line adapter. No heap allocations. */
int vjo_meiki_ocr_detected(const char *model_dir, const VjoOcrImage *image,
                          const VjoMeikiEngine *engine,
                          float *input, size_t input_elements,
                          void *scratch, size_t scratch_bytes,
                          char *text, size_t text_cap, VjoMeikiStats *stats);
#ifdef __cplusplus
}
#endif
#endif
