/* Verify the shipped kernel input boundary used by a disabled title. */
#include "acutest.h"
#include "games.h"
#include <psp2kern/ctrl.h>
#include "../../include/vjo_api.h"
#include "../../kernel/triggers.h"
#define VJO_KERNEL_H
static struct {
    SceUID game_pid, shell_pid, evf;
    int game_active, input_block, trigger[TRIG_COUNT];
    uint32_t trigger_gen, suppress_mask, raw_buttons, input_us32, input_edge_us32;
    VjoInput last_input;
} g;
void klog(const char *fmt, ...) {}
void display_static_fb_sample(void) {}
#include "../../kernel/input.c"
static SceCtrlData physical;
static unsigned events, copies, rear_peeks;
SceUID ksceKernelGetProcessId(void) { return 7; }
SceUID ksceKernelGetThreadId(void) { return 8; }
uint64_t ksceKernelGetSystemTimeWide(void) { return 1000000; }
int ksceKernelSetEventFlag(SceUID id, unsigned bits) { events |= bits; return 0; }
int ksceCtrlGetSamplingMode(int *mode) { *mode = SCE_CTRL_MODE_ANALOG; return 0; }
int ksceCtrlSetSamplingMode(int mode) { return 0; }
int ksceCtrlPeekBufferPositive(int port, SceCtrlData *out, int count) { *out = physical; return 1; }
int ksceKernelMemcpyUserToKernel(void *d, const void *s, SceSize n) { copies++; memcpy(d, s, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *d, const void *s, SceSize n) { copies++; memcpy(d, s, n); return 0; }
int ksceTouchPeek(uint32_t port, KTouchData *data, uint32_t n) { rear_peeks++; return 0; }
int ksceTouchSetSamplingState(uint32_t port, int state) { return 0; }

static void test_disabled_game_passes_all_input(void)
{
    VjoGames policy;
    vjo_games_defaults(&policy);
    TEST_ASSERT(vjo_games_set(&policy, "PCSA00029", 0) == 0);
    memset(&g, 0, sizeof(g));
    memset(&physical, 0x55, sizeof(physical));
    memset(edges, 0, sizeof(edges));
    events = copies = rear_peeks = 0;
    g.game_pid = 7;
    g.game_active = vjo_games_enabled(&policy, "PCSA00029") ? VJO_GAME : VJO_GAME_NONE;
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_L_R;
    g.trigger[TRIG_SUBTITLE] = VJO_TRIGGER_SELECT_R;
    /* Even stale overlay suppression must not consume this game's controls. */
    g.input_block = 1;
    g.suppress_mask = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    physical.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_CROSS;
    SceCtrlData pad = physical;
    filter_ctrl(0, &pad, 1, 0);
    TEST_CHECK(!memcmp(&pad, &physical, sizeof(pad)) && copies == 0);
    input_poll();
    TEST_CHECK(events == 0 && g.last_input.buttons == physical.buttons);
    pad.buttons = ~physical.buttons;
    SceCtrlData negative = pad;
    filter_ctrl(0, &pad, 1, 1);
    TEST_CHECK(!memcmp(&pad, &negative, sizeof(pad)));
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_REAR_DOUBLE_TAP;
    rear_sample();
    KTouchData touch;
    memset(&touch, 0x55, sizeof(touch));
    KTouchData before = touch;
    TEST_CHECK(filter_touch(&touch, 1) == 1);
    TEST_CHECK(!memcmp(&touch, &before, sizeof(touch)) && rear_peeks == 0);
}

static void test_enabled_game_still_triggers(void)
{
    memset(&g, 0, sizeof(g));
    memset(edges, 0, sizeof(edges));
    g.game_pid = 7; g.game_active = VJO_GAME;
    g.trigger[TRIG_TOGGLE] = VJO_TRIGGER_L_R;
    g.trigger[TRIG_SUBTITLE] = VJO_TRIGGER_SELECT_R;
    events = copies = 0;
    physical.buttons = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER;
    input_poll();
    TEST_CHECK(events & VJO_EV_TRIGGER);
    SceCtrlData pad = physical;
    filter_ctrl(0, &pad, 1, 0);
    TEST_CHECK(!(pad.buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)));
    TEST_CHECK(copies == 2);
}
TEST_LIST = {
    {"disabled games keep L/R, sticks, negative pad and rear/front touch", test_disabled_game_passes_all_input},
    {"enabled games still recognize and reserve the OCR combo", test_enabled_game_still_triggers},
    {NULL, NULL}
};
