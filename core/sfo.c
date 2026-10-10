#include "sfo.h"
#include <stdint.h>
#include <string.h>
static uint32_t u32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
int vjo_sfo_string(const void *data, size_t len, const char *key, char *out, size_t cap)
{
    const uint8_t *p = data;
    if (!p || !key || !out || !cap || len < 20 || u32(p) != 0x46535000) return -1;
    uint32_t keys = u32(p + 8), values = u32(p + 12), count = u32(p + 16);
    if (count > (len - 20) / 16 || keys < 20 + count * 16 || keys >= len ||
        values < keys || values > len) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = p + 20 + i * 16;
        uint32_t ko = u16(e), n = u32(e + 4), capacity = u32(e + 8), off = u32(e + 12);
        if (ko >= values - keys || !memchr(p + keys + ko, 0, values - keys - ko)) return -1;
        if (strcmp((const char *)p + keys + ko, key)) continue;
        if (u16(e + 2) != 0x0204 || !n || n > capacity || off > len - values ||
            capacity > len - values - off) return -1;
        const char *s = (const char *)p + values + off;
        const char *end = memchr(s, 0, n);
        if (!end || (size_t)(end - s) >= cap) return -1;
        memcpy(out, s, (size_t)(end - s) + 1);
        return 0;
    }
    return -1;
}
