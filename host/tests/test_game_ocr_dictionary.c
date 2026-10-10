/* Optional integration test against the user's private, installed dictionary.
 * No dictionary or game dialogue is checked in. Set VJO_JMDICT_FILE to a
 * converted .vjdict file; unset means an explicit CTest skip (exit 77).
 * Measure the production Shell pipeline with its game OCR result resident.
 * Host arena peaks are measurements of this host ABI, not physical Vita RAM. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "client.h"
#include "local_dict.h"
#include "net_posix.h"
#include "textfilter.h"
#include "vjo_game_ocr.h"

#define ARENA_BYTES (160u * 1024u)
#define REPEATS 3

typedef char ResultWireSizeMustBe4120[(sizeof(VjoGameOcrResult) == 4120) ? 1 : -1];
static struct {
    uint64_t before[4];
    uint8_t bytes[ARENA_BYTES];
    uint64_t after[4];
} storage;
static const uint64_t canary = UINT64_C(0xb7a0c4d52e9f8163);

typedef struct {
    VjoFile inner;
    int active;
} TrackedFile;
static TrackedFile files[VJO_LOCAL_MAX_DICTS];
static int opened, closed;
static uint64_t read_calls, read_bytes;
static int (*inner_open)(void *, const char *, VjoFile *);

static int tracked_read(void *ctx, uint64_t off, void *dst, size_t n)
{
    TrackedFile *f = ctx;
    read_calls++;
    read_bytes += n;
    return f->inner.read(f->inner.ctx, off, dst, n);
}

static void tracked_close(void *ctx)
{
    TrackedFile *f = ctx;
    f->inner.close(f->inner.ctx);
    f->active = 0;
    closed++;
}

static int tracked_open(void *ud, const char *path, VjoFile *out)
{
    for (int i = 0; i < VJO_LOCAL_MAX_DICTS; i++) {
        TrackedFile *f = &files[i];
        if (f->active) continue;
        if (inner_open(ud, path, &f->inner)) return -1;
        f->active = 1;
        opened++;
        *out = (VjoFile){f, f->inner.size, tracked_read, tracked_close};
        return 0;
    }
    return -1;
}

typedef struct {
    const char *name;
    const char *text;
    const char *required_spelling;
    int expected_rc;
    int needs_entries;
    int no_dictionary;
} Case;

static int contains_spelling(const VjoOverlayData *d, const char *spelling)
{
    for (int i = 0; i < d->list.n_entries; i++)
        if (!strcmp(d->list.entries[i].vocab->spelling, spelling)) return 1;
    return 0;
}

static int check_case(const Case *c, const VjoConfig *cfg, const VjoPlatform *p,
                      int run, size_t *peak)
{
    VjoArena a;
    VjoOverlayData d = {0};
    VjoGameOcrResult *result;
    int rc;
    for (int i = 0; i < 4; i++) storage.before[i] = storage.after[i] = canary;
    opened = closed = 0;
    read_calls = read_bytes = 0;
    vjo_arena_init(&a, storage.bytes, sizeof(storage.bytes));
    result = vjo_arena_zalloc(&a, sizeof(*result));
    if (!result || strlen(c->text) >= sizeof(result->text)) return 1;
    result->size = sizeof(*result);
    result->seq = 17;
    result->neural_peak = 20u * 1024u * 1024u;
    strcpy(result->text, c->text);

    /* Same ordering and retained result allocation as Shell's game branch. */
    rc = vjo_overlay_ocr_text(&a, cfg, result->text, &d);
    if (!rc) rc = vjo_overlay_lookup(&a, p, cfg, &d);
    if (rc != c->expected_rc || opened != closed || a.used > a.size || a.peak > a.size ||
        result->size != sizeof(*result) || result->seq != 17 ||
        result->neural_peak != 20u * 1024u * 1024u || strcmp(result->text, c->text) ||
        (c->needs_entries && d.list.n_entries < 1) ||
        (c->required_spelling && !contains_spelling(&d, c->required_spelling)) ||
        (c->no_dictionary && (opened || d.list.n_entries)) ||
        (rc && (d.failed_stage != VJO_STAGE_DICT || d.list.n_entries))) {
        fprintf(stderr, "FAIL %s run=%d rc=%d expected=%d handles=%d/%d entries=%d peak=%zu\n",
                c->name, run, rc, c->expected_rc, opened, closed, d.list.n_entries, a.peak);
        return 1;
    }
    for (int i = 0; i < 4; i++) {
        if (storage.before[i] != canary || storage.after[i] != canary) {
            fprintf(stderr, "FAIL %s: arena guard overwritten\n", c->name);
            return 1;
        }
    }
    if (a.peak > *peak) *peak = a.peak;
    printf("{\"case\":\"%s\",\"run\":%d,\"text_bytes\":%zu,\"rc\":%d,"
           "\"entries\":%d,\"arena_peak\":%zu,\"arena_used\":%zu,"
           "\"headroom\":%zu,\"read_calls\":%" PRIu64 ",\"read_bytes\":%" PRIu64 "}\n",
           c->name, run, strlen(c->text), rc, d.list.n_entries, a.peak, a.used,
           a.size - a.peak, read_calls, read_bytes);
    return 0;
}

int main(void)
{
    const char *fixture = getenv("VJO_JMDICT_FILE");
    const char *slash;
    struct stat st;
    VjoConfig cfg;
    PosixPlatform pp = {0};
    VjoPlatform p;
    size_t peak = 0;
    char repeated[577], wire_success[VJO_GAME_OCR_TEXT_BYTES];
    char wire_limit[VJO_GAME_OCR_TEXT_BYTES];
    Case cases[] = {
        {"blank", "", NULL, VJO_OK, 0, 1},
        {"filtered_menu", "MENU\nSCORE 100\nSKIP", NULL, VJO_OK, 0, 1},
        {"exact_word", "日本語", "日本語", VJO_OK, 1, 0},
        {"inflected_verb", "食べました。", "食べる", VJO_OK, 1, 0},
        {"multiline", "学校へ行きました。\n日本語の本を読んでいます。", "学校", VJO_OK, 1, 0},
        {"mixed_menu", "MENU 100\n学校で日本語を勉強します。\nSKIP", "日本語", VJO_OK, 1, 0},
        {"katakana", "コンピューターでゲームを遊びます。", NULL, VJO_OK, 1, 0},
        {"rich_matches", "生の意味を考えます。愛と心について話します。", NULL, VJO_OK, 1, 0},
        {"64_repeated_tokens", repeated, "学校", VJO_OK, 1, 0},
        {"wire_limit_success", wire_success, "学校", VJO_OK, 1, 0},
        {"wire_limit_bounded_error", wire_limit, NULL, VJO_E_TOO_LARGE, 0, 0},
        /* A fresh request must remain usable after the previous bounded error. */
        {"after_bounded_error", "日本語", "日本語", VJO_OK, 1, 0}
    };
    if (!fixture || !*fixture) {
        puts("SKIP: set VJO_JMDICT_FILE to a private installed .vjdict fixture");
        return 77;
    }
    slash = strrchr(fixture, '/');
    if (fixture[0] != '/' || !slash || slash == fixture || !slash[1] || stat(fixture, &st) ||
        !S_ISREG(st.st_mode) || st.st_size <= 0) {
        fprintf(stderr, "FAIL: VJO_JMDICT_FILE must name an existing absolute .vjdict file\n");
        return 1;
    }
    vjo_config_defaults(&cfg);
    cfg.dictionary = VJO_DICT_LOCAL;
    cfg.non_japanese_filter = VJO_FILTER_LINES;
    if ((size_t)(slash - fixture) >= sizeof(cfg.local_dictionary_dir) ||
        strlen(slash + 1) >= sizeof(cfg.local_dictionaries)) {
        fprintf(stderr, "FAIL: fixture path exceeds production configuration capacity\n");
        return 1;
    }
    memcpy(cfg.local_dictionary_dir, fixture, (size_t)(slash - fixture));
    cfg.local_dictionary_dir[slash - fixture] = 0;
    strcpy(cfg.local_dictionaries, slash + 1);
    posix_platform_init(&pp, &p);
    inner_open = p.file_open;
    p.file_open = tracked_open;
    for (int i = 0; i < 64; i++) memcpy(repeated + i * 9, "学校。", 9);
    repeated[576] = 0;
    memcpy(wire_success, "学校。", 9);
    for (int i = 0; i < 1362; i++) memcpy(wire_success + 9 + i * 3, "。", 3);
    wire_success[4095] = 0;
    for (int i = 0; i < 455; i++) memcpy(wire_limit + i * 9, "学校。", 9);
    wire_limit[4095] = 0;

    for (int run = 0; run < REPEATS; run++)
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            if (check_case(&cases[i], &cfg, &p, run, &peak)) return 1;
    printf("{\"summary\":true,\"fixture_bytes\":%" PRIdMAX ",\"arena_bytes\":%u,"
           "\"resident_result_bytes\":%zu,\"cases\":%zu,\"repeats\":%d,"
           "\"max_host_arena_peak\":%zu,\"min_host_headroom\":%zu}\n",
           (intmax_t)st.st_size, ARENA_BYTES, sizeof(VjoGameOcrResult),
           sizeof(cases) / sizeof(cases[0]), REPEATS, peak, ARENA_BYTES - peak);
    return 0;
}
