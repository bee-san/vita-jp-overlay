/* Capture buffer, allocated on request. Native text releases an idle buffer;
 * OCR and optional Anki screenshots can request it again. */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>

#include "vjo_kernel.h"

#define ALIGN(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

/* Raw A8B8G8R8 rows of the largest region. Plain kernel main memory: only
 * the CPU touches it (the display hook writes it, vjoReadRaw copies it out),
 * so it needs neither physical contiguity nor cache maintenance. */
#define RAW_SIZE ALIGN(VJO_MAX_W * VJO_MAX_H * 4, 0x1000)

int buffers_alloc(void)
{
    void *base = NULL;
    SceUID uid;

    if (g.mem_uid > 0)
        return 0;
    uid = ksceKernelAllocMemBlock("VjoCapture", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, RAW_SIZE, NULL);
    if (uid < 0) {
        klog("alloc capture buffer (%u KiB) failed 0x%08X", RAW_SIZE >> 10, uid);
        g.alloc_status = VJO_ALLOC_FAIL;
        return uid;
    }
    ksceKernelGetMemBlockBase(uid, &base);
    g.mem_uid = uid;
    g.raw = (uint8_t *)base;
    g.alloc_status = VJO_ALLOC_OK;
    klog("capture buffer: %u KiB at %p", RAW_SIZE >> 10, base);
    return 0;
}

void buffers_free(void)
{
    if (g.mem_uid > 0) {
        ksceKernelFreeMemBlock(g.mem_uid);
        klog("capture buffer freed");
    }
    g.mem_uid = 0;
    g.raw = NULL;
    g.alloc_status = VJO_ALLOC_NONE;
}

void buffers_trim(void)
{
    if (!g.capture_release) return;
    VJO_LOCK();
    if (g.capture_release && g.capture_state == CAPTURE_IDLE && !(g.capture_full && g.raw_valid)) {
        g.capture_release = 0;
        g.raw_valid = 0;
        buffers_free();
    }
    VJO_UNLOCK();
}

void buffers_read_done(uint32_t row, uint32_t rows)
{
    /* Native source changes must not free an Anki image before its reader
     * finishes. The final copied row ends that ownership. */
    if (g.capture_full && row < g.crop_h && rows >= g.crop_h-row) {
        g.capture_full = 0;
        g.capture_release = 1;
    }
}
