#include "local_dict.h"

#include <string.h>
#include "port.h"
#include "utf.h"

#define HEADER_SIZE 64u
#define INDEX_SIZE 24u
#define PAGE_SIZE 4096u
#define MAX_CANDIDATES 128
#define MAX_SCAN_CHARS 32
#define MAX_HOMONYMS 64
#define RULE_MASK 127u

typedef struct {
    const char *in, *out;
    uint8_t in_len, out_len;
    uint16_t rules_in, rules_out;
} DeinflectRule;
#include "deinflect.inc"

typedef struct {
    char key[VJO_LOCAL_MAX_KEY + 1];
    uint16_t rules, len;
} Candidate;

typedef struct {
    VjoFile file;
    uint32_t count;
    uint64_t data;
} Dictionary;

typedef struct {
    Dictionary *dict;
    uint64_t off;
    size_t len;
    uint8_t bytes[PAGE_SIZE];
} Page;

typedef struct {
    uint64_t key_off, entry_off;
    uint32_t entry_len;
    uint16_t key_len, rules;
} Record;

typedef struct {
    int dict;
    Record record;
} Match;

typedef struct {
    Dictionary dicts[VJO_LOCAL_MAX_DICTS];
    Page index_page, data_page;
    Candidate candidates[MAX_CANDIDATES];
    Match matches[VJO_LOCAL_MAX_MATCHES];
    uint64_t seen_off[VJO_LOCAL_MAX_RESULTS];
    int seen_dict[VJO_LOCAL_MAX_RESULTS];
    int n_dicts, n_matches;
} Workspace;

typedef char WorkspaceFits[(sizeof(Workspace) <= VJO_LOCAL_SCRATCH_BUDGET) ? 1 : -1];

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }

/* One filename at a time, bounded even for malformed config. */
static int next_name(const char **cursor, char name[64])
{
    const char *s = *cursor;
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    while (*s && *s != ',') {
        if (n >= 63) return -1;
        name[n++] = *s++;
    }
    while (n && (name[n - 1] == ' ' || name[n - 1] == '\t')) n--;
    name[n] = 0;
    if (n < 8 || name[0] == '.' || strstr(name, "..") || strcmp(name + n - 7, ".vjdict")) return -1;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return -1;
    }
    if (*s == ',') {
        s++;
        const char *tail = s;
        while (*tail == ' ' || *tail == '\t') tail++;
        if (!*tail) return -1;
    }
    *cursor = s;
    return 1;
}

int vjo_local_names_valid(const char *names)
{
    char name[64];
    int n = 0, rc;
    if (!names) return 0;
    while ((rc = next_name(&names, name)) > 0)
        if (++n > VJO_LOCAL_MAX_DICTS) return 0;
    return rc == 0 && n > 0;
}

static int range_ok(Dictionary *d, uint64_t off, size_t n)
{
    return off <= d->file.size && n <= d->file.size - off;
}

static int cached_read(Page *page, Dictionary *d, uint64_t off, void *dst, size_t n)
{
    uint8_t *out = dst;
    if (!range_ok(d, off, n)) return VJO_E_LOCAL_FORMAT;
    while (n) {
        uint64_t base = off & ~(uint64_t)(PAGE_SIZE - 1);
        size_t pos = (size_t)(off - base), take;
        if (page->dict != d || page->off != base) {
            uint64_t remaining = d->file.size - base;
            page->dict = NULL;
            page->off = base;
            page->len = remaining < PAGE_SIZE ? (size_t)remaining : PAGE_SIZE;
            if (d->file.read(d->file.ctx, base, page->bytes, page->len)) return VJO_E_LOCAL_IO;
            page->dict = d;
        }
        take = page->len - pos;
        if (take > n) take = n;
        memcpy(out, page->bytes + pos, take);
        off += take; out += take; n -= take;
    }
    return VJO_OK;
}

static int open_dictionaries(Workspace *w, const VjoPlatform *p, const VjoConfig *cfg, VjoErr *err)
{
    const char *names = cfg->local_dictionaries;
    char name[64], path[256];
    if (!p->file_open || !vjo_local_names_valid(names) || !cfg->local_dictionary_dir[0]) {
        err->detail = "Set local_dictionaries to up to 8 .vjdict filenames in config.ini";
        return VJO_E_LOCAL_IO;
    }
    while (next_name(&names, name) > 0) {
        uint8_t h[HEADER_SIZE];
        Dictionary *d = &w->dicts[w->n_dicts];
        int n = vjo_snprintf(path, sizeof(path), "%s/%s", cfg->local_dictionary_dir, name);
        if (n < 0 || (size_t)n >= sizeof(path) || p->file_open(p->ud, path, &d->file)) {
            err->detail = "Cannot open a configured dictionary; check local_dictionary_dir and local_dictionaries";
            return VJO_E_LOCAL_IO;
        }
        w->n_dicts++; /* close even if header validation fails */
        if (d->file.size < HEADER_SIZE) return VJO_E_LOCAL_FORMAT;
        if (!d->file.read || !d->file.close || d->file.read(d->file.ctx, 0, h, sizeof(h))) return VJO_E_LOCAL_IO;
        d->count = u32(h + 16);
        d->data = u64(h + 32);
        if (memcmp(h, "VJDICT1\0", 8) || u32(h + 8) != 1 || u32(h + 12) != HEADER_SIZE ||
            u32(h + 20) != INDEX_SIZE || u64(h + 24) != HEADER_SIZE ||
            d->data != HEADER_SIZE + (uint64_t)d->count * INDEX_SIZE ||
            d->data > d->file.size || u64(h + 40) != d->file.size || !d->count ||
            d->file.size > INT64_MAX || u64(h + 48) || u64(h + 56)) return VJO_E_LOCAL_FORMAT;
    }
    return VJO_OK;
}

static int record_at(Workspace *w, Dictionary *d, uint32_t i, Record *r, char *key)
{
    uint8_t bytes[INDEX_SIZE];
    int rc = cached_read(&w->index_page, d, HEADER_SIZE + (uint64_t)i * INDEX_SIZE, bytes, sizeof(bytes));
    if (rc) return rc;
    r->key_off = u64(bytes); r->entry_off = u64(bytes + 8);
    r->key_len = u16(bytes + 16); r->rules = u16(bytes + 18); r->entry_len = u32(bytes + 20);
    if (!r->key_len || r->key_len > VJO_LOCAL_MAX_KEY || (r->rules & ~RULE_MASK) ||
        r->key_off < d->data || r->entry_off < d->data || r->entry_len < 4 ||
        r->entry_len > VJO_LOCAL_MAX_ENTRY || !range_ok(d, r->entry_off, r->entry_len)) return VJO_E_LOCAL_FORMAT;
    rc = cached_read(&w->data_page, d, r->key_off, key, r->key_len);
    if (rc) return rc;
    if (memchr(key, 0, r->key_len)) return VJO_E_LOCAL_FORMAT;
    key[r->key_len] = 0;
    return VJO_OK;
}

static int find_matches(Workspace *w, int di, const Candidate *c)
{
    Dictionary *d = &w->dicts[di];
    uint32_t lo = 0, hi = d->count;
    Record r;
    char key[VJO_LOCAL_MAX_KEY + 1];
    int rc;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if ((rc = record_at(w, d, mid, &r, key))) return rc;
        if (strcmp(key, c->key) < 0) lo = mid + 1; else hi = mid;
    }
    /* Bound duplicate-heavy dictionaries as well as result storage. */
    for (unsigned scanned = 0; lo < d->count && scanned < MAX_HOMONYMS; lo++, scanned++) {
        int duplicate = 0;
        if ((rc = record_at(w, d, lo, &r, key))) return rc;
        if (strcmp(key, c->key)) break;
        if (c->rules && !(c->rules & r.rules)) continue;
        for (int j = 0; j < w->n_matches; j++)
            if (w->matches[j].dict == di && w->matches[j].record.entry_off == r.entry_off) duplicate = 1;
        if (duplicate) continue;
        if (w->n_matches >= VJO_LOCAL_MAX_MATCHES) break;
        w->matches[w->n_matches].dict = di;
        w->matches[w->n_matches++].record = r;
    }
    return VJO_OK;
}

static int deinflect(Candidate *c, const char *key)
{
    int count = 1;
    size_t len = strlen(key);
    memcpy(c[0].key, key, len + 1); c[0].len = (uint16_t)len; c[0].rules = 0;
    for (int i = 0; i < count; i++) {
        for (unsigned r = 0; r < sizeof(deinflect_rules) / sizeof(deinflect_rules[0]); r++) {
            const DeinflectRule *rule = &deinflect_rules[r];
            size_t stem, n;
            char next[VJO_LOCAL_MAX_KEY + 1];
            int duplicate = 0;
            if ((c[i].rules && !(c[i].rules & rule->rules_in)) || c[i].len < rule->in_len) continue;
            stem = c[i].len - rule->in_len;
            if (memcmp(c[i].key + stem, rule->in, rule->in_len)) continue;
            n = stem + rule->out_len;
            if (!n || n > VJO_LOCAL_MAX_KEY) continue;
            memcpy(next, c[i].key, stem); memcpy(next + stem, rule->out, rule->out_len); next[n] = 0;
            for (int j = 0; j < count; j++)
                if (c[j].rules == rule->rules_out && !strcmp(next, c[j].key)) duplicate = 1;
            if (duplicate) continue;
            if (count >= MAX_CANDIDATES) return count;
            memcpy(c[count].key, next, n + 1);
            c[count].rules = rule->rules_out; c[count++].len = (uint16_t)n;
        }
    }
    return count;
}

static int boundary(uint32_t cp)
{
    return cp <= 0x20 || cp == 0x3000 || cp == 0x3001 || cp == 0x3002 ||
           (cp >= 0x3008 && cp <= 0x3011) || cp == 0xff01 || cp == 0xff1f ||
           cp == '!' || cp == '?' || cp == ',' || cp == '.';
}

static int append_match(VjoArena *a, Workspace *w, const Match *m, int pos16, int len16, VjoDictResult *res)
{
    int n = res->n_vocab, rc;
    VjoVocab *v;
    char *payload, *reading, *meaning;
    size_t left;
    /* Repeated words reuse vocabulary, but keep every token. */
    for (int i = 0; i < n; i++) {
        if (w->seen_dict[i] == m->dict && w->seen_off[i] == m->record.entry_off) {
            if (res->n_tokens >= VJO_LOCAL_MAX_RESULTS) return VJO_E_TOO_LARGE;
            res->tokens[res->n_tokens++] = (VjoToken){i, pos16, len16};
            return VJO_OK;
        }
    }
    if (n >= VJO_LOCAL_MAX_RESULTS || res->n_tokens >= VJO_LOCAL_MAX_RESULTS) return VJO_E_TOO_LARGE;
    payload = vjo_arena_alloc(a, m->record.entry_len);
    if (!payload) return VJO_E_TOO_LARGE;
    rc = w->dicts[m->dict].file.read(w->dicts[m->dict].file.ctx, m->record.entry_off, payload, m->record.entry_len);
    if (rc) return VJO_E_LOCAL_IO;
    if (!payload[0] || payload[m->record.entry_len - 1]) return VJO_E_LOCAL_FORMAT;
    reading = memchr(payload, 0, m->record.entry_len);
    if (!reading) return VJO_E_LOCAL_FORMAT;
    reading++;
    left = m->record.entry_len - (size_t)(reading - payload);
    meaning = memchr(reading, 0, left);
    if (!meaning || meaning + 1 >= payload + m->record.entry_len) return VJO_E_LOCAL_FORMAT;
    meaning++;
    if (memchr(meaning, 0, m->record.entry_len - (size_t)(meaning - payload) - 1)) return VJO_E_LOCAL_FORMAT;
    v = &res->vocab[n];
    v->spelling = payload; v->reading = *reading ? reading : payload; v->rank = VJO_NO_RANK;
    v->meanings = vjo_arena_alloc(a, sizeof(char *));
    if (!v->meanings) return VJO_E_TOO_LARGE;
    v->meanings[0] = meaning; v->n_meanings = 1;
    w->seen_dict[n] = m->dict; w->seen_off[n] = m->record.entry_off;
    res->tokens[res->n_tokens++] = (VjoToken){n, pos16, len16};
    res->n_vocab++;
    return VJO_OK;
}

int vjo_local_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                     const char *text, VjoDictResult *res, VjoErr *err)
{
    size_t mark = vjo_arena_mark(a), len = strlen(text), off = 0;
    VjoArena output;
    Workspace *w;
    void *outmem;
    int rc = VJO_OK, pos16 = 0;
    memset(res, 0, sizeof(*res));
    if (len > VJO_LOCAL_MAX_TEXT) return err->rc = VJO_E_TOO_LARGE;
    outmem = vjo_arena_alloc(a, VJO_LOCAL_RESULT_BUDGET);
    w = vjo_arena_zalloc(a, sizeof(*w));
    if (!outmem || !w) { vjo_arena_release(a, mark); return err->rc = VJO_E_OOM; }
    vjo_arena_init(&output, outmem, VJO_LOCAL_RESULT_BUDGET);
    res->vocab = vjo_arena_zalloc(&output, VJO_LOCAL_MAX_RESULTS * sizeof(VjoVocab));
    res->tokens = vjo_arena_zalloc(&output, VJO_LOCAL_MAX_RESULTS * sizeof(VjoToken));
    if ((rc = open_dictionaries(w, p, cfg, err))) goto done;
    while (off < len) {
        char key[VJO_LOCAL_MAX_KEY + 1];
        size_t ends[MAX_SCAN_CHARS + 1], lengths[MAX_SCAN_CHARS + 1], scan = off, bytes = 0;
        int units[MAX_SCAN_CHARS + 1], count = 0, u = 0, matched = 0;
        while (scan < len && count < MAX_SCAN_CHARS) {
            uint32_t cp = vjo_utf8_next(text, len, &scan);
            char enc[4];
            int k;
            if (boundary(cp)) break;
            u += vjo_utf16_units(cp);
            /* Same normalization as the converter; source lengths stay intact. */
            if (cp >= 0x30a1 && cp <= 0x30f6) cp -= 0x60;
            if (cp >= 0xff01 && cp <= 0xff5e) cp -= 0xfee0;
            k = vjo_utf8_encode(cp, enc);
            if (bytes + (size_t)k > VJO_LOCAL_MAX_KEY) break;
            memcpy(key + bytes, enc, (size_t)k); bytes += (size_t)k;
            count++; ends[count] = scan; lengths[count] = bytes; units[count] = u;
        }
        for (int k = count; k > 0 && !matched; k--) {
            int candidates;
            key[lengths[k]] = 0;
            candidates = deinflect(w->candidates, key);
            w->n_matches = 0;
            /* Exact matches in all dictionaries precede deinflected matches. */
            for (int c = 0; c < candidates && w->n_matches < VJO_LOCAL_MAX_MATCHES; c++)
                for (int d = 0; d < w->n_dicts && w->n_matches < VJO_LOCAL_MAX_MATCHES; d++)
                    if ((rc = find_matches(w, d, &w->candidates[c]))) goto done;
            if (!w->n_matches) continue;
            for (int j = 0; j < w->n_matches; j++)
                if ((rc = append_match(&output, w, &w->matches[j], pos16, units[k], res))) goto done;
            off = ends[k]; pos16 += units[k]; matched = 1;
        }
        if (!matched) pos16 += vjo_utf16_units(vjo_utf8_next(text, len, &off));
    }
done:
    for (int i = 0; i < w->n_dicts; i++)
        if (w->dicts[i].file.close) w->dicts[i].file.close(w->dicts[i].file.ctx);
    if (rc) {
        memset(res, 0, sizeof(*res));
        vjo_arena_release(a, mark);
    } else {
        /* The scratch workspace and unused result capacity do not stay resident. */
        vjo_arena_release(a, (size_t)((uint8_t *)outmem - a->base) + output.used);
    }
    return err->rc = rc;
}
