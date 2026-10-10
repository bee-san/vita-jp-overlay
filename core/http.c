#include "http.h"

#include <stdarg.h>
#include <string.h>

#include "port.h"
#include "utf.h"

int vjo_conn_send_all(VjoConn *c, const void *p, size_t n)
{
    const uint8_t *u = (const uint8_t *)p;
    while (n) {
        int w = c->send(c->ctx, u, n);
        if (w <= 0)
            return VJO_E_NET;
        u += w;
        n -= (size_t)w;
    }
    return VJO_OK;
}

/* Appends to the request head; once it overflows, *n stays at cap. Output
 * that fills the buffer counts as an overflow, whether vsnprintf returns
 * the length it wanted (C) or the length it wrote. */
static void head_add(char *head, size_t cap, size_t *n, const char *fmt, ...)
{
    va_list ap;
    int k;
    if (*n >= cap)
        return;
    va_start(ap, fmt);
    k = vjo_vsnprintf(head + *n, cap - *n, fmt, ap);
    va_end(ap);
    *n = k < 0 || (size_t)k >= cap - *n - 1 ? cap : *n + (size_t)k;
}

int vjo_http_send(VjoConn *c, const VjoHttpRequest *req)
{
    char head[1024];
    size_t n = 0;
    int rc;
    head_add(head, sizeof(head), &n, "%s %s HTTP/1.1\r\nHost: %s", req->method, req->path, req->host);
    if (req->host_port)
        head_add(head, sizeof(head), &n, ":%d", req->host_port);
    head_add(head, sizeof(head), &n, "\r\n");
    if (req->content_type)
        head_add(head, sizeof(head), &n, "Content-Type: %s\r\n", req->content_type);
    if (req->body_len || req->write_body)
        head_add(head, sizeof(head), &n, "Content-Length: %lu\r\n", (unsigned long)req->body_len);
    if (!req->keep_alive)
        head_add(head, sizeof(head), &n, "Connection: close\r\n");
    head_add(head, sizeof(head), &n, "%s\r\n", req->extra_headers ? req->extra_headers : "");
    if (n >= sizeof(head))
        return VJO_E_HTTP;
    rc = vjo_conn_send_all(c, head, n);
    if (rc)
        return rc;
    return req->write_body ? req->write_body(req->ud, c) : VJO_OK;
}

#define MAX_TRAILERS 32 /* more: the connection is not kept */

/* Buffered reader over a connection. */
typedef struct {
    VjoConn *c;
    uint8_t buf[2048];
    size_t pos, len;
    int eof, err;
} Rd;

static int rd_fill(Rd *r)
{
    int n;
    if (r->eof || r->err)
        return 0;
    n = r->c->recv(r->c->ctx, r->buf, sizeof(r->buf));
    if (n < 0) {
        /* A VJO_E_* code from the connection is kept, other errors are I/O. */
        r->err = n == VJO_E_HTTP ? VJO_E_HTTP : VJO_E_NET;
        return 0;
    }
    if (n == 0) {
        r->eof = 1;
        return 0;
    }
    r->pos = 0;
    r->len = (size_t)n;
    return n;
}

static int rd_byte(Rd *r)
{
    if (r->pos >= r->len && !rd_fill(r))
        return -1;
    return r->buf[r->pos++];
}

/* Reads one CRLF-terminated line (without CRLF). Returns length or -1. */
static int rd_line(Rd *r, char *out, size_t cap)
{
    size_t n = 0;
    for (;;) {
        int ch = rd_byte(r);
        if (ch < 0 || n + 1 >= cap || ch == '\n' || !ch ||
            (ch < 0x20 && ch != '\r' && ch != '\t') || ch == 0x7f) {
            if (!r->err) r->err = VJO_E_HTTP;
            return -1;
        }
        if (ch == '\r') {
            if (rd_byte(r) != '\n') {
                if (!r->err) r->err = VJO_E_HTTP;
                return -1;
            }
            break;
        }
        out[n++] = (char)ch;
    }
    out[n] = '\0';
    return (int)n;
}

static int token_char(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

static int value_equals(const char *value, const char *name)
{
    size_t n = strlen(value);
    while (n && (value[n-1] == ' ' || value[n-1] == '\t')) n--;
    return n == strlen(name) && vjo_ieq_prefix(value, name);
}

static int connection_close(const char *value)
{
    const char *p = value;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p-start);
        while (n && (start[n-1] == ' ' || start[n-1] == '\t')) n--;
        if (n == 5 && vjo_ieq_prefix(start, "close")) return 1;
    }
    return 0;
}

static int content_size(const char *value, size_t *size)
{
    size_t n = 0;
    if (*value < '0' || *value > '9') return VJO_E_HTTP;
    while (*value >= '0' && *value <= '9') {
        unsigned d = (unsigned)(*value++ - '0');
        if (n > (SIZE_MAX-d)/10) return VJO_E_TOO_LARGE;
        n = n*10+d;
    }
    while (*value == ' ' || *value == '\t') value++;
    if (*value) return VJO_E_HTTP;
    *size = n;
    return VJO_OK;
}

/* Extensions cannot alter framing. Validate their token / quoted-string
 * syntax instead of interpreting a malformed chunk size as the final zero. */
static int chunk_size(const char *line, size_t max, size_t *size)
{
    size_t n = 0;
    const char *p = line;
    unsigned digits = 0;
    for (;;) {
        unsigned d;
        if (*p >= '0' && *p <= '9') d = (unsigned)(*p-'0');
        else if (*p >= 'a' && *p <= 'f') d = (unsigned)(*p-'a'+10);
        else if (*p >= 'A' && *p <= 'F') d = (unsigned)(*p-'A'+10);
        else break;
        if (n > max/16 || (n == max/16 && d > max%16)) return VJO_E_TOO_LARGE;
        n = n*16+d;
        p++; digits++;
    }
    if (!digits) return VJO_E_HTTP;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != ';') return VJO_E_HTTP;
        while (*p == ' ' || *p == '\t') p++;
        const char *name = p;
        while (token_char((unsigned char)*p)) p++;
        if (p == name) return VJO_E_HTTP;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '=') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\') {
                        p++;
                        if (!*p) return VJO_E_HTTP;
                    }
                    if ((unsigned char)*p < 0x20 && *p != '\t') return VJO_E_HTTP;
                    if ((unsigned char)*p == 0x7f) return VJO_E_HTTP;
                    p++;
                }
                if (*p++ != '"') return VJO_E_HTTP;
            } else {
                const char *value = p;
                while (token_char((unsigned char)*p)) p++;
                if (p == value) return VJO_E_HTTP;
            }
        }
    }
    *size = n;
    return VJO_OK;
}

static const char *hval(const char *line, size_t name_len)
{
    const char *v = line + name_len;
    while (*v == ' ' || *v == '\t')
        v++;
    return v;
}

static int body_put(VjoBuf *b, size_t max, const void *p, size_t n)
{
    if (b->len + n > max)
        return VJO_E_TOO_LARGE;
    if (vjo_buf_append(b, p, n) < 0)
        return VJO_E_OOM;
    return VJO_OK;
}

static int read_exact(Rd *r, VjoBuf *b, size_t max, size_t n)
{
    while (n) {
        size_t avail;
        int rc;
        if (r->pos >= r->len && !rd_fill(r))
            return r->err ? r->err : VJO_E_HTTP;
        avail = r->len - r->pos;
        if (avail > n)
            avail = n;
        rc = body_put(b, max, r->buf + r->pos, avail);
        if (rc)
            return rc;
        r->pos += avail;
        n -= avail;
    }
    return VJO_OK;
}

int vjo_http_recv(VjoArena *a, VjoConn *c, size_t max_body, VjoHttpResponse *resp)
{
    Rd *r;
    char line[512];
    size_t content_length = 0;
    int have_length = 0, chunked = 0, conn_close = 0, status = 0, rc = VJO_OK;
    unsigned interim = 0;
    VjoBuf body;

    memset(resp, 0, sizeof(*resp));
    /* The reader lives in the arena rather than on (small) thread stacks;
     * the body buffer then grows at the arena top after it. */
    r = (Rd *)vjo_arena_alloc(a, sizeof(Rd));
    if (!r)
        return VJO_E_OOM;
    /* Arena memory is reused and not zeroed: initialize every field. */
    r->c = c;
    r->pos = 0;
    r->len = 0;
    r->eof = 0;
    r->err = 0;

read_status:
    if (rd_line(r, line, sizeof(line)) < 0)
        return r->err ? r->err : VJO_E_HTTP;
    /* "HTTP/1.x NNN ..." */
    if (!vjo_ieq_prefix(line, "http/1.") || strlen(line) < 12 || line[8] != ' ')
        return VJO_E_HTTP;
    status = 0;
    for (int k = 9; k < 12; k++) {
        if (line[k] < '0' || line[k] > '9')
            return VJO_E_HTTP;
        status = status * 10 + (line[k] - '0');
    }
    resp->status = status;
    conn_close = line[7] != '1'; /* HTTP/1.0 */
    have_length = chunked = 0;
    resp->gzip = 0;

    for (;;) {
        int n = rd_line(r, line, sizeof(line));
        if (n < 0)
            return r->err ? r->err : VJO_E_HTTP;
        if (n == 0)
            break;
        const char *colon = strchr(line, ':');
        if (!colon || colon == line) return VJO_E_HTTP;
        for (const char *p = line; p < colon; p++)
            if (!token_char((unsigned char)*p)) return VJO_E_HTTP;
        if (vjo_ieq_prefix(line, "content-length:")) {
            const char *v = hval(line, 15);
            size_t length;
            rc = content_size(v, &length);
            if (rc) return rc;
            if (have_length && length != content_length) return VJO_E_HTTP;
            content_length = length;
            have_length = 1;
        } else if (vjo_ieq_prefix(line, "transfer-encoding:")) {
            if (chunked || !value_equals(hval(line, 18), "chunked")) return VJO_E_HTTP;
            chunked = 1;
        } else if (vjo_ieq_prefix(line, "content-encoding:")) {
            if (value_equals(hval(line, 17), "gzip"))
                resp->gzip = 1;
        } else if (vjo_ieq_prefix(line, "connection:")) {
            conn_close |= connection_close(hval(line, 11));
        }
    }

    if (chunked && have_length) return VJO_E_HTTP;
    if (status >= 100 && status < 200) {
        /* Informational replies precede the real response. Protocol upgrades
         * have no place in this bounded HTTP request/response client. */
        if (status == 101 || have_length || chunked || ++interim > 8) return VJO_E_HTTP;
        goto read_status;
    }

    vjo_buf_init(&body, a);

    /* These never have a body (RFC 9112 6.3), whatever the headers say. */
    if (status == 204 || status == 304) {
        chunked = 0;
        have_length = 1;
        content_length = 0;
    }
    if (chunked) {
        for (;;) {
            size_t sz = 0;
            if (rd_line(r, line, sizeof(line)) < 0) {
                rc = r->err ? r->err : VJO_E_HTTP;
                break;
            }
            rc = chunk_size(line, max_body, &sz);
            if (rc)
                break;
            if (sz == 0) {
                /* Trailers (ignored) up to the empty line that ends them. */
                int k = 1;
                for (int n = 0; k > 0 && n < MAX_TRAILERS; n++)
                    k = rd_line(r, line, sizeof(line));
                if (k != 0) rc = r->err ? r->err : VJO_E_HTTP;
                break;
            }
            rc = read_exact(r, &body, max_body, sz);
            if (rc)
                break;
            if (rd_byte(r) != '\r' || rd_byte(r) != '\n') {
                rc = r->err ? r->err : VJO_E_HTTP;
                break;
            }
        }
    } else if (have_length) {
        if (content_length > max_body)
            rc = VJO_E_TOO_LARGE;
        else
            rc = read_exact(r, &body, max_body, content_length);
    } else {
        /* Read until close. */
        conn_close = 1;
        for (;;) {
            if (r->pos >= r->len && !rd_fill(r))
                break;
            rc = body_put(&body, max_body, r->buf + r->pos, r->len - r->pos);
            if (rc)
                break;
            r->pos = r->len;
        }
        if (r->err)
            rc = r->err;
    }
    if (rc)
        return rc;
    resp->body = vjo_buf_cstr(&body);
    if (!resp->body)
        return VJO_E_OOM;
    resp->body_len = body.len;
    resp->keep_alive = !conn_close && r->pos == r->len;
    return VJO_OK;
}
