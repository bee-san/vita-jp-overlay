/* Result arenas reuse the reserved Paf heap rather than requesting another
 * physical memblock from SceShell's limited USER partition. */
#include <paf/memory/heap_allocator.h>
#include <paf/std/stdlib.h>
#include <psp2/kernel/sysmem.h>
extern "C" {
#include "shell.h"
}
static constexpr size_t RESERVE = 2u * 1024u * 1024u;
extern "C" void *vjo_paf_alloc(size_t bytes) {
    auto *heap = paf::memory::GetGlobalHeapAllocator();
    if (!heap) return nullptr;
    size_t free = heap->GetFreeSize();
    if (free < RESERVE || bytes > free - RESERVE) return nullptr;
    return sce_paf_memalign(64, bytes);
}
extern "C" void vjo_paf_free(void *pointer) { if (pointer) sce_paf_free(pointer); }
extern "C" void vjo_paf_memory_report() {
    auto *heap = paf::memory::GetGlobalHeapAllocator();
    SceKernelFreeMemorySizeInfo info = { sizeof(info) };
    int rc = sceKernelGetFreeMemorySize(&info);
    vjo_log("memory: Paf free=%u KiB; USER partition free=%d KiB (rc=%d)",
        heap ? (unsigned)(heap->GetFreeSize() >> 10) : 0, info.size_user >> 10, rc);
}
