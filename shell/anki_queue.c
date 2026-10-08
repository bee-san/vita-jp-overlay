/* Vita queue storage: one committed JSON record plus optional JPEG per card.
 * Temp files are ignored. Neither failed writes nor failed sends erase notes.
 * Every failed storage call is logged with its SCE error code (status.txt). */
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

static int io_fail(const char *call, const char *path, int rc)
{
    vjo_log("anki queue: %s %s failed 0x%08X", call, path, (unsigned)rc);
    return -1;
}

/* The mount's buffer cache, including renames and removals. */
static int mount_sync(void)
{
    int rc = sceIoSync("ux0:", 0);
    return rc < 0 ? io_fail("sync", "ux0:", rc) : 0;
}

static int prepare(void)
{
    SceIoStat st;
    int rc;
    sceIoMkdir(VJO_DATA_DIR, 0777); /* fails when it exists */
    sceIoMkdir(QUEUE_DIR, 0777);
    if ((rc = sceIoGetstat(QUEUE_DIR, &st)) < 0)
        return io_fail("getstat", QUEUE_DIR, rc);
    if (!SCE_S_ISDIR(st.st_mode)) {
        vjo_log("anki queue: %s is not a directory", QUEUE_DIR);
        return -1;
    }
    return 0;
}

static int scan_queue(char *first)
{
    SceUID dir;
    SceIoDirent ent;
    int n = 0, rc, closed;
    if (prepare() < 0)
        return -1;
    if ((dir = sceIoDopen(QUEUE_DIR)) < 0)
        return io_fail("dopen", QUEUE_DIR, dir);
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
    if (rc < 0)
        io_fail("dread", QUEUE_DIR, rc);
    if ((closed = sceIoDclose(dir)) < 0)
        return io_fail("dclose", QUEUE_DIR, closed);
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
    int rc = 0, n;
    sceClibSnprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0)
        return io_fail("open", tmp, fd);
    while (!rc && off < len) {
        n = sceIoWrite(fd, (const char *)data + off, len - off);
        if (n <= 0)
            rc = io_fail("write", tmp, n);
        else
            off += (size_t)n;
    }
    if (!rc && (n = sceIoSyncByFd(fd, 0)) < 0)
        rc = io_fail("flush", tmp, n);
    if ((n = sceIoClose(fd)) < 0 && !rc)
        rc = io_fail("close", tmp, n);
    if (!rc && (n = sceIoRename(tmp, path)) < 0)
        rc = io_fail("rename", tmp, n);
    if (rc) {
        sceIoRemove(tmp); /* never a pending card, but up to 192 KiB of space */
        return -1;
    }
    return mount_sync();
}

int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note, const VjoAnkiMedia *media)
{
    char id[VJO_QUEUE_ID_SIZE], path[PATH_CAP];
    SceIoStat st;
    char *json = vjo_queue_encode(a, note, media->picture_len != 0);
    if (!json || media->picture_len > VJO_QUEUE_PICTURE_MAX) {
        vjo_log("anki queue: card over %u KiB of text, out of memory, or JPEG over %u KiB",
                VJO_QUEUE_TEXT_MAX >> 10, VJO_QUEUE_PICTURE_MAX >> 10);
        return -1;
    }
    if (prepare() < 0)
        return -1;
    vjo_queue_id(json, strlen(json), id);
    path_for(path, id, "json");
    if (sceIoGetstat(path, &st) >= 0)
        /* A previous rename may be visible even though its final mount sync
         * failed. Never acknowledge a retry until the directory is durable. */
        return mount_sync() < 0 ? -1 : 1;
    path_for(path, id, "duplicate.json");
    if (sceIoGetstat(path, &st) >= 0)
        return mount_sync() < 0 ? -1 : 2;
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
    int n;
    if (fd < 0) {
        io_fail("open", path, fd);
        return NULL;
    }
    size = sceIoLseek(fd, 0, SCE_SEEK_END);
    if (size <= 0 || (uint64_t)size > max || sceIoLseek(fd, 0, SCE_SEEK_SET) < 0 ||
        !(data = vjo_arena_alloc(a, (size_t)size + 1))) {
        vjo_log("anki queue: %s: size %d (limit %u) or out of memory", path, (int)size, (unsigned)max);
        sceIoClose(fd);
        return NULL;
    }
    while (off < (size_t)size) {
        n = sceIoRead(fd, data + off, (size_t)size - off);
        if (n <= 0) {
            io_fail("read", path, n);
            sceIoClose(fd);
            return NULL;
        }
        off += (size_t)n;
    }
    if ((n = sceIoClose(fd)) < 0) {
        io_fail("close", path, n);
        return NULL;
    }
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
    if (strcmp(actual, id) || vjo_queue_decode(a, json, len, note, &picture) < 0) {
        vjo_log("anki queue: %s: checksum or format mismatch (edited or damaged)", path);
        return -1;
    }
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
    int rc;
    if (!vjo_queue_valid_id(id))
        return -1;
    path_for(path, id, "json");
    if ((rc = sceIoRemove(path)) < 0)
        return io_fail("remove", path, rc);
    if (mount_sync() < 0)
        return -1;
    path_for(path, id, "jpg");
    sceIoRemove(path); /* orphan JPEGs are harmless if cleanup fails */
    return 0;
}

int vjo_queue_archive_duplicate(const char *id)
{
    char path[PATH_CAP], archive[PATH_CAP];
    SceIoStat st;
    int rc;
    if (!vjo_queue_valid_id(id))
        return -1;
    path_for(path, id, "json");
    path_for(archive, id, "duplicate.json");
    /* Repeated saves can encounter the same archived content-derived ID. */
    if (sceIoGetstat(archive, &st) >= 0) {
        /* leave the pending copy rather than overwrite a backup */
        vjo_log("anki queue: %s exists; pending copy kept", archive);
        return -1;
    }
    if ((rc = sceIoRename(path, archive)) < 0)
        return io_fail("rename", path, rc);
    return mount_sync();
}
