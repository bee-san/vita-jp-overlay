/* Vita JP Overlay kernel module: shared state between the hooks, the worker
 * thread and the syscall exports. */
#ifndef VJO_KERNEL_H
#define VJO_KERNEL_H

#include <psp2kern/ctrl.h>
#include <psp2kern/display.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/types.h>
#include <stdint.h>

#include "../include/vjo_api.h"
#include "foreground.h" /* IEV_* */
#include "triggers.h"   /* TRIG_* */

#define CAPTURE_IDLE    0
#define CAPTURE_PENDING 1     /* the next game frame is copied in game context (display.c) */
#define CAPTURE_COPYING 2     /* claimed (CAS from PENDING) by a hook or the worker */
#define CAPTURE_COPIED  3     /* copy finished; the worker publishes the result */

#define VJO_MAX_W 960
#define VJO_MAX_H 544
#define VJO_CHECK_ROW_STEP 16 /* change detection samples every 16th row */
#define VJO_CHECK_EVERY_FRAMES 8

typedef struct {
    SceUID evf;    /* public events (VJO_EV_*) */
    SceUID ievf;   /* internal events (IEV_*) */
    SceUID lock;   /* mutex: buffers and capture */
    SceUID game_lock; /* mutex: game_pid, game_active, prev_game_pid changes (lifecycle.c) */

    /* foreground game (VjoForeground in foreground.h) */
    volatile SceUID game_pid;
    volatile int game_active;  /* the shell confirmed game_pid is a game: its VJO_GAME* mode */
    volatile uint32_t text_epoch; /* invalidates sources even on a fast same-PID suspend/resume */
    SceUID prev_game_pid;      /* confirmed game behind game_pid, still running */

    /* last game frame seen (index 0, primary head; submitted or sampled) */
    volatile uint32_t fb_pitch, fb_fmt, fb_w, fb_h;

    /* OCR region (normalized), set by the shell */
    VjoRect region;

    /* per-game capture buffer (see buffers.c) */
    SceUID mem_uid;
    uint8_t *raw;           /* A8B8G8R8, crop_h rows of raw_stride bytes */
    uint32_t raw_capacity;  /* allocated bytes; checked before a display-hook copy */
    volatile int alloc_status;

    /* capture */
    volatile int capture_state; /* CAPTURE_*, claimed with __sync CAS */
    int capture_result;
    uint32_t crop_w, crop_h;   /* captured pixels */
    uint32_t capture_x, capture_y, capture_fb_w, capture_fb_h; /* request geometry */
    uint32_t raw_stride;       /* bytes per row in raw */
    uint32_t capture_checksum;
    int64_t capture_requested_us;
    volatile int raw_valid;
    volatile int capture_release; /* release an idle buffer after switching to native text */
    uint32_t capture_seq;      /* bumped per request */
    uint32_t done_seq;         /* seq of the last finished capture */
    int capture_full;          /* the pending capture is the whole frame (VJO_CAPTURE_FULL) */
    int capture_once;          /* protect then release a single-pass JPEG capture */

    /* change detection */
    uint32_t checksum;
    volatile uint32_t hook_checksum;     /* latest, computed in the display hook */
    volatile uint32_t hook_checksum_seq; /* bumped per new hook_checksum */
    uint32_t seen_checksum_seq;
    volatile int stable;
    int stable_fired;
    int64_t last_change_us;

    /* input */
    volatile int trigger[TRIG_COUNT]; /* enum VjoTrigger, by TRIG_* */
    volatile uint32_t trigger_gen;    /* bumped when trigger changes */
    volatile int input_block;
    volatile SceUID shell_pid;
    volatile uint32_t suppress_mask; /* combo buttons hidden until released */
    volatile uint32_t raw_buttons;
    VjoInput last_input;
} VjoKernelState;

extern VjoKernelState g;
void input_trace_reset(void);

/* log.c: ring buffer, flushed to kernel.txt by the worker */
void klog(const char *fmt, ...);
/* Atomically claims a pending capture; only the winner copies. */
static inline int capture_claim(void)
{
    return __sync_bool_compare_and_swap(&g.capture_state, CAPTURE_PENDING, CAPTURE_COPYING);
}
int klog_read(char *dst, int len);

/* buffers.c */
int buffers_alloc(uint32_t bytes); /* capture lock held, idle; page-aligned internally */
void buffers_free(void);
void buffers_trim(void);
void buffers_read_done(uint32_t row, uint32_t rows); /* lock held, successful user copy */
int capture_request(uint32_t flags);

/* capture.c */
void capture_compute_crop(uint32_t fb_w, uint32_t fb_h, uint32_t *x, uint32_t *y, uint32_t *w,
                          uint32_t *h);
int capture_copy(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h);
uint32_t region_checksum_hook(uintptr_t base, uint32_t pitch, uint32_t fmt, uint32_t w, uint32_t h);

/* input.c */
int input_hooks_install(void);
void input_hooks_release(void);
void input_poll(void);

/* lifecycle.c */
int lifecycle_hooks_install(void);
void lifecycle_hooks_release(void);
int lifecycle_set_game_active(SceUID pid, int mode);

/* display.c */
int display_hook_install(void);
void display_hook_release(void);
void display_static_fb_sample(void); /* game context, from its pad calls */

#define VJO_LOCK() ksceKernelLockMutex(g.lock, 1, NULL)
#define VJO_UNLOCK() ksceKernelUnlockMutex(g.lock, 1)

#endif
