/* Production kernel code; SDK substitutes model PID mappings and failures. */
#include "acutest.h"
#include <psp2kern/host_stubs.h>
#include "../../kernel/vjo_kernel.h"
#include "../../kernel/text.c"
#include "../../kernel/buffers.c"
#include "../../kernel/capture_request.c"

VjoKernelState g;
unsigned syscall_depth;
static uint8_t memory[8192];
static const uint32_t base_address = 0x81000000u;
static SceUID caller, allocation;
static unsigned allocation_type, maps, releases, pinned, probes;
static int bad_window, copy_failure, mutex_busy, info_failure;
static unsigned capture_allocations, capture_frees, capture_bytes;
static int allocation_failure;
void klog(const char *fmt, ...) {}

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
    *base = uid == 22 ? (void *)memory : (void *)(uintptr_t)base_address; return 0;
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
int ksceKernelMemcpyUserToKernel(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }
int ksceKernelMemcpyKernelToUser(void *dst, const void *src, SceSize n) { memcpy(dst, src, n); return 0; }
int ksceKernelStrncpyUserToKernel(char *dst, const char *src, SceSize n) {
    size_t len = strnlen(src, n); memcpy(dst, src, len);
    if (len < n) memset(dst+len, 0, n-len);
    return 0;
}
SceUID ksceKernelAllocMemBlock(const char *n, unsigned int type, SceSize bytes, void *o) {
    TEST_CHECK(type == SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW);
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
    TEST_CHECK(capture_request(0) == 1 && capture_allocations == 1 && capture_bytes == RAW_SIZE);
    TEST_CHECK(capture_request(0) == VJO_ERR_BUSY && capture_allocations == 1);
    g.capture_release = 1; buffers_trim();
    TEST_CHECK(capture_frees == 0 && g.raw != NULL); /* a pending copy owns it */
    g.capture_state = CAPTURE_IDLE; g.raw_valid = 1; buffers_trim();
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

TEST_LIST = {
    {"text syscall permissions", permissions}, {"mapped copy and nonblocking submit", submit_mapping},
    {"register pointer and UTF16", indirection}, {"submit capacity and scratch reuse", submit_capacity},
    {"blank/freed source invalidation", source_lifetime},
    {"same-PID session invalidation", session_epoch}, {"bounded discovery/listen", scan_budget},
    {"stable driver memory type and failures", scan_memory_type},
    {"lazy capture and safe native release", lazy_capture}, {NULL, NULL}
};
