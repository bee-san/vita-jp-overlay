/* OCR-only game process worker. No Paf/newlib initialization, game text reads,
 * import hooks, network or borrowed game allocations. Kernel-owned raw rows
 * are read only through the claimed capture transport. */
#include "main.h"
#include "../shell/meiki_bridge.h"
#include "../include/vjo_api.h"
#include "../include/vjo_import.h"
#include "../include/vjo_game_ocr_diag.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>

#define WORKER_LOG "ux0:data/VitaJPOverlay/ocr-worker.log"
#define CONTROL_FILE "app0:/sce_sys/param.sfo"
#define MODEL_READ_BYTES 8192u
#define GAME_RESERVE_BYTES (4u * 1024u * 1024u)
#define MODULE_ALLOWANCE_BYTES (2u * 1024u * 1024u)
#define USER_MAX_BYTES (512u * 1024u * 1024u)
#define WORKER_JOIN_US 2000000u

typedef struct {
    SceUID uid;
    void *base;
    uint32_t bytes;
    int free_accepted;
} OwnedBlock;
static OwnedBlock blocks[3] = {{.uid=-1}, {.uid=-1}, {.uid=-1}};
static VjoMeikiBridge bridge = {.module=-1};
static SceUID worker = -1, model_fd = -1, control_fd = -1, log_fd = -1;
static uint32_t quitting, job_busy, active_seq;
static int worker_started, initialized, blocked, claim_outstanding;
static VjoGameOcrResult pending_result;
static VjoGameOcrIoDiagnostic io_failure;
static char model_asset;

void vjo_log(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    int count = sceClibVsnprintf(line, sizeof(line)-1, format, args);
    va_end(args);
    if (count < 0) return;
    unsigned bytes = (unsigned)count < sizeof(line)-2 ? (unsigned)count : sizeof(line)-2;
    line[bytes++] = '\n';
    if (log_fd < 0) log_fd = sceIoOpen(WORKER_LOG, SCE_O_WRONLY|SCE_O_CREAT|SCE_O_APPEND, 0644);
    if (log_fd >= 0) {
        unsigned written = 0;
        while (written < bytes) {
            int rc = sceIoWrite(log_fd, line+written, bytes-written);
            if (rc <= 0) break;
            written += (unsigned)rc;
        }
    }
}

static int uid_invalid(int rc)
{
    return (uint32_t)rc == (uint32_t)SCE_KERNEL_ERROR_INVALID_UID ||
           (uint32_t)rc == (uint32_t)SCE_KERNEL_ERROR_UNKNOWN_UID;
}

static int memory_imports_ready(void)
{
    return VJO_IMPORT_READY(sceKernelAllocMemBlock) &&
           VJO_IMPORT_READY(sceKernelGetMemBlockBase) &&
           VJO_IMPORT_READY(sceKernelGetMemBlockInfoByRange) &&
           VJO_IMPORT_READY(sceKernelFreeMemBlock) &&
           VJO_IMPORT_READY(sceKernelGetFreeMemorySize);
}

static int release_owned(OwnedBlock *block)
{
    if (block->uid < 0) return VJO_OK;
    if (!memory_imports_ready()) return VJO_E_OCR_UNAVAILABLE;
    /* Once Free succeeded, never free that UID again: a still-valid lookup
     * could be a reused object. Only definitive invalidation clears ownership. */
    if (!block->free_accepted) {
        void *current = NULL;
        int rc = sceKernelGetMemBlockBase(block->uid, &current);
        if (uid_invalid(rc)) {
            block->uid = -1; block->base = NULL; block->bytes = 0; block->free_accepted = 0;
            return VJO_OK;
        }
        if (rc < 0 || (block->base && current != block->base)) {
            vjo_log("USER release identity failed uid=0x%08X rc=0x%08X", block->uid, rc);
            return VJO_E_OCR_UNAVAILABLE;
        }
        rc = sceKernelFreeMemBlock(block->uid);
        if (rc < 0) {
            vjo_log("USER free failed uid=0x%08X bytes=%u rc=0x%08X", block->uid, block->bytes, rc);
            return VJO_E_OCR_UNAVAILABLE;
        }
        block->free_accepted = 1;
    }
    void *current = NULL;
    int rc = sceKernelGetMemBlockBase(block->uid, &current);
    if (!uid_invalid(rc)) {
        vjo_log("USER free invalidation unproved uid=0x%08X rc=0x%08X", block->uid, rc);
        return VJO_E_OCR_UNAVAILABLE;
    }
    vjo_log("USER released uid=0x%08X bytes=%u lookup=0x%08X", block->uid, block->bytes, rc);
    block->uid = -1; block->base = NULL; block->bytes = 0; block->free_accepted = 0;
    return VJO_OK;
}

static void *owned_alloc(void *ud, size_t bytes)
{
    (void)ud;
    OwnedBlock *block = NULL;
    if (!bytes || bytes > USER_MAX_BYTES-4095u || !memory_imports_ready()) return NULL;
    for (unsigned i=0; i<3; i++) if (blocks[i].uid < 0) { block = &blocks[i]; break; }
    if (!block) return NULL;
    block->bytes = ((uint32_t)bytes+4095u)&~4095u;
    block->free_accepted = 0;
    block->uid = sceKernelAllocMemBlock("VjoGameOcrData", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                       block->bytes, NULL);
    vjo_log("USER alloc requested=%u rounded=%u uid=0x%08X", (unsigned)bytes, block->bytes, block->uid);
    if (block->uid < 0) { block->bytes = 0; return NULL; }
    int rc = sceKernelGetMemBlockBase(block->uid, &block->base);
    SceKernelMemBlockInfo info = {.size=sizeof(info)};
    if (rc >= 0 && block->base && !((uintptr_t)block->base&63u))
        rc = sceKernelGetMemBlockInfoByRange(block->base, block->bytes, &info);
    else rc = -1;
    if (rc < 0 || info.mappedBase != block->base || info.mappedSize < block->bytes ||
        info.memoryType != SCE_KERNEL_MEMORY_TYPE_NORMAL ||
        (info.access & (SCE_KERNEL_MEMORY_ACCESS_R|SCE_KERNEL_MEMORY_ACCESS_W)) !=
            (SCE_KERNEL_MEMORY_ACCESS_R|SCE_KERNEL_MEMORY_ACCESS_W) ||
        info.type != SCE_KERNEL_MEMBLOCK_TYPE_USER_RW) {
        vjo_log("USER mapping refused uid=0x%08X rc=0x%08X size=%u type=0x%08X",
                 block->uid, rc, info.mappedSize, info.type);
        (void)release_owned(block);
        return NULL;
    }
    return block->base;
}

static int owned_free(void *ud, void *pointer)
{
    (void)ud;
    if (!pointer) return VJO_OK;
    for (unsigned i=0; i<3; i++)
        if (blocks[i].uid >= 0 && blocks[i].base == pointer) return release_owned(&blocks[i]);
    vjo_log("USER free refused unknown pointer");
    return VJO_E_OCR_UNAVAILABLE;
}

static int has_owned_blocks(void)
{
    for (unsigned i=0; i<3; i++) if (blocks[i].uid >= 0) return 1;
    return 0;
}

static int finish_engine(void)
{
    int rc = vjo_meiki_bridge_finish(&bridge);
    /* A failed stop/unload may still execute against any caller buffer. */
    if (rc) return rc;
    for (unsigned i=0; i<3; i++) if (release_owned(&blocks[i])) rc = VJO_E_OCR_UNAVAILABLE;
    if (model_fd >= 0) {
        int close_rc = sceIoClose(model_fd);
        if (close_rc < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else model_fd = -1;
    }
    if (control_fd >= 0) {
        int close_rc = sceIoClose(control_fd);
        if (close_rc < 0) rc = VJO_E_OCR_UNAVAILABLE;
        else control_fd = -1;
    }
    return rc;
}

static int initialize(void)
{
    if (initialized) return VJO_OK;
    VjoMeikiAllocator allocator = {NULL, owned_alloc, owned_free, NULL};
    int rc = vjo_meiki_bridge_set_allocator(&bridge, &allocator);
    if (!rc) initialized = 1;
    return rc;
}

static int cancelled(void *ud)
{
    (void)ud;
    return __atomic_load_n(&quitting, __ATOMIC_ACQUIRE) ||
           !active_seq || vjoOcrCancelled(active_seq) != 0;
}

static int raw_rows(void *ud, unsigned row, unsigned count, unsigned char *rgba)
{
    const VjoGameOcrRequest *request = ud;
    if (!rgba || row >= request->height || !count || count > request->height-row || cancelled(NULL))
        return -1;
    int rc = vjoReadRaw(row, count, rgba);
    return cancelled(NULL) ? -1 : rc;
}

static void remember_io_failure(char stage, uint32_t code)
{
    if (!io_failure.stage) {
        io_failure.asset = model_asset;
        io_failure.stage = stage;
        io_failure.code = code;
    }
}

static int file_read(void *ud, uint64_t offset, void *out, size_t bytes)
{
    (void)ud;
    if (model_fd < 0 || cancelled(NULL) || !out || offset > INT64_MAX || bytes > MODEL_READ_BYTES)
        return -1;
    SceOff position = sceIoLseek(model_fd, (SceOff)offset, SCE_SEEK_SET);
    if (position != (SceOff)offset) {
        remember_io_failure('L', (uint32_t)position);
        return -1;
    }
    /* The bridge hashes chunks of at most 8192 bytes. Every successful short
     * read advances; cancellation is checked between calls, and EOF fails. */
    for (size_t used=0; used<bytes;) {
        if (cancelled(NULL)) return -1;
        int rc = sceIoRead(model_fd, (unsigned char *)out+used, (SceSize)(bytes-used));
        if (rc <= 0 || (size_t)rc > bytes-used) {
            remember_io_failure('R', (uint32_t)rc);
            return -1;
        }
        used += (size_t)rc;
    }
    return 0;
}

static void file_close(void *ud)
{
    (void)ud;
    if (model_fd >= 0 && sceIoClose(model_fd) >= 0) model_fd = -1;
}

static int file_open(void *ud, const char *path, VjoFile *out)
{
    (void)ud;
    if (model_fd >= 0 || !path || !out || cancelled(NULL)) return -1;
    const char *filename = path;
    for (const char *p=path; *p; p++) if (*p == '/' || *p == ':') filename = p+1;
    model_asset = !sceClibStrcmp(filename, VJO_MEIKI_DETECT_MODEL_FILENAME) ? 'D' : 'R';
    model_fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (model_fd < 0) {
        remember_io_failure('O', (uint32_t)model_fd);
        return -1;
    }
    SceOff size = sceIoLseek(model_fd, 0, SCE_SEEK_END);
    if (size <= 0) {
        if (size < 0) remember_io_failure('S', (uint32_t)size);
        /* Open succeeded, so finish_engine must still close this owned fd. */
        return -1;
    }
    *out = (VjoFile){NULL, (uint64_t)size, file_read, file_close};
    return 0;
}

static void probe_control_file(void)
{
    /* This same-PID, one-byte read tests the game's own ordinary namespace.
     * It never reads another game's files or changes filesystem permissions. */
    io_failure.control_stage = 'P';
    io_failure.control_code = 0;
    if (cancelled(NULL)) return;
    control_fd = sceIoOpen(CONTROL_FILE, SCE_O_RDONLY, 0);
    if (control_fd < 0) {
        io_failure.control_stage = 'O';
        io_failure.control_code = (uint32_t)control_fd;
        return;
    }
    if (!cancelled(NULL)) {
        unsigned char byte;
        int rc = sceIoRead(control_fd, &byte, 1);
        if (rc != 1) {
            io_failure.control_stage = 'R';
            io_failure.control_code = (uint32_t)rc;
        }
    }
    int rc = sceIoClose(control_fd);
    if (rc >= 0) control_fd = -1;
    else if (io_failure.control_stage == 'P') {
        io_failure.control_stage = 'C';
        io_failure.control_code = (uint32_t)rc;
    }
}

static int budget_allows(uint32_t layout)
{
    SceKernelFreeMemorySizeInfo info = {.size=sizeof(info)};
    if (!memory_imports_ready()) {
        vjo_log("game sysmem import unavailable; no OCR allocation");
        return 0;
    }
    int rc = sceKernelGetFreeMemorySize(&info);
    size_t input = layout == VJO_GAME_OCR_DIALOGUE_BOX ? MEIKI_DETECT_ELEMENTS : MEIKI_PREPROCESS_ELEMENTS;
    uint32_t preprocessing = ((uint32_t)(input*sizeof(float))+MEIKI_PREPROCESS_SCRATCH_BYTES+4095u)&~4095u;
    uint32_t owned = MEIKI_MODULE_METADATA_BYTES+MEIKI_WORKSPACE_BYTES+preprocessing;
    uint32_t required = owned+MODULE_ALLOWANCE_BYTES+GAME_RESERVE_BYTES;
    vjo_log("game budget rc=0x%08X USER=%d/0x%08X CDRAM=%d PHYCONT=%d",
             rc, info.size_user, (unsigned)info.size_user, info.size_cdram, info.size_phycont);
    vjo_log("game budget owned=%u module_allowance=%u reserve=%u required=%u",
             owned, MODULE_ALLOWANCE_BYTES, GAME_RESERVE_BYTES, required);
    return rc >= 0 && info.size_user > 0 && (uint32_t)info.size_user <= USER_MAX_BYTES &&
           (uint32_t)info.size_user >= required;
}

int vjo_game_ocr_run_request(const VjoGameOcrRequest *request, VjoGameOcrResult *result)
{
    if (!request || !result) return VJO_E_SOURCE;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&job_busy, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return VJO_E_OCR_UNAVAILABLE;
    sceClibMemset(result, 0, sizeof(*result));
    result->size = sizeof(*result); result->seq = request->seq;
    int rc = VJO_E_SOURCE, valid_request = 0, engine_attempted = 0;
    VjoMeikiStats stats = {0};
    active_seq = request->seq;
    sceClibMemset(&io_failure, 0, sizeof(io_failure));
    model_asset = 'R';
    if (blocked || bridge.module >= 0 || has_owned_blocks() || model_fd >= 0 || control_fd >= 0) {
        rc = VJO_E_OCR_UNAVAILABLE; goto done;
    }
    if (request->size != sizeof(*request) || !request->seq || !request->done_seq ||
        !request->width || request->width > VJO_OCR_MAX_WIDTH ||
        !request->height || request->height > VJO_OCR_MAX_HEIGHT ||
        request->stride != request->width*4u || request->layout > VJO_GAME_OCR_DIALOGUE_BOX ||
        !request->model_dir[0] || !sceClibMemchr(request->model_dir, 0, sizeof(request->model_dir))) goto done;
    valid_request = 1;
    if (cancelled(NULL)) { rc = VJO_E_CANCELLED; goto done; }
    rc = initialize();
    if (rc) goto done;
    if (!budget_allows(request->layout)) { rc = VJO_E_OOM; goto done; }
    VjoPlatform platform = {.file_open=file_open};
    sceClibMemset(&bridge.module_stats, 0, sizeof(bridge.module_stats));
    engine_attempted = 1;
    rc = vjo_meiki_bridge_start_mode(&bridge, &platform, request->model_dir,
                                     request->layout == VJO_GAME_OCR_DIALOGUE_BOX, cancelled, NULL);
    if (rc) goto done;
    VjoOcrImage image = {(void *)request, request->width, request->height, raw_rows, cancelled};
    VjoMeikiEngine engine = vjo_meiki_bridge_engine(&bridge);
    if (request->layout == VJO_GAME_OCR_DIALOGUE_BOX)
        rc = vjo_meiki_ocr_detected(request->model_dir, &image, &engine, bridge.input,
                                   bridge.input_elements, bridge.scratch, MEIKI_PREPROCESS_SCRATCH_BYTES,
                                   result->text, sizeof(result->text), &stats);
    else
        rc = vjo_meiki_ocr_single_line(request->model_dir, &image, &engine, bridge.input,
                                      bridge.input_elements, bridge.scratch, MEIKI_PREPROCESS_SCRATCH_BYTES,
                                      result->text, sizeof(result->text), &stats);
done:
    result->neural_peak = (uint32_t)stats.heap_peak;
    result->metadata_peak = engine_attempted ? bridge.module_stats.metadata_brk_peak : 0;
    if (rc == VJO_E_OCR_MODEL && io_failure.stage && !cancelled(NULL)) probe_control_file();
    result->cleanup_status = finish_engine();
    if (result->cleanup_status) { blocked = 1; rc = VJO_E_OCR_UNAVAILABLE; }
    else if (valid_request && cancelled(NULL)) rc = VJO_E_CANCELLED;
    if (rc) sceClibMemset(result->text, 0, sizeof(result->text));
    if (rc == VJO_E_OCR_MODEL && io_failure.stage) {
        rc = vjo_game_ocr_io_write(result->text, sizeof(result->text), &io_failure)
           ? VJO_E_OCR_UNAVAILABLE : VJO_E_OCR_MODEL_IO;
        if (rc != VJO_E_OCR_MODEL_IO) sceClibMemset(result->text, 0, sizeof(result->text));
    }
    result->rc = rc;
    vjo_log("job seq=%u dims=%ux%u layout=%u rc=%d neural=%u metadata=%u cleanup=%d",
             request->seq, request->width, request->height, request->layout, rc,
             result->neural_peak, result->metadata_peak, result->cleanup_status);
    active_seq = 0;
    __atomic_store_n(&job_busy, 0, __ATOMIC_RELEASE);
    return rc;
}

static int worker_main(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int last_registration = 0;
    while (!__atomic_load_n(&quitting, __ATOMIC_ACQUIRE) && !blocked) {
        /* A failed Complete may leave the kernel capture pinned even when
         * local engine cleanup succeeded. Retry its acknowledgement before
         * registering or claiming any other job. */
        if (claim_outstanding) {
            int complete_rc = vjoOcrComplete(&pending_result);
            vjo_log("OCR complete retry seq=%u rc=%d cleanup=%d", pending_result.seq,
                     complete_rc, pending_result.cleanup_status);
            if (!complete_rc && !pending_result.cleanup_status) claim_outstanding = 0;
            if (claim_outstanding) {
                sceKernelDelayThread(50000);
                continue;
            }
            if (__atomic_load_n(&quitting, __ATOMIC_ACQUIRE)) break;
        }
        int rc = vjoOcrRegister();
        if (rc != last_registration) { vjo_log("OCR registration rc=%d", rc); last_registration = rc; }
        if (!rc) {
            VjoGameOcrRequest request = {.size=sizeof(request)};
            rc = vjoOcrTake(&request);
            if (!rc) {
                claim_outstanding = 1;
                (void)vjo_game_ocr_run_request(&request, &pending_result);
                int complete_rc = vjoOcrComplete(&pending_result);
                vjo_log("OCR complete seq=%u rc=%d cleanup=%d", request.seq, complete_rc, pending_result.cleanup_status);
                if (!complete_rc && !pending_result.cleanup_status) claim_outstanding = 0;
            }
        }
        if (!blocked && !__atomic_load_n(&quitting, __ATOMIC_ACQUIRE)) sceKernelDelayThread(50000);
    }
    return 0;
}

int vjo_game_ocr_worker_start(void)
{
    if (worker >= 0 || blocked || initialize()) return VJO_E_OCR_UNAVAILABLE;
    if (!memory_imports_ready() ||
        !VJO_IMPORT_READY(vjoGetVersion) || !VJO_IMPORT_READY(vjoOcrRegister) ||
        !VJO_IMPORT_READY(vjoOcrTake) || !VJO_IMPORT_READY(vjoOcrCancelled) ||
        !VJO_IMPORT_READY(vjoOcrComplete) || !VJO_IMPORT_READY(vjoReadRaw) ||
        vjoGetVersion() != VJO_API_VERSION) return VJO_E_OCR_UNAVAILABLE;
    __atomic_store_n(&quitting, 0, __ATOMIC_RELEASE);
    worker = sceKernelCreateThread("VjoGameOcr", worker_main, 0x10000100, 64u*1024u, 0, 0, NULL);
    if (worker < 0) return VJO_E_OCR_UNAVAILABLE;
    int rc = sceKernelStartThread(worker, 0, NULL);
    if (rc < 0) {
        if (sceKernelDeleteThread(worker) >= 0) worker = -1;
        else blocked = 1;
        return VJO_E_OCR_UNAVAILABLE;
    }
    worker_started = 1;
    return VJO_OK;
}

int vjo_game_ocr_worker_stop(void)
{
    __atomic_store_n(&quitting, 1, __ATOMIC_RELEASE);
    if (worker >= 0) {
        SceUInt timeout = WORKER_JOIN_US;
        if (worker_started && sceKernelWaitThreadEnd(worker, NULL, &timeout) < 0)
            return VJO_E_OCR_UNAVAILABLE;
        worker_started = 0;
        if (sceKernelDeleteThread(worker) < 0) return VJO_E_OCR_UNAVAILABLE;
        worker = -1;
    }
    if (__atomic_load_n(&job_busy, __ATOMIC_ACQUIRE) || finish_engine()) return VJO_E_OCR_UNAVAILABLE;
    if (claim_outstanding) {
        pending_result.cleanup_status = 0;
        pending_result.rc = VJO_E_CANCELLED;
        sceClibMemset(pending_result.text, 0, sizeof(pending_result.text));
        if (vjoOcrComplete(&pending_result)) return VJO_E_OCR_UNAVAILABLE;
        claim_outstanding = 0;
    }
    if (log_fd >= 0) {
        if (sceIoClose(log_fd) < 0) return VJO_E_OCR_UNAVAILABLE;
        log_fd = -1;
    }
    blocked = 0;
    return VJO_OK;
}

#ifndef VJO_GAME_OCR_NO_MODULE_ENTRY
int module_start(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int rc = vjo_game_ocr_worker_start();
    /* Failed thread deletion retains its UID and the module containing its
     * entrypoint. module_stop may retry; automatic failed-start unloading
     * must not discard code still referenced by that thread object. */
    if (rc && worker >= 0) {
        blocked = 1;
        vjo_log("OCR worker start failed with retained thread; unload refused");
        return SCE_KERNEL_START_SUCCESS;
    }
    return rc ? SCE_KERNEL_START_FAILED : SCE_KERNEL_START_SUCCESS;
}
int module_stop(SceSize args, void *argp)
{
    (void)args; (void)argp;
    return vjo_game_ocr_worker_stop() ? SCE_KERNEL_STOP_FAIL : SCE_KERNEL_STOP_SUCCESS;
}
#endif
