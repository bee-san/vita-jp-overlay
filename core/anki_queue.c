#include "anki_queue.h"
#include <string.h>
#include "bearssl.h"
#include "json.h"

char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *n, int picture)
{
    VjoBuf b;
    if (n->n_meanings < 0 || n->n_meanings > 1024 || !n->spelling || !*n->spelling)
        return NULL;
    vjo_buf_init(&b, a);
    vjo_buf_puts(&b, "{\"version\":1,\"spelling\":");
    vjo_json_write_string(&b, n->spelling);
    vjo_buf_puts(&b, ",\"reading\":");
    vjo_json_write_string(&b, n->reading);
    vjo_buf_puts(&b, ",\"sentence\":");
    vjo_json_write_string(&b, n->sentence);
    vjo_buf_printf(&b, ",\"rank\":%d,\"hl_start\":%d,\"hl_end\":%d,\"picture\":%s,\"meanings\":[",
                   n->rank, n->hl_start, n->hl_end, picture ? "true" : "false");
    for (int i = 0; i < n->n_meanings; i++) {
        if (i)
            vjo_buf_putc(&b, ',');
        vjo_json_write_string(&b, n->meanings[i]);
    }
    vjo_buf_puts(&b, "]}\n");
    return b.oom || b.len > VJO_QUEUE_TEXT_MAX ? NULL : vjo_buf_cstr(&b);
}

static int number(const VjoJson *j, const char *key, int *out)
{
    int t = vjo_json_obj_get(j, 0, key);
    long value;
    if (vjo_json_int(j, t, &value) < 0)
        return -1;
    /* vjo_json_int also accepts fractional numbers; queue integers must be exact. */
    for (int i = j->t[t].start; i < j->t[t].end; i++)
        if ((j->js[i] < '0' || j->js[i] > '9') && !(i == j->t[t].start && j->js[i] == '-'))
            return -1;
    *out = (int)value;
    return 0;
}

static const char *string(VjoArena *a, const VjoJson *j, int t)
{
    if (!vjo_json_is_type(j, t, JSMN_STRING))
        return NULL;
    /* Embedded NUL would silently truncate the note on send. */
    for (int i = j->t[t].start; i < j->t[t].end; i++) {
        if ((unsigned char)j->js[i] < 0x20)
            return NULL;
        if (j->js[i] == '\\') {
            if (i + 5 < j->t[t].end && !memcmp(j->js + i, "\\u0000", 6))
                return NULL;
            i++;
        }
    }
    return vjo_json_str(a, j, t);
}

int vjo_queue_decode(VjoArena *a, const char *json, size_t len, VjoAnkiNote *n, int *picture)
{
    VjoJson j;
    int version, m;
    memset(n, 0, sizeof(*n));
    if (!len || len > VJO_QUEUE_TEXT_MAX || memchr(json, 0, len) ||
        vjo_json_parse(a, json, len, &j) < 0 || !vjo_json_is_type(&j, 0, JSMN_OBJECT) ||
        vjo_json_skip(&j, 0) != j.n || number(&j, "version", &version) || version != 1 ||
        number(&j, "rank", &n->rank) || number(&j, "hl_start", &n->hl_start) ||
        number(&j, "hl_end", &n->hl_end) ||
        vjo_json_bool(&j, vjo_json_obj_get(&j, 0, "picture"), picture))
        return -1;
    n->spelling = string(a, &j, vjo_json_obj_get(&j, 0, "spelling"));
    n->reading = string(a, &j, vjo_json_obj_get(&j, 0, "reading"));
    n->sentence = string(a, &j, vjo_json_obj_get(&j, 0, "sentence"));
    m = vjo_json_obj_get(&j, 0, "meanings");
    if (!n->spelling || !*n->spelling || !n->reading || !n->sentence ||
        !vjo_json_is_type(&j, m, JSMN_ARRAY) || j.t[m].size > 1024)
        return -1;
    n->n_meanings = j.t[m].size;
    n->meanings = vjo_arena_alloc(a, sizeof(char *) * (size_t)(n->n_meanings + 1));
    if (!n->meanings)
        return -1;
    for (int i = 0, t = m + 1; i < n->n_meanings; i++, t = vjo_json_skip(&j, t))
        if (!(n->meanings[i] = string(a, &j, t)))
            return -1;
    return 0;
}

void vjo_queue_id(const char *json, size_t len, char id[VJO_QUEUE_ID_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    br_sha256_context ctx;
    unsigned char digest[32];
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, json, len);
    br_sha256_out(&ctx, digest);
    for (int i = 0; i < 16; i++) {
        id[2 * i] = hex[digest[i] >> 4];
        id[2 * i + 1] = hex[digest[i] & 15];
    }
    id[32] = '\0';
}

int vjo_queue_valid_id(const char *id)
{
    if (strlen(id) != 32)
        return 0;
    for (int i = 0; i < 32; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return 0;
    return 1;
}
