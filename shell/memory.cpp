/* Paf's free-byte count is a policy input, not a largest-allocation guarantee.
 * Keep result allocation, reserve refusals and allocator failures distinct. */
#include <paf/memory/heap_allocator.h>
#include <paf/std/stdlib.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include "../include/vjo_import.h"
extern "C" {
#include "shell.h"
}

#ifdef VJO_MEIKI_GAME_WORKER
/* Shell only holds UI and dictionary results in this build; Meiki's model and
 * inference buffers belong to the game worker. Keep the same UI reserve as
 * the Lens build, without reducing either result arena. */
static constexpr size_t RESERVE = 1536u * 1024u;
#else
static constexpr size_t RESERVE = 2u * 1024u * 1024u;
#endif

template <typename Function> static bool import_ready(Function function)
{
    if (!function) return false;
#ifdef __arm__
    return VJO_IMPORT_READY(function) != 0;
#else
    return true;
#endif
}

#ifdef VJO_MEMORY_DIAGNOSTICS
/* C aliases expose imported ARM stubs to the readiness check. The target links
 * ScePaf_stub_weak. Direct calls supply `this` in r0 and bypass virtual dispatch. */
extern "C" size_t paf_free_size_direct(paf::memory::HeapAllocator *)
    __asm__("_ZN3paf6memory13HeapAllocator11GetFreeSizeEv");
extern "C" void paf_query_info(paf::memory::HeapAllocator *, paf::memory::HeapInfo *)
    __asm__("_ZN3paf6memory13HeapAllocator9QueryInfoEPNS0_8HeapInfoE");
extern "C" void paf_heap_range(const paf::memory::HeapAllocator *, void **, size_t *)
    __asm__("_ZNK3paf6memory13HeapAllocator8GetRangeEPPvPj");
#ifdef __arm__
static_assert(sizeof(paf::memory::HeapInfo) == 8, "Paf query ABI must be two 32-bit words");
#endif
#endif

struct PafStats {
    paf::memory::HeapAllocator *heap;
    uintptr_t vtable, free_target;
    size_t virtual_free, direct_free, query_free, total;
    void *base;
    int free_count;
    bool global_ready, direct_ready, query_ready, range_ready;
};

static PafStats paf_stats()
{
    PafStats stats = {};
    stats.free_count = -1;
    stats.global_ready = import_ready(GetGlobalHeapAllocator);
    if (!stats.global_ready) return stats;
    stats.heap = paf::memory::GetGlobalHeapAllocator();
    if (!stats.heap) return stats;
    sceClibMemcpy(&stats.vtable, stats.heap, sizeof(stats.vtable));
    if (!stats.vtable) return stats;
    const auto *vtable = reinterpret_cast<const volatile uintptr_t *>(stats.vtable);
    stats.free_target = vtable[4];
    if (!stats.free_target) return stats;
    stats.virtual_free = stats.heap->GetFreeSize();
#ifdef VJO_MEMORY_DIAGNOSTICS
    stats.direct_ready = import_ready(paf_free_size_direct);
    if (stats.direct_ready) stats.direct_free = paf_free_size_direct(stats.heap);
    stats.query_ready = import_ready(paf_query_info);
    if (stats.query_ready) {
        paf::memory::HeapInfo info = {};
        info.num_of_free = -1;
        paf_query_info(stats.heap, &info);
        stats.free_count = info.num_of_free;
        stats.query_free = info.total_free_size;
    }
    stats.range_ready = import_ready(paf_heap_range);
    if (stats.range_ready) paf_heap_range(stats.heap, &stats.base, &stats.total);
#endif
    return stats;
}

static void log_paf(const char *phase, const PafStats &stats)
{
    vjo_log("Paf %s: global_ready=%d heap=%08X vtable=%08X free_target=%08X",
            phase, stats.global_ready, (unsigned)(uintptr_t)stats.heap,
            (unsigned)stats.vtable, (unsigned)stats.free_target);
    vjo_log("Paf %s free: virtual=%u direct_ready=%d direct=%u query_ready=%d "
            "query=%u free_count=%d bytes", phase, (unsigned)stats.virtual_free,
            stats.direct_ready, (unsigned)stats.direct_free, stats.query_ready,
            (unsigned)stats.query_free, stats.free_count);
    vjo_log("Paf %s range: ready=%d base=%08X total=%u reserve=%u bytes; "
            "free totals do not prove contiguous space", phase,
            stats.range_ready, (unsigned)(uintptr_t)stats.base, (unsigned)stats.total,
            (unsigned)RESERVE);
}

extern "C" void *vjo_paf_alloc(size_t bytes)
{
    PafStats stats = paf_stats();
    log_paf("allocation", stats);
    if (!stats.global_ready || !stats.heap || !stats.vtable || !stats.free_target) {
        vjo_log("Paf alloc: requested=%u align=64 reason=%s; allocator not called",
                (unsigned)bytes, !stats.global_ready ? "global_import_unbound" :
                !stats.heap ? "null_global_heap" :
                !stats.vtable ? "null_heap_vtable" : "null_free_method");
        return nullptr;
    }
    if (stats.virtual_free < RESERVE || bytes > stats.virtual_free - RESERVE) {
        vjo_log("Paf alloc: requested=%u align=64 free=%u reserve=%u "
                "reason=reserve_guard; allocator not called",
                (unsigned)bytes, (unsigned)stats.virtual_free, (unsigned)RESERVE);
        return nullptr;
    }
    if (!import_ready(sce_paf_memalign)) {
        vjo_log("Paf alloc: requested=%u align=64 reason=memalign_import_unbound", (unsigned)bytes);
        return nullptr;
    }
    if (!import_ready(sce_paf_free)) {
        vjo_log("Paf alloc: requested=%u align=64 reason=free_import_unbound; allocator not called",
                (unsigned)bytes);
        return nullptr;
    }
    void *pointer = sce_paf_memalign(64, bytes);
    vjo_log("Paf alloc: requested=%u align=64 pointer=%08X reason=%s",
            (unsigned)bytes, (unsigned)(uintptr_t)pointer, pointer ? "allocated" : "memalign_null");
    return pointer;
}

extern "C" void vjo_paf_free(void *pointer) { if (pointer) sce_paf_free(pointer); }

#ifdef VJO_MEMORY_DIAGNOSTICS
extern "C" void vjo_paf_probe_once(void)
{
    static bool attempted;
    if (attempted) return;
    attempted = true;
    constexpr size_t PROBE = 128u * 1024u;
    constexpr size_t OVERHEAD_MARGIN = 64u * 1024u;
    PafStats before = paf_stats();
    log_paf("probe_before", before);
    size_t minimum = before.virtual_free;
    if (before.direct_free < minimum) minimum = before.direct_free;
    if (before.query_free < minimum) minimum = before.query_free;
    bool metadata_valid = before.heap && before.vtable && before.free_target &&
        before.direct_ready && before.query_ready &&
        before.free_count >= 0 && before.range_ready && before.base &&
        before.total >= before.virtual_free && before.total >= before.direct_free &&
        before.total >= before.query_free;
    if (!metadata_valid || !import_ready(sce_paf_memalign) || !import_ready(sce_paf_free) ||
        minimum < RESERVE + PROBE + OVERHEAD_MARGIN) {
        vjo_log("Paf probe: requested=%u attempted=0 metadata_valid=%d minimum_free=%u "
                "required_with_reserve_and_margin=%u; no allocation",
                (unsigned)PROBE, metadata_valid, (unsigned)minimum,
                (unsigned)(RESERVE + PROBE + OVERHEAD_MARGIN));
        return;
    }
    void *pointer = sce_paf_memalign(64, PROBE);
    PafStats during = paf_stats();
    if (pointer) sce_paf_free(pointer);
    PafStats after = paf_stats();
    uintptr_t address = (uintptr_t)pointer, base = (uintptr_t)before.base;
    bool in_range = pointer && address >= base && address - base <= before.total &&
                    PROBE <= before.total - (address - base);
    vjo_log("Paf probe: requested=%u align=64 pointer=%08X allocated=%d freed=%d "
            "in_reported_range=%d; no large Paf allocation attempted",
            (unsigned)PROBE, (unsigned)address, pointer != nullptr, pointer != nullptr, in_range);
    vjo_log("Paf probe virtual free: before=%u during=%u after=%u bytes",
            (unsigned)before.virtual_free, (unsigned)during.virtual_free,
            (unsigned)after.virtual_free);
    vjo_log("Paf probe direct free: before=%u during=%u after=%u bytes",
            (unsigned)before.direct_free, (unsigned)during.direct_free,
            (unsigned)after.direct_free);
    vjo_log("Paf probe query free: before=%u during=%u after=%u bytes",
            (unsigned)before.query_free, (unsigned)during.query_free,
            (unsigned)after.query_free);
}
#endif

extern "C" void vjo_paf_memory_report()
{
    log_paf("report", paf_stats());
    if (!import_ready(sceKernelGetFreeMemorySize)) {
        vjo_log("memory raw: Sysmem free-size import unavailable; counters unknown");
        return;
    }
    SceKernelFreeMemorySizeInfo info = { sizeof(info), -1, -1, -1 };
    int rc = sceKernelGetFreeMemorySize(&info);
    vjo_log("memory raw: rc=%d info_size=%d USER=%d (0x%08X) CDRAM=%d (0x%08X) "
            "PHYCONT=%d (0x%08X) bytes; negative values are not free budgets",
            rc, info.size, info.size_user, (unsigned)info.size_user,
            info.size_cdram, (unsigned)info.size_cdram, info.size_phycont, (unsigned)info.size_phycont);
}
