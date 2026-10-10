#include "text_scan.h"
#include <string.h>

void vjo_text_scan(VjoTextSources *s, const uint8_t *data, size_t bytes, size_t first, size_t starts,
                   uint32_t base, uint32_t mapping)
{
    char text[VJO_TEXT_BYTES];
    uint32_t cp[VJO_TEXT_CODEPOINTS];
    if (starts > bytes) starts = bytes;
    for (size_t i = first; i < starts; i++) {
        for (int encoding = VJO_TEXT_UTF8; encoding <= VJO_TEXT_CP932; encoding++) {
            size_t end = i, unit = encoding == VJO_TEXT_UTF16LE ? 2u : 1u;
            unsigned japanese;
            int n;
            if (unit == 2 && ((base + (uint32_t)i) & 1)) continue;
            /* A C string boundary. This avoids returning suffixes of the same
             * string as separate sources. Backends supply up to two preceding
             * bytes so a page boundary cannot create a false string suffix. */
            if (i >= unit && (data[i-unit] || (unit == 2 && data[i-1]))) continue;
            if (!data[i] && (unit == 1 || i+1 >= bytes || !data[i+1])) continue;
            while (end + unit <= bytes && end - i < VJO_TEXT_BYTES) {
                if (!data[end] && (unit == 1 || !data[end+1])) break;
                end += unit;
            }
            if (end + unit > bytes || end - i >= VJO_TEXT_BYTES) continue;
            if (vjo_text_decode(encoding, data + i, end-i, text, sizeof(text)) < 0) continue;
            n = vjo_text_normalize(text, cp, VJO_TEXT_CODEPOINTS, &japanese);
            /* Binary sometimes decodes as kanji. Require multiple characters
             * and at least half Japanese; this is a discovery heuristic. */
            if (n < 4 || japanese < 2 || japanese * 2 < (unsigned)n) continue;
            vjo_text_offer(s, VJO_TEXT_MEMORY, (unsigned)encoding, base + (uint32_t)i, mapping, text);
        }
        /* A pointer to a discovered string supplies a source that can follow
         * script-buffer changes. Pointer reads stay in the backend; this only
         * discovers the location of the pointer, never dereferences it. */
        if (!((base + (uint32_t)i) & 3) && bytes - i >= 4) {
            uint32_t ptr = data[i] | ((uint32_t)data[i+1] << 8) |
                           ((uint32_t)data[i+2] << 16) | ((uint32_t)data[i+3] << 24);
            for (unsigned j = 0; j < VJO_TEXT_SOURCES; j++) {
                const VjoTextCandidate *c = &s->items[j];
                if (c->id && c->kind == VJO_TEXT_MEMORY && c->address == ptr) {
                    /* offer copies the string before it can evict its source. */
                    memcpy(text, c->text, sizeof(text));
                    vjo_text_offer(s, VJO_TEXT_POINTER, c->encoding, base + (uint32_t)i, mapping, text);
                    break;
                }
            }
        }
    }
}
