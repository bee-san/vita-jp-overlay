#include "local_ocr.h"
#include <string.h>

int vjo_ocr_find_lines(const VjoOcrImage *im, VjoOcrLines *out)
{
    unsigned char row[VJO_OCR_MAX_WIDTH * 4];
    unsigned left[VJO_OCR_MAX_HEIGHT], right[VJO_OCR_MAX_HEIGHT];
    unsigned start = 0, last = 0, active = 0;
    if (!im || !out || !im->rows || !im->width || !im->height ||
        im->width > VJO_OCR_MAX_WIDTH || im->height > VJO_OCR_MAX_HEIGHT)
        return VJO_E_OCR_REGION;
    memset(out, 0, sizeof(*out));
    for (unsigned y = 0; y <= im->height; y++) {
        unsigned count = 0;
        if (im->cancelled && im->cancelled(im->ud)) return VJO_E_CANCELLED;
        if (y < im->height) {
            if (im->rows(im->ud, y, 1, row) != 1) return VJO_E_SOURCE;
            left[y] = im->width; right[y] = 0;
            for (unsigned x = 0; x < im->width; x++) {
                const unsigned char *p = row + x * 4;
                if (p[0] >= 180 && p[1] >= 180 && p[2] >= 180) {
                    if (x < left[y]) left[y] = x;
                    right[y] = x + 1; count++;
                }
            }
            /* A light background is not the supported text-box layout. */
            if (count > im->width * 3 / 4) return VJO_E_OCR_REGION;
        }
        if (count >= 2) {
            if (!active) { start = y; active = 1; }
            last = y;
        }
        /* Join small gaps within glyphs; preserve gaps between dialogue rows. */
        if (active && (y == im->height || y - last >= 3)) {
            unsigned h = last - start + 1;
            if (h >= 8) {
                unsigned x1 = im->width, x2 = 0;
                if (h > VJO_OCR_MAX_LINE_HEIGHT - 8 || out->count == VJO_OCR_MAX_LINES)
                    return VJO_E_OCR_REGION;
                for (unsigned yy = start; yy <= last; yy++) {
                    if (left[yy] < x1) x1 = left[yy];
                    if (right[yy] > x2) x2 = right[yy];
                }
                if (x2 > x1) {
                    VjoOcrLine *line = &out->lines[out->count++];
                    line->x = x1 > 4 ? x1 - 4 : 0;
                    line->y = start > 4 ? start - 4 : 0;
                    unsigned endx = x2 + 4 < im->width ? x2 + 4 : im->width;
                    unsigned endy = last + 5 < im->height ? last + 5 : im->height;
                    line->w = endx - line->x; line->h = endy - line->y;
                }
            }
            active = 0;
        }
    }
    return VJO_OK;
}

int vjo_ocr_ctc_append(unsigned token, unsigned *last, const char *const *vocab,
                      unsigned count, char *text, size_t cap, size_t *used)
{
    if (!last || !vocab || !text || !used || !cap || *used >= cap || token > count)
        return VJO_E_PARSE;
    if (token && token != *last) {
        size_t n = strlen(vocab[token - 1]);
        if (n >= cap - *used) return VJO_E_TOO_LARGE;
        memcpy(text + *used, vocab[token - 1], n);
        *used += n;
        text[*used] = 0;
    }
    *last = token;
    return VJO_OK;
}
