/* Exercise the real Vita queue files and worker against POSIX I/O and canned
 * AnkiConnect replies. The arena is the actual Vita-sized 384 KiB allocation. */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include "acutest.h"
#include "replay.h"
#include "../../shell/anki.c"
#include "../../shell/anki_queue.c"

VjoView g_view;
void vjo_view_lock(void) {}
void vjo_view_unlock(void) {}
void vjo_log(const char *fmt, ...) {}
void vjo_platform_vita(VjoPlatform *p) {}
uint64_t sceKernelGetProcessTimeWide(void) { return 1000; }
int sceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }
int sceRtcGetCurrentTick(SceRtcTick *tick) { tick->tick = 64000000000000000ull; return 0; }
int vjo_net_local_ipv4(uint32_t *ip, uint32_t *mask) { return -1; }
int vjo_net_probe_port(const uint32_t *ips, int n, int port, int timeout, uint8_t *open) { return 0; }
int vjo_capture_jpeg(VjoArena *a, uint32_t flags, int quality, VjoBuf *out, VjoState *st) { return VJO_E_NET; }
char *vjo_file_read(VjoArena *a, const char *path, size_t *len) { return NULL; }
int vjo_file_write(const char *path, const void *data, size_t len) { return 0; }

static DIR *directory;
static char directory_path[256], test_dir[128];
static int write_budget, short_io, fail_rename, fail_sync, fail_remove, fail_close, fail_scan;
int sceIoOpen(const char *p, int flags, unsigned mode) { return open(p, flags, mode); }
int sceIoClose(int fd) { int rc = close(fd); return fail_close ? -1 : rc; }
int sceIoRead(int fd, void *p, size_t n) { return (int)read(fd, p, short_io && n > 7 ? 7 : n); }
int sceIoWrite(int fd, const void *p, size_t n)
{
    if (!write_budget)
        return -1;
    if (write_budget > 0 && n > (size_t)write_budget)
        n = (size_t)write_budget;
    if (short_io && n > 7)
        n = 7;
    if (write_budget > 0)
        write_budget -= (int)n;
    return (int)write(fd, p, n);
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return mkdir(p, mode); }
int sceIoGetstat(const char *p, SceIoStat *st)
{
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) st->st_mode = s.st_mode;
    return rc;
}
int sceIoRename(const char *from, const char *to) { return fail_rename ? -1 : rename(from, to); }
int sceIoRemove(const char *p) { return fail_remove ? -1 : unlink(p); }
int sceIoSyncByFd(int fd, int flag) { return fail_sync ? -1 : fsync(fd); }
int sceIoSync(const char *device, unsigned flags) { return fail_sync ? -1 : 0; }
int sceIoDopen(const char *p)
{
    TEST_ASSERT(!directory);
    snprintf(directory_path, sizeof(directory_path), "%s", p);
    directory = opendir(p);
    return directory ? 10000 : -1;
}
int sceIoDread(int fd, SceIoDirent *ent)
{
    struct dirent *e;
    char path[512];
    if (fail_scan) return -1;
    e = readdir(directory);
    if (!e) return 0;
    snprintf(ent->d_name, sizeof(ent->d_name), "%s", e->d_name);
    snprintf(path, sizeof(path), "%s/%s", directory_path, e->d_name);
    TEST_ASSERT(sceIoGetstat(path, &ent->d_stat) == 0);
    return 1;
}
int sceIoDclose(int fd) { int rc = closedir(directory); directory = NULL; return rc; }

static uint8_t memory[ANKI_MEM_SIZE], jpeg[96 * 1024];
static const char *meanings[] = {"cat \"quoted\" & <tag>\nnext line", "back\\slash", "日本語"};
static VjoAnkiNote note = {"猫", "ねこ", 1234, meanings, 3, "猫が\n好き𠮷", 0, 3, NULL};
static const char *replies[512];
static int n_replies, n_connect, n_sent;
static VjoMemConn connection;
static char response[2048], requests[512][2048];
static int connect_fake(void *ud, const char *host, int port, int timeout, int io_timeout, VjoConn *out)
{
    const char *reply;
    int index = n_connect++;
    if (index >= n_replies || !(reply = replies[index])) return VJO_E_NET;
    TEST_CHECK(!strcmp(host, "anki.local") && port == 8765);
    snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Length: %u\r\n\r\n%s",
             (unsigned)strlen(reply), reply);
    memset(&connection, 0, sizeof(connection));
    connection.in = response;
    connection.len = strlen(response);
    connection.out = requests[n_sent];
    connection.out_cap = sizeof(requests[0]) - 1;
    vjo_memconn_init(&connection, out);
    return VJO_OK;
}
static void disconnect_fake(void *ud, VjoConn *c)
{
    requests[n_sent][connection.out_len] = '\0';
    n_sent++;
}
static void network(int n, const char **responses)
{
    n_connect = n_sent = 0;
    n_replies = n;
    memset(requests, 0, sizeof(requests));
    memcpy(replies, responses, sizeof(*responses) * (size_t)n);
}
#define EMPTY "{\"result\":[],\"error\":null}"
#define ADDED "{\"result\":1791436200000,\"error\":null}"
#define FOUND "{\"result\":[1791436200000],\"error\":null}"
#define DUPLICATE "{\"result\":null,\"error\":\"cannot create note because it is a duplicate\"}"

static void clear_files(void)
{
    char p[512];
    DIR *d = opendir(QUEUE_DIR);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), QUEUE_DIR "/%s", e->d_name);
        unlink(p);
    }
    closedir(d);
}
static void setup(void)
{
    if (!test_dir[0]) {
        snprintf(test_dir, sizeof(test_dir), "/tmp/vjo-queue-tests-XXXXXX");
        TEST_ASSERT(mkdtemp(test_dir) != NULL);
        TEST_ASSERT(chdir(test_dir) == 0);
        mkdir("ux0:", 0777);
        mkdir("ux0:data", 0777);
    }
    clear_files();
    memset(memory, 0xA5, sizeof(memory));
    vjo_arena_init(&arena, memory, sizeof(memory));
    memset(&g_view, 0, sizeof(g_view));
    memset(&plat, 0, sizeof(plat));
    plat.connect = connect_fake;
    plat.disconnect = disconnect_fake;
    write_budget = -1;
    short_io = fail_rename = fail_sync = fail_remove = fail_close = fail_scan = 0;
    running = 1;
    vjo_config_defaults(&acfg);
    strcpy(acfg.anki_host, "anki.local");
    n_replies = n_connect = n_sent = 0;
}
static void save_note(const char *spelling)
{
    VjoAnkiNote n = note;
    VjoAnkiMedia m = {0};
    n.spelling = spelling;
    vjo_arena_reset(&arena);
    TEST_ASSERT(vjo_queue_save(&arena, &n, &m) == 0);
}

static void test_offline_restart(void)
{
    VjoAnkiNote restored;
    VjoAnkiMedia m = {jpeg, sizeof(jpeg), "ignored.jpg", NULL, NULL}, loaded;
    char id[VJO_QUEUE_ID_SIZE];
    setup();
    short_io = 1;
    memset(jpeg, 0xCC, sizeof(jpeg));
    TEST_ASSERT(vjo_queue_save(&arena, &note, &m) == 0);
    TEST_CHECK(vjo_queue_count() == 1);
    vjo_arena_reset(&arena);
    TEST_CHECK(vjo_queue_save(&arena, &note, &m) == 1);
    TEST_CHECK(vjo_queue_count() == 1);
    /* A reboot loses all memory but preserves text and screenshot on disk. */
    memset(memory, 0xA5, sizeof(memory));
    vjo_arena_reset(&arena);
    TEST_ASSERT(vjo_queue_first(id) == 1);
    TEST_ASSERT(vjo_queue_load(&arena, id, &restored, &loaded) == 0);
    TEST_CHECK(!strcmp(restored.spelling, note.spelling));
    TEST_CHECK(!strcmp(restored.reading, note.reading));
    TEST_CHECK(!strcmp(restored.sentence, note.sentence));
    TEST_CHECK(restored.rank == 1234 && restored.hl_end == 3 && restored.n_meanings == 3);
    for (int i = 0; i < 3; i++) TEST_CHECK(!strcmp(restored.meanings[i], meanings[i]));
    TEST_CHECK(loaded.picture_len == sizeof(jpeg) && !memcmp(loaded.picture, jpeg, sizeof(jpeg)));
    TEST_CHECK(n_connect == 0);
    TEST_CHECK(vjo_queue_remove(id) == 0 && vjo_queue_count() == 0);
}

static void test_storage_failures(void)
{
    VjoAnkiMedia m = {0};
    char id[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    setup();
    for (int mode = 0; mode < 4; mode++) {
        write_budget = mode == 0 ? 11 : -1;
        fail_rename = mode == 1;
        fail_sync = mode == 2;
        fail_close = mode == 3;
        vjo_arena_reset(&arena);
        TEST_CHECK(vjo_queue_save(&arena, &note, &m) == -1);
        TEST_CHECK(vjo_queue_count() == 0); /* .tmp is never a pending card */
        fail_close = fail_rename = fail_sync = 0;
    }
    write_budget = -1;
    vjo_arena_reset(&arena);
    TEST_CHECK(vjo_queue_save(&arena, &note, &m) == 0);
    TEST_ASSERT(vjo_queue_first(id) == 1);
    path_for(path, id, "json");
    /* Truncation, checksum mismatch, or missing media never delete anything. */
    int fd = open(path, O_WRONLY | O_TRUNC);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(write(fd, "{bad", 4) == 4);
    close(fd);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && n_connect == 0);
    TEST_CHECK(strstr(g_view.anki_status, "kept") != NULL);
    fail_scan = 1;
    TEST_CHECK(vjo_queue_count() == -1);
    fail_scan = 0;
    TEST_CHECK(vjo_queue_remove("../config") == -1);
}

static void test_partial_sync_and_retry(void)
{
    const char *partial[] = {EMPTY, ADDED, EMPTY, NULL};
    const char *retry[] = {EMPTY, ADDED};
    setup();
    save_note("猫");
    save_note("犬");
    network(4, partial);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && g_view.anki_pending == 1);
    TEST_CHECK(strstr(g_view.anki_status, "1 sent; rest kept") != NULL);
    TEST_CHECK(strstr(requests[1], "vita_queue_") != NULL);
    network(2, retry);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && g_view.anki_pending == 0);
    TEST_CHECK(n_connect == 2 && strstr(g_view.anki_status, "Sync complete"));
}

static void test_ambiguous_reply(void)
{
    const char *lost[] = {EMPTY, "{\"result\":"};
    const char *retry[] = {FOUND};
    setup();
    save_note("猫");
    network(2, lost);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1);
    /* Server accepted the add but the acknowledgement was lost. */
    network(1, retry);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && n_connect == 1);
    TEST_CHECK(strstr(requests[0], "findNotes") && !strstr(requests[0], "addNote"));
    TEST_CHECK(strstr(requests[0], "tag:vita_queue_"));
}

static void test_refused_and_bad_replies(void)
{
    const char *invalid[] = {"{\"result\":null,\"error\":null}", "{\"result\":true,\"error\":null}",
                            "{\"result\":0,\"error\":null}", "{\"result\":\"123\",\"error\":null}",
                            "{\"result\":123.5,\"error\":null}",
                            "{\"result\":123,\"error\":false}",
                            "{\"result\":123,\"error\":null} {}",
                            "{\"result\":null,\"error\":\"model was not found: Lapis\"}"};
    setup();
    save_note("猫");
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        const char *r[] = {EMPTY, invalid[i]};
        network(2, r);
        do_sync();
        TEST_CHECK(vjo_queue_count() == 1);
    }
    const char *bad_find[] = {"{\"result\":[null],\"error\":null}"};
    network(1, bad_find);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && n_connect == 1);
    acfg.anki_host[0] = '\0';
    n_connect = 0;
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && n_connect == 0 && strstr(g_view.anki_status, "anki_host"));
}

static void test_duplicates_continue(void)
{
    const char *r[] = {EMPTY, DUPLICATE, EMPTY, ADDED};
    char id[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    VjoAnkiMedia media = {0};
    VjoAnkiNote original;
    setup();
    save_note("猫");
    save_note("犬");
    TEST_ASSERT(vjo_queue_first(id) == 1);
    vjo_arena_reset(&arena);
    TEST_ASSERT(vjo_queue_load(&arena, id, &original, &media) == 0);
    char spelling[32];
    snprintf(spelling, sizeof(spelling), "%s", original.spelling);
    network(4, r);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && n_connect == 4);
    TEST_CHECK(strstr(g_view.anki_status, "1 sent, 1 duplicates kept"));
    path_for(path, id, "duplicate.json");
    TEST_CHECK(access(path, F_OK) == 0);
    original = note;
    original.spelling = spelling;
    vjo_arena_reset(&arena);
    TEST_CHECK(vjo_queue_save(&arena, &original, &media) == 2);
}

static void test_screenshot_sync_and_missing_media(void)
{
    const char *r[] = {EMPTY, ADDED};
    VjoAnkiMedia media = {jpeg, sizeof(jpeg), "capture.jpg", NULL, NULL};
    char id[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    setup();
    TEST_ASSERT(vjo_queue_save(&arena, &note, &media) == 0);
    network(2, r);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && n_connect == 2);
    TEST_CHECK(strstr(requests[1], "\"picture\"") && strstr(requests[1], "\"data\""));
    TEST_CHECK(arena.peak < ANKI_MEM_SIZE);
    TEST_MSG("96 KiB screenshot sync peak: %zu bytes", arena.peak);
    vjo_arena_reset(&arena);
    TEST_ASSERT(vjo_queue_save(&arena, &note, &media) == 0);
    TEST_ASSERT(vjo_queue_first(id) == 1);
    path_for(path, id, "jpg");
    TEST_ASSERT(unlink(path) == 0);
    network(2, r);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && n_connect == 0);
}

static void test_save_button_offline(void)
{
    VjoVocab vocab = {0};
    VjoEntry entry = {0};
    VjoEntryList list = {0};
    setup();
    acfg.anki_host[0] = '\0';
    vocab.spelling = note.spelling;
    vocab.reading = note.reading;
    vocab.rank = note.rank;
    vocab.meanings = note.meanings;
    vocab.n_meanings = note.n_meanings;
    entry.vocab = &vocab;
    entry.hl_start = 0;
    entry.hl_end = 3;
    list.header = note.sentence;
    list.entries = &entry;
    list.n_entries = 1;
    g_view.open = 1;
    g_view.list_seq = 42;
    g_view.list = &list;
    do_add(42, 0);
    TEST_CHECK(vjo_queue_count() == 1 && n_connect == 0 && g_view.anki_mark[0] == 2);
    TEST_CHECK(strstr(g_view.anki_status, "Saved offline without screenshot"));
    do_add(42, 0);
    TEST_CHECK(vjo_queue_count() == 1);
    /* The same entry on a new list is deduplicated by its persisted content ID. */
    g_view.list_seq++;
    do_add(43, 0);
    TEST_CHECK(vjo_queue_count() == 1 && strstr(g_view.anki_status, "Already queued"));
    g_view.list = NULL;
}

static void test_audio_waits_for_sync(void)
{
    const char *offline[] = {EMPTY, NULL};
    const char *without_audio[] = {EMPTY, ADDED};
    setup();
    strcpy(acfg.anki_audio_url, "http://anki.local:8765/audio?term={term}");
    save_note("猫");
    TEST_CHECK(n_connect == 0);
    network(2, offline);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && n_connect == 2);
    TEST_CHECK(strstr(g_view.anki_status, "audio source offline"));
    acfg.anki_audio_url[0] = '\0';
    network(2, without_audio);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && n_connect == 2);
}

static void test_invalid_record_and_size_limits(void)
{
    VjoAnkiNote decoded, huge = note;
    VjoAnkiMedia media = {0};
    int picture;
    char *json;
    setup();
    json = vjo_queue_encode(&arena, &note, 0);
    TEST_ASSERT(json != NULL);
    char saved[2048];
    snprintf(saved, sizeof(saved), "%s", json);
    const char *bad[] = {"{}", "null", "{\"version\":2}",
                        "{\"version\":1,\"spelling\":\"猫\"}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        vjo_arena_reset(&arena);
        TEST_CHECK(vjo_queue_decode(&arena, bad[i], strlen(bad[i]), &decoded, &picture) == -1);
    }
    vjo_arena_reset(&arena);
    saved[11] = '2'; /* schema version */
    TEST_CHECK(vjo_queue_decode(&arena, saved, strlen(saved), &decoded, &picture) == -1);
    static char huge_text[VJO_QUEUE_TEXT_MAX + 1];
    memset(huge_text, 'x', sizeof(huge_text) - 1);
    huge_text[sizeof(huge_text) - 1] = '\0';
    huge.sentence = huge_text;
    vjo_arena_reset(&arena);
    TEST_CHECK(vjo_queue_save(&arena, &huge, &media) == -1 && vjo_queue_count() == 0);
    media.picture = jpeg;
    media.picture_len = VJO_QUEUE_PICTURE_MAX + 1;
    vjo_arena_reset(&arena);
    TEST_CHECK(vjo_queue_save(&arena, &note, &media) == -1 && vjo_queue_count() == 0);
}

static void test_cleanup_retry(void)
{
    const char *add[] = {EMPTY, ADDED}, *retry[] = {FOUND};
    setup();
    save_note("猫");
    fail_remove = 1;
    network(2, add);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 1 && strstr(g_view.anki_status, "cleanup failed"));
    fail_remove = 0;
    network(1, retry);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && n_connect == 1);
}

static void test_large_queue_bounded_memory(void)
{
    const char *responses[200];
    char word[64];
    setup();
    for (int i = 0; i < 100; i++) {
        snprintf(word, sizeof(word), "言葉%d", i);
        save_note(word);
        responses[2 * i] = EMPTY;
        responses[2 * i + 1] = ADDED;
    }
    network(200, responses);
    do_sync();
    TEST_CHECK(vjo_queue_count() == 0 && n_connect == 200);
    TEST_CHECK(arena.peak < 32 * 1024);
    TEST_MSG("100-card text sync peak: %zu bytes", arena.peak);
}

TEST_LIST = {
    {"offline restart and screenshot", test_offline_restart},
    {"short I/O, failed writes, corrupt files", test_storage_failures},
    {"partial sync and retry", test_partial_sync_and_retry},
    {"lost response idempotence", test_ambiguous_reply},
    {"refused and malformed replies retained", test_refused_and_bad_replies},
    {"cleanup failure idempotence", test_cleanup_retry},
    {"duplicates archived without blocking", test_duplicates_continue},
    {"screenshot sync and missing JPEG", test_screenshot_sync_and_missing_media},
    {"save button with no Anki host", test_save_button_offline},
    {"100-card queue bounded memory", test_large_queue_bounded_memory},
    {"audio waits for sync", test_audio_waits_for_sync},
    {"invalid records and size limits", test_invalid_record_and_size_limits},
    {NULL, NULL}
};
