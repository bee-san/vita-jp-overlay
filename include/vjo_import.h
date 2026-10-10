/* Inspect a Vita ARM import without executing it. A weak import is not always
 * an error-return stub: retail firmware can replace an unresolved import with
 * `sub ip, pc, #8; mov pc, #0`, which crashes its caller. */
#ifndef VJO_IMPORT_H
#define VJO_IMPORT_H

#include <stdint.h>

static inline int vjo_import_stub_ready(const volatile uint32_t *stub)
{
    uint32_t first = stub[0], second = stub[1];
    if (first == 0xE24FC008u &&
        (second == 0xE3A0F000u || second == 0xE12FFF1Eu))
        return 0;
    if (first == 0xE3E00000u && second == 0xE12FFF1Eu)
        return 0;
    return 1;
}

#define VJO_IMPORT_READY(fn) vjo_import_stub_ready((const volatile uint32_t *)(uintptr_t)(fn))

#endif
