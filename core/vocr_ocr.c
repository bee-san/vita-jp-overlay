/* vita-vn-ocr backend (vocr_ocr.h): region -> grey -> vocr_find_lines ->
 * vocr_run per line, in one workspace. The runtime (vocr.c, vocr_lines.c) is
 * bee-san/vita-vn-ocr's, at the commit cmake/VocrOCR.cmake pins. */
#include "vocr_ocr.h"

#include <bearssl_hash.h>
#include <string.h>

#include "vocr.h"
#include "vocr_lines.h"

/* The int8 assets of github.com/bee-san/vita-vn-ocr release v0.2.0
 * (docs/vita-vn-ocr.md). Arena bytes: vocr_arena_bytes() in W8I mode with the
 * default 4-frame tiles and 16-frame head batches, any line width. */
static const VjoVocrModel models[] = {
    {"H15_w8.vocr", 1155968,
     {0x31,0x6e,0xbc,0x37,0x7e,0xc3,0x45,0x20,0x44,0x09,0x91,0xc5,0x08,0x51,0x99,0xd8,
      0xe7,0x40,0x31,0xf0,0x1a,0x86,0x79,0x38,0xbe,0x90,0xa6,0x0c,0x1e,0x3f,0x03,0xcd},
     182944},
    {"FL10_w8.vocr", 657664,
     {0x50,0x0e,0x0b,0x84,0x2e,0x5f,0x0c,0xb3,0x26,0xf5,0x5e,0x7d,0x99,0x1a,0x52,0xab,
      0x66,0xdd,0x24,0x97,0x7c,0xcc,0x89,0xbd,0x18,0x82,0x28,0xae,0xdc,0x82,0x5e,0x84},
     124832},
    {"F20_w8.vocr", 1558016,
     {0xf8,0xc1,0xab,0xe4,0x74,0x3a,0x71,0xa7,0x42,0xe7,0x18,0x52,0x3f,0x42,0x06,0xf9,
      0x92,0xd3,0xbd,0x4f,0xb6,0x9d,0xe3,0x21,0x99,0x7b,0x2b,0xd6,0xf1,0x67,0x79,0xc0},
     241056},
};
#define N_MODELS (sizeof(models) / sizeof(models[0]))

#define ALIGN16(x) (((x) + (size_t)15) & ~(size_t)15)
#define READ_CHUNK 32768u

const VjoVocrModel *vjo_vocr_models(unsigned *count)
{
    if (count) *count = (unsigned)N_MODELS;
    return models;
}

const VjoVocrModel *vjo_vocr_model(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < N_MODELS; i++)
        if (!strcmp(name, models[i].name)) return &models[i];
    return NULL;
}

/* Workspace: [weights | finder, then recognizer arena | vocr_model, vocr_ctx | grey region].
 * Before the weights are read, their space holds the RGBA rows being copied. */
typedef struct {
    size_t model_off, scratch_off, structs_off, grey_off, total;
    size_t scratch, finder;
} Layout;

static int layout(const VjoVocrModel *m, unsigned w, unsigned h, Layout *l)
{
    if (!m || !w || !h || w > VJO_OCR_MAX_WIDTH || h > VJO_OCR_MAX_HEIGHT) return -1;
    l->finder = vocr_lines_arena_bytes(w, h);
    l->scratch = ALIGN16(l->finder > m->arena_bytes ? l->finder : m->arena_bytes);
    l->model_off = 0;
    l->scratch_off = ALIGN16(m->bytes);
    l->structs_off = l->scratch_off + l->scratch;
    l->grey_off = l->structs_off + ALIGN16(sizeof(vocr_model)) + ALIGN16(sizeof(vocr_ctx));
    l->total = l->grey_off + ALIGN16((size_t)w * h);
    return 0;
}

size_t vjo_vocr_workspace_bytes(const char *model, unsigned width, unsigned height)
{
    Layout l;
    return layout(vjo_vocr_model(model), width, height, &l) < 0 ? 0 : l.total;
}

static int cancelled(const VjoOcrImage *im)
{
    return im->cancelled && im->cancelled(im->ud);
}

static int run_cancel(void *user)
{
    return cancelled((const VjoOcrImage *)user);
}

static uint32_t clock_us(const VjoPlatform *p)
{
    return p->now_us ? (uint32_t)p->now_us(p->ud) : 0;
}

/* All rows, once each and in order (the kernel frees a single-pass capture
 * after its last row), as grey = (77 R + 150 G + 29 B) >> 8: the luma both
 * the finder and the recognizer compute from RGBA, so this copy changes no
 * result. rgba holds whole rows (cap bytes). */
static int read_grey(const VjoOcrImage *im, unsigned char *rgba, size_t cap, unsigned char *grey)
{
    const unsigned w = im->width, batch = (unsigned)(cap / ((size_t)w * 4));
    if (!batch) return VJO_E_OOM;
    for (unsigned y = 0; y < im->height;) {
        unsigned n = im->height - y < batch ? im->height - y : batch;
        int got;
        if (cancelled(im)) return VJO_E_CANCELLED;
        got = im->rows(im->ud, y, n, rgba);
        if (got <= 0 || (unsigned)got > n) return VJO_E_SOURCE;
        for (unsigned r = 0; r < (unsigned)got; r++) {
            const unsigned char *s = rgba + (size_t)r * w * 4;
            unsigned char *d = grey + (size_t)(y + r) * w;
            for (unsigned x = 0; x < w; x++, s += 4)
                d[x] = (unsigned char)((77u * s[0] + 150u * s[1] + 29u * s[2]) >> 8);
        }
        y += (unsigned)got;
    }
    return VJO_OK;
}

/* Reads model_dir/<name> into dst after checking its size, and its SHA-256
 * while reading; cancellation is checked between chunks. */
static int load_weights(const VjoPlatform *p, const char *dir, const VjoVocrModel *m,
                        unsigned char *dst, const VjoOcrImage *im)
{
    char path[256];
    size_t dn = strlen(dir), fn = strlen(m->name);
    VjoFile file;
    br_sha256_context hash;
    unsigned char digest[32];
    int rc = VJO_OK;
    if (!dn || dn + 1 + fn >= sizeof(path)) return VJO_E_OCR_MODEL;
    memcpy(path, dir, dn);
    path[dn] = '/';
    memcpy(path + dn + 1, m->name, fn + 1);
    memset(&file, 0, sizeof(file));
    if (!p->file_open || p->file_open(p->ud, path, &file) < 0) return VJO_E_OCR_MODEL;
    if (file.size != m->bytes || !file.read || !file.close) {
        if (file.close) file.close(file.ctx);
        return VJO_E_OCR_MODEL;
    }
    br_sha256_init(&hash);
    for (size_t off = 0; off < m->bytes;) {
        size_t n = m->bytes - off < READ_CHUNK ? m->bytes - off : READ_CHUNK;
        if (cancelled(im)) { rc = VJO_E_CANCELLED; break; }
        if (file.read(file.ctx, off, dst + off, n) < 0) { rc = VJO_E_OCR_MODEL; break; }
        br_sha256_update(&hash, dst + off, n);
        off += n;
    }
    file.close(file.ctx);
    if (rc) return rc;
    br_sha256_out(&hash, digest);
    return memcmp(digest, m->sha256, sizeof(digest)) ? VJO_E_OCR_MODEL : VJO_OK;
}

static int run_error(int rc)
{
    switch (rc) {
    case VOCR_E_TEXT: return VJO_E_TOO_LARGE;
    case VOCR_E_CANCELLED: return VJO_E_CANCELLED;
    default: return VJO_E_OCR_INFERENCE;
    }
}

static int recognize(const VjoPlatform *p, const char *dir, const VjoVocrModel *mi,
                     const VjoOcrImage *im, unsigned char *ws, const Layout *l,
                     char *text, size_t cap, VjoVocrStats *st)
{
    unsigned char *grey = ws + l->grey_off;
    const unsigned w = im->width;
    VocrLinesImage region = {grey, w, im->height, w, VOCR_LINES_GREY};
    VocrLineSet set;
    vocr_model *model = (vocr_model *)(void *)(ws + l->structs_off);
    vocr_ctx *ctx = (vocr_ctx *)(void *)(ws + l->structs_off + ALIGN16(sizeof(vocr_model)));
    vocr_opts opts;
    size_t used = 0;
    uint32_t t0;
    int rc;

    t0 = clock_us(p);
    rc = read_grey(im, ws + l->model_off, l->scratch_off, grey);
    if (rc) return rc;
    rc = vocr_find_lines(&region, VOCR_LINES_GRAD, ws + l->scratch_off, l->finder, &set);
    if (rc) return rc == VOCR_LINES_E_ARENA ? VJO_E_OOM : VJO_E_OCR_REGION;
    st->find_us = clock_us(p) - t0;
    st->lines = set.count;
    st->truncated = set.truncated;
    for (unsigned i = 0; i < set.count; i++)
        st->dark += set.lines[i].dark ? 1u : 0u;
    if (!set.count) return VJO_OK; /* nothing to read: the weights stay on the card */
    if (cancelled(im)) return VJO_E_CANCELLED;

    t0 = clock_us(p);
    rc = load_weights(p, dir, mi, ws + l->model_off, im);
    if (rc) return rc;
    st->model_loaded = 1;
    if (vocr_load(model, ws + l->model_off, mi->bytes) != VOCR_OK) return VJO_E_OCR_MODEL;
    memset(&opts, 0, sizeof(opts));
    opts.mode = VOCR_MODE_W8I;
    {
        size_t need = vocr_arena_bytes(model, &opts);
        if (!need || need > l->scratch) return VJO_E_OCR_MODEL;
        if (vocr_init(ctx, model, &opts, ws + l->scratch_off, need) != VOCR_OK) return VJO_E_OCR_INFERENCE;
    }
    st->load_us = clock_us(p) - t0;

    t0 = clock_us(p);
    for (unsigned i = 0; i < set.count; i++) {
        const VocrLine *ln = &set.lines[i];
        vocr_image img;
        vocr_extra extra;
        size_t start = used ? used + 1 : 0, n;
        if (cancelled(im)) return VJO_E_CANCELLED;
        if (start + 1 >= cap) return VJO_E_TOO_LARGE;
        img.pixels = grey + (size_t)ln->y * w + ln->x;
        img.width = ln->w;
        img.height = ln->h;
        img.stride = w;
        img.format = VOCR_GREY8 | (ln->dark ? VOCR_INVERT : 0);
        memset(&extra, 0, sizeof(extra));
        extra.cancel = run_cancel;
        extra.user = (void *)im;
        rc = vocr_run(ctx, &img, text + start, cap - start, &extra);
        if (rc != VOCR_OK) return run_error(rc);
        st->frames += extra.frames;
        n = strlen(text + start);
        if (!n) { /* nothing read on this line: no separator either */
            text[used] = '\0';
            continue;
        }
        if (start) text[used] = '\n';
        used = start + n;
    }
    st->recognize_us = clock_us(p) - t0;
    return VJO_OK;
}

int vjo_vocr_ocr(const VjoPlatform *platform, const char *model_dir, const char *model,
                 const VjoOcrImage *image, void *workspace, size_t workspace_size,
                 char *text, size_t text_cap, VjoVocrStats *stats)
{
    const VjoVocrModel *mi;
    Layout l;
    int rc;
    if (!platform || !model_dir || !model || !image || !text || !text_cap || !stats) return VJO_E_SOURCE;
    text[0] = '\0';
    memset(stats, 0, sizeof(*stats));
    mi = vjo_vocr_model(model);
    if (!mi) return VJO_E_OCR_MODEL;
    if (!image->rows || layout(mi, image->width, image->height, &l) < 0) return VJO_E_OCR_REGION;
    stats->workspace = l.total;
    stats->model_bytes = mi->bytes;
    stats->scratch_bytes = l.scratch;
    stats->region_bytes = (size_t)image->width * image->height;
    stats->width = image->width;
    stats->height = image->height;
    if (cancelled(image)) return VJO_E_CANCELLED;
    if (!workspace || ((uintptr_t)workspace & 15u) || workspace_size < l.total) return VJO_E_OOM;
    rc = recognize(platform, model_dir, mi, image, (unsigned char *)workspace, &l, text, text_cap, stats);
    if (rc) text[0] = '\0'; /* never publish a partial recognition */
    return rc;
}
