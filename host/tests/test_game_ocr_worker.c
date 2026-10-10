/* Actual game worker, bridge, model SHA checks and C RGBA preprocessor. Only
 * SDK/transport/module boundaries are fake. Private pinned weights are read
 * via VJO_MEIKI_TEST_MODEL / VJO_MEIKI_TEST_DETECT_MODEL, never committed. */
#include "acutest.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#define VJO_GAME_OCR_WORKER 1
/* ARM import words are tested separately; this host test controls availability. */
#define VJO_IMPORT_H
static int test_import_ready(uintptr_t function);
#define VJO_IMPORT_READY(fn) test_import_ready((uintptr_t)(fn))
int vjoGetVersion(void);
int vjoReadRaw(uint32_t row, uint32_t rows, void *out);
#include "../../game_ocr/main.c"
#include "../../shell/meiki_bridge.c"
#define floorf vjo_game_test_floorf
#include "../../game_ocr/libc_math.c"
#undef floorf

typedef struct { SceUID uid; void *base; unsigned bytes; int live, lookup_valid; } TestBlock;
static TestBlock owned[3];
static uintptr_t unavailable_import;
static int budget_user, budget_rc, alloc_fail_at, map_failure, free_fail_slot;
static int base_fail_slot, identity_fail_slot, accepted_free_lookup_valid;
static int thread_start_rc, thread_delete_rc, thread_join_rc, module_stop_rc;
static int model_close_rc, complete_failures, cancel_on_raw, cancellation;
static int model_open_rc, model_size_set, model_read_result_set, model_read_result;
static int model_offset_seek_rc, model_short_read_bytes, corrupt_model_bytes;
static int cancel_after_model_read, cancel_on_model_failure;
static int cancel_on_control_read;
static SceOff model_size_result;
static char fault_asset, opened_asset;
static int control_open_rc, control_read_rc, control_close_failures, control_live;
static unsigned alloc_calls, free_calls, loads, unloads, engine_runs, engine_stops;
static unsigned free_calls_at_load, model_opens, model_reads, stat_calls;
static unsigned control_opens, control_reads, control_closes;
static unsigned takes, completes, delays, foreign_frees, closes, writes;
static unsigned stop_loop_after_delays, stop_loop_on_complete;
static int module_live;
static SceUID next_uid;
static FILE *model_file;
static VjoGameOcrRequest queued_request;
static int queued;

static int test_import_ready(uintptr_t function) { return function != unavailable_import; }
static TestBlock *by_uid(SceUID uid)
{
    for (unsigned i=0; i<3; i++) if (owned[i].uid == uid) return &owned[i];
    return NULL;
}
static unsigned slot_of(TestBlock *block) { return (unsigned)(block-owned); }
SceUID sceKernelAllocMemBlock(const char *name, int type, SceSize bytes, void *opt)
{
    (void)name; TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_USER_RW && !opt);
    TEST_CHECK(bytes && !(bytes&4095u));
    alloc_calls++;
    if ((int)alloc_calls == alloc_fail_at) return -123;
    for (unsigned i=0; i<3; i++) if (!owned[i].live && !owned[i].lookup_valid) {
        TEST_ASSERT(posix_memalign(&owned[i].base, 64, bytes) == 0);
        owned[i].uid = ++next_uid; owned[i].bytes = bytes;
        owned[i].live = owned[i].lookup_valid = 1;
        return owned[i].uid;
    }
    TEST_CHECK(0); return -1;
}
int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TestBlock *block = by_uid(uid);
    if (!block || !block->lookup_valid) return SCE_KERNEL_ERROR_INVALID_UID;
    if ((int)slot_of(block) == base_fail_slot) return -456;
    *base = block->base;
    if ((int)slot_of(block) == identity_fail_slot) *base = (char *)*base+64;
    return 0;
}
int sceKernelGetMemBlockInfoByRange(void *base, SceSize bytes, SceKernelMemBlockInfo *info)
{
    for (unsigned i=0; i<3; i++) if (owned[i].live && owned[i].base == base) {
        TEST_CHECK(bytes == owned[i].bytes && info->size == sizeof(*info));
        *info = (SceKernelMemBlockInfo){sizeof(*info),base,bytes,SCE_KERNEL_MEMORY_TYPE_NORMAL,
                                      SCE_KERNEL_MEMORY_ACCESS_R|SCE_KERNEL_MEMORY_ACCESS_W,
                                      SCE_KERNEL_MEMBLOCK_TYPE_USER_RW};
        if (map_failure == 1) return -1;
        if (map_failure == 2) info->mappedSize -= 4096;
        if (map_failure == 3) info->memoryType = 0x80;
        if (map_failure == 4) info->access = SCE_KERNEL_MEMORY_ACCESS_R;
        if (map_failure == 5) info->type++;
        return 0;
    }
    TEST_CHECK(0); return -1;
}
int sceKernelFreeMemBlock(SceUID uid)
{
    TestBlock *block = by_uid(uid);
    if (!block || !block->live || !block->lookup_valid) { foreign_frees++; return -1; }
    TEST_CHECK(!module_live); /* no engine may still access its caller buffers */
    if ((int)slot_of(block) == free_fail_slot) return -789;
    free_calls++; free(block->base); block->live = 0;
    block->lookup_valid = accepted_free_lookup_valid;
    return 0;
}
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info)
{
    TEST_CHECK(info->size == sizeof(*info));
    info->size_user = budget_user; info->size_cdram = info->size_phycont = 0;
    return budget_rc;
}
SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize,void *), int priority,
                            SceSize stack, unsigned attr, int affinity, void *opt)
{
    (void)name; (void)priority; (void)attr; (void)affinity; (void)opt;
    TEST_CHECK(entry == worker_main && stack == 65536); return 77;
}
int sceKernelStartThread(SceUID uid, SceSize bytes, void *args)
{ TEST_CHECK(uid == 77 && !bytes && !args); return thread_start_rc; }
int sceKernelWaitThreadEnd(SceUID uid, int *status, SceUInt *timeout)
{ TEST_CHECK(uid == 77 && !status && *timeout == WORKER_JOIN_US); return thread_join_rc; }
int sceKernelDeleteThread(SceUID uid) { TEST_CHECK(uid == 77); return thread_delete_rc; }
int sceKernelDelayThread(SceUInt us)
{
    TEST_CHECK(us == 50000); delays++;
    if (stop_loop_after_delays && delays >= stop_loop_after_delays) quitting = 1;
    TEST_ASSERT(delays < 20); return 0;
}
static int mock_module_run(const MeikiModuleRequest *request, MeikiModuleStats *stats)
{
    TEST_CHECK(module_live && request->workspace == bridge.workspace && request->input == bridge.input);
    engine_runs++; memset(request->output, 0, sizeof(*request->output));
    request->output->codes[0] = 0x4e00;
    request->output->boxes[2] = 10; request->output->scores[0] = .9f;
    stats->metadata_brk_peak = 2000000; stats->runtime.heap_peak = 123456;
    return 0;
}
static int mock_module_stop(MeikiModuleStats *stats)
{ (void)stats; engine_stops++; return module_stop_rc; }
static int mock_module_detect(const MeikiModuleDetectRequest *request, MeikiModuleStats *stats)
{
    (void)stats; TEST_CHECK(module_live); memset(request->output,0,sizeof(*request->output));
    return 0;
}
SceUID sceKernelLoadStartModule(const char *path, SceSize bytes, void *arg, int flags,
                               SceKernelLMOption *opt, int *status)
{
    (void)flags; (void)opt; TEST_CHECK(!strcmp(path,VJO_MEIKI_MODULE_PATH));
    TEST_CHECK(bytes == sizeof(MeikiModuleStart)); MeikiModuleStart *start = arg;
    TEST_CHECK(start->metadata_heap == bridge.metadata);
    TEST_CHECK(!module_live && bridge.metadata && bridge.workspace && bridge.preprocessing);
    *start->api_out = (MeikiModuleApi){sizeof(MeikiModuleApi),MEIKI_MODULE_ABI,
                                     mock_module_run,mock_module_stop,mock_module_detect};
    *status = SCE_KERNEL_START_SUCCESS; loads++; module_live = 1; free_calls_at_load = free_calls; return 99;
}
int sceKernelStopUnloadModule(SceUID uid, SceSize bytes, void *arg, int flags,
                              SceKernelULMOption *opt, int *status)
{
    (void)bytes; (void)arg; (void)flags; (void)opt;
    TEST_CHECK(uid == 99 && module_live && engine_stops && bridge.stopped);
    TEST_CHECK(free_calls == free_calls_at_load); unloads++; module_live = 0; *status = SCE_KERNEL_STOP_SUCCESS; return 0;
}
static const char *model_on_host(const char *path)
{
    return getenv(strstr(path,VJO_MEIKI_DETECT_MODEL_FILENAME) ?
                  "VJO_MEIKI_TEST_DETECT_MODEL" : "VJO_MEIKI_TEST_MODEL");
}
SceUID sceIoOpen(const char *path, int flags, unsigned mode)
{
    (void)mode;
    if (flags != SCE_O_RDONLY) { TEST_CHECK(!strcmp(path,WORKER_LOG)); return 1; }
    if (!strcmp(path,CONTROL_FILE)) {
        control_opens++; TEST_CHECK(!control_live);
        if (control_open_rc < 0) return control_open_rc;
        control_live = 1; return 3;
    }
    model_opens++;
    opened_asset = strstr(path,VJO_MEIKI_DETECT_MODEL_FILENAME) ? 'D' : 'R';
    if (model_open_rc < 0 && opened_asset == fault_asset) {
        if (cancel_on_model_failure) cancellation = 1;
        return model_open_rc;
    }
    TEST_CHECK(!model_file); const char *host = model_on_host(path);
    model_file = host ? fopen(host,"rb") : NULL;
    return model_file ? 2 : -1;
}
int sceIoGetstat(const char *path, SceIoStat *info)
{
    stat_calls++;
    const char *host = model_on_host(path); struct stat st;
    if (!host || stat(host,&st)) return -1;
    info->st_size = st.st_size; return 0;
}
int sceIoClose(SceUID fd)
{
    if (fd == 1) return 0;
    if (fd == 3) {
        TEST_CHECK(control_live); control_closes++;
        if (control_close_failures) {
            if (control_close_failures > 0) control_close_failures--;
            return (int32_t)0x80010005;
        }
        control_live = 0; return 0;
    }
    TEST_CHECK(fd == 2 && model_file); closes++;
    if (model_close_rc) return model_close_rc;
    fclose(model_file); model_file = NULL; return 0;
}
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence)
{
    TEST_CHECK(fd == 2 && model_file);
    if (whence == SCE_SEEK_END) {
        TEST_CHECK(!offset);
        if (model_size_set && opened_asset == fault_asset) return model_size_result;
        return fseek(model_file,0,SEEK_END) ? -1 : (SceOff)ftell(model_file);
    }
    TEST_CHECK(whence == SCE_SEEK_SET);
    if (model_offset_seek_rc && opened_asset == fault_asset) return model_offset_seek_rc;
    return fseek(model_file,(long)offset,SEEK_SET) ? -1 : offset;
}
int sceIoRead(SceUID fd, void *out, SceSize bytes)
{
    if (fd == 3) {
        TEST_CHECK(control_live && bytes == 1); control_reads++;
        if (control_read_rc == 1) *(unsigned char *)out = 0;
        if (cancel_on_control_read) cancellation = 1;
        return control_read_rc;
    }
    TEST_CHECK(fd == 2 && model_file); model_reads++;
    if (model_read_result_set && opened_asset == fault_asset) return model_read_result;
    if (model_short_read_bytes && bytes > (unsigned)model_short_read_bytes) bytes = model_short_read_bytes;
    int rc = (int)fread(out,1,bytes,model_file);
    if (rc > 0 && corrupt_model_bytes && opened_asset == fault_asset) ((unsigned char *)out)[0] ^= 1;
    if (cancel_after_model_read) cancellation = 1;
    return rc;
}
int sceIoWrite(SceUID fd, const void *data, SceSize bytes)
{
    TEST_CHECK(fd == 1 && bytes <= 255);
    for (unsigned i=0;i+3<=bytes;i++)
        TEST_CHECK(memcmp((const char *)data+i,"\xe4\xb8\x80",3)); /* no decoded text in logs */
    writes++; return (int)bytes;
}
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoOcrRegister(void) { return 0; }
int vjoOcrTake(VjoGameOcrRequest *request)
{
    takes++; TEST_CHECK(!claim_outstanding);
    if (!queued) return 1;
    *request = queued_request; queued = 0; return 0;
}
int vjoOcrCancelled(uint32_t seq) { TEST_CHECK(seq == queued_request.seq); return cancellation; }
int vjoReadRaw(uint32_t row, uint32_t rows, void *out)
{
    TEST_CHECK(row+rows <= queued_request.height);
    memset(out,255,rows*queued_request.width*4u);
    if (cancel_on_raw) cancellation = 1;
    return (int)rows;
}
int vjoOcrComplete(const VjoGameOcrResult *result)
{
    completes++; TEST_CHECK(claim_outstanding && result->seq == queued_request.seq);
    if (!result->cleanup_status) TEST_CHECK(!module_live && !has_owned_blocks() && model_fd < 0 && control_fd < 0);
    if (complete_failures) { if (complete_failures > 0) complete_failures--; return -7; }
    if (stop_loop_on_complete && !result->cleanup_status) quitting = 1;
    return 0;
}

static int have_model(void)
{
    if (getenv("VJO_MEIKI_TEST_MODEL")) return 1;
    TEST_SKIP("Set VJO_MEIKI_TEST_MODEL to exercise pinned hash and job lifecycle"); return 0;
}
static void setup(void)
{
    TEST_ASSERT(!module_live && !has_owned_blocks() && !model_file && !control_live);
    memset(owned,0,sizeof(owned)); memset(blocks,0,sizeof(blocks));
    for (unsigned i=0;i<3;i++) blocks[i].uid = -1;
    memset(&bridge,0,sizeof(bridge)); bridge.module = -1;
    worker = model_fd = control_fd = log_fd = -1;
    quitting = job_busy = active_seq = 0;
    worker_started = initialized = blocked = claim_outstanding = 0;
    unavailable_import = 0; budget_user = 64*1024*1024; budget_rc = alloc_fail_at = map_failure = 0;
    free_fail_slot = base_fail_slot = identity_fail_slot = -1; accepted_free_lookup_valid = 0;
    thread_start_rc = thread_delete_rc = thread_join_rc = module_stop_rc = model_close_rc = 0;
    complete_failures = cancel_on_raw = cancellation = 0;
    model_open_rc = model_size_set = model_read_result_set = model_read_result = 0;
    model_offset_seek_rc = model_short_read_bytes = corrupt_model_bytes = 0;
    cancel_after_model_read = cancel_on_model_failure = cancel_on_control_read = 0; model_size_result = 0;
    fault_asset = opened_asset = 'R';
    control_open_rc = control_close_failures = control_live = 0; control_read_rc = 1;
    free_calls_at_load = model_opens = model_reads = stat_calls = 0;
    control_opens = control_reads = control_closes = 0;
    alloc_calls = free_calls = loads = unloads = engine_runs = engine_stops = 0;
    takes = completes = delays = foreign_frees = closes = writes = 0;
    stop_loop_after_delays = stop_loop_on_complete = 0; next_uid = 100;
    queued_request = (VjoGameOcrRequest){sizeof(VjoGameOcrRequest),7,5,24,8,96,0,"ocr"}; queued = 0;
}
static int run(VjoGameOcrResult *result) { return vjo_game_ocr_run_request(&queued_request,result); }
static void drained(void)
{
    TEST_CHECK(!module_live && !has_owned_blocks() && !bridge.metadata && !bridge.workspace && !bridge.preprocessing);
    TEST_CHECK(!model_file && !foreign_frees && !control_live && control_fd < 0);
    for (unsigned i=0;i<3;i++) TEST_CHECK(!owned[i].live);
}
static void test_budget_and_import_guards_allocate_nothing(void)
{
    int budgets[] = {-1048576,0,16*1024*1024,USER_MAX_BYTES+4096};
    for (unsigned i=0;i<sizeof(budgets)/sizeof(budgets[0]);i++) {
        setup(); budget_user = budgets[i]; VjoGameOcrResult result;
        TEST_CHECK(run(&result) == VJO_E_OOM && !result.cleanup_status);
        TEST_CHECK(!alloc_calls && !loads && !result.metadata_peak && !result.neural_peak); drained();
    }
    setup(); budget_rc = -1; VjoGameOcrResult result;
    TEST_CHECK(run(&result) == VJO_E_OOM && !alloc_calls); drained();
    uintptr_t imports[] = {(uintptr_t)sceKernelAllocMemBlock,(uintptr_t)sceKernelGetMemBlockBase,
                           (uintptr_t)sceKernelGetMemBlockInfoByRange,(uintptr_t)sceKernelFreeMemBlock,
                           (uintptr_t)sceKernelGetFreeMemorySize};
    for (unsigned i=0;i<sizeof(imports)/sizeof(imports[0]);i++) {
        setup(); unavailable_import = imports[i];
        TEST_CHECK(vjo_game_ocr_worker_start() == VJO_E_OCR_UNAVAILABLE && worker < 0);
        TEST_CHECK(run(&result) == VJO_E_OOM && !alloc_calls); drained();
    }
}
static void test_job_and_budget_threshold(void)
{
    if (!have_model()) return;
    for (unsigned layout=0;layout<2;layout++) {
        if (layout && !getenv("VJO_MEIKI_TEST_DETECT_MODEL")) continue;
        setup(); queued_request.layout = layout;
        size_t elements = layout ? MEIKI_DETECT_ELEMENTS : MEIKI_PREPROCESS_ELEMENTS;
        unsigned pre = (elements*sizeof(float)+MEIKI_PREPROCESS_SCRATCH_BYTES+4095u)&~4095u;
        budget_user = MEIKI_MODULE_METADATA_BYTES+MEIKI_WORKSPACE_BYTES+pre+MODULE_ALLOWANCE_BYTES+GAME_RESERVE_BYTES;
        VjoGameOcrResult result; budget_user--;
        TEST_CHECK(run(&result) == VJO_E_OOM && !alloc_calls);
        budget_user++; TEST_CHECK(run(&result) == VJO_OK);
        TEST_CHECK(alloc_calls == 3 && free_calls == 3 && loads == 1 && unloads == 1 && !result.cleanup_status);
        TEST_CHECK(owned[2].bytes == pre); drained();
    }
}
static void test_partial_allocations_and_invalid_mapping(void)
{
    if (!have_model()) return;
    VjoGameOcrResult result;
    for (int ordinal=1;ordinal<=3;ordinal++) {
        setup(); alloc_fail_at = ordinal;
        TEST_CHECK(run(&result) == VJO_E_OOM && !result.cleanup_status && !loads);
        TEST_CHECK(free_calls == (unsigned)(ordinal-1)); drained();
    }
    for (int invalid=1;invalid<=5;invalid++) {
        setup(); map_failure = invalid;
        TEST_CHECK(run(&result) == VJO_E_OOM && !result.cleanup_status && !loads);
        TEST_CHECK(alloc_calls == 1 && free_calls == 1); drained();
    }
}
static void test_orphan_mapping_cleanup_failure_is_retained(void)
{
    if (!have_model()) return;
    setup(); map_failure = 1; free_fail_slot = 0; VjoGameOcrResult result;
    TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && result.cleanup_status);
    TEST_CHECK(blocked && blocks[0].uid >= 0 && !bridge.metadata && !free_calls && !loads);
    TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && alloc_calls == 1);
    free_fail_slot = -1; TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK && free_calls == 1); drained();
}
static void test_base_failure_retains_uid_and_foreign_free_is_refused(void)
{
    setup(); base_fail_slot = 0;
    TEST_CHECK(!owned_alloc(NULL,4096) && blocks[0].uid >= 0 && !blocks[0].base);
    TEST_CHECK(!free_calls && !foreign_frees);
    unsigned char foreign[64];
    TEST_CHECK(owned_free(NULL,foreign) == VJO_E_OCR_UNAVAILABLE && !free_calls && !foreign_frees);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && blocks[0].uid >= 0);
    base_fail_slot = -1;
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK && free_calls == 1); drained();
}
static void test_free_failure_and_identity_failure_preserve_owner(void)
{
    if (!have_model()) return;
    for (int slot=0;slot<3;slot++) {
        setup(); free_fail_slot = slot; VjoGameOcrResult result;
        TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && result.cleanup_status);
        TEST_CHECK(blocked && !module_live && free_calls == 2 && blocks[slot].uid >= 0);
        SceUID held = blocks[slot].uid; void *base = blocks[slot].base;
        identity_fail_slot = slot; free_fail_slot = -1;
        TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE);
        TEST_CHECK(blocks[slot].uid == held && blocks[slot].base == base && free_calls == 2);
        identity_fail_slot = -1;
        TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK && free_calls == 3); drained();
    }
}
static void test_accepted_free_never_repeats_uid_free(void)
{
    if (!have_model()) return;
    setup(); accepted_free_lookup_valid = 1; VjoGameOcrResult result;
    TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && result.cleanup_status && free_calls == 3);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && free_calls == 3 && !foreign_frees);
    for (unsigned i=0;i<3;i++) { TEST_CHECK(!owned[i].live && blocks[i].free_accepted); owned[i].lookup_valid = 0; }
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK && free_calls == 3); drained();
}
static void test_raw_cancellation_and_model_close_failure(void)
{
    if (!have_model()) return;
    setup(); cancel_on_raw = 1; VjoGameOcrResult result;
    TEST_CHECK(run(&result) == VJO_E_CANCELLED && !result.text[0] && !engine_runs && !result.cleanup_status); drained();
    setup(); model_close_rc = -1;
    TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && result.cleanup_status && model_file && model_fd == 2);
    TEST_CHECK(!has_owned_blocks() && !module_live);
    model_close_rc = 0; TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK); drained();
}
static void test_complete_failure_retries_without_new_job(void)
{
    if (!have_model()) return;
    setup(); queued = 1; complete_failures = 1; stop_loop_on_complete = 1;
    TEST_ASSERT(vjo_game_ocr_worker_start() == VJO_OK);
    TEST_CHECK(worker_main(0,NULL) == 0);
    TEST_CHECK(takes == 1 && completes == 2 && engine_runs == 1 && !claim_outstanding);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK); drained();
}
static void test_persistent_complete_failure_refuses_unload(void)
{
    if (!have_model()) return;
    setup(); queued = 1; complete_failures = -1; stop_loop_after_delays = 3;
    TEST_ASSERT(vjo_game_ocr_worker_start() == VJO_OK); TEST_CHECK(worker_main(0,NULL) == 0);
    TEST_CHECK(claim_outstanding && takes == 1 && engine_runs == 1);
    TEST_CHECK(module_stop(0,NULL) == SCE_KERNEL_STOP_FAIL && claim_outstanding);
    complete_failures = 0;
    TEST_CHECK(module_stop(0,NULL) == SCE_KERNEL_STOP_SUCCESS && !claim_outstanding); drained();
}
static void test_start_failure_retains_undeleted_thread_module(void)
{
    setup(); thread_start_rc = thread_delete_rc = -1;
    TEST_CHECK(module_start(0,NULL) == SCE_KERNEL_START_SUCCESS && worker == 77 && blocked);
    TEST_CHECK(!alloc_calls && module_stop(0,NULL) == SCE_KERNEL_STOP_FAIL && worker == 77);
    thread_delete_rc = 0;
    TEST_CHECK(module_stop(0,NULL) == SCE_KERNEL_STOP_SUCCESS && worker < 0); drained();
}
static void test_portable_floorf_matches_finite_binary32(void)
{
    float (*volatile reference)(float) = floorf;
    uint32_t random = 0x9192a3b4;
    for (unsigned i=0;i<200000;i++) {
        random ^= random<<13; random ^= random>>17; random ^= random<<5;
        union { float f; uint32_t u; } value = {.u=random};
        union { float f; uint32_t u; } actual = {vjo_game_test_floorf(value.f)};
        if ((random&0x7f800000u) == 0x7f800000u) TEST_CHECK(actual.u == value.u);
        else {
            union { float f; uint32_t u; } expected = {reference(value.f)};
            TEST_CHECK(actual.u == expected.u);
        }
    }
    /* Both preprocessors use half-pixel coordinates between -.5 and 960. */
    for (int i=-2048;i<960*4096;i+=17) {
        float value = (float)i/4096.0f;
        TEST_CHECK(vjo_game_test_floorf(value) == reference(value));
    }
    union {float f;uint32_t u;} negative_zero = {.u=0x80000000u};
    union {float f;uint32_t u;} actual = {vjo_game_test_floorf(negative_zero.f)};
    TEST_CHECK(actual.u == negative_zero.u);
}
static void assert_empty_error_text(const VjoGameOcrResult *result)
{
    for (unsigned i=0;i<sizeof(result->text);i++) TEST_CHECK(result->text[i] == 0);
}
static void assert_io_diagnostic(const VjoGameOcrResult *result, char asset, char stage,
                                 uint32_t code, char control_stage, uint32_t control_code)
{
    VjoGameOcrIoDiagnostic parsed;
    TEST_CHECK(result->rc == VJO_E_OCR_MODEL_IO && !result->cleanup_status);
    TEST_CHECK(!result->metadata_peak && !result->neural_peak && !alloc_calls && !loads);
    TEST_CHECK(strlen(result->text) == 28);
    TEST_ASSERT(!vjo_game_ocr_io_parse(result->text,sizeof(result->text),&parsed));
    TEST_CHECK(parsed.asset == asset && parsed.stage == stage && parsed.code == code);
    TEST_CHECK(parsed.control_stage == control_stage && parsed.control_code == control_code);
    for (unsigned i=29;i<sizeof(result->text);i++) TEST_CHECK(result->text[i] == 0);
    TEST_CHECK(control_opens == 1 && !stat_calls); drained();
}
static void test_native_open_failure_and_control_outcomes(void)
{
    const int open_errors[] = {(int32_t)0x8001000D,(int32_t)0x80010002};
    for (unsigned i=0;i<sizeof(open_errors)/sizeof(open_errors[0]);i++) {
        setup(); model_open_rc = open_errors[i]; VjoGameOcrResult result;
        TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
        assert_io_diagnostic(&result,'R','O',(uint32_t)open_errors[i],'P',0);
        TEST_CHECK(control_reads == 1 && control_closes == 1);
    }
    setup(); model_open_rc = (int32_t)0x8001000D; control_open_rc = (int32_t)0x80010002;
    VjoGameOcrResult result; TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
    assert_io_diagnostic(&result,'R','O',0x8001000D,'O',0x80010002);
    TEST_CHECK(!control_reads && !control_closes);
    int read_errors[] = {(int32_t)0x80010005,0};
    for (unsigned i=0;i<sizeof(read_errors)/sizeof(read_errors[0]);i++) {
        setup(); model_open_rc = (int32_t)0x8001000D; control_read_rc = read_errors[i];
        TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
        assert_io_diagnostic(&result,'R','O',0x8001000D,'R',(uint32_t)read_errors[i]);
        TEST_CHECK(control_reads == 1 && control_closes == 1);
    }
    setup(); model_open_rc = (int32_t)0x8001000D; control_close_failures = 1;
    TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
    assert_io_diagnostic(&result,'R','O',0x8001000D,'C',0x80010005);
    TEST_CHECK(control_closes == 2); /* failed close UID retained, cleanup retried it */
}
static void test_size_offset_and_read_native_failures(void)
{
    if (!have_model()) return;
    VjoGameOcrResult result;
    setup(); model_size_set = 1; model_size_result = (int32_t)0x80010005;
    TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
    assert_io_diagnostic(&result,'R','S',0x80010005,'P',0);
    TEST_CHECK(closes == 1 && !model_reads);
    setup(); model_offset_seek_rc = (int32_t)0x80010016;
    TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
    assert_io_diagnostic(&result,'R','L',0x80010016,'P',0);
    TEST_CHECK(closes == 1 && !model_reads);
    int read_errors[] = {(int32_t)0x80010005,0};
    for (unsigned i=0;i<sizeof(read_errors)/sizeof(read_errors[0]);i++) {
        setup(); model_read_result_set = 1; model_read_result = read_errors[i];
        TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
        assert_io_diagnostic(&result,'R','R',(uint32_t)read_errors[i],'P',0);
        TEST_CHECK(closes == 1 && model_reads == 1);
    }
    if (getenv("VJO_MEIKI_TEST_DETECT_MODEL")) {
        setup(); queued_request.layout = VJO_GAME_OCR_DIALOGUE_BOX;
        fault_asset = 'D'; model_open_rc = (int32_t)0x8001000D;
        TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO);
        assert_io_diagnostic(&result,'D','O',0x8001000D,'P',0);
        TEST_CHECK(model_opens == 2 && closes == 1);
    }
}
static void test_short_reads_succeed_and_hash_size_errors_stay_model(void)
{
    if (!have_model()) return;
    setup(); model_short_read_bytes = 3072; VjoGameOcrResult result;
    TEST_CHECK(run(&result) == VJO_OK && !result.cleanup_status && !control_opens && !stat_calls);
    TEST_CHECK(model_reads > MODEL_BYTES/8192 && engine_runs == 1); drained();
    setup(); corrupt_model_bytes = 1;
    TEST_CHECK(run(&result) == VJO_E_OCR_MODEL && !result.cleanup_status);
    TEST_CHECK(!alloc_calls && !loads && !control_opens && !io_failure.stage); assert_empty_error_text(&result); drained();
    SceOff wrong_sizes[] = {0,1,MODEL_BYTES-1,MODEL_BYTES+1};
    for (unsigned i=0;i<sizeof(wrong_sizes)/sizeof(wrong_sizes[0]);i++) {
        setup(); model_size_set = 1; model_size_result = wrong_sizes[i];
        TEST_CHECK(run(&result) == VJO_E_OCR_MODEL && !result.cleanup_status);
        TEST_CHECK(closes == 1 && !model_reads && !alloc_calls && !control_opens && !io_failure.stage);
        assert_empty_error_text(&result); drained();
    }
}
static void test_io_cleanup_and_cancellation_suppress_diagnostics(void)
{
    VjoGameOcrResult result;
    setup(); model_open_rc = (int32_t)0x8001000D; control_close_failures = -1;
    TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && result.cleanup_status);
    TEST_CHECK(blocked && control_fd == 3 && control_live && !alloc_calls); assert_empty_error_text(&result);
    TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_E_OCR_UNAVAILABLE && control_fd == 3);
    control_close_failures = 0; TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK); drained();
    setup(); model_open_rc = (int32_t)0x8001000D; cancel_on_model_failure = 1;
    TEST_CHECK(run(&result) == VJO_E_CANCELLED && !control_opens && !result.cleanup_status);
    assert_empty_error_text(&result); drained();
    setup(); model_open_rc = (int32_t)0x8001000D; cancel_on_control_read = 1;
    TEST_CHECK(run(&result) == VJO_E_CANCELLED && control_closes == 1 && !result.cleanup_status);
    assert_empty_error_text(&result); drained();
    if (!have_model()) return;
    setup(); model_read_result_set = 1; model_read_result = 0; model_close_rc = -1;
    TEST_CHECK(run(&result) == VJO_E_OCR_UNAVAILABLE && result.cleanup_status && model_fd == 2);
    TEST_CHECK(!alloc_calls && blocked); assert_empty_error_text(&result);
    model_close_rc = 0; TEST_CHECK(vjo_game_ocr_worker_stop() == VJO_OK); drained();
    setup(); model_short_read_bytes = 3072; cancel_after_model_read = 1;
    TEST_CHECK(run(&result) == VJO_E_CANCELLED && model_reads == 1 && !control_opens && !alloc_calls);
    assert_empty_error_text(&result); drained();
}
static void test_io_failure_and_successful_job_retries_reset_metadata(void)
{
    if (!have_model()) return;
    setup(); VjoGameOcrResult result;
    TEST_CHECK(run(&result) == VJO_OK && engine_runs == 1); drained();
    model_open_rc = (int32_t)0x8001000D;
    TEST_CHECK(run(&result) == VJO_E_OCR_MODEL_IO && !result.cleanup_status);
    VjoGameOcrIoDiagnostic diagnostic;
    TEST_CHECK(!vjo_game_ocr_io_parse(result.text,sizeof(result.text),&diagnostic));
    TEST_CHECK(!result.metadata_peak && !result.neural_peak && engine_runs == 1 && control_opens == 1); drained();
    model_open_rc = 0;
    TEST_CHECK(run(&result) == VJO_OK && engine_runs == 2 && loads == 2 && unloads == 2);
    TEST_CHECK(!io_failure.stage && control_opens == 1 && result.text[0] != 'M'); drained();
}
TEST_LIST = {
    {"budget_and_import_guards_allocate_nothing",test_budget_and_import_guards_allocate_nothing},
    {"job_and_budget_threshold",test_job_and_budget_threshold},
    {"partial_allocations_and_invalid_mapping",test_partial_allocations_and_invalid_mapping},
    {"orphan_mapping_cleanup_failure_is_retained",test_orphan_mapping_cleanup_failure_is_retained},
    {"base_failure_retains_uid_and_foreign_free_is_refused",test_base_failure_retains_uid_and_foreign_free_is_refused},
    {"free_failure_and_identity_failure_preserve_owner",test_free_failure_and_identity_failure_preserve_owner},
    {"accepted_free_never_repeats_uid_free",test_accepted_free_never_repeats_uid_free},
    {"raw_cancellation_and_model_close_failure",test_raw_cancellation_and_model_close_failure},
    {"complete_failure_retries_without_new_job",test_complete_failure_retries_without_new_job},
    {"persistent_complete_failure_refuses_unload",test_persistent_complete_failure_refuses_unload},
    {"start_failure_retains_undeleted_thread_module",test_start_failure_retains_undeleted_thread_module},
    {"portable_floorf_matches_finite_binary32",test_portable_floorf_matches_finite_binary32},
    {"native_open_failure_and_control_outcomes",test_native_open_failure_and_control_outcomes},
    {"size_offset_and_read_native_failures",test_size_offset_and_read_native_failures},
    {"short_reads_succeed_and_hash_size_errors_stay_model",test_short_reads_succeed_and_hash_size_errors_stay_model},
    {"io_cleanup_and_cancellation_suppress_diagnostics",test_io_cleanup_and_cancellation_suppress_diagnostics},
    {"io_failure_and_successful_job_retries_reset_metadata",test_io_failure_and_successful_job_retries_reset_metadata},
    {NULL,NULL}
};
