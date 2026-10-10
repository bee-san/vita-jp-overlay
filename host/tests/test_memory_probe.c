#define _POSIX_C_SOURCE 200112L
#include "acutest.h"
#include "../../shell/memory_probe.h"
#include <stdlib.h>
#include <string.h>

/* Real canary/ownership policy; only SDK allocation/query boundaries are fake. */
static struct {
    VjoMemoryProbeFree free;
    unsigned allocations, frees, touches, skips, snapshots, max_live;
    unsigned fail_alloc_at, fail_base_at, invalid_info_at;
    unsigned info_mode, free_failures, ambiguous_post_free;
    int snapshot_error, live, reused_uid, changed_live_base;
    int32_t uid, last_freed;
    uint32_t bytes, type, peak_bytes, requests[8], types[8];
    unsigned char *backing, *base;
    int touched;
} fake;
static VjoMemoryProbeState state;

static void reset(void)
{
    TEST_ASSERT(!fake.backing);
    memset(&fake, 0, sizeof(fake));
    fake.free = (VjoMemoryProbeFree){-1048576, 64 * 1024 * 1024, 32 * 1024 * 1024};
    fake.last_freed = -1;
    state = (VjoMemoryProbeState){.held_uid = -1};
}
static int snapshot(void *ud, VjoMemoryProbeFree *out)
{
    (void)ud;
    *out = fake.free;
    return fake.snapshot_error;
}
static int32_t alloc(void *ud, const char *name, uint32_t type, uint32_t bytes)
{
    (void)ud;
    TEST_CHECK(!fake.live && !fake.backing);
    TEST_CHECK(name && strlen(name) < 32);
    TEST_ASSERT(fake.allocations < 8);
    unsigned slot = fake.allocations++;
    fake.requests[slot] = bytes;
    fake.types[slot] = type;
    if (fake.allocations == fake.fail_alloc_at) return -77;
    void *base = NULL;
    TEST_ASSERT(posix_memalign(&base, 64, bytes + 128u) == 0);
    fake.backing = base;
    fake.base = fake.backing + 64;
    fake.bytes = bytes;
    fake.type = type;
    fake.uid = (int32_t)(100 + fake.allocations);
    fake.live = 1;
    fake.touched = 0;
    if (bytes > fake.peak_bytes) fake.peak_bytes = bytes;
    if ((unsigned)fake.live > fake.max_live) fake.max_live = (unsigned)fake.live;
    memset(fake.backing, 0xA5, 64);
    memset(fake.base, 0xCC, bytes);
    memset(fake.base + bytes, 0xA5, 64);
    return fake.uid;
}
static int base(void *ud, int32_t uid, void **out)
{
    (void)ud;
    if (fake.live && uid == fake.uid) {
        if (fake.allocations == fake.fail_base_at) return -88;
        *out = fake.changed_live_base ? fake.base + 64 : fake.base;
        return 0;
    }
    if (uid == fake.last_freed && fake.reused_uid) {
        *out = (void *)(uintptr_t)0x10000; /* A different object's mapping: never dereferenced. */
        return 0;
    }
    if (uid == fake.last_freed && fake.ambiguous_post_free) {
        fake.ambiguous_post_free--;
        return -91; /* A negative lookup alone does not prove UID invalidity. */
    }
    return (int32_t)0x80024501u;
}
static int info(void *ud, void *base, uint32_t bytes, VjoMemoryProbeInfo *out)
{
    (void)ud;
    TEST_CHECK(fake.live && base == fake.base && bytes == fake.bytes);
    *out = (VjoMemoryProbeInfo){base, bytes,
              fake.type == VJO_MEMORY_PROBE_CDRAM ? 0x80u : 0xD0u, 6, fake.type};
    if (fake.allocations == fake.invalid_info_at) {
        switch (fake.info_mode) {
        case 0: out->access = 4; break;
        case 1: out->bytes -= 64; break;
        case 2: out->type ^= 0x1000; break;
        case 3: out->memory_type = 8; break; /* Device, not normal memory. */
        case 4: out->base = (void *)((uintptr_t)base + 64); break;
        }
    }
    return 0;
}
static void check_owned_bytes(void)
{
    for (unsigned i = 0; i < 64; ++i) {
        TEST_CHECK(fake.backing[i] == 0xA5 && fake.base[fake.bytes + i] == 0xA5);
        TEST_CHECK(fake.base[i] == (fake.touched ? (unsigned char)(0x35u ^ i) : 0xCC));
        TEST_CHECK(fake.base[fake.bytes - 64u + i] ==
                   (fake.touched ? (unsigned char)(0xCAu ^ i) : 0xCC));
    }
    for (uint32_t i = 64; i < fake.bytes - 64; ++i)
        if (fake.base[i] != 0xCC) { TEST_CHECK(0); break; }
}
static int release(void *ud, int32_t uid)
{
    (void)ud;
    fake.frees++;
    TEST_ASSERT(fake.live && uid == fake.uid && fake.backing);
    check_owned_bytes();
    if (fake.free_failures) { fake.free_failures--; return -89; }
    free(fake.backing);
    fake.last_freed = uid;
    fake.backing = fake.base = NULL;
    fake.live = 0;
    return 0;
}
static void event(void *ud, const VjoMemoryProbeEvent *e)
{
    (void)ud;
    if (e->stage == VJO_MEMORY_PROBE_TOUCH) {
        TEST_CHECK(fake.live && e->rc == 0);
        fake.touched = 1;
        fake.touches++;
    }
    if (e->stage == VJO_MEMORY_PROBE_SKIP) fake.skips++;
    if (e->stage == VJO_MEMORY_PROBE_SNAPSHOT) {
        fake.snapshots++;
        TEST_CHECK(e->free.user == fake.free.user); /* Preserve negative raw counter. */
    }
}
static const VjoMemoryProbeBackend backend = {NULL, snapshot, alloc, base, info, release, event};
static void test_success_rounding_and_canary_bounds(void)
{
    reset();
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0);
    TEST_CHECK(fake.allocations == 5 && fake.frees == 5 && fake.touches == 5);
    TEST_CHECK(fake.max_live == 1 && !fake.live && state.held_uid == -1);
    TEST_CHECK(fake.requests[0] == 4096 && fake.requests[1] == 1048576);
    TEST_CHECK(fake.requests[2] == 22020096 && fake.requests[3] == 262144);
    TEST_CHECK(fake.requests[4] == 21495808);
    TEST_CHECK(VJO_MEMORY_PROBE_ENGINE_BYTES == 21353600);
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0 && fake.allocations == 5);
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == 0 && fake.frees == 5);
}
static void test_negative_unknown_and_headroom_boundaries(void)
{
    for (unsigned i = 0; i < 4; ++i) {
        reset();
        if (i == 0) fake.free.phycont = fake.free.cdram = -1;
        if (i == 1) fake.free.phycont = fake.free.cdram = 0;
        if (i == 2) {
            fake.free.phycont = 22020096 + VJO_MEMORY_PROBE_HEADROOM - 1;
            fake.free.cdram = 21495808 + VJO_MEMORY_PROBE_HEADROOM - 1;
        }
        if (i == 3) fake.snapshot_error = -1;
        TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0);
        TEST_CHECK(fake.allocations == 3 && fake.frees == 3 && fake.skips == 2);
    }
    reset();
    fake.free.phycont = 22020096 + VJO_MEMORY_PROBE_HEADROOM;
    fake.free.cdram = 21495808 + VJO_MEMORY_PROBE_HEADROOM;
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0 && fake.allocations == 5);
}
static void test_allocation_failures_skip_only_affected_pool(void)
{
    for (unsigned fail = 1; fail <= 5; ++fail) {
        reset(); fake.fail_alloc_at = fail;
        TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0);
        TEST_CHECK(!fake.live && state.held_uid < 0);
        TEST_CHECK(fake.frees + 1 == fake.allocations);
        if (fail == 2) TEST_CHECK(fake.types[2] == VJO_MEMORY_PROBE_CDRAM);
        if (fail == 4) TEST_CHECK(fake.allocations == 4); /* Failed small CDRAM never grows. */
    }
}
static void test_invalid_mapping_is_not_touched(void)
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        reset(); fake.invalid_info_at = 2; fake.info_mode = mode;
        TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0);
        TEST_CHECK(fake.allocations == 4 && fake.frees == 4 && fake.touches == 3);
        TEST_CHECK(fake.types[2] == VJO_MEMORY_PROBE_CDRAM);
    }
    reset(); fake.fail_base_at = 1;
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0);
    TEST_CHECK(fake.allocations == 5 && fake.frees == 5 && fake.touches == 4);
}
static void test_failed_free_stops_and_retains_owner_for_retry(void)
{
    reset(); fake.free_failures = 1;
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    TEST_CHECK(fake.allocations == 1 && fake.frees == 1 && fake.live);
    TEST_CHECK(state.held_uid == fake.uid && state.held_base == fake.base);
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    TEST_CHECK(fake.allocations == 1 && fake.frees == 1);
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == 0);
    TEST_CHECK(fake.frees == 2 && !fake.live && state.held_uid == -1);
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == 0 && fake.allocations == 1);
}
static void test_ambiguous_post_free_never_frees_twice(void)
{
    reset(); fake.ambiguous_post_free = 1;
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    TEST_CHECK(fake.allocations == 1 && fake.frees == 1 && !fake.live);
    TEST_CHECK(state.held_uid == fake.last_freed && state.free_accepted);
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == 0);
    TEST_CHECK(fake.frees == 1 && state.held_uid == -1);
}
static void test_reused_or_changed_uid_is_not_freed(void)
{
    reset(); fake.reused_uid = 1;
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    TEST_CHECK(fake.allocations == 1 && fake.frees == 1 && !fake.live);
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    TEST_CHECK(fake.frees == 1 && state.free_accepted);
    fake.reused_uid = 0;
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == 0);
    TEST_CHECK(fake.frees == 1);

    reset(); fake.free_failures = 1;
    TEST_CHECK(vjo_memory_probe_once_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    fake.changed_live_base = 1;
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == VJO_MEMORY_PROBE_CLEANUP);
    TEST_CHECK(fake.frees == 1 && state.held_base == fake.base);
    fake.changed_live_base = 0;
    TEST_CHECK(vjo_memory_probe_release_with(&backend, &state) == 0);
    TEST_CHECK(fake.frees == 2 && !fake.live);
}
TEST_LIST = {
    {"success_rounding_and_canary_bounds", test_success_rounding_and_canary_bounds},
    {"negative_unknown_and_headroom_boundaries", test_negative_unknown_and_headroom_boundaries},
    {"allocation_failures_skip_only_affected_pool", test_allocation_failures_skip_only_affected_pool},
    {"invalid_mapping_is_not_touched", test_invalid_mapping_is_not_touched},
    {"failed_free_stops_and_retains_owner_for_retry", test_failed_free_stops_and_retains_owner_for_retry},
    {"ambiguous_post_free_never_frees_twice", test_ambiguous_post_free_never_frees_twice},
    {"reused_or_changed_uid_is_not_freed", test_reused_or_changed_uid_is_not_freed},
    {NULL, NULL}
};
