/* Which game the foreground process is: its title ID, and whether that is
 * a game at all. */
#include <psp2/appmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "shell.h"
#include "../core/games.h"
#include "../vita/games_storage.h"

int vjo_game_enabled(const char *tid, VjoArena *scratch)
{
    size_t mark = vjo_arena_mark(scratch);
    VjoGames *games = vjo_arena_alloc(scratch, sizeof(*games));
    int rc = games ? vjo_games_file_load(games, scratch) : -1;
    int enabled = rc >= 0 && vjo_games_enabled(games, tid);
    if (rc < 0) vjo_log("games.ini unreadable: OCR disabled until settings are repaired");
    else if (rc > 0) vjo_log("games.ini recovered from backup");
    vjo_arena_release(scratch, mark);
    return enabled;
}

int sceKernelGetProcessTitleId(SceUID pid, char *titleid, SceSize len); /* SceProcessmgr, not in headers */

int vjo_title_id(SceUID pid, char *tid, int size)
{
    int ret;
    sceClibMemset(tid, 0, size);
    ret = sceKernelGetProcessTitleId(pid, tid, size);
    if (ret >= 0 && tid[0])
        return 0;
    sceClibMemset(tid, 0, size);
    ret = sceAppMgrAppParamGetString(pid, 12, tid, size); /* 12 = title ID */
    if (ret >= 0 && tid[0])
        return 0;
    return ret < 0 ? ret : -1;
}

/* vjoSetGameActive's mode for a title: system apps, SceShell and VitaShell
 * (Select starts its FTP server) are not games. The PSP emulator, where
 * Adrenaline's games run, is one with a static framebuffer; all its games
 * share its title ID, so one region. */
int vjo_title_game_mode(const char *tid)
{
    if (!sceClibStrcmp(tid, "NPXS10028"))
        return VJO_GAME_STATIC_FB;
    if (!vjo_game_is_candidate(tid))
        return VJO_GAME_NONE;
    return VJO_GAME;
}
