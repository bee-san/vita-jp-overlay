/* Native Vita memory discovery and game-hook transport. All reads map the
 * target PID explicitly and hold the mapping reference through the copy. */
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include "vjo_kernel.h"
#include "text.h"
#include "../core/text_scan.h"

#define SCAN_BEGIN 0x80000000u
#define SCAN_END   0xA0000000u
#define SCAN_CHUNK 4096u
#define READ_ATTEMPT_US 200000u

static VjoTextSources sources;
static VjoTextSnapshot snapshot;
static SceUID lock = -1;
static SceUID process;
static uint32_t session = 1, scan_address, process_epoch;
static volatile int mode;
static int scanning;
static uint32_t last_poll_us;
static uint8_t scan_bytes[SCAN_CHUNK + VJO_TEXT_BYTES + 2];
_Static_assert(sizeof(scan_bytes) >= 2 * VJO_TEXT_BYTES, "text submit scratch must fit");

static int shell_caller(void)
{
    return g.shell_pid > 0 && ksceKernelGetProcessId() == g.shell_pid;
}

static void sync_process(void)
{
    uint32_t epoch = __atomic_load_n(&g.text_epoch, __ATOMIC_ACQUIRE);
    SceUID current = g.game_active == VJO_GAME ? g.game_pid : 0;
    if (process == current && process_epoch == epoch) return;
    process = current;
    process_epoch = epoch;
    if (++session == 0) session = 1;
    vjo_text_sources_init(&sources);
    mode = VJO_TEXT_OFF; scanning = 0;
    scan_address = SCAN_BEGIN;
    last_poll_us = 0;
}

static int mapped_copy(SceUID pid, uint32_t address, void *dst, uint32_t bytes)
{
    void *page = NULL;
    SceSize size = 0;
    uint32_t offset = 0;
    SceUID uid;
    if (!bytes || address < 0x40000000u || bytes > 0xFFFFFFFFu - address) return -1;
    uid = ksceKernelProcUserMap(pid, "VjoTextRead", 1, (const void *)(uintptr_t)address,
                               bytes, &page, &size, &offset);
    if (uid < 0) return uid;
    if (page && offset <= size && bytes <= size - offset)
        memcpy(dst, (const uint8_t *)page + offset, bytes);
    else {
        ksceKernelMemBlockRelease(uid);
        return -1;
    }
    ksceKernelMemBlockRelease(uid);
    return 0;
}

/* Read only to an allocation's end. The mapping copy pins it, and the UID
 * check rejects an old buffer that has been freed/replaced at the same VA. */
static int read_string(uint32_t address, uint32_t expected_mapping, int encoding, char *text)
{
    uint8_t raw[VJO_TEXT_BYTES];
    void *base = NULL;
    SceSize mapped_size = 0;
    SceUID uid = ksceKernelFindProcMemBlockByAddr(process, (void *)(uintptr_t)address, 0);
    uint32_t available, n;
    if (uid < 0 || (expected_mapping && (uint32_t)uid != expected_mapping) ||
        ksceKernelGetMemBlockBase(uid, &base) < 0 ||
        ksceKernelGetMemBlockAllocMapSize(uid, &mapped_size) < 0 ||
        address < (uint32_t)(uintptr_t)base || address - (uint32_t)(uintptr_t)base >= mapped_size)
        return -1;
    available = mapped_size - (address - (uint32_t)(uintptr_t)base);
    n = available < sizeof(raw) ? available : sizeof(raw);
    if (encoding == VJO_TEXT_UTF16LE) n &= ~1u;
    if (!n || mapped_copy(process, address, raw, n) < 0 ||
        ksceKernelFindProcMemBlockByAddr(process, (void *)(uintptr_t)address, 0) != uid) return -1;
    /* Fixed buffers must terminate. A prefix of a long script is not dialogue. */
    for (uint32_t i = 0; i < n; i += encoding == VJO_TEXT_UTF16LE ? 2u : 1u)
        if (!raw[i] && (encoding != VJO_TEXT_UTF16LE || !raw[i+1]))
            return vjo_text_decode(encoding, raw, i, text, VJO_TEXT_BYTES);
    return -1;
}

static void poll_memory(void)
{
    char text[VJO_TEXT_BYTES];
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++) {
        VjoTextCandidate *c = &sources.items[i];
        uint32_t address = c->address;
        if (!c->id || (mode == VJO_TEXT_FOLLOW && c->id != sources.selected) ||
            (c->kind != VJO_TEXT_MEMORY && c->kind != VJO_TEXT_POINTER)) continue;
        if (c->kind == VJO_TEXT_POINTER) {
            uint32_t ptr;
            SceUID uid = ksceKernelFindProcMemBlockByAddr(process, (void *)(uintptr_t)address, 4);
            if (uid < 0 || (uint32_t)uid != c->locator || mapped_copy(process, address, &ptr, 4) < 0 ||
                ksceKernelFindProcMemBlockByAddr(process, (void *)(uintptr_t)address, 4) != uid) {
                vjo_text_forget(&sources, c->id); continue;
            }
            address = ptr;
        }
        int rc = read_string(address, c->kind == VJO_TEXT_MEMORY ? c->locator : 0,
                              (int)c->encoding, text);
        if (rc < 0) { vjo_text_forget(&sources, c->id); continue; }
        if (!rc) {
            if (c->text[0]) {
                c->text[0] = 0; c->score = c->length = c->japanese = 0;
                sources.sequence++;
            }
            continue;
        }
        vjo_text_offer(&sources, c->kind, c->encoding, c->address, c->locator, text);
    }
}

static void scan_step(void)
{
    /* Up to 64 hole probes or four pages per tick; no scan runs in a game's
     * display/input call. The worker yields between ticks. */
    unsigned pages = 0;
    for (unsigned attempts = 0; attempts < 64 && pages < 4; attempts++) {
        SceUID uid;
        void *base = NULL;
        SceSize size = 0;
        SceKernelMemBlockInfoEx info;
        unsigned type;
        uint32_t left, starts, bytes, prefix;
        if (scan_address >= SCAN_END) { scanning = 0; return; }
        uid = ksceKernelFindProcMemBlockByAddr(process, (void *)(uintptr_t)scan_address, 0);
        if (uid < 0 || ksceKernelGetMemBlockBase(uid, &base) < 0 ||
            ksceKernelGetMemBlockAllocMapSize(uid, &size) < 0 ||
            scan_address < (uint32_t)(uintptr_t)base ||
            scan_address - (uint32_t)(uintptr_t)base >= size) {
            scan_address += SCAN_CHUNK; continue;
        }
        left = size - (scan_address - (uint32_t)(uintptr_t)base);
        prefix = scan_address - (uint32_t)(uintptr_t)base;
        if (prefix > 2) prefix = 2;
        /* The ForDriver export is stable across 3.60/3.65. GetMemBlockType
         * belongs to ForKernel, whose library and function NIDs changed in
         * 3.63; a strong import prevents this entire plugin from starting. */
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ksceKernelMemBlockGetInfoEx(uid, &info) < 0) {
            scan_address += left; continue;
        }
        type = info.core_info.type;
        /* CDRAM, device mappings and uncached textures are not text heaps. */
        if (type != SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW &&
            type != SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R &&
            type != SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW) {
            scan_address += left; continue;
        }
        starts = left < SCAN_CHUNK ? left : SCAN_CHUNK;
        bytes = left < sizeof(scan_bytes)-prefix ? left : sizeof(scan_bytes)-prefix;
        if (mapped_copy(process, scan_address-prefix, scan_bytes, bytes+prefix) == 0)
            vjo_text_scan(&sources, scan_bytes, bytes+prefix, prefix, starts+prefix,
                           scan_address-prefix, (uint32_t)uid);
        scan_address += starts;
        pages++;
    }
}

void text_tick(void)
{
    uint32_t now = (uint32_t)ksceKernelGetSystemTimeWide();
    ksceKernelLockMutex(lock, 1, NULL);
    sync_process();
    if (process > 0 && mode != VJO_TEXT_OFF) {
        if (now - last_poll_us >= READ_ATTEMPT_US) {
            poll_memory(); last_poll_us = now;
        }
        if (mode == VJO_TEXT_DISCOVER && scanning) scan_step();
    }
    ksceKernelUnlockMutex(lock, 1);
}

int vjoTextControl(uint32_t expected, int next_mode, uint32_t selected)
{
    uint32_t state;
    int rc = 0;
    if (!shell_caller()) return VJO_ERR_PERM;
    if (next_mode < VJO_TEXT_OFF || next_mode > VJO_TEXT_LISTEN) return VJO_ERR_ARG;
    ENTER_SYSCALL(state);
    ksceKernelLockMutex(lock, 1, NULL);
    sync_process();
    if (expected != session || process <= 0) rc = VJO_ERR_NO_GAME;
    else if (selected && !vjo_text_find(&sources, selected)) rc = VJO_ERR_ARG;
    else {
        sources.selected = selected;
        mode = next_mode;
        if (mode != VJO_TEXT_OFF) g.capture_release = 1;
        if (mode == VJO_TEXT_DISCOVER) { scan_address = SCAN_BEGIN; scanning = 1; }
        else scanning = 0;
        sources.sequence++;
    }
    ksceKernelUnlockMutex(lock, 1);
    EXIT_SYSCALL(state);
    return rc;
}

int vjoTextReference(uint32_t expected, const char *user_text)
{
    char text[VJO_TEXT_BYTES];
    uint32_t state;
    int rc;
    if (!shell_caller()) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    memset(text, 0, sizeof(text));
    text[sizeof(text)-1] = 1;
    rc = ksceKernelStrncpyUserToKernel(text, user_text, sizeof(text));
    if (rc >= 0 && text[sizeof(text)-1] != 0) rc = VJO_ERR_ARG;
    if (rc >= 0) {
        ksceKernelLockMutex(lock, 1, NULL);
        sync_process();
        rc = expected == session && process > 0 ? vjo_text_reference(&sources, text) : VJO_ERR_NO_GAME;
        if (rc == 0) {
            scan_address = SCAN_BEGIN; scanning = 1; mode = VJO_TEXT_DISCOVER;
            g.capture_release = 1;
        }
        ksceKernelUnlockMutex(lock, 1);
    }
    EXIT_SYSCALL(state);
    return rc;
}

int vjoTextRead(VjoTextSnapshot *out)
{
    uint32_t state;
    int rc;
    if (!shell_caller()) return VJO_ERR_PERM;
    ENTER_SYSCALL(state);
    ksceKernelLockMutex(lock, 1, NULL);
    sync_process();
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.size = sizeof(snapshot); snapshot.pid = process;
    snapshot.session = session; snapshot.sequence = sources.sequence;
    snapshot.selected = sources.selected; snapshot.scanning = (uint32_t)scanning;
    snapshot.scan_address = scan_address;
    snapshot.count = vjo_text_top(&sources, snapshot.candidates);
    VjoTextCandidate *current = vjo_text_find(&sources, sources.selected);
    if (current) snapshot.current = *current;
    rc = ksceKernelMemcpyKernelToUser(out, &snapshot, sizeof(snapshot));
    ksceKernelUnlockMutex(lock, 1);
    EXIT_SYSCALL(state);
    return rc;
}

int vjoTextSubmit(const VjoTextEvent *user_event)
{
    uint32_t state;
    VjoTextEvent event;
    /* Scanning and submission share lock. Reuse its scratch in two disjoint
     * regions: stack buffers here would overflow the 4 KiB syscall stack
     * when vjo_text_offer calls the reference scorer. */
    uint8_t *raw = scan_bytes;
    char *text = (char *)scan_bytes + VJO_TEXT_BYTES;
    int rc = VJO_ERR_PERM;
    SceUID pid = ksceKernelGetProcessId();
    if (pid <= 0 || pid != g.game_pid || !g.game_active || g.game_active == VJO_GAME_STATIC_FB)
        return rc;
    ENTER_SYSCALL(state);
    if (ksceKernelMemcpyUserToKernel(&event, user_event, sizeof(event)) < 0 ||
        event.size != sizeof(event) || (event.kind != VJO_TEXT_CALL && event.kind != VJO_TEXT_REGISTER) ||
        event.encoding > VJO_TEXT_CP932 || event.bytes > VJO_TEXT_BYTES ||
        (event.encoding == VJO_TEXT_AUTO && event.kind != VJO_TEXT_CALL) ||
        event.indirections > 4 || event.padding < -0x100000 || event.padding > 0x100000) {
        rc = VJO_ERR_ARG; goto out;
    }
    if (ksceKernelTryLockMutex(lock, 1) < 0) { rc = VJO_ERR_BUSY; goto out; }
    sync_process();
    if (pid != process || mode == VJO_TEXT_OFF) rc = VJO_ERR_NO_GAME;
    else {
        rc = 0;
        for (unsigned i = 0; i < event.indirections; i++)
            if (mapped_copy(pid, event.address, &event.address, 4) < 0) { rc = VJO_ERR_COPY; break; }
        int64_t address = (int64_t)event.address + event.padding;
        if (address < 0x40000000LL || address > 0xFFFFFFFFLL) rc = VJO_ERR_ARG;
        event.address = (uint32_t)address;
        if (rc == 0) {
            if (!event.bytes) rc = read_string(event.address, 0, (int)event.encoding, text);
            else if (mapped_copy(pid, event.address, raw, event.bytes) < 0) rc = VJO_ERR_COPY;
            else rc = vjo_text_decode((int)event.encoding, raw, event.bytes, text, VJO_TEXT_BYTES);
        }
    }
    if (rc > 0) {
        vjo_text_offer(&sources, event.kind, event.encoding, event.stream, 0, text);
        rc = 0;
    }
    ksceKernelUnlockMutex(lock, 1);
out:
    EXIT_SYSCALL(state);
    return rc;
}

int text_init(void)
{
    vjo_text_sources_init(&sources);
    lock = ksceKernelCreateMutex("VjoText", 0, 0, NULL);
    return lock < 0 ? lock : 0;
}

void text_shutdown(void)
{
    if (lock >= 0) ksceKernelDeleteMutex(lock);
    lock = -1;
}

int text_uses_frame_checks(void) { return mode == VJO_TEXT_OFF; }
