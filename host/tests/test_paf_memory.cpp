/* Actual Paf allocation policy with fake heap/import/SDK boundaries. It does
 * not estimate physical fragmentation, Paf availability or retail stability. */
#if defined(__GNUC__) && !defined(__clang__)
/* Acutest's C++ exception/longjmp runner triggers GCC's clobbered warning. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wclobbered"
#endif
#include "acutest.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <limits>
#include <paf/memory/heap_allocator.h>
#include <paf/std/stdlib.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>

static uintptr_t unavailable_import;
static int test_import_ready(uintptr_t function) { return function != unavailable_import; }
/* Execute the production ARM readiness branch; replace only import decoding,
 * since host function bodies are not Vita ARM import stubs. */
#define VJO_IMPORT_H
#define VJO_IMPORT_READY(fn) test_import_ready(reinterpret_cast<uintptr_t>(fn))
#define VJO_SHELL_H
extern "C" void vjo_log(const char *format, ...);
#define __arm__ 1
#include "../../shell/memory.cpp"
#undef __arm__

static paf::memory::HeapAllocator heap;
static size_t free_bytes, allocated_bytes;
static unsigned global_calls, free_queries, allocation_calls, release_calls, sysmem_calls;
static bool null_heap, allocator_null;
static void *owned_pointer;
static char log_text[4096];

extern "C" void *GetGlobalHeapAllocator(void)
{
    TEST_CHECK(test_import_ready(reinterpret_cast<uintptr_t>(GetGlobalHeapAllocator)));
    global_calls++;
    return null_heap ? nullptr : &heap;
}
size_t paf::memory::HeapAllocator::GetFreeSize() { free_queries++; return free_bytes; }
extern "C" void *sce_paf_memalign(size_t alignment, size_t bytes)
{
    TEST_CHECK(test_import_ready(reinterpret_cast<uintptr_t>(sce_paf_memalign)));
    TEST_CHECK(test_import_ready(reinterpret_cast<uintptr_t>(sce_paf_free)));
    TEST_CHECK(alignment == 64 && !owned_pointer && bytes <= free_bytes);
    allocation_calls++;
    if (allocator_null) return nullptr;
    TEST_ASSERT(posix_memalign(&owned_pointer, alignment, bytes) == 0);
    allocated_bytes = bytes;
    free_bytes -= bytes;
    return owned_pointer;
}
extern "C" void sce_paf_free(void *pointer)
{
    TEST_CHECK(test_import_ready(reinterpret_cast<uintptr_t>(sce_paf_free)));
    TEST_CHECK(pointer && pointer == owned_pointer);
    release_calls++;
    std::free(pointer);
    owned_pointer = nullptr;
    free_bytes += allocated_bytes;
}
extern "C" int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info)
{
    TEST_CHECK(test_import_ready(reinterpret_cast<uintptr_t>(sceKernelGetFreeMemorySize)));
    TEST_CHECK(info->size == sizeof(*info));
    sysmem_calls++;
    info->size_user = -1048576;
    info->size_cdram = info->size_phycont = 0;
    return 0;
}
extern "C" void vjo_log(const char *format, ...)
{
    size_t used = std::strlen(log_text);
    va_list arguments;
    va_start(arguments,format);
    int n = std::vsnprintf(log_text+used,sizeof(log_text)-used,format,arguments);
    va_end(arguments);
    TEST_ASSERT(n >= 0 && static_cast<size_t>(n) < sizeof(log_text)-used);
    used += static_cast<size_t>(n);
    TEST_ASSERT(used+1 < sizeof(log_text));
    log_text[used] = '\n'; log_text[used+1] = 0;
}

/* Independent expectation: a wrong production build macro must fail rather
 * than silently moving these test boundaries with RESERVE. */
#ifdef VJO_MEIKI_GAME_WORKER
static constexpr size_t expected_reserve = 1536u*1024u;
#else
static constexpr size_t expected_reserve = 2u*1024u*1024u;
#endif
static constexpr size_t results_bytes = 2u*160u*1024u;
static void reset(size_t available)
{
    TEST_ASSERT(!owned_pointer);
    free_bytes = available;
    allocated_bytes = 0;
    global_calls = free_queries = allocation_calls = release_calls = sysmem_calls = 0;
    unavailable_import = 0; null_heap = allocator_null = false;
    log_text[0] = 0;
}
static void test_free_count_reserve_boundary(void)
{
    const size_t refused[] = {0,expected_reserve-1,expected_reserve,
                              expected_reserve+results_bytes-1};
    for (size_t available : refused) {
        reset(available);
        TEST_CHECK(vjo_paf_alloc(results_bytes) == nullptr);
        TEST_CHECK(!allocation_calls && !release_calls && !owned_pointer);
        TEST_CHECK(global_calls == 1 && free_queries == 1);
        TEST_CHECK(std::strstr(log_text,"reason=reserve_guard") != nullptr);
    }
    reset(expected_reserve+results_bytes);
    auto *p = static_cast<unsigned char *>(vjo_paf_alloc(results_bytes));
    TEST_ASSERT(p != nullptr);
    TEST_CHECK(reinterpret_cast<uintptr_t>(p)%64 == 0 && allocated_bytes == results_bytes);
    p[0] = 0x39; p[results_bytes-1] = 0x76;
    TEST_CHECK(p[0] == 0x39 && p[results_bytes-1] == 0x76);
    TEST_CHECK(allocation_calls == 1 && free_bytes == expected_reserve);
    TEST_CHECK(std::strstr(log_text,"reason=allocated") != nullptr);
    vjo_paf_free(p);
    TEST_CHECK(release_calls == 1 && !owned_pointer && free_bytes == expected_reserve+results_bytes);
    vjo_paf_free(nullptr);
    TEST_CHECK(release_calls == 1);
}
static void test_game_and_default_policy_differ(void)
{
    /* The same free count permits game result memory while preserving1.5MiB,
     * but refuses the old Shell engine's stricter2MiB reserve. */
    reset(2u*1024u*1024u);
    void *p = vjo_paf_alloc(results_bytes);
#ifdef VJO_MEIKI_GAME_WORKER
    TEST_ASSERT(p != nullptr);
    TEST_CHECK(allocation_calls == 1);
    vjo_paf_free(p);
#else
    TEST_CHECK(p == nullptr && !allocation_calls && !release_calls);
#endif
    TEST_CHECK(!owned_pointer);
}
static void test_allocator_null_is_distinct_from_reserve_refusal(void)
{
    reset(expected_reserve+results_bytes);
    allocator_null = true;
    TEST_CHECK(vjo_paf_alloc(results_bytes) == nullptr);
    TEST_CHECK(allocation_calls == 1 && !release_calls && !owned_pointer);
    TEST_CHECK(std::strstr(log_text,"reason=memalign_null") != nullptr);
    TEST_CHECK(std::strstr(log_text,"reason=reserve_guard") == nullptr);
}
static void test_unbound_imports_never_allocate(void)
{
    const uintptr_t imports[] = {reinterpret_cast<uintptr_t>(GetGlobalHeapAllocator),
                                  reinterpret_cast<uintptr_t>(sce_paf_memalign),
                                  reinterpret_cast<uintptr_t>(sce_paf_free)};
    for (uintptr_t import : imports) {
        reset(expected_reserve+results_bytes);
        unavailable_import = import;
        TEST_CHECK(vjo_paf_alloc(results_bytes) == nullptr);
        TEST_CHECK(!allocation_calls && !release_calls && !owned_pointer);
        TEST_CHECK(std::strstr(log_text,"unbound") != nullptr);
        if (import == reinterpret_cast<uintptr_t>(GetGlobalHeapAllocator))
            TEST_CHECK(!global_calls && !free_queries);
    }
}
static void test_null_heap_and_large_requests_refuse_safely(void)
{
    reset(expected_reserve+results_bytes); null_heap = true;
    TEST_CHECK(vjo_paf_alloc(results_bytes) == nullptr && !allocation_calls && !free_queries);
    TEST_CHECK(std::strstr(log_text,"reason=null_global_heap") != nullptr);
    reset(std::numeric_limits<size_t>::max());
    TEST_CHECK(vjo_paf_alloc(std::numeric_limits<size_t>::max()) == nullptr);
    TEST_CHECK(!allocation_calls && std::strstr(log_text,"reason=reserve_guard") != nullptr);
}
static void test_memory_report_never_allocates(void)
{
    reset(expected_reserve+results_bytes);
    vjo_paf_memory_report();
    TEST_CHECK(sysmem_calls == 1 && free_queries == 1 && !allocation_calls && !release_calls);
    TEST_CHECK(std::strstr(log_text,"USER=-1048576") != nullptr);
    TEST_CHECK(std::strstr(log_text,"negative values are not free budgets") != nullptr);
    reset(expected_reserve+results_bytes);
    unavailable_import = reinterpret_cast<uintptr_t>(sceKernelGetFreeMemorySize);
    vjo_paf_memory_report();
    TEST_CHECK(!sysmem_calls && !allocation_calls && !release_calls);
    TEST_CHECK(std::strstr(log_text,"counters unknown") != nullptr);
}
TEST_LIST = {
    {"free_count_reserve_boundary",test_free_count_reserve_boundary},
    {"game_and_default_policy_differ",test_game_and_default_policy_differ},
    {"allocator_null_is_distinct_from_reserve_refusal",test_allocator_null_is_distinct_from_reserve_refusal},
    {"unbound_imports_never_allocate",test_unbound_imports_never_allocate},
    {"null_heap_and_large_requests_refuse_safely",test_null_heap_and_large_requests_refuse_safely},
    {"memory_report_never_allocates",test_memory_report_never_allocates},
    {nullptr,nullptr}
};
