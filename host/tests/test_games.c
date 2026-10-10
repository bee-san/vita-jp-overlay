#include "acutest.h"
#include "games.h"
#include "sfo.h"
#include "../../vita/games_storage.h"
#include <psp2/io/fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static unsigned char memory[40 * 1024];
static VjoArena arena;
static int write_fail, rename_fail, renames, stat_fail;
static size_t written;
static int result(int n) { return n < 0 ? (int)(0x80010000u | errno) : n; }
int sceIoOpen(const char *p, int flags, unsigned mode) { return result(open(p, flags, mode)); }
int sceIoClose(int fd) { return result(close(fd)); }
int sceIoRead(int fd, void *p, size_t len) { return result(read(fd, p, len > 13 ? 13 : len)); }
int sceIoWrite(int fd, const void *p, size_t len)
{
    if (write_fail && written >= 21) return -1;
    int n = (int)write(fd, p, len > 7 ? 7 : len);
    if (n > 0) written += n;
    return result(n);
}
SceOff sceIoLseek(int fd, SceOff off, int whence) { return lseek(fd, off, whence); }
int sceIoMkdir(const char *p, unsigned mode) { return result(mkdir(p, mode)); }
int sceIoGetstat(const char *p, SceIoStat *out)
{
    if (stat_fail) return (int)0x8001000du;
    struct stat s;
    int rc = stat(p, &s);
    if (!rc) out->st_mode = s.st_mode;
    return result(rc);
}
int sceIoRename(const char *from, const char *to)
{ if (++renames == rename_fail) return -1; return result(rename(from, to)); }
int sceIoRemove(const char *p) { return result(unlink(p)); }
int sceIoSync(const char *p, unsigned flags) { (void)p; (void)flags; return 0; }

static void reset_files(void)
{
    static int initialized;
    if (!initialized) {
        char dir[] = "/tmp/vjo-games-XXXXXX";
        TEST_ASSERT(mkdtemp(dir));
        TEST_ASSERT(chdir(dir) == 0);
        mkdir("ux0:data", 0777);
        mkdir("ux0:data/VitaJPOverlay", 0777);
        initialized = 1;
    }
    unlink(VJO_GAMES_PATH); unlink(VJO_GAMES_PATH ".bak"); unlink(VJO_GAMES_PATH ".tmp");
    write_fail = rename_fail = renames = stat_fail = 0;
    written = 0;
    vjo_arena_init(&arena, memory, sizeof(memory));
}

static void test_policy(void)
{
    VjoGames g, copy;
    char out[VJO_GAMES_TEXT_MAX + 1];
    vjo_games_defaults(&g);
    TEST_CHECK(vjo_games_enabled(&g, "PCSA00029"));
    TEST_CHECK(vjo_games_enabled(&g, "GAME"));
    TEST_ASSERT(vjo_games_set(&g, "PCSA00029", 0) == 0);
    TEST_CHECK(!vjo_games_enabled(&g, "GAME"));
    TEST_CHECK(!vjo_games_enabled(&g, "PCSA00029"));
    TEST_CHECK(vjo_games_enabled(&g, "PCSG00202"));
    g.default_enabled = 0;
    TEST_CHECK(!vjo_games_enabled(&g, "PCSG00202"));
    TEST_ASSERT(vjo_games_set(&g, "PCSG00202", 1) == 0);
    TEST_CHECK(vjo_games_enabled(&g, "PCSG00202"));
    TEST_CHECK(!vjo_games_enabled(&g, VJO_SETTINGS_TITLE_ID));
    TEST_CHECK(!vjo_games_enabled(&g, "VITASHELL"));
    TEST_CHECK(!vjo_games_enabled(&g, "NPXS10015"));
    TEST_ASSERT(vjo_games_set(&g, "NPXS10028", 1) == 0);
    TEST_CHECK(vjo_games_enabled(&g, "NPXS10028"));
    int n = vjo_games_format(&g, out, sizeof(out));
    TEST_ASSERT(n > 0);
    TEST_ASSERT(vjo_games_parse(&copy, out, n) == 0);
    TEST_CHECK(!memcmp(&g, &copy, sizeof(g)));
    TEST_CHECK(vjo_games_format(&g, out, 4) < 0);
    const char *ini = " # policy\r\n default = disabled\r\nPCSG00202=enabled\nPCSG00202=disabled\n";
    TEST_ASSERT(vjo_games_parse(&g, ini, strlen(ini)) == 0);
    TEST_CHECK(g.count == 1 && !vjo_games_enabled(&g, "PCSG00202"));
    const char *invalid[] = {"default=maybe", "../bad=disabled", "PCSA00029 disabled", "=enabled", "123456789012=enabled"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        TEST_CHECK(vjo_games_parse(&g, invalid[i], strlen(invalid[i])) < 0);
    TEST_CHECK(vjo_games_parse(&g, "a\0b", 3) < 0);
    vjo_games_defaults(&g);
    for (int i = 0; i < VJO_GAMES_MAX; i++) {
        char id[VJO_TITLE_ID_SIZE];
        snprintf(id, sizeof(id), "GAME%07d", i);
        TEST_ASSERT(vjo_games_set(&g, id, 0) == 0);
    }
    TEST_CHECK(vjo_games_set(&g, "EXTRA", 1) < 0);
    n = vjo_games_format(&g, out, sizeof(out));
    TEST_ASSERT(n > 0 && n <= VJO_GAMES_TEXT_MAX);
    TEST_ASSERT(vjo_games_parse(&copy, out, n) == 0);
    TEST_CHECK(copy.count == VJO_GAMES_MAX);
}

static void test_save_restart(void)
{
    VjoGames g, recovered;
    reset_files();
    TEST_ASSERT(vjo_games_file_load(&g, &arena) == 0);
    TEST_CHECK(g.default_enabled);
    TEST_ASSERT(vjo_games_set(&g, "PCSA00029", 0) == 0);
    TEST_ASSERT(vjo_games_file_save(&g, &arena) == 0);
    memset(&recovered, 0x55, sizeof(recovered));
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 0);
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSA00029"));
    TEST_CHECK(vjo_games_enabled(&recovered, "PCSG00202"));
    TEST_CHECK(arena.used == 0);
    g.default_enabled = 0;
    TEST_ASSERT(vjo_games_set(&g, "PCSG00202", 1) == 0);
    TEST_ASSERT(vjo_games_file_save(&g, &arena) == 0);
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 0);
    TEST_CHECK(vjo_games_enabled(&recovered, "PCSG00202"));
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSG00999"));
}

static void test_failed_save_and_backup(void)
{
    VjoGames g, recovered;
    reset_files();
    vjo_games_defaults(&g);
    vjo_games_set(&g, "PCSA00029", 0);
    TEST_ASSERT(vjo_games_file_save(&g, &arena) == 0);
    vjo_games_set(&g, "PCSA00029", 1);
    write_fail = 1; written = 0;
    TEST_CHECK(vjo_games_file_save(&g, &arena) < 0);
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 0);
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSA00029"));
    write_fail = 0; renames = 0; rename_fail = 2;
    TEST_CHECK(vjo_games_file_save(&g, &arena) < 0);
    /* Publication failed after old primary was moved: recover from .bak. */
    TEST_ASSERT(vjo_games_file_load(&recovered, &arena) == 1);
    TEST_CHECK(!vjo_games_enabled(&recovered, "PCSA00029"));
    rename_fail = 0;
    TEST_ASSERT(vjo_games_file_save(&recovered, &arena) == 0);
    TEST_ASSERT(vjo_games_file_load(&g, &arena) == 0);
    TEST_CHECK(!vjo_games_enabled(&g, "PCSA00029"));
    TEST_CHECK(arena.used == 0);
}

static void test_corruption_disables_input_reservation(void)
{
    VjoGames g;
    reset_files();
    int fd = open(VJO_GAMES_PATH, O_CREAT | O_WRONLY, 0666);
    TEST_ASSERT(fd >= 0);
    TEST_ASSERT(write(fd, "default=garbage", 15) == 15);
    close(fd);
    TEST_CHECK(vjo_games_file_load(&g, &arena) < 0);
    TEST_CHECK(!vjo_games_enabled(&g, "PCSA00029"));
    reset_files();
    stat_fail = 1;
    TEST_CHECK(vjo_games_file_load(&g, &arena) < 0);
    TEST_CHECK(!g.default_enabled); /* permission failure is not a missing file */
}

static void put32(unsigned char *p, uint32_t n)
{ for (int i = 0; i < 4; i++) p[i] = n >> (i * 8); }
static void test_sfo_bounds(void)
{
    unsigned char sfo[80] = {0};
    char out[80];
    put32(sfo, 0x46535000); put32(sfo + 8, 36); put32(sfo + 12, 48); put32(sfo + 16, 1);
    sfo[22] = 4; sfo[23] = 2;
    put32(sfo + 24, 10); put32(sfo + 28, 16);
    memcpy(sfo + 36, "TITLE", 6); memcpy(sfo + 48, "日本語", 10);
    TEST_ASSERT(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) == 0);
    TEST_CHECK(!strcmp(out, "日本語"));
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, 3) < 0);
    for (size_t i = 0; i < 64; i++) TEST_CHECK(vjo_sfo_string(sfo, i, "TITLE", out, sizeof(out)) < 0);
    put32(sfo + 32, UINT32_MAX);
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
    put32(sfo + 16, UINT32_MAX);
    TEST_CHECK(vjo_sfo_string(sfo, sizeof(sfo), "TITLE", out, sizeof(out)) < 0);
}

TEST_LIST = {
    {"per-title blacklist and selected-only policies", test_policy},
    {"saved choices survive fresh loads", test_save_restart},
    {"partial writes and interrupted rename retain disabled games", test_failed_save_and_backup},
    {"corrupt or unreadable settings disable OCR", test_corruption_disables_input_reservation},
    {"Japanese SFO titles and malicious offsets", test_sfo_bounds},
    {NULL, NULL}
};
