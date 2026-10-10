/* Metadata-only diagnostic carried in result.text exclusively for MODEL_IO.
 * It contains no paths, model bytes or recognized text. The API9 wire layout
 * stays unchanged; every other failed result must have empty text. */
#ifndef VJO_GAME_OCR_DIAG_H
#define VJO_GAME_OCR_DIAG_H

#include <stddef.h>
#include <stdint.h>

#define VJO_GAME_OCR_IO_DIAG_BYTES 29u
typedef struct {
    char asset, stage;
    uint32_t code;
    char control_stage;
    uint32_t control_code;
} VjoGameOcrIoDiagnostic;

static inline int vjo_game_ocr_io_fields_valid(const VjoGameOcrIoDiagnostic *d)
{
    return d && (d->asset == 'R' || d->asset == 'D') &&
        (d->stage == 'O' || d->stage == 'S' || d->stage == 'L' || d->stage == 'R') &&
        (d->control_stage == 'P' || d->control_stage == 'O' ||
         d->control_stage == 'R' || d->control_stage == 'C') &&
        (d->control_stage != 'P' || d->control_code == 0);
}

static inline int vjo_game_ocr_io_write(char *text, size_t capacity,
                                       const VjoGameOcrIoDiagnostic *d)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!text || capacity < VJO_GAME_OCR_IO_DIAG_BYTES ||
        !vjo_game_ocr_io_fields_valid(d)) return -1;
    text[0] = 'M'; text[1] = 'I'; text[2] = 'O'; text[3] = '1';
    text[4] = ':'; text[5] = d->asset; text[6] = ':'; text[7] = d->stage;
    text[8] = ':'; text[17] = ':'; text[18] = d->control_stage; text[19] = ':';
    for (unsigned i = 0; i < 8; i++) {
        unsigned shift = (7u-i)*4u;
        text[9+i] = hex[(d->code >> shift)&15u];
        text[20+i] = hex[(d->control_code >> shift)&15u];
    }
    text[28] = 0;
    return 0;
}

static inline int vjo_game_ocr_io_parse(const char *text, size_t capacity,
                                       VjoGameOcrIoDiagnostic *out)
{
    if (!text || !out || capacity < VJO_GAME_OCR_IO_DIAG_BYTES ||
        text[0] != 'M' || text[1] != 'I' || text[2] != 'O' || text[3] != '1' ||
        text[4] != ':' || text[6] != ':' || text[8] != ':' ||
        text[17] != ':' || text[19] != ':' || text[28] != 0) return -1;
    VjoGameOcrIoDiagnostic d = {text[5], text[7], 0, text[18], 0};
    for (unsigned i = 0; i < 8; i++) {
        char a = text[9+i], b = text[20+i];
        unsigned av, bv;
        if (a >= '0' && a <= '9') av = (unsigned)(a-'0');
        else if (a >= 'A' && a <= 'F') av = (unsigned)(a-'A')+10u;
        else return -1;
        if (b >= '0' && b <= '9') bv = (unsigned)(b-'0');
        else if (b >= 'A' && b <= 'F') bv = (unsigned)(b-'A')+10u;
        else return -1;
        d.code = (d.code << 4)|av;
        d.control_code = (d.control_code << 4)|bv;
    }
    if (!vjo_game_ocr_io_fields_valid(&d)) return -1;
    *out = d;
    return 0;
}

#endif
