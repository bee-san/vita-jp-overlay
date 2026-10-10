#ifndef VJO_TEXT_SOURCE_H
#define VJO_TEXT_SOURCE_H
#include "../include/vjo_text.h"
#include <stddef.h>

#define VJO_TEXT_CODEPOINTS 256
typedef struct {
    VjoTextCandidate items[VJO_TEXT_SOURCES];
    uint32_t selected, sequence, next_id;
    uint32_t reference[VJO_TEXT_CODEPOINTS];
    unsigned reference_length;
} VjoTextSources;

/* Strict decoding: invalid, binary and oversized strings are rejected, never
 * silently truncated into a misleading match. Returns UTF-8 bytes, or -1. */
int vjo_text_decode(int encoding, const void *raw, size_t bytes, char *out, size_t cap);
int vjo_text_normalize(const char *text, uint32_t *out, unsigned cap, unsigned *japanese);
unsigned vjo_text_similarity(const uint32_t *a, unsigned na, const uint32_t *b, unsigned nb);
void vjo_text_sources_init(VjoTextSources *sources);
int vjo_text_reference(VjoTextSources *sources, const char *text);
/* Stable stream identity = kind/encoding/address/locator. Returns source ID,
 * or 0 when rejected. Selected entries cannot be evicted. */
uint32_t vjo_text_offer(VjoTextSources *sources, unsigned kind, unsigned encoding,
                        uint32_t address, uint32_t locator, const char *text);
void vjo_text_forget(VjoTextSources *sources, uint32_t id);
VjoTextCandidate *vjo_text_find(VjoTextSources *sources, uint32_t id);
unsigned vjo_text_top(const VjoTextSources *sources, VjoTextCandidate *out);
/* Exactly 80% is accepted. Short/non-Japanese/ambiguous matches need a user. */
uint32_t vjo_text_auto_select(const VjoTextCandidate *choices, unsigned count);
#endif
