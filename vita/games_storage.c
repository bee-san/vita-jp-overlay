#include "games_storage.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#define BACKUP VJO_GAMES_PATH ".bak"
#define TEMP VJO_GAMES_PATH ".tmp"

static int exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

static int missing(const char *path)
{
    SceIoStat st;
    /* sceIo returns encoded Sce errno values, not newlib's errno. */
    return sceIoGetstat(path, &st) == (int)0x80010002u;
}

static int read_policy(const char *path, VjoGames *g, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    int rc = -1;
    if (fd < 0) return -1;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size >= 0 && size <= VJO_GAMES_TEXT_MAX && sceIoLseek(fd, 0, SCE_SEEK_SET) >= 0) {
        char *text = vjo_arena_alloc(scratch, (size_t)size + 1);
        size_t used = 0;
        if (text) {
            while (used < (size_t)size) {
                int n = sceIoRead(fd, text + used, (SceSize)size - used);
                if (n <= 0) break;
                used += (size_t)n;
            }
            if (used == (size_t)size && size > 0) rc = vjo_games_parse(g, text, used);
        }
    }
    sceIoClose(fd);
    vjo_arena_release(scratch, mark);
    return rc;
}

int vjo_games_file_load(VjoGames *g, VjoArena *scratch)
{
    if (read_policy(VJO_GAMES_PATH, g, scratch) == 0) return 0;
    if (read_policy(BACKUP, g, scratch) == 0) return 1;
    vjo_games_defaults(g);
    if (missing(VJO_GAMES_PATH) && missing(BACKUP)) return 0;
    g->default_enabled = 0;
    return -1;
}

int vjo_games_file_save(const VjoGames *g, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch);
    char *text = vjo_arena_alloc(scratch, VJO_GAMES_TEXT_MAX + 1);
    int len = text ? vjo_games_format(g, text, VJO_GAMES_TEXT_MAX + 1) : -1;
    int rc = -1;
    if (len < 0) goto done;
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(VJO_GAMES_DIR, 0777);
    SceUID fd = sceIoOpen(TEMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) goto done;
    int used = 0;
    while (used < len) {
        int n = sceIoWrite(fd, text + used, (SceSize)(len - used));
        if (n <= 0) break;
        used += n;
    }
    int close_rc = sceIoClose(fd);
    if (used != len || close_rc < 0 || sceIoSync("ux0:", 0) < 0) goto done;
    VjoGames *old = vjo_arena_alloc(scratch, sizeof(*old));
    if (!old) goto done;
    if (read_policy(VJO_GAMES_PATH, old, scratch) == 0) {
        if (exists(BACKUP) && sceIoRemove(BACKUP) < 0) goto done;
        if (sceIoRename(VJO_GAMES_PATH, BACKUP) < 0) goto done;
    } else if (exists(VJO_GAMES_PATH) && sceIoRemove(VJO_GAMES_PATH) < 0) goto done;
    if (sceIoSync("ux0:", 0) < 0) goto done;
    if (sceIoRename(TEMP, VJO_GAMES_PATH) < 0) goto done;
    rc = sceIoSync("ux0:", 0) < 0 ? -1 : 0;
done:
    vjo_arena_release(scratch, mark);
    return rc;
}
