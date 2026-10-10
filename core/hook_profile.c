#include "hook_profile.h"
#include "../include/vjo_text.h"
#include <string.h>

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}

static int number(const char *s, uint32_t *out)
{
    uint32_t value = 0;
    unsigned count = 0;
    if (s[0] == '0' && s[1] == 'x') s += 2;
    while (*s) {
        int d = hex(*s++);
        if (d < 0 || count++ >= 8) return -1;
        value = (value << 4) | (unsigned)d;
    }
    if (!count) return -1;
    *out = value;
    return 0;
}

int vjo_hook_profile_parse(VjoNativeProfile *p, const char *data, size_t bytes)
{
    size_t pos = 0;
    int header = 0;
    memset(p, 0, sizeof(*p));
    while (pos < bytes) {
        char line[192], *words[9];
        unsigned n = 0;
        size_t end = pos;
        while (end < bytes && data[end] != '\n') end++;
        if (end-pos >= sizeof(line)) return -1;
        for (size_t i = pos; i < end; i++) if (!data[i]) return -1;
        memcpy(line, data+pos, end-pos); line[end-pos] = 0;
        pos = end < bytes ? end+1 : end;
        char *s = line;
        while (*s) {
            while (*s == ' ' || *s == '\t' || *s == '\r') s++;
            if (!*s || *s == '#') break;
            if (n == sizeof(words)/sizeof(*words)) return -1;
            words[n++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\r') s++;
            if (*s) *s++ = 0;
        }
        if (!n) continue;
        if (!header) {
            size_t title_len;
            if (n != 3 || strcmp(words[0], "VJOHOOK1") ||
                (title_len = strlen(words[1])) != 9 || number(words[2], &p->module_nid) ||
                !p->module_nid) return -1;
            for (size_t i = 0; i < title_len; i++)
                if (!((words[1][i] >= 'A' && words[1][i] <= 'Z') ||
                      (words[1][i] >= '0' && words[1][i] <= '9'))) return -1;
            memcpy(p->title, words[1], title_len+1); header = 1;
        } else {
            VjoNativeHook *h;
            uint32_t *fields[6], pad;
            const char *padding;
            size_t sig;
            if (n != 8 || p->count == VJO_NATIVE_HOOKS) return -1;
            h = &p->hooks[p->count];
            fields[0] = &h->segment; fields[1] = &h->offset; fields[2] = &h->thumb;
            fields[3] = &h->reg; fields[4] = &h->encoding; fields[5] = &h->indirections;
            for (unsigned i = 0; i < 6; i++) if (number(words[i], fields[i])) return -1;
            padding = words[6];
            if (*padding == '-') padding++;
            if (number(padding, &pad) || pad > 0x100000) return -1;
            h->padding = words[6][0] == '-' ? -(int32_t)pad : (int32_t)pad;
            if (h->segment > 3 || h->thumb > 1 || h->reg > 14 || h->indirections > 4 ||
                h->encoding < VJO_TEXT_UTF8 || h->encoding > VJO_TEXT_CP932 ||
                (h->offset & (h->thumb ? 1u : 3u))) return -1;
            sig = strlen(words[7]);
            if (sig < 16 || sig > 32 || sig % 2) return -1;
            if ((h->offset & 2u) && sig < 24) return -1;
            h->signature_bytes = (unsigned)(sig/2);
            for (unsigned i = 0; i < h->signature_bytes; i++) {
                int hi = hex(words[7][i*2]), lo = hex(words[7][i*2+1]);
                if (hi < 0 || lo < 0) return -1;
                h->signature[i] = (uint8_t)((hi << 4) | lo);
            }
            p->count++;
        }
    }
    return header && p->count ? 0 : -1;
}
