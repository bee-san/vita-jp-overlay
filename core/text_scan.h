#ifndef VJO_TEXT_SCAN_H
#define VJO_TEXT_SCAN_H
#include "text_source.h"
/* data includes up to VJO_TEXT_BYTES of lookahead. Only starts in [first, starts)
 * are considered, so strings split across pages are retained without duplicates.
 * mapping is an allocation identity; changing it invalidates a saved buffer. */
void vjo_text_scan(VjoTextSources *sources, const uint8_t *data, size_t bytes,
                   size_t first, size_t starts, uint32_t base, uint32_t mapping);
#endif
