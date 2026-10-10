/* Borrow reserved Shell pages while leaving room for the LiveArea and UI. */
#include <paf/memory/heap_allocator.h>
#include <paf/std/stdlib.h>
#include <psp2/kernel/clib.h>
#include "../include/vjo_import.h"
extern "C" {
#include "shell.h"
}

static const size_t RESERVE = 1536u * 1024u;

template <typename Function> static bool ready(Function fn)
{
    if (!fn) return false;
#ifdef __arm__
    return VJO_IMPORT_READY(fn) != 0;
#else
    return true;
#endif
}

extern "C" void *vjo_shell_heap_alloc_for(const char *what, size_t bytes)
{
    if (!ready(::GetGlobalHeapAllocator) ||
        !ready(sce_paf_memalign) || !ready(sce_paf_free))
        return nullptr;
    auto *heap = static_cast<paf::memory::HeapAllocator *>(::GetGlobalHeapAllocator());
    if (!heap) return nullptr;
    uintptr_t vtable = 0;
    sceClibMemcpy(&vtable, heap, sizeof(vtable));
    if (!vtable || !reinterpret_cast<const volatile uintptr_t *>(vtable)[4])
        return nullptr;
    const size_t available = heap->GetFreeSize();
    if (available < RESERVE || bytes > available - RESERVE) {
        vjo_log("%s Paf reserve guard: free=%u requested=%u reserve=%u",
                what, (unsigned)available, (unsigned)bytes, (unsigned)RESERVE);
        return nullptr;
    }
    void *p = sce_paf_memalign(16, bytes);
    vjo_log("%s Paf allocation: free=%u requested=%u reserve=%u success=%d",
            what, (unsigned)available, (unsigned)bytes, (unsigned)RESERVE, p != nullptr);
    return p;
}

extern "C" void *vjo_shell_heap_alloc(size_t bytes)
{
    return vjo_shell_heap_alloc_for("Lens", bytes);
}

extern "C" void vjo_shell_heap_free(void *p)
{
    if (p) sce_paf_free(p);
}
