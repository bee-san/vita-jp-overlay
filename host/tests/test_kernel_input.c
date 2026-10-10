/* Exercise the shipped controller wrappers and filter. Only SDK boundaries
 * are substituted; the trigger state machine is linked from production. */
#include "acutest.h"
#include <stdarg.h>
#include <psp2kern/host_stubs.h>
#include <psp2kern/ctrl.h>
#include "../../include/vjo_api.h"
#include "../../kernel/triggers.h"
#define VJO_TEST_INPUT_HOOKS 1
#include <taihen.h>

/* Keep unrelated framebuffer/text structures out of this input-only harness. */
#define VJO_KERNEL_H
static struct {
    SceUID game_pid, shell_pid, evf;
    int game_active, input_block;
    int trigger[TRIG_COUNT];
    uint32_t trigger_gen, suppress_mask, raw_buttons;
    VjoInput last_input;
} g;
void klog(const char *fmt, ...);
void display_static_fb_sample(void);
SceUID ksceKernelGetThreadId(void);
int ksceKernelSetEventFlag(SceUID, unsigned);
#include "../../kernel/input.c"

unsigned syscall_depth;
static SceUID caller;
static uint64_t now;
static SceCtrlData physical;
static unsigned read_calls, write_calls, continue_calls, frame_samples;
static unsigned fail_read_at, fail_write_at;
static unsigned log_calls;
static int longest_log;
static int original_error;
static tai_hook_ref_t continued_ref;
static const int copy_error = -123;

void klog(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    va_start(ap, fmt);
    int length = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_calls++;
    if (length > longest_log) longest_log = length;
    TEST_CHECK(length >= 0 && length <= 190); /* production logger reserves newline */
}
SceUID ksceKernelGetProcessId(void) { return caller; }
SceUID ksceKernelGetThreadId(void) { return 3; }
uint64_t ksceKernelGetSystemTimeWide(void) { return now; }
int ksceKernelSetEventFlag(SceUID uid, unsigned bits) { return 0; }
void display_static_fb_sample(void) { frame_samples++; }
int ksceCtrlGetSamplingMode(int *mode) { *mode = SCE_CTRL_MODE_ANALOG; return 0; }
int ksceCtrlSetSamplingMode(int mode) { return 0; }
int ksceCtrlPeekBufferPositive(int port, SceCtrlData *out, int count)
{
    TEST_CHECK(count == 1);
    *out = physical;
    return 1;
}
int ksceTouchPeek(uint32_t port, KTouchData *data, uint32_t count) { return 0; }
int ksceTouchSetSamplingState(uint32_t port, int state) { return 0; }
SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref,
                                    const char *module, uint32_t library, uint32_t nid, void *fn)
{
    TEST_CHECK(pid == KERNEL_PID && fn != NULL);
    *ref = nid;
    return 1;
}
int taiHookReleaseForKernel(SceUID uid, tai_hook_ref_t ref) { return 0; }
int test_tai_continue(tai_hook_ref_t ref, int port, void *out, unsigned count, ...)
{
    continued_ref = ref;
    continue_calls++;
    if (original_error) return original_error;
    TEST_ASSERT(count <= 64);
    SceCtrlData *pads = out;
    int negative = ref == 0x104ED1A7 || ref == 0x81A89660 ||
                   ref == 0x15F96FB0 || ref == 0x27A0C5FB;
    for (unsigned i = 0; i < count; i++) {
        pads[i] = physical;
        pads[i].timeStamp += i;
        if (negative) pads[i].buttons = ~pads[i].buttons;
    }
    return (int)count;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize bytes)
{
    if (++read_calls == fail_read_at) return copy_error;
    memcpy(dst, src, bytes);
    return 0;
}
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize bytes)
{
    if (++write_calls == fail_write_at) return copy_error;
    memcpy(dst, src, bytes);
    return 0;
}

static void setup(void)
{
    memset(&g, 0, sizeof(g));
    memset(&physical, 0xA5, sizeof(physical));
    memset(hold, 0, sizeof(hold));
    memset(edges, 0, sizeof(edges));
    hold_busy = rear_busy = rear_peek_thid = hold_gen = analog_on = 0;
    input_trace_reset();
    rear_down = rear_taps = 0;
    rear_sampled_us = rear_down_t = rear_up_t = 0;
    g.game_pid = caller = 7;
    g.shell_pid = 3;
    g.game_active = VJO_GAME;
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_L_R;
    g.trigger[TRIG_SUBTITLE] = VJO_TRIGGER_SELECT_L;
    g.trigger_gen = 1;
    physical.timeStamp = now = 1000000;
    physical.buttons = SCE_CTRL_CROSS;
    g.raw_buttons = physical.buttons;
    physical.lx = 12; physical.ly = 34; physical.rx = 210; physical.ry = 240;
    read_calls = write_calls = continue_calls = frame_samples = 0;
    fail_read_at = fail_write_at = 0;
    log_calls = 0; longest_log = 0;
    original_error = 0;
    continued_ref = 0;
    syscall_depth = 0;
    TEST_ASSERT(sizeof(SceCtrlData) == 32);
    TEST_ASSERT(input_hooks_install() == 0);
}

static void check_neutral(const SceCtrlData *pad, unsigned index, int negative)
{
    TEST_CHECK(pad->buttons == (negative ? UINT32_MAX : 0));
    TEST_CHECK(pad->lx == 128 && pad->ly == 128 && pad->rx == 128 && pad->ry == 128);
    TEST_CHECK(pad->timeStamp == physical.timeStamp + index);
    TEST_CHECK(memcmp(pad->reserved, physical.reserved, sizeof(pad->reserved)) == 0);
}

static void test_positive2_block_and_raw_poll(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    fail_read_at = 1; /* A failed diagnostic read cannot disable blocking. */
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(continued_ref == 0xC4226A3E && continue_calls == 1);
    check_neutral(&pad, 0, 0);
    TEST_CHECK(read_calls == 1 && write_calls == 1);
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    check_neutral(&pad, 0, 0);
    TEST_CHECK(read_calls == 1 && write_calls == 2); /* no per-frame diagnostic reads */
    caller = KERNEL_PID;
    input_poll();
    TEST_CHECK(g.raw_buttons == SCE_CTRL_CROSS && g.last_input.buttons == SCE_CTRL_CROSS);
    TEST_CHECK(g.last_input.lx == physical.lx && g.last_input.ry == physical.ry);
    TEST_CHECK(physical.buttons == SCE_CTRL_CROSS);
}

static void test_negative_and_full_batch(void)
{
    struct { uint64_t before; SceCtrlData pads[64]; uint64_t after; } batch;
    setup();
    g.input_block = 1;
    batch.before = batch.after = UINT64_C(0xFEED123456789ABC);
    TEST_CHECK(read_neg2_patched(0, batch.pads, 64) == 64);
    TEST_CHECK(continued_ref == 0x27A0C5FB && read_calls == 1 && write_calls == 64);
    for (unsigned i = 0; i < 64; i++) check_neutral(&batch.pads[i], i, 1);
    TEST_CHECK(batch.before == UINT64_C(0xFEED123456789ABC));
    TEST_CHECK(batch.after == UINT64_C(0xFEED123456789ABC));
}

static void test_foreign_and_shell_processes(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    const SceUID callers[] = { 99, 3, KERNEL_PID };
    for (unsigned i = 0; i < sizeof(callers)/sizeof(callers[0]); i++) {
        caller = callers[i];
        TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
        TEST_CHECK(memcmp(&pad, &physical, sizeof(pad)) == 0);
    }
    TEST_CHECK(read_calls == 0 && write_calls == 0);
}

static void test_blocked_write_failure(void)
{
    SceCtrlData pads[3];
    setup();
    g.input_block = 1;
    fail_write_at = 2;
    TEST_CHECK(read_pos2_patched(0, pads, 3) == copy_error);
    TEST_CHECK(read_calls == 1 && write_calls == 2);
    check_neutral(&pads[0], 0, 0);
    TEST_CHECK(pads[1].buttons == SCE_CTRL_CROSS); /* failure cannot report these as valid */
    TEST_CHECK(!hold_busy);
    fail_write_at = 0;
    TEST_CHECK(read_pos2_patched(0, pads, 1) == 1);
    check_neutral(&pads[0], 0, 0);
}

static void test_unblocked_copy_failures(void)
{
    SceCtrlData pad;
    setup();
    fail_read_at = 1;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    TEST_CHECK(read_calls == 1 && write_calls == 0 && !hold_busy);
    fail_read_at = 0;
    fail_write_at = 1;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    TEST_CHECK(write_calls == 1 && !hold_busy);
    fail_write_at = 0;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(pad.buttons == SCE_CTRL_CROSS && pad.lx == physical.lx);
}

static void test_original_error_and_inactive_game(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    original_error = -456;
    memset(&pad, 0xCD, sizeof(pad));
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == -456);
    TEST_CHECK(read_calls == 0 && write_calls == 0 && pad.buttons == 0xCDCDCDCD);
    original_error = 0;
    g.game_active = VJO_GAME_NONE;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(memcmp(&pad, &physical, sizeof(pad)) == 0);
    TEST_CHECK(read_calls == 0 && write_calls == 0);
}

static void test_diagnostics_are_bounded(void)
{
    SceCtrlData pad;
    setup();
    g.input_block = 1;
    g.raw_buttons = 0;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    g.raw_buttons = SCE_CTRL_CROSS;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(read_calls == 2 && log_calls == 2);
    for (unsigned i = 0; i < 256; i++) {
        fail_write_at = write_calls + 1;
        TEST_CHECK(read_pos2_patched(0, &pad, 1) == copy_error);
    }
    TEST_CHECK(read_calls == 2 && log_calls == 3);
    fail_write_at = 0;
    caller = 99;
    for (unsigned i = 0; i < 256; i++) TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(log_calls == 4);
    caller = g.game_pid;
    original_error = -456;
    for (unsigned i = 0; i < 256; i++) TEST_CHECK(read_pos2_patched(0, &pad, 1) == -456);
    TEST_CHECK(log_calls == 5 && longest_log <= 190);
    input_trace_reset();
    original_error = 0;
    TEST_CHECK(read_pos2_patched(0, &pad, 1) == 1);
    TEST_CHECK(read_calls == 3 && log_calls == 6);
}

TEST_LIST = {
    { "positive2 blocks cross without changing raw shell input", test_positive2_block_and_raw_poll },
    { "negative logic and all 64 queued samples", test_negative_and_full_batch },
    { "foreign processes and shell keep their input", test_foreign_and_shell_processes },
    { "blocked copy failure never reports a successful batch", test_blocked_write_failure },
    { "unblocked copy failures release serialization", test_unblocked_copy_failures },
    { "native errors and inactive games preserve original behavior", test_original_error_and_inactive_game },
    { "controller diagnostics stay bounded across repeated failures", test_diagnostics_are_bounded },
    { NULL, NULL }
};
