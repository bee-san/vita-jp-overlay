#include "meiki_bridge.h"
#include <bearssl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>

extern void vjo_log(const char *fmt, ...);
#ifndef VJO_GAME_OCR_WORKER
extern void *vjo_paf_alloc(size_t bytes);
extern void vjo_paf_free(void *pointer);
extern void vjo_paf_memory_report(void);
#endif

int vjo_meiki_bridge_set_allocator(VjoMeikiBridge *b,
                                   const VjoMeikiAllocator *allocator)
{
    if (!b || b->module >= 0 || b->metadata || b->workspace || b->preprocessing ||
        (allocator && (!allocator->alloc || !allocator->free)))
        return VJO_E_OCR_UNAVAILABLE;
    if (allocator) b->allocator = *allocator;
    else sceClibMemset(&b->allocator, 0, sizeof(b->allocator));
    return VJO_OK;
}

static void *allocate_buffer(VjoMeikiBridge *b, size_t bytes)
{
    if (b->allocator.alloc) return b->allocator.alloc(b->allocator.ud, bytes);
#ifndef VJO_GAME_OCR_WORKER
    return vjo_paf_alloc(bytes);
#else
    (void)bytes;
    return NULL;
#endif
}

static int release_buffer(VjoMeikiBridge *b, void **pointer)
{
    if (!*pointer) return VJO_OK;
    if (b->allocator.free) {
        if (b->allocator.free(b->allocator.ud, *pointer))
            return VJO_E_OCR_UNAVAILABLE;
    } else {
#ifndef VJO_GAME_OCR_WORKER
        vjo_paf_free(*pointer);
#else
        return VJO_E_OCR_UNAVAILABLE;
#endif
    }
    *pointer = NULL;
    return VJO_OK;
}

/* This is the evaluated, pinned conversion. Validate before MNN parses it. */
#define MODEL_BYTES 5392856u
static const unsigned char model_sha256[32] = {
    0xb7,0xba,0x32,0x19,0xcd,0x1b,0xfe,0xef,0x5c,0xb6,0xb4,0x2e,0xf6,0x68,0x66,0x83,
    0xb9,0x91,0xc6,0x56,0x1b,0x7a,0xd3,0xb1,0xc4,0x1d,0x32,0x81,0x7d,0xeb,0xa2,0x1e
};
#define DETECT_MODEL_BYTES 4119220u
static const unsigned char detect_model_sha256[32] = {
    0xf0,0x86,0x5e,0xd2,0x91,0x84,0x6a,0x13,0x09,0x05,0x52,0x7f,0x67,0xc3,0x3f,0x17,
    0xe6,0x4d,0x0f,0x2c,0x5e,0xb3,0x83,0x78,0xed,0x1f,0xae,0xa5,0x7f,0x74,0xb1,0xba
};

static int validate_model(const VjoPlatform *p, const char *path,
                           uint64_t expected_bytes, const unsigned char *expected_sha256,
                           int (*cancelled)(void *), void *ud)
{
    VjoFile file = {0};
    br_sha256_context hash;
    unsigned char buffer[8192], digest[32];
    int rc = VJO_E_OCR_MODEL;
    if (!p || !p->file_open || p->file_open(p->ud, path, &file)) return rc;
    if (file.size != expected_bytes || !file.read || !file.close) goto done;
    br_sha256_init(&hash);
    for (uint64_t offset = 0; offset < file.size;) {
        size_t n = file.size - offset < sizeof(buffer) ? (size_t)(file.size - offset) : sizeof(buffer);
        if (cancelled && cancelled(ud)) { rc = VJO_E_CANCELLED; goto done; }
        if (file.read(file.ctx, offset, buffer, n)) goto done;
        br_sha256_update(&hash, buffer, n);
        offset += n;
    }
    br_sha256_out(&hash, digest);
    if (!sceClibMemcmp(digest, expected_sha256, sizeof(digest))) rc = VJO_OK;
done:
    if (file.close) file.close(file.ctx);
    return rc;
}

static int free_buffers(VjoMeikiBridge *b)
{
    int rc = release_buffer(b, &b->preprocessing);
    if (!b->preprocessing) {
        b->input = NULL;
        b->scratch = NULL;
        b->input_elements = 0;
    }
    if (release_buffer(b, &b->workspace)) rc = VJO_E_OCR_UNAVAILABLE;
    if (release_buffer(b, &b->metadata)) rc = VJO_E_OCR_UNAVAILABLE;
    if (rc) vjo_log("Meiki buffer release failed; retaining allocation provenance");
    return rc;
}

int vjo_meiki_bridge_finish(VjoMeikiBridge *b)
{
    if (!b) return VJO_E_OCR_UNAVAILABLE;
    if (b->module >= 0) {
        if (!b->stopped) {
            if (b->api.size != sizeof(b->api) || b->api.abi != MEIKI_MODULE_ABI ||
                !b->api.stop || b->api.stop(&b->module_stats)) {
                vjo_log("Meiki stop failed; keeping buffers until the module stops");
                return VJO_E_OCR_UNAVAILABLE;
            }
            b->stopped = 1;
        }
        int status = 0;
        int rc = sceKernelStopUnloadModule(b->module, 0, NULL, 0, NULL, &status);
        if (rc < 0 || status != SCE_KERNEL_STOP_SUCCESS) {
            vjo_log("Meiki unload failed rc=0x%08X status=%d; keeping buffers", rc, status);
            return VJO_E_OCR_UNAVAILABLE;
        }
        b->module = -1;
        sceClibMemset(&b->api, 0, sizeof(b->api));
    }
    return free_buffers(b);
}

int vjo_meiki_bridge_start(VjoMeikiBridge *b, const VjoPlatform *p,
                           const char *model_dir, int (*cancelled)(void *), void *ud)
{
    return vjo_meiki_bridge_start_mode(b, p, model_dir, 0, cancelled, ud);
}

int vjo_meiki_bridge_start_mode(VjoMeikiBridge *b, const VjoPlatform *p,
                                const char *model_dir, int dialogue_box,
                                int (*cancelled)(void *), void *ud)
{
    if (!b || b->metadata || b->workspace || b->preprocessing || b->module >= 0)
        return VJO_E_OCR_UNAVAILABLE;
#ifdef VJO_GAME_OCR_WORKER
    if (!b->allocator.alloc || !b->allocator.free) return VJO_E_OCR_UNAVAILABLE;
#endif
    int n = sceClibSnprintf(b->model_path, sizeof(b->model_path), "%s/%s",
                           model_dir ? model_dir : "", VJO_MEIKI_MODEL_FILENAME);
    if (!model_dir || !model_dir[0] || n < 0 || (size_t)n >= sizeof(b->model_path))
        return VJO_E_OCR_MODEL;
    int rc = validate_model(p, b->model_path, MODEL_BYTES, model_sha256, cancelled, ud);
    if (rc) return rc;
    b->detect_model_path[0] = 0;
    if (dialogue_box) {
        n = sceClibSnprintf(b->detect_model_path, sizeof(b->detect_model_path), "%s/%s",
                            model_dir, VJO_MEIKI_DETECT_MODEL_FILENAME);
        if (n < 0 || (size_t)n >= sizeof(b->detect_model_path)) return VJO_E_OCR_MODEL;
        rc = validate_model(p, b->detect_model_path, DETECT_MODEL_BYTES,
                             detect_model_sha256, cancelled, ud);
        if (rc) return rc;
    }
    if (cancelled && cancelled(ud)) return VJO_E_CANCELLED;
    if (b->allocator.report) b->allocator.report(b->allocator.ud);
#ifndef VJO_GAME_OCR_WORKER
    else if (!b->allocator.alloc) vjo_paf_memory_report();
#endif
    b->metadata = allocate_buffer(b, MEIKI_MODULE_METADATA_BYTES);
    if (b->metadata) b->workspace = allocate_buffer(b, MEIKI_WORKSPACE_BYTES);
    b->input_elements = dialogue_box ? MEIKI_DETECT_ELEMENTS : MEIKI_PREPROCESS_ELEMENTS;
    if (b->workspace) b->preprocessing = allocate_buffer(b, b->input_elements * sizeof(float) + MEIKI_PREPROCESS_SCRATCH_BYTES);
    if (!b->metadata || !b->workspace || !b->preprocessing) {
        return free_buffers(b) ? VJO_E_OCR_UNAVAILABLE : VJO_E_OOM;
    }
    b->input = b->preprocessing;
    b->scratch = (unsigned char *)b->preprocessing + b->input_elements * sizeof(float);
    b->stopped = 0;
    sceClibMemset(&b->api, 0, sizeof(b->api));
    sceClibMemset(&b->module_stats, 0, sizeof(b->module_stats));
    b->module_stats.size = sizeof(b->module_stats);
    MeikiModuleStart start = {sizeof(start), MEIKI_MODULE_MAGIC, MEIKI_MODULE_ABI,
                              b->metadata, MEIKI_MODULE_METADATA_BYTES,
                              &b->api, &b->module_stats};
    int status = 0;
    b->module = sceKernelLoadStartModule(VJO_MEIKI_MODULE_PATH, sizeof(start), &start, 0, NULL, &status);
    if (b->module < 0) {
        vjo_log("Meiki module load failed 0x%08X status=%d", b->module, status);
        free_buffers(b);
        return VJO_E_OCR_UNAVAILABLE;
    }
    if (status != SCE_KERNEL_START_SUCCESS || b->api.size != sizeof(b->api) || b->api.abi != MEIKI_MODULE_ABI ||
        !b->api.run || !b->api.stop || (dialogue_box && !b->api.detect)) {
        vjo_log("Meiki module start/ABI failed status=%d abi=%u", status, b->api.abi);
        /* The pinned module joins its worker synchronously on start failure. */
        if (status != SCE_KERNEL_START_SUCCESS) b->stopped = 1;
        if (b->stopped || (b->api.abi == MEIKI_MODULE_ABI && b->api.stop))
            vjo_meiki_bridge_finish(b);
        return VJO_E_OCR_UNAVAILABLE;
    }
    return VJO_OK;
}

static int engine_result(VjoMeikiBridge *b, int rc, MeikiStats *stats)
{
    *stats = b->module_stats.runtime;
    if (!rc) return VJO_OK;
    if (rc == -10 || rc == -12 || rc == MEIKI_MODULE_TAINTED || stats->allocation_failed)
        return VJO_E_OOM;
    if (rc == -2 || rc == -4) return VJO_E_OCR_MODEL;
    if (rc == -13 || rc <= MEIKI_MODULE_INVALID) return VJO_E_OCR_UNAVAILABLE;
    return VJO_E_OCR_INFERENCE;
}

static int engine_run(void *ud, const char *path, const float *input,
                       MeikiOutput *output, MeikiStats *stats)
{
    VjoMeikiBridge *b = ud;
    if (!b || b->module < 0 || b->stopped || !b->api.run ||
        sceClibStrcmp(path, b->model_path)) return VJO_E_OCR_UNAVAILABLE;
    MeikiModuleRequest request = {sizeof(request), path, input, b->workspace,
                                  MEIKI_WORKSPACE_BYTES, output};
    int rc = b->api.run(&request, &b->module_stats);
    return engine_result(b, rc, stats);
}

static int engine_detect(void *ud, const char *path, const float *input,
                          int32_t target_width, int32_t target_height,
                          MeikiDetectOutput *output, MeikiStats *stats)
{
    VjoMeikiBridge *b = ud;
    if (!b || b->module < 0 || b->stopped || !b->api.detect ||
        !b->detect_model_path[0] || sceClibStrcmp(path, b->detect_model_path))
        return VJO_E_OCR_UNAVAILABLE;
    MeikiModuleDetectRequest request = {sizeof(request), path, input, target_width,
                                        target_height, b->workspace, MEIKI_WORKSPACE_BYTES, output};
    return engine_result(b, b->api.detect(&request, &b->module_stats), stats);
}

VjoMeikiEngine vjo_meiki_bridge_engine(VjoMeikiBridge *b)
{
    VjoMeikiEngine engine = {b, engine_run, engine_detect};
    return engine;
}
