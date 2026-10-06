#include "hachidori.h"

#include <string.h>

#include "json.h"
#include "port.h"
#include "utf.h"

/* Only strings needed by the display survive each request. Offsets, rather
 * than pointers, let the packed text move over HTTP/JSON scratch in one copy. */
typedef struct {
    VjoBuf text;
    size_t reading, meaning[VJO_HACHIDORI_MAX_MEANINGS];
    int n_meanings, rank, len16;
} Term;

static int integer(const VjoJson *j, int i, int *out)
{
    int v = 0;
    const char *s, *end;
    if (!vjo_json_is_type(j, i, JSMN_PRIMITIVE))
        return -1;
    s = j->js + j->t[i].start;
    end = j->js + j->t[i].end;
    if (s == end || (*s == '0' && end - s > 1))
        return -1;
    for (; s < end; s++) {
        if (*s < '0' || *s > '9' || v > 214748364 || (v == 214748364 && *s > '7'))
            return -1;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return 0;
}

static int string_eq(const VjoJson *j, int i, const char *s)
{
    return vjo_json_is_type(j, i, JSMN_STRING) &&
           (size_t)(j->t[i].end - j->t[i].start) == strlen(s) &&
           !memcmp(j->js + j->t[i].start, s, strlen(s));
}

static int separator(const VjoJson *j, int start, int end, char expected)
{
    for (; start < end; start++) {
        char c = j->js[start];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (!expected || c != expected)
            return 0;
        expected = 0;
    }
    return start == end && !expected;
}

/* jsmn tokenises but does not enforce separators or a single JSON root.
 * Keep the stricter relay checks local; NUL cannot be stored in C strings. */
static int valid_reply(const VjoJson *j, size_t len)
{
    if (vjo_json_skip(j, 0) != j->n || !separator(j, 0, j->t[0].start, 0) ||
        !separator(j, j->t[0].end, (int)len, 0))
        return 0;
    for (int i = 0; i < j->n; i++) {
        const jsmntok_t *t = &j->t[i];
        if (t->type == JSMN_STRING) {
            for (int p = t->start; p < t->end; p++) {
                if ((unsigned char)j->js[p] < 0x20)
                    return 0;
                if (j->js[p] == '\\') {
                    if (++p >= t->end)
                        return 0;
                    if (j->js[p] == 'u') {
                        if (t->end - p < 5 || !memcmp(j->js + p + 1, "0000", 4))
                            return 0;
                        p += 4;
                    }
                }
            }
        } else if (t->type == JSMN_OBJECT || t->type == JSMN_ARRAY) {
            int child = i + 1, end = t->start + 1;
            for (int k = 0; k < t->size; k++) {
                if (child >= j->n ||
                    !separator(j, end, j->t[child].start - (j->t[child].type == JSMN_STRING), k ? ',' : 0))
                    return 0;
                if (t->type == JSMN_OBJECT) {
                    if (!vjo_json_is_type(j, child, JSMN_STRING) || j->t[child].size != 1)
                        return 0;
                    end = j->t[child++].end + 1;
                    if (child >= j->n ||
                        !separator(j, end, j->t[child].start - (j->t[child].type == JSMN_STRING), ':'))
                        return 0;
                }
                end = j->t[child].end + (j->t[child].type == JSMN_STRING);
                child = vjo_json_skip(j, child);
            }
            if (!separator(j, end, t->end - 1, 0))
                return 0;
        }
    }
    return 1;
}

/* Structured content is a tree of text, arrays and content-bearing nodes.
 * Never traverse arbitrary keys: tags, CSS, URLs and data are not glosses. */
static int glossary(VjoBuf *b, const VjoJson *j, int i, int depth)
{
    int rc = 0;
    if (depth > 16)
        return VJO_E_TOO_LARGE;
    if (vjo_json_is_type(j, i, JSMN_STRING)) {
        rc = vjo_json_append_str(b, j, i) < 0 ? VJO_E_OOM : 0;
    } else if (vjo_json_is_type(j, i, JSMN_ARRAY)) {
        int child = i + 1;
        for (int k = 0; k < j->t[i].size && !rc; k++) {
            rc = glossary(b, j, child, depth + 1);
            child = vjo_json_skip(j, child);
        }
    } else if (vjo_json_is_type(j, i, JSMN_OBJECT)) {
        int content = vjo_json_obj_get(j, i, "content");
        if (string_eq(j, vjo_json_obj_get(j, i, "tag"), "br"))
            rc = vjo_buf_putc(b, '\n') < 0 ? VJO_E_OOM : 0;
        else if (content >= 0) {
            if (string_eq(j, vjo_json_obj_get(j, i, "tag"), "li") && b->len &&
                b->data[b->len - 1] != '\0' && b->data[b->len - 1] != '\n')
                rc = vjo_buf_putc(b, '\n') < 0 ? VJO_E_OOM : 0;
            if (!rc)
                rc = glossary(b, j, content, depth + 1);
        }
    } else {
        return VJO_E_PARSE;
    }
    if (b->len > VJO_HACHIDORI_MAX_GLOSS)
        return VJO_E_TOO_LARGE;
    return rc;
}

static int parse_term(VjoArena *a, const char *json, size_t len, Term *term)
{
    VjoJson j;
    int index, entries, entry, heads, head, spelling, reading, definitions, frequencies;
    int rc;
    if (vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT) ||
        !valid_reply(&j, len))
        return VJO_E_PARSE;
    entries = vjo_json_obj_get(&j, 0, "dictionaryEntries");
    if (integer(&j, vjo_json_obj_get(&j, 0, "index"), &index) < 0 || index != 0 ||
        integer(&j, vjo_json_obj_get(&j, 0, "originalTextLength"), &term->len16) < 0 ||
        !vjo_json_is_type(&j, entries, JSMN_ARRAY))
        return VJO_E_PARSE;
    if (!j.t[entries].size)
        return term->len16 == 0 ? VJO_OK : VJO_E_PARSE;
    if (!term->len16)
        return VJO_E_PARSE;
    entry = vjo_json_arr_get(&j, entries, 0); /* host-ranked first result only */
    heads = vjo_json_obj_get(&j, entry, "headwords");
    head = vjo_json_arr_get(&j, heads, 0);
    spelling = vjo_json_obj_get(&j, head, "term");
    reading = vjo_json_obj_get(&j, head, "reading");
    definitions = vjo_json_obj_get(&j, entry, "definitions");
    frequencies = vjo_json_obj_get(&j, entry, "frequencies");
    if (!vjo_json_is_type(&j, spelling, JSMN_STRING) || j.t[spelling].start == j.t[spelling].end ||
        !vjo_json_is_type(&j, reading, JSMN_STRING) || !vjo_json_is_type(&j, definitions, JSMN_ARRAY))
        return VJO_E_PARSE;
    vjo_buf_init(&term->text, a);
    if (vjo_json_append_str(&term->text, &j, spelling) < 0 || vjo_buf_putc(&term->text, '\0') < 0)
        return VJO_E_OOM;
    term->reading = term->text.len;
    if (vjo_json_append_str(&term->text, &j, reading) < 0 || vjo_buf_putc(&term->text, '\0') < 0)
        return VJO_E_OOM;
    int def = definitions + 1;
    for (int d = 0; d < j.t[definitions].size; d++) {
        int glosses = vjo_json_obj_get(&j, def, "entries");
        if (!vjo_json_is_type(&j, glosses, JSMN_ARRAY))
            return VJO_E_PARSE;
        int gloss = glosses + 1;
        for (int k = 0; k < j.t[glosses].size; k++) {
            size_t start = term->text.len;
            if (term->n_meanings == VJO_HACHIDORI_MAX_MEANINGS)
                return VJO_E_TOO_LARGE;
            rc = glossary(&term->text, &j, gloss, 0);
            if (rc)
                return rc;
            if (term->text.len != start) {
                term->meaning[term->n_meanings++] = start;
                if (vjo_buf_putc(&term->text, '\0') < 0)
                    return VJO_E_OOM;
            }
            gloss = vjo_json_skip(&j, gloss);
        }
        def = vjo_json_skip(&j, def);
    }
    term->rank = VJO_NO_RANK;
    if (vjo_json_is_type(&j, frequencies, JSMN_ARRAY)) {
        int freq = frequencies + 1;
        for (int k = 0; k < j.t[frequencies].size; k++) {
            int value;
            if (!integer(&j, vjo_json_obj_get(&j, freq, "frequency"), &value) && value > 0) {
                term->rank = value;
                break;
            }
            freq = vjo_json_skip(&j, freq);
        }
    }
    return VJO_OK;
}

static int send_body(void *ud, VjoConn *c)
{
    const char *s = (const char *)ud;
    return vjo_conn_send_all(c, s, strlen(s));
}

int vjo_hachidori_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                         const char *text, VjoDictResult *res, VjoErr *err)
{
    char host[64], authority[80];
    int port, units = 0, count = 0, pos = 0;
    size_t len = strlen(text), off = 0;
    if (vjo_hachidori_endpoint(cfg->hachidori_host, host, sizeof(host), &port) < 0)
        return err->rc = VJO_E_NO_HOST;
    if (len > VJO_HACHIDORI_MAX_TEXT)
        return err->rc = VJO_E_TOO_LARGE;
    while (off < len) {
        uint32_t cp = vjo_utf8_next(text, len, &off);
        units += cp > 0xFFFF ? 2 : 1;
        count++;
    }
    if (units > VJO_HACHIDORI_MAX_UNITS)
        return err->rc = VJO_E_TOO_LARGE;
    if (!count)
        return VJO_OK;
    res->vocab = (VjoVocab *)vjo_arena_zalloc(a, (size_t)count * sizeof(VjoVocab));
    res->tokens = (VjoToken *)vjo_arena_zalloc(a, (size_t)count * sizeof(VjoToken));
    if (!res->vocab || !res->tokens)
        return err->rc = VJO_E_OOM;
    vjo_snprintf(authority, sizeof(authority), "%s:%d", host, port);
    for (off = 0; off < len; ) {
        VjoHttpRequest req;
        VjoHttpResponse resp;
        VjoBuf body;
        Term term;
        int rc, vocab;
        size_t mark = vjo_arena_mark(a), end = off;
        memset(&term, 0, sizeof(term));
        vjo_buf_init(&body, a);
        vjo_buf_puts(&body, "{\"term\":");
        vjo_json_write_string(&body, text + off);
        vjo_buf_putc(&body, '}');
        memset(&req, 0, sizeof(req));
        req.method = "POST"; req.host = host; req.host_header = authority;
        req.path = "/termEntries"; req.content_type = "application/json; charset=utf-8";
        req.ud = vjo_buf_cstr(&body); req.body_len = body.len;
        req.write_body = send_body; req.connect_timeout_us = 3000000;
        rc = req.ud ? vjo_http_request(a, p, port, 0, &req, VJO_DICT_MAX_RESPONSE, &resp, err) : VJO_E_OOM;
        if (rc == VJO_E_STATUS && resp.body) {
            VjoJson j;
            char *detail = NULL;
            if (vjo_json_parse(a, resp.body, resp.body_len, &j) == 0) {
                int msg = vjo_json_obj_get(&j, 0, "error");
                if (vjo_json_is_type(&j, msg, JSMN_STRING))
                    detail = vjo_json_str(a, &j, msg);
            }
            size_t n = detail ? strlen(detail) : 0;
            vjo_arena_release(a, mark);
            if (detail) {
                char *copy = (char *)vjo_arena_alloc(a, n + 1);
                if (copy) memmove(copy, detail, n + 1);
                err->detail = copy;
            }
            return err->rc = rc;
        }
        if (!rc)
            rc = resp.gzip ? VJO_E_PARSE : parse_term(a, resp.body, resp.body_len, &term);
        if (!rc && term.len16) {
            int n = 0;
            while (end < len && n < term.len16) {
                uint32_t cp = vjo_utf8_next(text, len, &end);
                n += cp > 0xFFFF ? 2 : 1;
            }
            if (n != term.len16)
                rc = VJO_E_PARSE; /* out of range, or half a surrogate pair */
        }
        if (rc) {
            vjo_arena_release(a, mark);
            return err->rc = rc;
        }
        if (!term.len16) {
            vjo_arena_release(a, mark);
            uint32_t cp = vjo_utf8_next(text, len, &off);
            pos += cp > 0xFFFF ? 2 : 1;
            continue;
        }
        for (vocab = 0; vocab < res->n_vocab; vocab++)
            if (!strcmp(res->vocab[vocab].spelling, (char *)term.text.data) &&
                !strcmp(res->vocab[vocab].reading, (char *)term.text.data + term.reading))
                break;
        vjo_arena_release(a, mark);
        if (vocab == res->n_vocab) {
            size_t pointers = (size_t)term.n_meanings * sizeof(char *);
            char *block = (char *)vjo_arena_alloc(a, pointers + term.text.len);
            if (!block)
                return err->rc = VJO_E_OOM;
            char *strings = block + pointers;
            memmove(strings, term.text.data, term.text.len);
            VjoVocab *v = &res->vocab[res->n_vocab++];
            v->spelling = strings; v->reading = strings + term.reading;
            v->rank = term.rank; v->n_meanings = term.n_meanings;
            v->meanings = (const char **)block;
            for (int i = 0; i < term.n_meanings; i++)
                v->meanings[i] = strings + term.meaning[i];
        }
        VjoToken *token = &res->tokens[res->n_tokens++];
        token->vocab = vocab; token->pos16 = pos; token->len16 = term.len16;
        pos += term.len16; off = end;
    }
    return VJO_OK;
}
