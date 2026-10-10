/* Production kernel code; SDK substitutes model PID mappings and failures. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/text.c"
#include "../../kernel/buffers.c"
#include "../../kernel/capture.c"
#include "../../kernel/capture_request.c"

VjoKernelState g;
unsigned syscall_depth;
static uint8_t memory[8192];
static const uint32_t base_address = 0x81000000u;
static SceUID caller, allocation;
static unsigned allocation_type, maps, releases, pinned, probes;
static int bad_window, copy_failure, mutex_busy, info_failure;
static unsigned capture_allocations, capture_frees, capture_bytes;
static int allocation_failure, capture_base_failure, capture_base_null, capture_copy_failure;
static unsigned capture_copies;
static uint32_t capture_storage[VJO_MAX_W * VJO_MAX_H + 8];
static uint32_t frame_pixels[VJO_MAX_W * VJO_MAX_H];
void klog(const char *fmt, ...) {}
/* This suite exercises the pre-existing non-OCR capture path. */
int game_ocr_capture_busy_locked(void) { return 0; }

SceUID ksceKernelGetProcessId(void) { return caller; }
uint64_t ksceKernelGetSystemTimeWide(void) { return 1000000; }
SceUID ksceKernelCreateMutex(const char *n, unsigned int a, int c, void *o) { return 1; }
int ksceKernelDeleteMutex(SceUID u) { return 0; }
int ksceKernelLockMutex(SceUID u, int c, void *t) { return 0; }
int ksceKernelTryLockMutex(SceUID u, int c) { return mutex_busy ? -1 : 0; }
int ksceKernelUnlockMutex(SceUID u, int c) { return 0; }
SceUID ksceKernelFindProcMemBlockByAddr(SceUID pid, const void *p, SceSize bytes)
{
    uintptr_t address = (uintptr_t)p;
    probes++;
    if (pid != g.game_pid || address < base_address || address >= base_address+sizeof(memory) ||
        bytes > base_address+sizeof(memory)-address) return -1;
    return allocation;
}
int ksceKernelGetMemBlockBase(SceUID uid, void **base) {
    if (uid == 22) {
        *base = capture_base_null ? NULL : (void *)(capture_storage + 4);
        return capture_base_failure ? -1 : 0;
    }
    *base = (void *)(uintptr_t)base_address; return 0;
}
int ksceKernelGetMemBlockAllocMapSize(SceUID uid, SceSize *size) { *size = sizeof(memory); return 0; }
int ksceKernelMemBlockGetInfoEx(SceUID uid, SceKernelMemBlockInfoEx *info) {
    TEST_CHECK(sizeof(*info) == 0xB8 && info->size == 0xB8);
    TEST_CHECK(offsetof(SceKernelMemBlockInfoEx, core_info.type) == 4);
    const uint8_t *bytes = (const uint8_t *)info;
    for (unsigned i = sizeof(info->size); i < sizeof(*info); i++) TEST_CHECK(bytes[i] == 0);
    if (info_failure) return -1;
    info->core_info.type = allocation_type;
    return 0;
}
SceUID ksceKernelProcUserMap(SceUID pid, const char *n, int permission, const void *p,
                            SceSize bytes, void **page, SceSize *size, uint32_t *offset)
{
    uintptr_t address = (uintptr_t)p;
    TEST_CHECK(permission == 1 && syscall_depth <= 1);
    if (copy_failure || pid != g.game_pid || address < base_address ||
        address >= base_address+sizeof(memory) || bytes > base_address+sizeof(memory)-address) return -1;
    maps++; pinned++;
    *page = memory; *size = bad_window ? 0 : sizeof(memory);
    *offset = (uint32_t)(address-base_address);
    return 111;
}
int ksceKernelMemBlockRelease(SceUID uid) { TEST_CHECK(uid == 111 && pinned == 1); pinned--; releases++; return 0; }
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) {
    capture_copies++;
    if (capture_copy_failure) return -1;
    memcpy(dst, src, n); return 0;
}
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }
int ksceKernelStrncpyUserToKernel(char *dst, const char *src, SceSize n) {
    size_t len = strnlen(src, n); memcpy(dst, src, len);
    if (len < n) memset(dst+len, 0, n-len);
    return 0;
}
SceUID ksceKernelAllocMemBlock(const char *n, unsigned int type, SceSize bytes, void *o) {
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
    TEST_CHECK(bytes && bytes <= VJO_MAX_W * VJO_MAX_H * 4 && !(bytes & 4095));
    memset(capture_storage, 0xA5, sizeof(capture_storage));
    capture_allocations++; capture_bytes = bytes; return allocation_failure ? -1 : 22;
}
int ksceKernelFreeMemBlock(SceUID uid) { TEST_CHECK(uid == 22); capture_frees++; return 0; }

static void setup(void)
{
    memset(&g, 0, sizeof(g)); memset(memory, 0, sizeof(memory));
    g.game_pid = 7; g.game_active = VJO_GAME; g.shell_pid = 3; g.text_epoch = 1;
    caller = 3; allocation = 11; allocation_type = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW;
    maps = releases = pinned = probes = syscall_depth = 0;
    bad_window = copy_failure = mutex_busy = info_failure = 0;
    capture_allocations = capture_frees = capture_bytes = 0; allocation_failure = 0;
    capture_base_failure = capture_base_null = capture_copy_failure = 0;
    capture_copies = 0;
    memset(capture_storage, 0xA5, sizeof(capture_storage));
    process = process_epoch = 0; session = 1;
    TEST_ASSERT(text_init() == 0);
    VjoTextSnapshot out;
    TEST_ASSERT(vjoTextRead(&out) == 0 && out.pid == 7);
    TEST_ASSERT(vjoTextControl(out.session, VJO_TEXT_LISTEN, 0) == 0);
}

static void permissions(void)
{
    setup(); VjoTextSnapshot out;
    caller = 8;
    TEST_CHECK(vjoTextRead(&out) == VJO_ERR_PERM);
    TEST_CHECK(vjoTextControl(session, VJO_TEXT_DISCOVER, 0) == VJO_ERR_PERM);
    VjoTextEvent e = {sizeof(e), VJO_TEXT_CALL, VJO_TEXT_UTF8, 10, base_address, 0, 0, 0};
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_PERM && maps == 0);
    caller = 7; g.game_active = VJO_GAME_STATIC_FB;
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_PERM);
    TEST_CHECK(syscall_depth == 0);
}

static void submit_mapping(void)
{
    setup(); caller = 7;
    const char *line = "猫と犬がいる";
    memcpy(memory+100, line, strlen(line)+1);
    VjoTextEvent e = {sizeof(e), VJO_TEXT_CALL, VJO_TEXT_UTF8, 10, base_address+100, 0, 0, 0};
    TEST_CHECK(vjoTextSubmit(&e) == 0 && maps == releases && pinned == 0);
    TEST_CHECK(sources.items[0].id && !strcmp(sources.items[0].text, line));
    unsigned seq = sources.sequence, previous_maps = maps;
    mutex_busy = 1;
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_BUSY && maps == previous_maps && sources.sequence == seq);
    mutex_busy = 0; bad_window = 1;
    TEST_CHECK(vjoTextSubmit(&e) < 0 && maps == releases && pinned == 0);
    bad_window = 0; copy_failure = 1;
    TEST_CHECK(vjoTextSubmit(&e) < 0 && maps == releases && pinned == 0);
    copy_failure = 0; e.indirections = 5;
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_ARG);
    TEST_CHECK(syscall_depth == 0);
}

static void auto_terminated(void)
{
    setup(); caller = 7;
    /* CP932 NUL-terminated output at the allocation end: strcpy provides no
     * byte count, so the kernel must discover the terminator within bounds. */
    const uint8_t line[] = {0x94,0x4c,0x82,0xc6,0x8c,0xa2,0x82,0xaa,0x82,0xa2,0x82,0xe9,0};
    unsigned offset = sizeof(memory) - sizeof(line);
    memcpy(memory + offset, line, sizeof(line));
    VjoTextEvent e = {sizeof(e), VJO_TEXT_CALL, VJO_TEXT_AUTO, 10,
                     base_address + offset, 0, 0, 0};
    TEST_CHECK(vjoTextSubmit(&e) == 0 && maps == releases && pinned == 0);
    TEST_ASSERT(sources.items[0].id);
    TEST_CHECK(!strcmp(sources.items[0].text, "猫と犬がいる"));
    unsigned sequence = sources.sequence;
    memset(memory + offset, 'a', sizeof(line));
    TEST_CHECK(vjoTextSubmit(&e) < 0 && sources.sequence == sequence);
    memory[sizeof(memory)-1] = 0;
    TEST_CHECK(vjoTextSubmit(&e) == 0 && sources.sequence == sequence); /* ASCII ignored */
    unsigned previous_maps = maps;
    e.kind = VJO_TEXT_REGISTER;
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_ARG && maps == previous_maps);
    TEST_CHECK(maps == releases && pinned == 0 && syscall_depth == 0);
}

static void indirection(void)
{
    setup(); caller = 7;
    uint32_t ptr = base_address+120;
    memcpy(memory+4, &ptr, 4);
    const uint8_t wide[] = {0x2B,0x73,0x68,0x30,0xAC,0x72,0x4C,0x30,0x44,0x30,0x8B,0x30,0,0};
    memcpy(memory+100, wide, sizeof(wide));
    VjoTextEvent e = {sizeof(e), VJO_TEXT_REGISTER, VJO_TEXT_UTF16LE, 10, base_address+4, 0, 1, -20};
    TEST_CHECK(vjoTextSubmit(&e) == 0 && !strcmp(sources.items[0].text, "猫と犬がいる"));
    TEST_CHECK(maps == 2 && releases == 2 && pinned == 0);
    ptr = 0x40000000u; memcpy(memory+4, &ptr, 4);
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_ARG && pinned == 0);
}

static void submit_capacity(void)
{
    setup();
    char expected[VJO_TEXT_BYTES];
    for (unsigned i = 0; i < VJO_TEXT_CODEPOINTS; i++) {
        memory[2*i] = 0x2B; memory[2*i+1] = 0x73; /* UTF-16LE 猫 */
        memcpy(expected+3*i, "猫", 3);
    }
    expected[3*VJO_TEXT_CODEPOINTS] = 0;
    TEST_ASSERT(vjoTextReference(session, expected) == 0);
    caller = 7;
    VjoTextEvent e = {sizeof(e), VJO_TEXT_CALL, VJO_TEXT_UTF16LE, 10,
                     base_address, VJO_TEXT_BYTES, 0, 0};
    TEST_ASSERT(vjoTextSubmit(&e) == 0);
    VjoTextCandidate *c = &sources.items[0];
    TEST_CHECK(c->id && c->length == VJO_TEXT_CODEPOINTS && c->score == 100);
    TEST_CHECK(!strcmp(c->text, expected) && maps == releases && pinned == 0);
    uint32_t id = c->id, sequence = sources.sequence;
    unsigned previous_maps = maps;
    e.bytes++;
    TEST_CHECK(vjoTextSubmit(&e) == VJO_ERR_ARG && maps == previous_maps);
    TEST_CHECK(sources.sequence == sequence);

    /* A subsequent scan reuses the same scratch; submitted text must persist. */
    memset(memory, 0, sizeof(memory));
    scan_address = base_address;
    previous_maps = maps;
    text_tick();
    c = vjo_text_find(&sources, id);
    TEST_CHECK(c && !strcmp(c->text, expected) && c->score == 100);
    TEST_CHECK(maps > previous_maps);
    TEST_CHECK(syscall_depth == 0 && maps == releases && pinned == 0);
}

static void source_lifetime(void)
{
    setup();
    const char *line = "今日は新しい台詞を読む";
    memcpy(memory+4090, line, strlen(line)+1);
    TEST_CHECK(vjoTextReference(session, line) == 0);
    scan_address = base_address;
    scan_step();
    VjoTextSnapshot out;
    vjoTextRead(&out);
    TEST_ASSERT(out.count && out.candidates[0].score == 100);
    uint32_t id = out.candidates[0].id;
    TEST_CHECK(vjoTextControl(session, VJO_TEXT_FOLLOW, id) == 0 && !scanning);
    memory[4090] = 0;
    poll_memory(); vjoTextRead(&out);
    TEST_CHECK(out.selected == id && out.current.text[0] == 0 && out.count == 0);
    TEST_CHECK(vjo_text_auto_select(out.candidates, out.count) == 0);
    memcpy(memory+4090, line, strlen(line)+1); poll_memory();
    TEST_CHECK(vjo_text_find(&sources, id) && !strcmp(vjo_text_find(&sources, id)->text, line));
    allocation++; poll_memory();
    TEST_CHECK(!sources.selected && !vjo_text_find(&sources, id) && maps == releases);
}

static void session_epoch(void)
{
    setup(); uint32_t previous_session = session;
    vjo_text_offer(&sources, VJO_TEXT_CALL, VJO_TEXT_UTF8, 1, 0, "日本語の台詞を読む");
    /* A suspend/resume completes between worker ticks, with the same PID. */
    g.text_epoch += 2;
    VjoTextSnapshot out; vjoTextRead(&out);
    TEST_CHECK(out.session != previous_session && out.count == 0 && mode == VJO_TEXT_OFF);
    TEST_CHECK(vjoTextControl(previous_session, VJO_TEXT_DISCOVER, 0) == VJO_ERR_NO_GAME);
    TEST_CHECK(vjoTextReference(previous_session, "日本語の台詞を読む") == VJO_ERR_NO_GAME);
    g.game_active = 0; vjoTextRead(&out);
    TEST_CHECK(out.pid == 0 && vjoTextReference(out.session, "日本語の台詞を読む") == VJO_ERR_NO_GAME);
    TEST_CHECK(syscall_depth == 0);
}

static void scan_budget(void)
{
    setup(); probes = 0;
    scan_address = SCAN_BEGIN; scanning = 1;
    scan_step();
    TEST_CHECK(probes == 64 && scan_address == SCAN_BEGIN + 64*SCAN_CHUNK && maps == 0);
    scan_address = base_address; allocation_type = 99;
    scan_step();
    TEST_CHECK(scan_address >= base_address+sizeof(memory) && maps == 0);
    allocation_type = SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW;
    mode = VJO_TEXT_LISTEN; scan_address = base_address;
    text_tick();
    TEST_CHECK(scan_address == base_address && maps == 0);
}

static void scan_memory_type(void)
{
    const unsigned allowed[] = {SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW,
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW};
    for (unsigned i = 0; i < sizeof(allowed)/sizeof(allowed[0]); i++) {
        setup();
        allocation_type = allowed[i]; scan_address = base_address;
        scan_step();
        TEST_CHECK(maps > 0 && maps == releases && pinned == 0);
    }
    setup();
    info_failure = 1; scan_address = base_address;
    scan_step();
    TEST_CHECK(maps == 0 && scan_address >= base_address+sizeof(memory));
    /* Failure must not leave a stale accepted type from the preceding scan. */
    info_failure = 0; allocation_type = 99; scan_address = base_address;
    scan_step();
    TEST_CHECK(maps == 0 && scan_address >= base_address+sizeof(memory));
}

static void lazy_capture(void)
{
    setup();
    TEST_CHECK(capture_allocations == 0 && !g.raw && !text_uses_frame_checks());
    TEST_CHECK(capture_request(VJO_CAPTURE_ONCE) == 1 && capture_allocations == 1 && capture_bytes == RAW_SIZE);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY && capture_allocations == 1);
    g.capture_release = 1; buffers_trim();
    TEST_CHECK(capture_frees == 0 && g.raw != NULL); /* a pending copy owns it */
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1; buffers_trim();
    TEST_CHECK(capture_frees == 0 && g.raw != NULL); /* a region reader owns it too */
    buffers_read_done(0, 256); buffers_trim();
    TEST_CHECK(capture_frees == 0 && g.raw_valid);
    buffers_read_done(256, 288);
    TEST_CHECK(capture_frees == 1 && !g.raw); /* release before Lens upload, without a worker tick */
    buffers_trim();
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_valid && g.alloc_status == VJO_ALLOC_NONE);
    /* Anki can allocate again; a new request cancels a queued trim. */
    g.capture_release = 1;
    TEST_CHECK(capture_request(VJO_CAPTURE_FULL) == 2 && capture_allocations == 2 && g.capture_full);
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1; buffers_trim();
    TEST_CHECK(capture_frees == 1 && g.raw != NULL && g.raw_valid);
    g.capture_release = 1; g.crop_h = 544; buffers_trim();
    TEST_CHECK(capture_frees == 1 && g.raw != NULL); /* native switch during Anki read */
    buffers_read_done(0, 256); buffers_trim();
    TEST_CHECK(capture_frees == 1 && g.raw != NULL);
    buffers_read_done(256, 288); buffers_trim();
    TEST_CHECK(capture_frees == 2 && !g.raw && !g.raw_valid);
    buffers_free(); allocation_failure = 1;
    TEST_CHECK(capture_request(0) == VJO_ERR_NO_MEMORY && g.capture_state == CAPTURE_IDLE);
    TEST_CHECK(vjoTextControl(session, VJO_TEXT_OFF, 0) == 0 && text_uses_frame_checks());
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
    TEST_ASSERT(g.raw && g.raw_valid && !capture_frees && !g.capture_release);
    buffers_trim();
    TEST_CHECK(g.raw && g.raw_valid && !memcmp(g.raw, frame_pixels, 32 * 32 * 4));
    buffers_read_done(8, 24);
    TEST_CHECK(g.raw && g.raw_valid && !capture_frees);
    /* Completion can explicitly switch back to native text and queue a trim. */
    TEST_ASSERT(vjoTextControl(session, VJO_TEXT_LISTEN, 0) == 0);
    buffers_trim();
    TEST_CHECK(capture_frees == 1 && !g.raw && !g.raw_valid && !g.raw_capacity);
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
    TEST_CHECK(!g.capture_once && !g.capture_full && !g.capture_release && g.capture_seq == 1);
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
    {"text syscall permissions", permissions}, {"mapped copy and nonblocking submit", submit_mapping},
    {"bounded NUL-terminated automatic encoding", auto_terminated},
    {"register pointer and UTF16", indirection}, {"submit capacity and scratch reuse", submit_capacity},
    {"blank/freed source invalidation", source_lifetime},
    {"same-PID session invalidation", session_epoch}, {"bounded discovery/listen", scan_budget},
    {"stable driver memory type and failures", scan_memory_type},
    {"lazy capture and safe native release", lazy_capture},
    {"region capture allocation and resizing", capture_region_allocation},
    {"multi-pass local OCR capture lifetime", capture_multi_pass},
    {"capture allocation and base failures", capture_allocation_failures},
    {"failed JPEG discard and ownership protection", capture_discard},
    {"capture geometry bounds and frozen region", capture_geometry_bounds},
    {"capture source pixel conversion", capture_format_conversion}, {NULL, NULL}
};
