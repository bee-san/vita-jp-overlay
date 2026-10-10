#include "text_source.h"
#include "utf.h"
#include <string.h>
#include "cp932_table.inc"

static int cp932_next(const uint8_t *raw, size_t n, size_t *i, uint32_t *cp)
{
    unsigned a = raw[(*i)++], b, lead, trail;
    if (a < 0x80) { *cp = a; return 0; }
    if (a >= 0xA1 && a <= 0xDF) { *cp = 0xFF61 + a - 0xA1; return 0; }
    if (*i >= n) return -1;
    b = raw[(*i)++];
    if (a >= 0x81 && a <= 0x9F) lead = a - 0x81;
    else if (a >= 0xE0 && a <= 0xFC) lead = a - 0xE0 + 31;
    else return -1;
    if (b >= 0x40 && b <= 0x7E) trail = b - 0x40;
    else if (b >= 0x80 && b <= 0xFC) trail = b - 0x80 + 63;
    else return -1;
    *cp = cp932_pairs[lead * 188 + trail];
    return *cp ? 0 : -1;
}

int vjo_text_decode(int enc, const void *data, size_t n, char *out, size_t cap)
{
    const uint8_t *raw = data;
    size_t i = 0, w = 0;
    unsigned count = 0;
    if (!raw || !out || !cap || enc < VJO_TEXT_AUTO || enc > VJO_TEXT_CP932) return -1;
    if (enc == VJO_TEXT_AUTO) {
        unsigned high = 0;
        for (size_t k = 0; k < n; k++) high |= raw[k] & 0x80;
        out[0] = 0;
        if (!high) return 0;
        int rc = vjo_text_decode(VJO_TEXT_UTF8, raw, n, out, cap);
        return rc >= 0 ? rc : vjo_text_decode(VJO_TEXT_CP932, raw, n, out, cap);
    }
    while (i < n) {
        uint32_t cp;
        char encoded[4];
        int len;
        if (enc == VJO_TEXT_UTF8) {
            size_t start = i;
            cp = vjo_utf8_next((const char *)raw, n, &i);
            if (cp == 0xFFFD || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
                (size_t)vjo_utf8_encode(cp, encoded) != i-start) return -1;
        } else if (enc == VJO_TEXT_UTF16LE) {
            if (n - i < 2) return -1;
            cp = raw[i] | ((uint32_t)raw[i+1] << 8); i += 2;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t low;
                if (n - i < 2) return -1;
                low = raw[i] | ((uint32_t)raw[i+1] << 8); i += 2;
                if (low < 0xDC00 || low > 0xDFFF) return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + low - 0xDC00;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) return -1;
        } else if (cp932_next(raw, n, &i, &cp)) return -1;
        if (!cp) break;
        if ((cp < 0x20 && cp != '\n' && cp != '\r' && cp != '\t') ||
            (cp >= 0x7F && cp < 0xA0) || cp == 0xFFFF || cp == 0xFFFE) return -1;
        if (++count > VJO_TEXT_CODEPOINTS) return -1;
        len = vjo_utf8_encode(cp, encoded);
        if (w + (size_t)len >= cap) return -1;
        memcpy(out + w, encoded, (size_t)len); w += (size_t)len;
    }
    out[w] = 0;
    return (int)w;
}

static int ignored(uint32_t cp)
{
    return cp <= 0x20 || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x206F) ||
           (cp >= 0x3001 && cp <= 0x303F) || cp == 0xFF61 || cp == 0xFF62 ||
           cp == 0xFF63 || cp == 0xFF64 ||
           (cp >= 0x21 && cp <= 0x2F) || (cp >= 0x3A && cp <= 0x40) ||
           (cp >= 0x5B && cp <= 0x60) || (cp >= 0x7B && cp <= 0x7E);
}

int vjo_text_normalize(const char *s, uint32_t *out, unsigned cap, unsigned *jp)
{
    size_t i = 0, len = strlen(s);
    unsigned n = 0, japanese = 0;
    while (i < len) {
        size_t start = i;
        char encoded[4];
        uint32_t cp = vjo_utf8_next(s, len, &i);
        if (cp == 0xFFFD || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ||
            (size_t)vjo_utf8_encode(cp, encoded) != i-start) return -1;
        if (cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;
        if (ignored(cp)) continue;
        if (n >= cap) return -1;
        out[n++] = cp;
        if (vjo_is_japanese_char(cp) || (cp >= 0xFF65 && cp <= 0xFF9F) ||
            (cp >= 0x20000 && cp <= 0x2FA1F)) japanese++;
    }
    if (jp) *jp = japanese;
    return (int)n;
}

unsigned vjo_text_similarity(const uint32_t *a, unsigned na, const uint32_t *b, unsigned nb)
{
    uint16_t row[VJO_TEXT_CODEPOINTS + 1];
    unsigned i, j, longest = na > nb ? na : nb;
    if (!na || !nb || na > VJO_TEXT_CODEPOINTS || nb > VJO_TEXT_CODEPOINTS) return 0;
    for (j = 0; j <= nb; j++) row[j] = (uint16_t)j;
    for (i = 1; i <= na; i++) {
        unsigned diag = row[0];
        row[0] = (uint16_t)i;
        for (j = 1; j <= nb; j++) {
            unsigned old = row[j], best = diag + (a[i-1] != b[j-1]);
            if (old + 1 < best) best = old + 1;
            if (row[j-1] + 1u < best) best = row[j-1] + 1u;
            row[j] = (uint16_t)best;
            diag = old;
        }
    }
    return (longest - row[nb]) * 100u / longest;
}

static void score(VjoTextSources *s, VjoTextCandidate *c, const uint32_t *cp, unsigned n)
{
    c->score = s->reference_length ? vjo_text_similarity(s->reference, s->reference_length, cp, n) : 0;
}

void vjo_text_sources_init(VjoTextSources *s)
{
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
}

int vjo_text_reference(VjoTextSources *s, const char *text)
{
    uint32_t cp[VJO_TEXT_CODEPOINTS];
    int n = vjo_text_normalize(text, cp, VJO_TEXT_CODEPOINTS, NULL);
    if (n < 0) return -1;
    memcpy(s->reference, cp, (size_t)n * sizeof(*cp));
    s->reference_length = (unsigned)n;
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++) {
        VjoTextCandidate *c = &s->items[i];
        if (!c->id) continue;
        n = vjo_text_normalize(c->text, cp, VJO_TEXT_CODEPOINTS, NULL);
        score(s, c, cp, n < 0 ? 0 : (unsigned)n);
    }
    s->sequence++;
    return 0;
}

/* Japanese first, then OCR score. Prefer the Japanese fraction and length of
 * readable text before activity: frequent short/binary copy streams must not
 * hide complete dialogue. Equally useful streams favor observed updates. */
static int better(const VjoTextCandidate *a, const VjoTextCandidate *b)
{
    if (!a->id) return 0;
    if (!b->id) return 1;
    if (!!a->japanese != !!b->japanese) return a->japanese != 0;
    if (a->score != b->score) return a->score > b->score;
    if (a->japanese * b->length != b->japanese * a->length)
        return a->japanese * b->length > b->japanese * a->length;
    if (a->length != b->length) return a->length > b->length;
    if (a->updates != b->updates) return a->updates > b->updates;
    return a->id < b->id;
}

uint32_t vjo_text_offer(VjoTextSources *s, unsigned kind, unsigned enc, uint32_t addr,
                        uint32_t locator, const char *text)
{
    uint32_t cp[VJO_TEXT_CODEPOINTS];
    unsigned japanese;
    int n;
    VjoTextCandidate candidate, *target = NULL, *worst = NULL;
    size_t bytes = strlen(text);
    if (bytes >= VJO_TEXT_BYTES || !bytes) return 0;
    n = vjo_text_normalize(text, cp, VJO_TEXT_CODEPOINTS, &japanese);
    if (n <= 0) return 0;
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++) {
        VjoTextCandidate *c = &s->items[i];
        if (c->id && c->kind == kind && c->encoding == enc && c->address == addr && c->locator == locator) {
            if (!strcmp(c->text, text)) return c->id;
            target = c;
            break;
        }
        if ((!c->id || c->id != s->selected) && (!worst || better(worst, c))) worst = c;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.id = target ? target->id : s->next_id;
    candidate.kind = kind; candidate.encoding = enc;
    candidate.address = addr; candidate.locator = locator;
    candidate.japanese = japanese; candidate.length = (unsigned)n;
    candidate.updates = target ? target->updates + 1 : 1;
    score(s, &candidate, cp, (unsigned)n);
    memcpy(candidate.text, text, bytes + 1);
    if (!target) {
        if (!worst || (worst->id && !better(&candidate, worst))) return 0;
        target = worst;
        if (++s->next_id == 0) s->next_id = 1;
    }
    *target = candidate;
    s->sequence++;
    return target->id;
}

VjoTextCandidate *vjo_text_find(VjoTextSources *s, uint32_t id)
{
    if (id) for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++)
        if (s->items[i].id == id) return &s->items[i];
    return NULL;
}

void vjo_text_forget(VjoTextSources *s, uint32_t id)
{
    VjoTextCandidate *c = vjo_text_find(s, id);
    if (!c) return;
    memset(c, 0, sizeof(*c));
    if (s->selected == id) s->selected = 0;
    s->sequence++;
}

unsigned vjo_text_top(const VjoTextSources *s, VjoTextCandidate *out)
{
    unsigned n = 0;
    memset(out, 0, VJO_TEXT_CHOICES * sizeof(*out));
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++) {
        const VjoTextCandidate *c = &s->items[i];
        unsigned pos = 0;
        if (!c->id || !c->text[0]) continue;
        while (pos < n && !better(c, &out[pos])) pos++;
        if (pos >= VJO_TEXT_CHOICES) continue;
        if (n < VJO_TEXT_CHOICES) n++;
        for (unsigned j = n - 1; j > pos; j--) out[j] = out[j-1];
        out[pos] = *c;
    }
    return n;
}
