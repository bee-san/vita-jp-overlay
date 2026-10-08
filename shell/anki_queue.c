/* Vita queue storage: one committed JSON record plus optional JPEG per card.
 * Temp files are ignored. Neither failed writes nor failed sends erase notes. */
#include <psp2/kernel/clib.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <string.h>
#include "anki_queue.h"
#include "shell.h"

#define QUEUE_DIR VJO_DATA_DIR "/anki_queue"
#define PATH_CAP 128

static void path_for(char path[PATH_CAP], const char *id, const char *ext)
{
    sceClibSnprintf(path, PATH_CAP, QUEUE_DIR "/%s.%s", id, ext);
}

static int prepare(void)
{
    SceIoStat st;
    sceIoMkdir(VJO_DATA_DIR, 0777);
    sceIoMkdir(QUEUE_DIR, 0777);
    return sceIoGetstat(QUEUE_DIR, &st) < 0 || !SCE_S_ISDIR(st.st_mode) ? -1 : 0;
}

static int scan_queue(char *first)
{
    SceUID dir;
    SceIoDirent ent;
    int n = 0, rc;
    if (prepare() < 0 || (dir = sceIoDopen(QUEUE_DIR)) < 0)
        return -1;
    if (first)
        first[0] = '\0';
    for (;;) {
        memset(&ent, 0, sizeof(ent));
        rc = sceIoDread(dir, &ent);
        if (rc <= 0)
            break;
        if (!SCE_S_ISREG(ent.d_stat.st_mode) || strlen(ent.d_name) != 37 || strcmp(ent.d_name + 32, ".json"))
            continue;
        ent.d_name[32] = '\0';
        if (!vjo_queue_valid_id(ent.d_name))
            continue;
        n++;
        if (first && (!first[0] || strcmp(ent.d_name, first) < 0))
            memcpy(first, ent.d_name, VJO_QUEUE_ID_SIZE);
    }
    if (sceIoDclose(dir) < 0)
        return -1;
    return rc < 0 ? -1 : n;
}

int vjo_queue_count(void) { return scan_queue(NULL); }
int vjo_queue_first(char id[VJO_QUEUE_ID_SIZE])
{
    int n = scan_queue(id);
    return n < 0 ? -1 : n > 0;
}

/* Never remove the destination to rename over it. A commit is a new filename. */
static int commit(const char *path, const void *data, size_t len)
{
    char tmp[PATH_CAP];
    SceUID fd;
    size_t off = 0;
    int ok = 1;
    sceClibSnprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return -1;
    while (off < len) {
        int n = sceIoWrite(fd, (const char *)data + off, len - off);
        if (n <= 0) { ok = 0; break; }
        off += (size_t)n;
    }
    if (sceIoSyncByFd(fd, 0) < 0)
        ok = 0;
    if (sceIoClose(fd) < 0)
        ok = 0;
    if (!ok || sceIoRename(tmp, path) < 0)
        return -1;
    return sceIoSync("ux0:", 0) < 0 ? -1 : 0;
}

int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note, const VjoAnkiMedia *media)
{
    char id[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    SceIoStat st;
    char *json = vjo_queue_encode(a, note, media->picture_len != 0);
    if (!json || media->picture_len > VJO_QUEUE_PICTURE_MAX || prepare() < 0)
        return -1;
    vjo_queue_id(json, strlen(json), id);
    path_for(path, id, "json");
    if (sceIoGetstat(path, &st) >= 0)
        /* A previous rename may be visible even though its final mount sync
         * failed. Never acknowledge a retry until the directory is durable. */
        return sceIoSync("ux0:", 0) < 0 ? -1 : 1;
    path_for(path, id, "duplicate.json");
    if (sceIoGetstat(path, &st) >= 0)
        return sceIoSync("ux0:", 0) < 0 ? -1 : 2;
    if (media->picture_len) {
        path_for(path, id, "jpg");
        /* An orphan from an interrupted save is safe to replace (no JSON yet). */
        sceIoRemove(path);
        if (commit(path, media->picture, media->picture_len) < 0)
            return -1;
    }
    path_for(path, id, "json");
    return commit(path, json, strlen(json));
}

static void *read_file(VjoArena *a, const char *path, size_t max, size_t *len)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    SceOff size;
    char *data = NULL;
    size_t off = 0;
    if (fd < 0)
        return NULL;
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size <= 0 || (uint64_t)size > max || sceIoLseek(fd, 0, SCE_SEEK_SET) < 0 ||
        !(data = vjo_arena_alloc(a, (size_t)size + 1))) {
        sceIoClose(fd);
        return NULL;
    }
    while (off < (size_t)size) {
        int n = sceIoRead(fd, data + off, (size_t)size - off);
        if (n <= 0) { sceIoClose(fd); return NULL; }
        off += (size_t)n;
    }
    if (sceIoClose(fd) < 0)
        return NULL;
    data[off] = '\0';
    *len = off;
    return data;
}

int vjo_queue_load(VjoArena *a, const char *id, VjoAnkiNote *note, VjoAnkiMedia *media)
{
    char path[PATH_CAP], actual[VJO_QUEUE_ID_SIZE];
    char *json, *name;
    size_t len;
    int picture;
    memset(media, 0, sizeof(*media));
    if (!vjo_queue_valid_id(id))
        return -1;
    path_for(path, id, "json");
    json = read_file(a, path, VJO_QUEUE_TEXT_MAX, &len);
    if (!json)
        return -1;
    vjo_queue_id(json, len, actual);
    if (strcmp(actual, id) || vjo_queue_decode(a, json, len, note, &picture) < 0)
        return -1;
    if (picture) {
        path_for(path, id, "jpg");
        media->picture = read_file(a, path, VJO_QUEUE_PICTURE_MAX, &media->picture_len);
        name = vjo_arena_alloc(a, 48);
        if (!media->picture || !name)
            return -1; /* never silently lose a saved screenshot */
        sceClibSnprintf(name, 48, "vitajp_%s.jpg", id);
        media->picture_name = name;
    }
    return 0;
}

int vjo_queue_remove(const char *id)
{
    char path[PATH_CAP];
    if (!vjo_queue_valid_id(id))
        return -1;
    path_for(path, id, "json");
    if (sceIoRemove(path) < 0 || sceIoSync("ux0:", 0) < 0)
        return -1;
    path_for(path, id, "jpg");
    sceIoRemove(path); /* orphan JPEGs are harmless if cleanup fails */
    return 0;
}

int vjo_queue_archive_duplicate(const char *id)
{
    char path[PATH_CAP], archive[PATH_CAP];
    SceIoStat st;
    if (!vjo_queue_valid_id(id))
        return -1;
    path_for(path, id, "json");
    path_for(archive, id, "duplicate.json");
    /* Repeated saves can encounter the same archived content-derived ID. */
    if (sceIoGetstat(archive, &st) >= 0)
        return -1; /* leave the pending copy rather than overwrite a backup */
    if (sceIoRename(path, archive) < 0)
        return -1;
    return sceIoSync("ux0:", 0) < 0 ? -1 : 0;
}
