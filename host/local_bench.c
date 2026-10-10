/* Benchmark the real overlay lookup with counted file callbacks, without OCR,
 * stdout rendering or process startup in the timer. All buffers here are host
 * instrumentation; reported arena_peak measures the production arena only. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "client.h"
#include "local_dict.h"
#include "net_posix.h"

typedef struct {
    VjoFile inner;
    uint64_t last_end;
    int active;
} TrackedFile;
static TrackedFile files[VJO_LOCAL_MAX_DICTS];
static uint64_t calls, bytes, seeks;
static int opened, closed;
static int (*inner_open)(void *, const char *, VjoFile *);

static int tracked_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    TrackedFile *f = ctx;
    calls++; bytes += n;
    if (off != f->last_end) seeks++;
    f->last_end = off + n;
    return f->inner.read(f->inner.ctx, off, dst, n);
}
static void tracked_close(void *ctx)
{
    TrackedFile *f = ctx;
    f->inner.close(f->inner.ctx); f->active = 0; closed++;
}
static int tracked_open(void *ud, const char *path, VjoFile *out)
{
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        if (files[i].active) continue;
        TrackedFile *f = &files[i];
        if (inner_open(ud, path, &f->inner)) return -1;
        f->active = 1; f->last_end = 0; opened++;
        *out = (VjoFile){f, f->inner.size, tracked_read, tracked_close};
        return 0;
    }
    return -1;
}
static uint64_t nanoseconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
static uint64_t hash_string(uint64_t hash, const char *s)
{
    do { hash = (hash ^ (uint8_t)*s) * UINT64_C(1099511628211); } while (*s++);
    return hash;
}
int main(int argc, char **argv)
{
    static uint8_t mem[384 * 1024];
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    int runs;
    if (argc != 5 || (runs = atoi(argv[4])) < 1 || runs > 10000) {
        fprintf(stderr, "usage: vjo-local-bench DIR FILENAMES TEXT RUNS\n"); return 2;
    }
    vjo_config_defaults(&cfg); cfg.dictionary = VJO_DICT_LOCAL;
    if (strlen(argv[1]) >= sizeof(cfg.local_dictionary_dir) || strlen(argv[2]) >= sizeof(cfg.local_dictionaries)) return 2;
    strcpy(cfg.local_dictionary_dir, argv[1]); strcpy(cfg.local_dictionaries, argv[2]);
    posix_platform_init(&pp, &p); inner_open = p.file_open; p.file_open = tracked_open;
    for (int run = 0; run < runs; run++) {
        VjoArena a;
        VjoOverlayData d = {0};
        uint64_t start, elapsed, digest = UINT64_C(14695981039346656037);
        char range[64];
        calls = bytes = seeks = 0; opened = closed = 0;
        vjo_arena_init(&a, mem, sizeof(mem));
        start = nanoseconds();
        int rc = vjo_overlay_from_text(&a, &p, &cfg, argv[3], &d);
        elapsed = nanoseconds() - start;
        if (rc || opened != closed) {
            fprintf(stderr, "%s (handles %d/%d)\n", vjo_err_text(&a, d.failed_stage, &d.err), opened, closed); return 1;
        }
        for (int i = 0; i < d.list.n_entries; i++) {
            const VjoEntry *e = &d.list.entries[i];
            digest = hash_string(digest, e->text);
            snprintf(range, sizeof(range), "%d,%d,%d", e->pos16, e->hl_start, e->hl_end);
            digest = hash_string(digest, range);
        }
        printf("{\"run\":%d,\"lookup_us\":%.3f,\"read_calls\":%" PRIu64 ",\"read_bytes\":%" PRIu64
               ",\"nonsequential_reads\":%" PRIu64 ",\"arena_peak\":%zu,\"entries\":%d,\"digest\":\"%016" PRIx64 "\"}\n",
               run, elapsed / 1000.0, calls, bytes, seeks, a.peak, d.list.n_entries, digest);
    }
    return 0;
}
