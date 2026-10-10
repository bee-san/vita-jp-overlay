#include "memory_probe.h"

#define MIB (1024u * 1024u)
#define RW_ACCESS 6u
#define NORMAL_CACHED 0xD0u
#define NORMAL_UNCACHED 0x80u

static void report(const VjoMemoryProbeBackend *b, const VjoMemoryProbeState *s,
                   unsigned stage, int rc, const VjoMemoryProbeInfo *info,
                   const VjoMemoryProbeFree *free)
{
    VjoMemoryProbeEvent event = {0};
    event.stage = stage;
    event.name = s->held_name ? s->held_name : "all";
    event.type = s->held_type;
    event.bytes = s->held_bytes;
    event.uid = s->held_uid;
    event.rc = rc;
    event.info.base = s->held_base;
    if (info) event.info = *info;
    if (free) event.free = *free;
    if (b->report) b->report(b->ud, &event);
}

static int snapshot(const VjoMemoryProbeBackend *b, const VjoMemoryProbeState *s,
                    VjoMemoryProbeFree *free)
{
    *free = (VjoMemoryProbeFree){0};
    int rc = b->snapshot(b->ud, free);
    report(b, s, VJO_MEMORY_PROBE_SNAPSHOT, rc, NULL, free);
    return rc;
}

static int uid_invalid(int rc)
{
    return (uint32_t)rc == 0x80024501u || /* SCE_KERNEL_ERROR_INVALID_UID */
           (uint32_t)rc == 0x80028001u;   /* SCE_KERNEL_ERROR_UNKNOWN_UID */
}

int vjo_memory_probe_release_with(const VjoMemoryProbeBackend *b,
                                  VjoMemoryProbeState *s)
{
    if (!b || !s || !b->free || !b->base) return VJO_MEMORY_PROBE_INVALID;
    if (s->held_uid < 0) return VJO_MEMORY_PROBE_OK;
    if (!s->free_accepted) {
        if (s->free_attempted) {
            void *base = NULL;
            int rc = b->base(b->ud, s->held_uid, &base);
            report(b, s, VJO_MEMORY_PROBE_UID, rc, NULL, NULL);
            if (uid_invalid(rc)) {
                s->held_uid = -1;
                s->held_base = NULL;
                s->free_attempted = 0;
                return VJO_MEMORY_PROBE_OK;
            }
            if (rc || !s->held_base || base != s->held_base) {
                report(b, s, VJO_MEMORY_PROBE_RETAINED, rc, NULL, NULL);
                return VJO_MEMORY_PROBE_CLEANUP;
            }
        }
        s->free_attempted = 1;
        int rc = b->free(b->ud, s->held_uid);
        report(b, s, VJO_MEMORY_PROBE_FREE, rc, NULL, NULL);
        if (rc) {
            report(b, s, VJO_MEMORY_PROBE_RETAINED, rc, NULL, NULL);
            return VJO_MEMORY_PROBE_CLEANUP;
        }
        s->free_accepted = 1;
    }
    void *base = NULL;
    int rc = b->base(b->ud, s->held_uid, &base);
    report(b, s, VJO_MEMORY_PROBE_UID, rc, NULL, NULL);
    if (!uid_invalid(rc)) {
        /* A successful free followed by an ambiguous lookup is not permission
         * to free that UID again: another thread may have reused its number. */
        report(b, s, VJO_MEMORY_PROBE_RETAINED, rc, NULL, NULL);
        return VJO_MEMORY_PROBE_CLEANUP;
    }
    s->held_uid = -1;
    s->held_base = NULL;
    s->free_attempted = 0;
    s->free_accepted = 0;
    return VJO_MEMORY_PROBE_OK;
}

static int mapped_range_valid(const VjoMemoryProbeState *s,
                              const VjoMemoryProbeInfo *info)
{
    uintptr_t base = (uintptr_t)s->held_base, mapped = (uintptr_t)info->base;
    uint32_t expected_memory = s->held_type == VJO_MEMORY_PROBE_CDRAM
                                ? NORMAL_UNCACHED : NORMAL_CACHED;
    return base && !(base & 63u) && mapped <= base &&
           base - mapped <= info->bytes &&
           s->held_bytes <= info->bytes - (base - mapped) &&
           s->held_bytes <= UINTPTR_MAX - base &&
           info->type == s->held_type && info->memory_type == expected_memory &&
           (info->access & RW_ACCESS) == RW_ACCESS;
}

static int canaries(VjoMemoryProbeState *s)
{
    volatile unsigned char *first = s->held_base;
    volatile unsigned char *last = first + s->held_bytes - 64u;
    /* Volatile scalar accesses prevent compiler SIMD/string expansion. The
     * diagnostic proves these 128 bytes only; it is not an MNN/SIMD test. */
    for (unsigned i = 0; i < 64; ++i) {
        first[i] = (unsigned char)(0x35u ^ i);
        last[i] = (unsigned char)(0xCAu ^ i);
    }
    __sync_synchronize();
    for (unsigned i = 0; i < 64; ++i)
        if (first[i] != (unsigned char)(0x35u ^ i) ||
            last[i] != (unsigned char)(0xCAu ^ i)) return VJO_MEMORY_PROBE_CANARY;
    return VJO_MEMORY_PROBE_OK;
}

static int allocation(const VjoMemoryProbeBackend *b, VjoMemoryProbeState *s,
                      const char *name, uint32_t type, uint32_t bytes)
{
    VjoMemoryProbeInfo info = {0};
    VjoMemoryProbeFree free;
    s->held_name = name;
    s->held_type = type;
    s->held_bytes = bytes;
    s->held_base = NULL;
    s->free_attempted = 0;
    s->free_accepted = 0;
    s->held_uid = b->alloc(b->ud, name, type, bytes);
    report(b, s, VJO_MEMORY_PROBE_ALLOC, s->held_uid < 0 ? s->held_uid : 0, NULL, NULL);
    if (s->held_uid < 0) return VJO_MEMORY_PROBE_UNAVAILABLE;
    int rc = b->base(b->ud, s->held_uid, &s->held_base);
    report(b, s, VJO_MEMORY_PROBE_BASE, rc, NULL, NULL);
    if (rc || !s->held_base) {
        rc = VJO_MEMORY_PROBE_INVALID;
        report(b, s, VJO_MEMORY_PROBE_SKIP, rc, NULL, NULL);
    }
    else {
        rc = b->info(b->ud, s->held_base, bytes, &info);
        report(b, s, VJO_MEMORY_PROBE_INFO, rc, &info, NULL);
        if (rc || !mapped_range_valid(s, &info)) {
            rc = VJO_MEMORY_PROBE_INVALID;
            report(b, s, VJO_MEMORY_PROBE_SKIP, rc, &info, NULL);
        }
        else {
            rc = canaries(s);
            report(b, s, VJO_MEMORY_PROBE_TOUCH, rc, &info, NULL);
        }
    }
    int cleanup = vjo_memory_probe_release_with(b, s);
    snapshot(b, s, &free);
    return cleanup ? cleanup : rc;
}

int vjo_memory_probe_once_with(const VjoMemoryProbeBackend *b,
                               VjoMemoryProbeState *s)
{
    if (!b || !s || !b->snapshot || !b->alloc || !b->base || !b->info || !b->free)
        return VJO_MEMORY_PROBE_INVALID;
    if (s->held_uid >= 0) {
        report(b, s, VJO_MEMORY_PROBE_RETAINED, VJO_MEMORY_PROBE_CLEANUP, NULL, NULL);
        return VJO_MEMORY_PROBE_CLEANUP;
    }
    if (s->started) return VJO_MEMORY_PROBE_OK;
    s->started = 1;
    VjoMemoryProbeFree free;
    snapshot(b, s, &free);
    int rc = allocation(b, s, "VjoProbeUser", VJO_MEMORY_PROBE_USER, 4096u);
    if (rc == VJO_MEMORY_PROBE_CLEANUP || rc == VJO_MEMORY_PROBE_CANARY) return rc;
    static const struct {
        const char *name;
        uint32_t type, quantum;
    } pools[] = {
        {"VjoProbePhy", VJO_MEMORY_PROBE_PHYCONT, MIB},
        {"VjoProbeCDR", VJO_MEMORY_PROBE_CDRAM, MIB / 4u}
    };
    for (unsigned i = 0; i < sizeof(pools) / sizeof(*pools); ++i) {
        rc = allocation(b, s, pools[i].name, pools[i].type, pools[i].quantum);
        if (rc == VJO_MEMORY_PROBE_CLEANUP || rc == VJO_MEMORY_PROBE_CANARY) return rc;
        if (rc) continue; /* Never grow a pool whose small test failed. */
        uint32_t bytes = (VJO_MEMORY_PROBE_ENGINE_BYTES + pools[i].quantum - 1u)
                          & ~(pools[i].quantum - 1u);
        s->held_bytes = bytes;
        int free_rc = snapshot(b, s, &free);
        int32_t available = pools[i].type == VJO_MEMORY_PROBE_PHYCONT ? free.phycont : free.cdram;
        if (free_rc || available <= 0 ||
            (uint32_t)available < bytes + VJO_MEMORY_PROBE_HEADROOM) {
            /* Negative/unknown counters are retained as raw values, not totals.
             * They do not veto small probes, but cannot justify a large one. */
            report(b, s, VJO_MEMORY_PROBE_SKIP, free_rc ? free_rc : VJO_MEMORY_PROBE_UNAVAILABLE,
                   NULL, &free);
            continue;
        }
        rc = allocation(b, s, pools[i].name, pools[i].type, bytes);
        if (rc == VJO_MEMORY_PROBE_CLEANUP || rc == VJO_MEMORY_PROBE_CANARY) return rc;
    }
    report(b, s, VJO_MEMORY_PROBE_COMPLETE, 0, NULL, NULL);
    return VJO_MEMORY_PROBE_OK;
}

#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#include "../include/vjo_import.h"
extern void vjo_log(const char *, ...);

static VjoMemoryProbeState native_state = {.held_uid = -1};
static int native_busy;
static int native_snapshot(void *ud, VjoMemoryProbeFree *free)
{
    (void)ud;
    SceKernelFreeMemorySizeInfo info = {sizeof(info), 0, 0, 0};
    int rc = sceKernelGetFreeMemorySize(&info);
    *free = (VjoMemoryProbeFree){info.size_user, info.size_cdram, info.size_phycont};
    return rc;
}
static int32_t native_alloc(void *ud, const char *name, uint32_t type, uint32_t bytes)
{
    (void)ud;
    return sceKernelAllocMemBlock(name, type, bytes, NULL);
}
static int native_base(void *ud, int32_t uid, void **base)
{
    (void)ud;
    return sceKernelGetMemBlockBase(uid, base);
}
static int native_info(void *ud, void *base, uint32_t bytes, VjoMemoryProbeInfo *out)
{
    (void)ud;
    SceKernelMemBlockInfo info = {0};
    info.size = sizeof(info);
    int rc = sceKernelGetMemBlockInfoByRange(base, bytes, &info);
    *out = (VjoMemoryProbeInfo){info.mappedBase, info.mappedSize, (uint32_t)info.memoryType,
                               info.access, info.type};
    return rc;
}
static int native_free(void *ud, int32_t uid)
{
    (void)ud;
    return sceKernelFreeMemBlock(uid);
}
static void native_report(void *ud, const VjoMemoryProbeEvent *e)
{
    (void)ud;
    static const char *stages[] = {"free-snapshot", "alloc", "base", "info", "canary",
                                  "free", "uid-after-free", "skip", "complete", "retained"};
    if (e->stage == VJO_MEMORY_PROBE_SNAPSHOT) {
        vjo_log("memprobe free-snapshot %s rc=0x%08X USER=%d/0x%08X CDRAM=%d/0x%08X PHYCONT=%d/0x%08X",
            e->name, (unsigned)e->rc, e->free.user, (unsigned)e->free.user,
            e->free.cdram, (unsigned)e->free.cdram, e->free.phycont, (unsigned)e->free.phycont);
        return;
    }
    vjo_log("memprobe %s %s type=0x%08X bytes=%u uid=0x%08X rc=0x%08X base=%p mapped=%u memory=0x%X access=0x%X actualtype=0x%08X",
        stages[e->stage], e->name, e->type, e->bytes, (unsigned)e->uid, (unsigned)e->rc,
        e->info.base, e->info.bytes, e->info.memory_type, e->info.access, e->info.type);
    if (e->stage == VJO_MEMORY_PROBE_ALLOC && e->bytes >= VJO_MEMORY_PROBE_ENGINE_BYTES)
        vjo_log("memprobe payload=%u rounded=%u; result buffers/module images/stacks excluded; no inference/SIMD test",
                VJO_MEMORY_PROBE_ENGINE_BYTES, e->bytes);
}
static const VjoMemoryProbeBackend native_backend = {
    NULL, native_snapshot, native_alloc, native_base, native_info, native_free, native_report
};
static int native_ready(void)
{
    return VJO_IMPORT_READY(sceKernelGetFreeMemorySize) && VJO_IMPORT_READY(sceKernelAllocMemBlock) &&
           VJO_IMPORT_READY(sceKernelGetMemBlockBase) && VJO_IMPORT_READY(sceKernelGetMemBlockInfoByRange) &&
           VJO_IMPORT_READY(sceKernelFreeMemBlock);
}
static int native_call(int release)
{
    if (__atomic_exchange_n(&native_busy, 1, __ATOMIC_ACQUIRE)) return VJO_MEMORY_PROBE_BUSY;
    int rc;
    if (release && native_state.held_uid < 0) rc = VJO_MEMORY_PROBE_OK;
    else if (!native_ready()) {
        vjo_log("memprobe SDK import unavailable; no allocations attempted");
        rc = VJO_MEMORY_PROBE_UNAVAILABLE;
    } else rc = release ? vjo_memory_probe_release_with(&native_backend, &native_state)
                        : vjo_memory_probe_once_with(&native_backend, &native_state);
    __atomic_store_n(&native_busy, 0, __ATOMIC_RELEASE);
    return rc;
}
int vjo_memory_probe_once(void) { return native_call(0); }
int vjo_memory_probe_release(void) { return native_call(1); }
#endif
