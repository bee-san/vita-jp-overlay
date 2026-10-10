/* Shared capture request path: OCR/calibration and optional Anki screenshots. */
#include "vjo_kernel.h"

int capture_request(uint32_t flags)
{
    int ret;
    VJO_LOCK();
    if (g.game_pid <= 0 || !g.game_active) ret = VJO_ERR_NO_GAME;
    else if (g.capture_state != CAPTURE_IDLE) ret = VJO_ERR_BUSY;
    else if (buffers_alloc() < 0) ret = VJO_ERR_NO_MEMORY;
    else {
        g.capture_release = 0;
        g.capture_seq = (g.capture_seq + 1) & 0x7FFFFFFF;
        if (!g.capture_seq) g.capture_seq = 1;
        ret = (int)g.capture_seq;
        g.raw_valid = 0;
        g.capture_full = (flags & VJO_CAPTURE_FULL) != 0;
        g.capture_requested_us = ksceKernelGetSystemTimeWide();
        g.capture_state = CAPTURE_PENDING;
    }
    VJO_UNLOCK();
    return ret;
}
