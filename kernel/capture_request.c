/* Shared capture request path: OCR/calibration and optional Anki screenshots. */
#include "vjo_kernel.h"

int capture_request(uint32_t flags)
{
    int ret;
    uint32_t fw, fh, x, y, w, h;
    VJO_LOCK();
    if (flags == VJO_CAPTURE_DISCARD) {
        /* A copying display hook still owns its buffer. JPEG discard is used
         * only after completion; never free a pending or multi-pass image. */
        if (g.capture_state != CAPTURE_IDLE) ret = VJO_ERR_BUSY;
        else if (!g.capture_once) ret = VJO_ERR_ARG;
        else {
            g.raw_valid = 0;
            g.capture_once = g.capture_full = g.capture_release = 0;
            buffers_free();
            ret = 0;
        }
        goto out;
    }
    if (flags & ~(VJO_CAPTURE_FULL | VJO_CAPTURE_ONCE)) { ret = VJO_ERR_ARG; goto out; }
    if (g.game_pid <= 0 || !g.game_active) ret = VJO_ERR_NO_GAME;
    else if (g.capture_state != CAPTURE_IDLE) ret = VJO_ERR_BUSY;
    else {
        /* Snapshot pixel geometry before allocation. The display hook checks
         * it again and never grows this buffer or reads a stale framebuffer. */
        fw = g.fb_w ? g.fb_w : VJO_MAX_W;
        fh = g.fb_h ? g.fb_h : VJO_MAX_H;
        if (fw > VJO_MAX_W) fw = VJO_MAX_W;
        if (fh > VJO_MAX_H) fh = VJO_MAX_H;
        if (flags & VJO_CAPTURE_FULL) { x = y = 0; w = fw; h = fh; }
        else capture_compute_crop(fw, fh, &x, &y, &w, &h);
        if (buffers_alloc(w * h * 4) < 0) {
            ret = VJO_ERR_NO_MEMORY;
            goto out;
        }
        g.capture_x = x;
        g.capture_y = y;
        g.capture_fb_w = fw;
        g.capture_fb_h = fh;
        g.crop_w = w;
        g.crop_h = h;
        g.capture_release = 0;
        g.capture_seq = (g.capture_seq + 1) & 0x7FFFFFFF;
        if (!g.capture_seq) g.capture_seq = 1;
        ret = (int)g.capture_seq;
        g.raw_valid = 0;
        g.capture_full = (flags & VJO_CAPTURE_FULL) != 0;
        g.capture_once = (flags & (VJO_CAPTURE_ONCE | VJO_CAPTURE_FULL)) != 0;
        g.capture_requested_us = ksceKernelGetSystemTimeWide();
        __atomic_store_n(&g.capture_state, CAPTURE_PENDING, __ATOMIC_RELEASE);
    }
out:
    VJO_UNLOCK();
    return ret;
}
