/* User plugin loaded in native Vita games. Common string imports provide
 * generic streams. Optional version-checked ARM/Thumb register profiles can
 * adapt simple Luna guest hooks; Windows/JIT code is never linked here. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <taihen.h>
#include "../include/vjo_api.h"
#include "../include/vjo_text.h"
#include "../include/vjo_import.h"
#include "../core/hook_profile.h"

#define IMPORT_HOOKS 8
static tai_hook_ref_t refs[IMPORT_HOOKS], native_refs[VJO_NATIVE_HOOKS];
static SceUID uids[IMPORT_HOOKS] = {-1,-1,-1,-1,-1,-1,-1,-1};
static SceUID native_uids[VJO_NATIVE_HOOKS];
static VjoNativeProfile profile;
static volatile int submitting;
static int enabled;

/* Single producer at a time. Recursion from our own syscalls is skipped;
 * a busy capture never waits on another game thread. */
static void offer_bounded(const void *ptr, unsigned bytes, uint32_t stream)
{
    if (!enabled || bytes >= VJO_TEXT_BYTES || !ptr) return;
    if (!__sync_bool_compare_and_swap(&submitting, 0, 1)) return;
    /* Inspect only the kernel's pinned copy. Another game thread may free a
     * source immediately after the original call returns. One syscall picks
     * valid UTF-8 or CP932 and skips ASCII without two candidate streams. */
    VjoTextEvent e = {sizeof(e), VJO_TEXT_CALL, VJO_TEXT_AUTO, stream,
                       (uint32_t)(uintptr_t)ptr, bytes, 0, 0};
    vjoTextSubmit(&e);
    __sync_lock_release(&submitting);
}

static void offer(const void *ptr, unsigned bytes, uint32_t stream)
{
    if (bytes >= 4) offer_bounded(ptr, bytes, stream);
}

static unsigned strnlen_hook(const char *s, unsigned max)
{
    unsigned n = TAI_CONTINUE(unsigned, refs[0], s, max);
    offer(s, n, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return n;
}
static unsigned strlcpy_hook(char *dst, const char *src, unsigned cap)
{
    unsigned result = TAI_CONTINUE(unsigned, refs[1], dst, src, cap);
    offer(dst, cap < VJO_TEXT_BYTES ? cap : VJO_TEXT_BYTES-1,
          (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}
static void *memcpy_hook(void *dst, const void *src, unsigned n)
{
    void *result = TAI_CONTINUE(void *, refs[2], dst, src, n);
    offer(dst, n, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}
static char *strncpy_hook(char *dst, const char *src, unsigned n)
{
    char *result = TAI_CONTINUE(char *, refs[3], dst, src, n);
    offer(dst, n < VJO_TEXT_BYTES ? n : VJO_TEXT_BYTES-1,
          (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}

/* Retail SDK games commonly import SceLibc rather than SceLibKernel's clib
 * helpers. Keep separate references: both families may exist in one module. */
static void *libc_memcpy_hook(void *dst, const void *src, unsigned n)
{
    void *result = TAI_CONTINUE(void *, refs[4], dst, src, n);
    offer(dst, n, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}
static char *libc_strcpy_hook(char *dst, const char *src)
{
    char *result = TAI_CONTINUE(char *, refs[5], dst, src);
    /* No user-side strlen: copy and bound the NUL-terminated result in the
     * kernel while the allocation is pinned, just like a register profile. */
    offer_bounded(dst, 0, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}
static char *libc_strncpy_hook(char *dst, const char *src, unsigned n)
{
    char *result = TAI_CONTINUE(char *, refs[6], dst, src, n);
    offer(dst, n < VJO_TEXT_BYTES ? n : VJO_TEXT_BYTES-1,
          (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}
static void *libc_memmove_hook(void *dst, const void *src, unsigned n)
{
    void *result = TAI_CONTINUE(void *, refs[7], dst, src, n);
    offer(dst, n, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return result;
}

extern void vjo_observer_0(void), vjo_observer_1(void), vjo_observer_2(void), vjo_observer_3(void);
extern void vjo_observer_4(void), vjo_observer_5(void), vjo_observer_6(void), vjo_observer_7(void);
static void (*const observers[VJO_NATIVE_HOOKS])(void) = {
    vjo_observer_0, vjo_observer_1, vjo_observer_2, vjo_observer_3,
    vjo_observer_4, vjo_observer_5, vjo_observer_6, vjo_observer_7
};

uintptr_t vjo_text_observe(unsigned slot, const uint32_t *registers)
{
    const VjoNativeHook *h = &profile.hooks[slot];
    if (enabled && __sync_bool_compare_and_swap(&submitting, 0, 1)) {
        VjoTextEvent e = {sizeof(e), VJO_TEXT_REGISTER, h->encoding, 0x70000000u + slot,
                           registers[h->reg], 0, h->indirections, h->padding};
        vjoTextSubmit(&e);
        __sync_lock_release(&submitting);
    }
    /* Same next-hook decision as TAI_CONTINUE, without imposing a C prototype
     * on the intercepted instruction's live registers. */
    struct _tai_hook_user *current = (struct _tai_hook_user *)native_refs[slot];
    struct _tai_hook_user *next = (struct _tai_hook_user *)current->next;
    return (uintptr_t)(next ? next->func : current->old);
}

/* Resolve eboot by its module path. Naming it explicitly also avoids taiHEN's
 * ambiguous TAI_MAIN_MODULE lookup in games with several main-memory modules. */
static int find_game_module(SceKernelModuleInfo *main)
{
    SceUID modules[64];
    SceSize count = sizeof(modules) / sizeof(*modules);
    int found = 0;
    if (sceKernelGetModuleList(0xFF, modules, &count) < 0 || count > 64) return -1;
    for (SceSize i = 0; i < count; i++) {
        SceKernelModuleInfo candidate;
        sceClibMemset(&candidate, 0, sizeof(candidate)); candidate.size = sizeof(candidate);
        if (sceKernelGetModuleInfo(modules[i], &candidate) < 0) continue;
        if (sceClibStrnlen(candidate.module_name, sizeof(candidate.module_name)) == sizeof(candidate.module_name)) continue;
        unsigned len = (unsigned)sceClibStrnlen(candidate.path, sizeof(candidate.path));
        if (len < 10 || len == sizeof(candidate.path) ||
            sceClibStrcmp(candidate.path + len - 9, "eboot.bin") ||
            (candidate.path[len-10] != '/' && candidate.path[len-10] != ':')) continue;
        if (found++) return -1;
        *main = candidate;
    }
    return found ? 0 : -1;
}

static void load_profile(const SceKernelModuleInfo *module)
{
    char title[12] = {0}, path[128], data[2048];
    tai_module_info_t tai;
    int count;
    if (sceAppMgrAppParamGetString(0, 12, title, sizeof(title)) < 0 || !title[0]) return;
    sceClibSnprintf(path, sizeof(path), "ux0:data/VitaJPOverlay/hooks/%s.vjhook", title);
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return;
    count = sceIoRead(fd, data, sizeof(data));
    sceIoClose(fd);
    if (count <= 0 || count == sizeof(data) || vjo_hook_profile_parse(&profile, data, (size_t)count) < 0 ||
        sceClibStrcmp(profile.title, title)) return;
    sceClibMemset(&tai, 0, sizeof(tai)); tai.size = sizeof(tai);
    if (taiGetModuleInfo(module->module_name, &tai) < 0 || tai.module_nid != profile.module_nid ||
        tai.modid != module->modid) return;
    for (unsigned i = 0; i < profile.count; i++) {
        const VjoNativeHook *h = &profile.hooks[i];
        const SceKernelSegmentInfo *seg = &module->segments[h->segment];
        if (!seg->vaddr || h->offset > seg->memsz || h->signature_bytes > seg->memsz - h->offset ||
            sceClibMemcmp((const uint8_t *)seg->vaddr + h->offset, h->signature, h->signature_bytes)) continue;
        native_uids[i] = taiHookFunctionOffset(&native_refs[i], tai.modid, (int)h->segment,
                                               h->offset, (int)h->thumb, observers[i]);
    }
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args)
{
    SceKernelModuleInfo module;
    (void)argc; (void)args;
    for (unsigned i = 0; i < VJO_NATIVE_HOOKS; i++) native_uids[i] = -1;
    if (!VJO_IMPORT_READY(vjoGetVersion) || !VJO_IMPORT_READY(vjoTextSubmit) ||
        vjoGetVersion() != VJO_API_VERSION || find_game_module(&module) < 0)
        return SCE_KERNEL_START_SUCCESS;
    enabled = 1;
    /* NIDs from the pinned VitaSDK SceLibKernel database. Import hooks affect
     * the main game module only, so our own clib calls cannot recurse. */
    uids[0] = taiHookFunctionImport(&refs[0], module.module_name, 0xCAE9ACE6, 0xAC595E68, strnlen_hook);
    uids[1] = taiHookFunctionImport(&refs[1], module.module_name, 0xCAE9ACE6, 0x2CDFCD1C, strlcpy_hook);
    uids[2] = taiHookFunctionImport(&refs[2], module.module_name, 0xCAE9ACE6, 0x14E9DBD7, memcpy_hook);
    uids[3] = taiHookFunctionImport(&refs[3], module.module_name, 0xCAE9ACE6, 0xC458D60A, strncpy_hook);
    /* Verified against VitaSDK and CLANNAD PCSG00415's loaded import table. */
    uids[4] = taiHookFunctionImport(&refs[4], module.module_name, 0xBE43BB07, 0x7205BFDB, libc_memcpy_hook);
    uids[5] = taiHookFunctionImport(&refs[5], module.module_name, 0xBE43BB07, 0x85B924B7, libc_strcpy_hook);
    uids[6] = taiHookFunctionImport(&refs[6], module.module_name, 0xBE43BB07, 0x9F87712D, libc_strncpy_hook);
    uids[7] = taiHookFunctionImport(&refs[7], module.module_name, 0xBE43BB07, 0xAF5C218D, libc_memmove_hook);
    load_profile(&module);
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc; (void)args;
    enabled = 0;
    for (unsigned i = 0; i < IMPORT_HOOKS; i++) if (uids[i] >= 0) taiHookRelease(uids[i], refs[i]);
    for (unsigned i = 0; i < VJO_NATIVE_HOOKS; i++)
        if (native_uids[i] >= 0) taiHookRelease(native_uids[i], native_refs[i]);
    return SCE_KERNEL_STOP_SUCCESS;
}
