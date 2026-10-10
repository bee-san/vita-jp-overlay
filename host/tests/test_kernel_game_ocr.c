/* Actual IPC and capture ownership code with bounded user-copy substitutes. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
typedef struct { size_t size; SceUID modid; } tai_module_info_t;
int taiGetModuleInfoForKernel(SceUID, const char *, tai_module_info_t *);
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/game_ocr.c"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "../../kernel/capture_request.c"

VjoKernelState g;
unsigned syscall_depth;
static SceUID caller;
static int module_missing, module_id, copy_in_fail, copy_out_fail;
static unsigned locks, frees, allocations;
static uint8_t raw_storage[VJO_MAX_W * VJO_MAX_H * 4];
static VjoGameOcrResult message;

void klog(const char *format, ...) { (void)format; }
SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return 1000000; }
int ksceKernelLockMutex(SceUID uid, int count, void *timeout)
{
    (void)count; (void)timeout; TEST_CHECK(uid == g.lock && locks == 0); locks++; return 0;
}
int ksceKernelUnlockMutex(SceUID uid, int count)
{
    (void)count; TEST_CHECK(uid == g.lock && locks == 1); locks--; return 0;
}
int taiGetModuleInfoForKernel(SceUID pid, const char *name, tai_module_info_t *info)
{
    TEST_CHECK(pid == caller && syscall_depth == 1 && locks == 0);
    TEST_CHECK(!strcmp(name, VJO_GAME_OCR_MODULE) && info->size == sizeof(*info));
    info->modid = module_id;
    return module_missing ? -1 : 0;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize bytes)
{
    TEST_CHECK(syscall_depth <= 1);
    if (copy_in_fail || !src) return -1;
    memcpy(dst, src, bytes); return 0;
}
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize bytes)
{
    TEST_CHECK(syscall_depth == 1 && locks == 1);
    if (copy_out_fail || !dst) return -1;
    memcpy(dst, src, bytes); return 0;
}
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize bytes, void *opt)
{
    (void)name; (void)opt;
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW && bytes <= sizeof(raw_storage));
    allocations++; return 27;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
    TEST_CHECK(uid == 27); *base = raw_storage; return 0;
}
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 27); frees++; return 0; }

static void setup(void)
{
    memset(&g, 0, sizeof(g));
    game_ocr_init();
    g.lock = 1; g.shell_pid = 3; g.game_pid = 7; g.game_active = VJO_GAME; g.text_epoch = 1;
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    g.raw = raw_storage; g.mem_uid = 27; g.raw_capacity = 4096;
    g.crop_w = g.crop_h = 32; g.raw_stride = 128;
    g.capture_seq = g.done_seq = 11; g.fb_w = g.fb_h = 32;
    g.capture_game_pid = 7; g.capture_epoch = 1;
    caller = 7; module_id = 99;
    module_missing = copy_in_fail = copy_out_fail = 0;
    locks = allocations = frees = syscall_depth = 0;
    memset(&message, 0, sizeof(message));
    TEST_ASSERT(vjoOcrRegister() == 0);
    caller = 3;
}

static VjoGameOcrRequest make_request(void)
{
    VjoGameOcrRequest r = {0};
    r.size = sizeof(r); r.done_seq = g.done_seq;
    strcpy(r.model_dir, "ux0:data/VitaJPOverlay/meiki");
    return r;
}

static uint32_t submit(void)
{
    VjoGameOcrRequest r = make_request();
    caller = 3;
    int seq = vjoOcrSubmit(&r);
    TEST_ASSERT(seq > 0);
    return (uint32_t)seq;
}

static void take(uint32_t seq)
{
    VjoGameOcrRequest r = {0};
    caller = 7;
    TEST_ASSERT(vjoOcrTake(&r) == 0);
    TEST_CHECK(r.size == sizeof(r) && r.seq == seq && r.done_seq == 11);
    TEST_CHECK(r.width == 32 && r.height == 32 && r.stride == 128);
    memset(&message, 0, sizeof(message));
    message.size = sizeof(message); message.seq = seq;
    strcpy(message.text, "猫");
}

static void registration_and_permissions(void)
{
    setup(); caller = 8;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrSubmit(&request) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrRead(1, &message) == VJO_ERR_PERM);
    TEST_CHECK(vjoOcrCancel(1) == VJO_ERR_PERM);
    caller = 7; module_missing = 1;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_PERM);
    module_missing = 0; g.game_active = 0;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
}

static void complete_and_replacement(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(game_ocr_capture_busy_locked());
    TEST_CHECK(!game_ocr_raw_caller_locked(7));
    TEST_CHECK(vjoOcrRead(seq, &message) == 1);
    take(seq);
    TEST_CHECK(vjoOcrRegister() == 0); /* Repeated registration keeps the claim. */
    TEST_CHECK(vjoOcrTake(&request) == VJO_ERR_BUSY);
    TEST_CHECK(game_ocr_raw_caller_locked(7) && !game_ocr_raw_caller_locked(8));
    TEST_CHECK(vjoOcrCancelled(seq) == 0);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked() && !game_ocr_raw_caller_locked(7));
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == 0 && !strcmp(message.text, "猫"));
    uint32_t second = submit();
    TEST_CHECK(second > seq && vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    take(second); message.seq = seq;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG && game_ocr_capture_busy_locked());
}

static void capture_pins_queued_and_claimed(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_BUSY);
    buffers_free();
    TEST_CHECK(g.raw == raw_storage && !frees);
    take(seq);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    g.capture_release = 1; buffers_trim();
    TEST_CHECK(g.raw == raw_storage && !frees);
    TEST_CHECK(buffers_alloc(8192) == VJO_ERR_BUSY && !allocations);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(capture_request(0) > 0);
}

static void cancellation_drains_reader(void)
{
    setup(); uint32_t seq = submit(); take(seq); caller = 3;
    TEST_CHECK(vjoOcrCancel(seq) == 0);
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    TEST_CHECK(!game_ocr_raw_caller_locked(7));
    buffers_free(); TEST_CHECK(!frees);
    caller = 7;
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(!game_ocr_capture_busy_locked());
    caller = 3;
    TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
    TEST_CHECK(capture_request(0) > 0);
}

static void cancel_before_take(void)
{
    setup(); uint32_t seq = submit();
    TEST_CHECK(vjoOcrCancel(seq) == 0 && !game_ocr_capture_busy_locked());
    caller = 7; VjoGameOcrRequest r;
    TEST_CHECK(vjoOcrTake(&r) == 1);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
}

static void suspended_and_same_pid_epoch(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.game_active = 0; g.text_epoch++;
    game_ocr_foreground_changed_locked();
    TEST_CHECK(game_ocr_capture_busy_locked() && !game_ocr_raw_caller_locked(7));
    TEST_CHECK(vjoOcrCancelled(seq) == 1);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    g.game_active = VJO_GAME; g.text_epoch++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0);
    g.capture_epoch = g.text_epoch; /* A fresh capture in the resumed session. */
    caller = 3; uint32_t next = submit(); TEST_CHECK(next > seq);
    caller = 7;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); /* Old reply cannot claim a new job. */
}

static void foreground_replacement_and_exit(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.game_pid = 8; g.game_active = VJO_GAME; g.text_epoch++;
    game_ocr_foreground_changed_locked();
    caller = 8; TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_PERM);
    game_ocr_process_gone(7);
    TEST_CHECK(!game_ocr_capture_busy_locked() && vjoOcrRegister() == 0);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_CANCELLED);
}

static void shell_restart(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    g.shell_pid = 4; game_ocr_shell_changed_locked();
    TEST_CHECK(game_ocr_capture_busy_locked() && vjoOcrCancelled(seq) == 1);
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_PERM);
    caller = 4; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_ARG);
    caller = 7; TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(vjoOcrRegister() == 0 && !game_ocr_capture_busy_locked());
    caller = 4; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); /* Old Shell's finished crop is stale too. */
}

static void cleanup_failure_and_module_replacement(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    message.cleanup_status = -1; message.rc = -12;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && game_ocr_capture_busy_locked());
    caller = 3; TEST_CHECK(vjoOcrRead(seq, &message) == 0 && message.cleanup_status == -1);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY);
    caller = 7; module_id++;
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_BUSY);
    module_id--; message.cleanup_status = 0;
    TEST_CHECK(vjoOcrComplete(&message) == 0 && !game_ocr_capture_busy_locked());
}

static void invalid_capture_and_wire(void)
{
    setup(); VjoGameOcrRequest r = make_request();
    r.size--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.size++;
    r.layout = 2; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.layout = 0;
    r.model_dir[255] = 'x'; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.model_dir[255] = 0;
    r.done_seq--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); r.done_seq++;
    g.capture_once = 1; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.capture_once = 0;
    g.raw_capacity--; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.raw_capacity++;
    g.raw_valid = 0; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.raw_valid = 1;
    g.capture_epoch++; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.capture_epoch--;
    g.capture_game_pid++; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG); g.capture_game_pid--;
    g.capture_state = CAPTURE_PENDING; TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_ARG);
    g.capture_state = CAPTURE_IDLE; uint32_t seq = submit(); take(seq);
    message.size--; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.size++;
    message.text[4095] = 'x'; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG); message.text[4095] = 0;
    message.cleanup_status = -1; TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_ARG);
    TEST_CHECK(game_ocr_capture_busy_locked());
    g.done_seq++; TEST_CHECK(!game_ocr_raw_caller_locked(7));
}

static void user_copy_failures_and_sequence_exhaustion(void)
{
    setup(); VjoGameOcrRequest r = make_request(); copy_in_fail = 1;
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_COPY && !game_ocr_capture_busy_locked());
    copy_in_fail = 0; uint32_t seq = submit(); caller = 7; copy_out_fail = 1;
    TEST_CHECK(vjoOcrTake(&r) == VJO_ERR_COPY && !claimed && pending);
    copy_out_fail = 0; take(seq); copy_in_fail = 1;
    TEST_CHECK(vjoOcrComplete(&message) == VJO_ERR_COPY && claimed);
    copy_in_fail = 0; TEST_CHECK(vjoOcrComplete(&message) == 0);
    caller = 3; copy_out_fail = 1; TEST_CHECK(vjoOcrRead(seq, &message) == VJO_ERR_COPY);
    copy_out_fail = 0; sequence = 0x7FFFFFFFu;
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_BUSY);
    TEST_CHECK(locks == 0 && syscall_depth == 0);
}

static void shutdown_refuses_claim_and_closes_registration(void)
{
    setup(); uint32_t seq = submit(); take(seq);
    TEST_CHECK(game_ocr_shutdown_locked() == VJO_ERR_BUSY);
    TEST_CHECK(vjoOcrComplete(&message) == 0);
    TEST_CHECK(game_ocr_shutdown_locked() == 0);
    TEST_CHECK(vjoOcrRegister() == VJO_ERR_NO_GAME);
    caller = 3; VjoGameOcrRequest r = make_request();
    TEST_CHECK(vjoOcrSubmit(&r) == VJO_ERR_NO_GAME);
}

TEST_LIST = {
    {"registration and caller permissions", registration_and_permissions},
    {"completion and stale replacement", complete_and_replacement},
    {"queued and claimed capture pins", capture_pins_queued_and_claimed},
    {"cancelled reader must drain", cancellation_drains_reader},
    {"cancel before take", cancel_before_take},
    {"suspend and same PID epoch", suspended_and_same_pid_epoch},
    {"foreground replacement and process exit", foreground_replacement_and_exit},
    {"Shell restart", shell_restart},
    {"failed cleanup and module replacement", cleanup_failure_and_module_replacement},
    {"capture and wire validation", invalid_capture_and_wire},
    {"copy failures and sequence exhaustion", user_copy_failures_and_sequence_exhaustion},
    {"shutdown ownership", shutdown_refuses_claim_and_closes_registration},
    {NULL, NULL}
};
