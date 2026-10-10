/* Production region capture code with failure-injecting SDK boundaries. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "../../kernel/capture_request.c"

VjoKernelState g;
unsigned syscall_depth;
static unsigned capture_allocations, capture_frees, capture_bytes, capture_copies;
static int allocation_failure, capture_base_failure, capture_base_null, capture_copy_failure;
static uint32_t capture_storage[VJO_MAX_W * VJO_MAX_H + 8];
static uint32_t frame_pixels[VJO_MAX_W * VJO_MAX_H];
void klog(const char *fmt, ...) {}
uint64_t ksceKernelGetSystemTimeWide(void) { return 1000000; }
int ksceKernelLockMutex(SceUID u, int c, void *t) { return 0; }
int ksceKernelUnlockMutex(SceUID u, int c) { return 0; }
int ksceKernelGetMemBlockBase(SceUID uid, void **base) {
    TEST_ASSERT(uid == 22);
    *base = capture_base_null ? NULL : (void *)(capture_storage + 4);
    return capture_base_failure ? -1 : 0;
}
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) {
    capture_copies++;
    if (capture_copy_failure) return -1;
    memcpy(dst, src, n); return 0;
}
SceUID ksceKernelAllocMemBlock(const char *n, unsigned int type, SceSize bytes, void *o) {
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    TEST_CHECK(bytes && bytes <= VJO_MAX_W * VJO_MAX_H * 4 && !(bytes & 4095));
    memset(capture_storage, 0xA5, sizeof(capture_storage));
    capture_allocations++; capture_bytes = bytes; return allocation_failure ? -1 : 22;
}
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 22); capture_frees++; return 0; }
static void setup(void) {
    memset(&g, 0, sizeof(g));
    g.game_pid = 7; g.game_active = VJO_GAME; g.shell_pid = 3;
    capture_allocations = capture_frees = capture_bytes = capture_copies = 0;
    allocation_failure = capture_base_failure = capture_base_null = capture_copy_failure = 0;
    memset(capture_storage, 0xA5, sizeof(capture_storage));
}
static void capture_lazy_single_pass(void) {
    setup();
    TEST_CHECK(!capture_allocations && !g.raw);
    TEST_ASSERT(capture_request(VJO_CAPTURE_ONCE) == 1 && capture_bytes == RAW_SIZE);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY && capture_allocations == 1);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    buffers_read_done(0, 256);
    TEST_CHECK(!capture_frees && g.raw && g.raw_valid);
    buffers_read_done(256, 288);
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_valid && g.alloc_status == VJO_ALLOC_NONE);
    TEST_ASSERT(capture_request(VJO_CAPTURE_FULL) == 2 && g.capture_full);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    buffers_read_done(0, 544);
    TEST_CHECK(capture_frees == 2 && !g.raw && !g.raw_valid);
    allocation_failure = 1;
    TEST_CHECK(capture_request(0) == VJO_ERR_NO_MEMORY && g.capture_state == CAPTURE_IDLE);
}
static void capture_guards(void)
{
    for (unsigned i = 0; i < 4; i++) {
        TEST_CHECK(capture_storage[i] == 0xA5A5A5A5u);
        TEST_CHECK(capture_storage[4 + capture_bytes / 4 + i] == 0xA5A5A5A5u);
    }
}

static void capture_region_allocation(void)
{
    setup();
    g.fb_w = 960; g.fb_h = 544;
    g.region = (VjoRect){6000, 45000, 51500, 17500};
    TEST_ASSERT(capture_request(0) == 1);
    TEST_CHECK(g.crop_w == 755 && g.crop_h == 146);
    TEST_CHECK(capture_bytes == 442368 && g.raw_capacity == capture_bytes);
    TEST_CHECK(capture_bytes < RAW_SIZE / 4);
    unsigned count = capture_allocations;
    TEST_CHECK(buffers_alloc(RAW_SIZE) < 0 && capture_allocations == count && !capture_frees);
    g.capture_state = CAPTURE_IDLE;
    TEST_ASSERT(capture_request(VJO_CAPTURE_FULL) == 2);
    TEST_CHECK(g.crop_w == 960 && g.crop_h == 544 && g.capture_x == 0 && g.capture_y == 0);
    TEST_CHECK(capture_allocations == 2 && capture_frees == 1 && g.raw_capacity == RAW_SIZE);
    g.capture_state = CAPTURE_IDLE;
    TEST_ASSERT(capture_request(0) == 3);
    TEST_CHECK(capture_allocations == 3 && capture_frees == 2 && g.raw_capacity == 442368);
    g.capture_state = CAPTURE_IDLE;
    TEST_ASSERT(capture_request(0) == 4);
    TEST_CHECK(capture_allocations == 3 && capture_frees == 2); /* exact-size reuse */
    capture_guards();
}

static void capture_multi_pass(void)
{
    setup(); g.fb_w = g.fb_h = 32;
    for (unsigned i = 0; i < 32 * 32; i++) frame_pixels[i] = 0xFF000000u | i;
    TEST_ASSERT(capture_request(0) == 1 && !g.capture_once);
    TEST_ASSERT(capture_copy((uintptr_t)frame_pixels, 32, FMT_A8B8G8R8, 32, 32) == 0);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    /* Local OCR finds lines by reading the entire image, then rereads each
     * detected line. The first final-row read must not invalidate that image. */
    buffers_read_done(0, 32);
    TEST_ASSERT(g.raw && g.raw_valid && !capture_frees);
    TEST_CHECK(g.raw && g.raw_valid && !memcmp(g.raw, frame_pixels, 32 * 32 * 4));
    buffers_read_done(8, 24);
    TEST_CHECK(g.raw && g.raw_valid && !capture_frees);
    buffers_free();
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_capacity);
}

static void capture_allocation_failures(void)
{
    for (int failure = 0; failure < 3; failure++) {
        setup();
        allocation_failure = failure == 0;
        capture_base_failure = failure == 1;
        capture_base_null = failure == 2;
        TEST_CHECK(capture_request(0) == VJO_ERR_NO_MEMORY);
        TEST_CHECK(g.capture_state == CAPTURE_IDLE && !g.mem_uid && !g.raw && !g.raw_capacity);
        TEST_CHECK(g.alloc_status == VJO_ALLOC_FAIL && !g.raw_valid && !g.capture_seq);
        TEST_CHECK(capture_allocations == 1 && capture_frees == (unsigned)(failure != 0));
        allocation_failure = capture_base_failure = capture_base_null = 0;
        TEST_CHECK(capture_request(0) == 1 && g.raw_capacity == RAW_SIZE);
    }
    setup();
    TEST_CHECK(buffers_alloc(0) < 0 && buffers_alloc(RAW_SIZE + 1) < 0 && !capture_allocations);
    TEST_CHECK(buffers_alloc(1) == 0 && capture_bytes == 4096);
    g.raw_valid = 1;
    allocation_failure = 1;
    TEST_CHECK(buffers_alloc(8192) < 0);
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_capacity && !g.raw_valid);
    TEST_CHECK(g.alloc_status == VJO_ALLOC_FAIL);
}

static void capture_discard(void)
{
    setup(); g.fb_w = g.fb_h = 32;
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_ARG && !capture_allocations);
    TEST_ASSERT(capture_request(VJO_CAPTURE_ONCE) == 1);
    uint8_t *raw = g.raw;
    for (int state = CAPTURE_PENDING; state <= CAPTURE_COPIED; state++) {
        g.capture_state = state;
        TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_BUSY);
        TEST_CHECK(g.raw == raw && !capture_frees && g.capture_state == state);
    }
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1;
    const uint32_t invalid[] = {VJO_CAPTURE_DISCARD | VJO_CAPTURE_ONCE,
        VJO_CAPTURE_DISCARD | VJO_CAPTURE_FULL, 8, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        TEST_CHECK(capture_request(invalid[i]) == VJO_ERR_ARG);
        TEST_CHECK(g.raw == raw && g.raw_valid && !capture_frees && capture_allocations == 1);
    }
    /* A failed JPEG may have read some bands but never its final one. */
    buffers_read_done(0, 16);
    TEST_ASSERT(g.raw_valid && !capture_frees);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == 0);
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_valid && !g.raw_capacity);
    TEST_CHECK(!g.capture_once && !g.capture_full && g.capture_seq == 1);
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_ARG && capture_frees == 1);
    TEST_ASSERT(capture_request(VJO_CAPTURE_FULL) == 2);
    g.capture_state = CAPTURE_IDLE; /* a completed copy failure has no valid image */
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == 0 && capture_frees == 2);

    TEST_ASSERT(capture_request(0) == 3); /* local OCR remains owned across passes */
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1; raw = g.raw;
    TEST_CHECK(capture_request(VJO_CAPTURE_DISCARD) == VJO_ERR_ARG);
    TEST_CHECK(g.raw == raw && g.raw_valid && g.raw_capacity == 4096 && capture_frees == 2);
    buffers_read_done(0, 32);
    TEST_CHECK(g.raw == raw && g.raw_valid && capture_frees == 2);
}

static void capture_geometry_bounds(void)
{
    setup(); g.fb_w = g.fb_h = 64;
    g.region = (VjoRect){16384, 32768, 32768, 32768};
    TEST_ASSERT(capture_request(0) == 1);
    TEST_CHECK(g.crop_w == 32 && g.crop_h == 32 && capture_bytes == 4096);
    TEST_CHECK(g.capture_x == 16 && g.capture_y == 32);
    uint32_t capacity = g.raw_capacity;
    uintptr_t base = (uintptr_t)frame_pixels;
    capture_copies = 0;
    TEST_CHECK(capture_copy(base, 64, FMT_A8B8G8R8, 63, 64) == VJO_ERR_ARG);
    TEST_CHECK(capture_copy(base, 64, FMT_A8B8G8R8, 64, 63) == VJO_ERR_ARG);
    TEST_CHECK(capture_copy(base, 63, FMT_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.raw_capacity = capacity - 1;
    TEST_CHECK(capture_copy(base, 64, FMT_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.raw_capacity = capacity;
    g.capture_x = 64;
    TEST_CHECK(capture_copy(base, 64, FMT_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.capture_x = 16;
    g.capture_y = 64;
    TEST_CHECK(capture_copy(base, 64, FMT_A8B8G8R8, 64, 64) == VJO_ERR_ARG);
    g.capture_y = 32;
    TEST_CHECK(capture_copy(base, 64, 12345, 64, 64) == VJO_ERR_FORMAT);
    TEST_CHECK(!capture_copies);
    capture_guards();
    for (unsigned i = 0; i < 64 * 64; i++) frame_pixels[i] = 0xFF000000u | i;
    g.region = (VjoRect){0}; /* a later region change must not grow this request */
    TEST_ASSERT(capture_copy(base, 64, FMT_A8B8G8R8, 64, 64) == 0);
    TEST_CHECK(g.raw_stride == 128 && capture_copies == 32);
    for (unsigned row = 0; row < 32; row++)
        TEST_CHECK(!memcmp(g.raw + row * 128, frame_pixels + (row + 32) * 64 + 16, 128));
    capture_guards();
    capture_copy_failure = 1; capture_copies = 0;
    TEST_CHECK(capture_copy(base, 64, FMT_A8B8G8R8, 64, 64) == VJO_ERR_COPY && capture_copies == 1);
    capture_guards();
}

static void capture_format_conversion(void)
{
    for (int ten_bit = 0; ten_bit < 2; ten_bit++) {
        setup(); g.fb_w = g.fb_h = 32;
        TEST_ASSERT(capture_request(0) == 1);
        if (ten_bit) {
            for (unsigned i = 0; i < 32 * 32; i++)
                frame_pixels[i] = (17u << 2) | (31u << 12) | (63u << 22);
        } else {
            const uint16_t pixel = 0x801F;
            for (unsigned i = 0; i < 32 * 32; i++)
                memcpy((uint8_t *)frame_pixels + i * 2, &pixel, sizeof(pixel));
        }
        TEST_ASSERT(capture_copy((uintptr_t)frame_pixels, 32,
            ten_bit ? FMT_A2B10G10R10 : FMT_BGRA5551, 32, 32) == 0);
        for (unsigned i = 0; i < 32 * 32; i++)
            TEST_CHECK(((uint32_t *)g.raw)[i] == (ten_bit ? 0xFF3F1F11u : 0xFF0000F8u));
        capture_guards();
    }
}

TEST_LIST = {
    {"lazy JPEG capture and final-row release", capture_lazy_single_pass},
    {"region capture allocation and resizing", capture_region_allocation},
    {"multi-pass local OCR capture lifetime", capture_multi_pass},
    {"capture allocation and base failures", capture_allocation_failures},
    {"failed JPEG discard and ownership protection", capture_discard},
    {"capture geometry bounds and frozen region", capture_geometry_bounds},
    {"capture source pixel conversion", capture_format_conversion}, {NULL, NULL}
};
