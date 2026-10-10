#include "catalog.h"
#include "../core/sfo.h"
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int name_order(const void *a, const void *b)
{
    const VjoInstalledGame *ga = a, *gb = b;
    int n = strcmp(ga->title, gb->title);
    return n ? n : strcmp(ga->id, gb->id);
}

static void read_title(const char *path, char *out, size_t cap)
{
    /* One bounded SFO buffer reused during enumeration, never while drawing. */
    static unsigned char sfo[64 * 1024];
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (size > 0 && size <= sizeof(sfo)) {
        size_t used = 0;
        while (used < (size_t)size) {
            int n = sceIoRead(fd, sfo + used, (size_t)size - used);
            if (n <= 0) break;
            used += n;
        }
        if (used == (size_t)size) vjo_sfo_string(sfo, used, "TITLE", out, cap);
    }
    sceIoClose(fd);
    /* Some game names contain line breaks, which do not belong in list rows. */
    for (char *s = out; *s; s++) if ((unsigned char)*s < 32) *s = ' ';
}

int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated)
{
    const char *roots[] = {"ux0:app", "ur0:app", "gro0:app"};
    int count = 0;
    *truncated = 0;
    for (unsigned r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        SceUID dir = sceIoDopen(roots[r]);
        if (dir < 0) continue;
        SceIoDirent e;
        memset(&e, 0, sizeof(e));
        while (sceIoDread(dir, &e) > 0) {
            if (!SCE_S_ISDIR(e.d_stat.st_mode) || !vjo_game_is_candidate(e.d_name)) continue;
            int found = 0;
            for (int i = 0; i < count; i++) if (!strcmp(out[i].id, e.d_name)) found = 1;
            if (found) continue;
            if (count >= capacity) { *truncated = 1; continue; }
            VjoInstalledGame *g = &out[count++];
            snprintf(g->id, sizeof(g->id), "%s", e.d_name);
            snprintf(g->title, sizeof(g->title), "%s", g->id);
            snprintf(g->icon, sizeof(g->icon), "%s/%s/sce_sys/icon0.png", roots[r], g->id);
            char path[160];
            snprintf(path, sizeof(path), "%s/%s/sce_sys/param.sfo", roots[r], g->id);
            read_title(path, g->title, sizeof(g->title));
        }
        sceIoDclose(dir);
    }
    /* Adrenaline can be installed under a homebrew ID but runs NPXS10028. */
    int psp = 0;
    for (int i = 0; i < count; i++) if (!strcmp(out[i].id, "NPXS10028")) psp = 1;
    if (!psp && count < capacity) {
        VjoInstalledGame *g = &out[count++];
        memset(g, 0, sizeof(*g));
        strcpy(g->id, "NPXS10028");
        strcpy(g->title, "Adrenaline / PSP games (shared setting)");
    }
    qsort(out, count, sizeof(*out), name_order);
    return count;
}
