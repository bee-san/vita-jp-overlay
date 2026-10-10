#include "meiki_ocr.h"
#include "decode.h"
#include <math.h>
#include <string.h>

static int cancelled(const VjoOcrImage *image)
{
    return image->cancelled && image->cancelled(image->ud);
}

static int preprocessing_error(int rc)
{
    if (rc == MEIKI_PREPROCESS_CANCELLED) return VJO_E_CANCELLED;
    if (rc == MEIKI_PREPROCESS_SOURCE) return VJO_E_SOURCE;
    return VJO_E_OCR_REGION;
}

static void add_stats(VjoMeikiStats *out, const MeikiStats *line)
{
    if (line->heap_peak > out->heap_peak) out->heap_peak = line->heap_peak;
    if (line->heap_remaining > out->heap_remaining) out->heap_remaining = line->heap_remaining;
    if (line->session_memory_mib > out->session_memory_mib)
        out->session_memory_mib = line->session_memory_mib;
    out->load_us += line->load_us;
    out->inference_us += line->inference_us;
    out->allocation_failed |= line->allocation_failed;
}

static int engine_result(const VjoOcrImage *image, int rc, const MeikiStats *stats)
{
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (stats->allocation_failed) return VJO_E_OOM;
    if (rc) return rc > 0 ? VJO_E_OCR_INFERENCE : rc;
    return stats->heap_remaining ? VJO_E_OCR_INFERENCE : VJO_OK;
}

static unsigned clamp_coordinate(float value, unsigned bound)
{
    if (value <= 0.0f) return 0;
    if (value >= (float)bound) return bound;
    return (unsigned)value;
}

static int detected_lines(const VjoOcrImage *image, const MeikiDetectOutput *detected,
                          VjoOcrLines *lines)
{
    memset(lines, 0, sizeof(*lines));
    for (unsigned i = 0; i < MEIKI_DETECT_BOXES; ++i) {
        if (!isfinite(detected->scores[i])) return VJO_E_OCR_INFERENCE;
        if (!(detected->scores[i] > .5f)) continue;
        const float *box = detected->boxes[i];
        for (unsigned j = 0; j < 4; ++j)
            if (!isfinite(box[j])) return VJO_E_OCR_INFERENCE;
        unsigned x1 = clamp_coordinate(box[0], image->width);
        unsigned y1 = clamp_coordinate(box[1], image->height);
        unsigned x2 = clamp_coordinate(box[2], image->width);
        unsigned y2 = clamp_coordinate(box[3], image->height);
        if (x2 <= x1 || y2 <= y1) continue;
        /* The selected region contains horizontal dialogue. A taller-than-wide
         * crop may be a single glyph/name, so aspect ratio does not reject it. */
        if (lines->count == VJO_OCR_MAX_LINES) return VJO_E_OCR_REGION;
        VjoOcrLine line = {x1, y1, x2 - x1, y2 - y1};
        unsigned at = lines->count++;
        while (at && lines->lines[at - 1].y > line.y) {
            lines->lines[at] = lines->lines[at - 1];
            --at;
        }
        lines->lines[at] = line;
    }
    return VJO_OK;
}

enum { PROJECTED_LINES, SINGLE_LINE, DETECTED_LINES };

static int recognize(const char *dir, const VjoOcrImage *image,
                     const VjoMeikiEngine *engine,
                     float *input, size_t input_elements,
                     void *scratch, size_t scratch_bytes,
                     char *text, size_t cap, VjoMeikiStats *stats,
                     int mode)
{
    VjoOcrLines lines;
    char path[256];
    const char filename[] = "/" VJO_MEIKI_MODEL_FILENAME;
    size_t used = 0, dn;
    int rc;
    if (text && cap) text[0] = 0;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!dir || !image || !text || !cap || !stats) return VJO_E_SOURCE;
    if (!engine || !engine->run) return VJO_E_OCR_UNAVAILABLE;
    if (mode == DETECTED_LINES && !engine->detect) return VJO_E_OCR_UNAVAILABLE;
    dn = strlen(dir);
    if (!dn || dn + sizeof(filename) > sizeof(path)) return VJO_E_OCR_MODEL;
    memcpy(path, dir, dn);
    memcpy(path + dn, filename, sizeof(filename));
    if (mode != PROJECTED_LINES) {
        if (!image->rows || !image->width || !image->height ||
            image->width > VJO_OCR_MAX_WIDTH || image->height > VJO_OCR_MAX_HEIGHT)
            return VJO_E_OCR_REGION;
        if (cancelled(image)) return VJO_E_CANCELLED;
        if (mode == DETECTED_LINES) {
            MeikiRgbaImage rgba = {image->ud, image->width, image->height,
                                   image->rows, image->cancelled};
            MeikiCrop crop = {0, 0, image->width, image->height};
            MeikiDetectPreprocessInfo info;
            MeikiDetectOutput output = {0};
            MeikiStats detector_stats = {0};
            const char detector_filename[] = "/" VJO_MEIKI_DETECT_MODEL_FILENAME;
            if (dn + sizeof(detector_filename) > sizeof(path)) return VJO_E_OCR_MODEL;
            if (!input || input_elements < MEIKI_DETECT_ELEMENTS || !scratch ||
                scratch_bytes < MEIKI_PREPROCESS_SCRATCH_BYTES) return VJO_E_OOM;
            rc = meiki_detect_preprocess_rgba(&rgba, &crop, input, input_elements,
                                              scratch, scratch_bytes, &info);
            if (rc) return preprocessing_error(rc);
            if (cancelled(image)) return VJO_E_CANCELLED;
            memcpy(path + dn, detector_filename, sizeof(detector_filename));
            rc = engine->detect(engine->ud, path, input, info.target_width,
                                 info.target_height, &output, &detector_stats);
            add_stats(stats, &detector_stats);
            rc = engine_result(image, rc, &detector_stats);
            if (rc) return rc;
            rc = detected_lines(image, &output, &lines);
            memcpy(path + dn, filename, sizeof(filename));
        } else {
            memset(&lines, 0, sizeof(lines));
            lines.count = 1;
            lines.lines[0] = (VjoOcrLine){0, 0, image->width, image->height};
            rc = VJO_OK;
        }
    } else {
        rc = vjo_ocr_find_lines(image, &lines);
    }
    if (rc) return rc;
    stats->line_boxes = lines;
    if (!lines.count) return VJO_OK;
    stats->lines = lines.count;
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (!input || input_elements < MEIKI_PREPROCESS_ELEMENTS || !scratch ||
        scratch_bytes < MEIKI_PREPROCESS_SCRATCH_BYTES) return VJO_E_OOM;
    if (cap > VJO_OCR_TEXT_CAP) cap = VJO_OCR_TEXT_CAP;
    MeikiRgbaImage rgba = {image->ud, image->width, image->height,
                           image->rows, image->cancelled};
    for (unsigned l = 0; l < lines.count; ++l) {
        const VjoOcrLine *line = &lines.lines[l];
        MeikiCrop crop = {line->x, line->y, line->w, line->h};
        MeikiPreprocessInfo info;
        MeikiOutput output = {0};
        MeikiStats line_stats = {0};
        char decoded[MEIKI_OUTPUT_CHARACTERS * 4 + 1];
        if (cancelled(image)) { rc = VJO_E_CANCELLED; break; }
        rc = meiki_preprocess_rgba(&rgba, &crop, input, input_elements,
                                   scratch, scratch_bytes, &info);
        if (rc) { rc = preprocessing_error(rc); break; }
        if (cancelled(image)) { rc = VJO_E_CANCELLED; break; }
        rc = engine->run(engine->ud, path, input, &output, &line_stats);
        add_stats(stats, &line_stats);
        rc = engine_result(image, rc, &line_stats);
        if (rc) break;
        if (meiki_decode(&output, crop.w, info.effective_w,
                         decoded, sizeof(decoded)) < 0) {
            rc = VJO_E_OCR_INFERENCE;
            break;
        }
        size_t n = strlen(decoded), separator = used && n ? 1 : 0;
        if (n && (separator >= cap - used || n >= cap - used - separator)) {
            rc = VJO_E_TOO_LARGE;
            break;
        }
        if (separator) text[used++] = '\n';
        if (n) memcpy(text + used, decoded, n);
        used += n;
        text[used] = 0;
        stats->completed_lines++;
    }
    if (rc) text[0] = 0;
    return rc;
}

int vjo_meiki_ocr(const char *dir, const VjoOcrImage *image,
                  const VjoMeikiEngine *engine,
                  float *input, size_t input_elements,
                  void *scratch, size_t scratch_bytes,
                  char *text, size_t cap, VjoMeikiStats *stats)
{
    return recognize(dir, image, engine, input, input_elements,
                     scratch, scratch_bytes, text, cap, stats, PROJECTED_LINES);
}

int vjo_meiki_ocr_single_line(const char *dir, const VjoOcrImage *image,
                             const VjoMeikiEngine *engine,
                             float *input, size_t input_elements,
                             void *scratch, size_t scratch_bytes,
                             char *text, size_t cap, VjoMeikiStats *stats)
{
    return recognize(dir, image, engine, input, input_elements,
                     scratch, scratch_bytes, text, cap, stats, SINGLE_LINE);
}

int vjo_meiki_ocr_detected(const char *dir, const VjoOcrImage *image,
                          const VjoMeikiEngine *engine,
                          float *input, size_t input_elements,
                          void *scratch, size_t scratch_bytes,
                          char *text, size_t cap, VjoMeikiStats *stats)
{
    return recognize(dir, image, engine, input, input_elements,
                     scratch, scratch_bytes, text, cap, stats, DETECTED_LINES);
}
