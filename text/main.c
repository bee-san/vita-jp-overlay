/* User plugin loaded in native Vita games. Common string imports provide
 * generic streams. Optional version-checked ARM/Thumb register profiles can
 * adapt simple Luna guest hooks; Windows/JIT code is never linked here. */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <stdarg.h>
#include <taihen.h>
#include "../include/vjo_api.h"
#include "../include/vjo_text.h"
#include "../include/vjo_import.h"
#include "../core/hook_profile.h"

static tai_hook_ref_t refs[4], native_refs[VJO_NATIVE_HOOKS];
static SceUID uids[4] = {-1,-1,-1,-1};
static SceUID native_uids[VJO_NATIVE_HOOKS];
static VjoNativeProfile profile;
static volatile int submitting;
static int enabled;
static SceUID diagnostic_fd = -1;

/* Startup metadata only: never write files from a game hook. A loaded module
 * can have zero usable imports, so module state alone is not capture evidence. */
static void diagnostic(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    int n;
    if (diagnostic_fd < 0) return;
    va_start(ap, fmt);
    n = sceClibVsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
    sceIoWrite(diagnostic_fd, line, n);
}

/* Single producer at a time. Recursion from our own syscalls is skipped;
 * a busy capture never waits on another game thread. */
static void offer(const void *ptr, unsigned bytes, uint32_t stream)
{
    if (!enabled || bytes < 4 || bytes >= VJO_TEXT_BYTES || !ptr) return;
    if (!__sync_bool_compare_and_swap(&submitting, 0, 1)) return;
    /* Inspect only the kernel's pinned copy. Another game thread may free a
     * source immediately after the original call returns. One syscall picks
     * valid UTF-8 or CP932 and skips ASCII without two candidate streams. */
    VjoTextEvent e = {sizeof(e), VJO_TEXT_CALL, VJO_TEXT_AUTO, stream,
                       (uint32_t)(uintptr_t)ptr, bytes, 0, 0};
    vjoTextSubmit(&e);
    __sync_lock_release(&submitting);
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
    int rc = sceKernelGetModuleList(0xFF, modules, &count);
    diagnostic("module list rc=%08X count=%u\n", rc, (unsigned)count);
    if (rc < 0 || count > 64) return -1;
    for (SceSize i = 0; i < count; i++) {
        SceKernelModuleInfo candidate;
        sceClibMemset(&candidate, 0, sizeof(candidate)); candidate.size = sizeof(candidate);
        rc = sceKernelGetModuleInfo(modules[i], &candidate);
        if (rc < 0) { diagnostic("module %08X info rc=%08X\n", modules[i], rc); continue; }
        if (sceClibStrnlen(candidate.module_name, sizeof(candidate.module_name)) == sizeof(candidate.module_name)) continue;
        unsigned len = (unsigned)sceClibStrnlen(candidate.path, sizeof(candidate.path));
        diagnostic("module %08X name=%.27s path=%.128s\n", modules[i], candidate.module_name, candidate.path);
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
    char title[12] = {0}, path[96];
    int version_ready, submit_ready, version;
    (void)argc; (void)args;
    if (sceAppMgrAppParamGetString(0, 12, title, sizeof(title)) >= 0 && title[0]) {
        sceClibSnprintf(path, sizeof(path), "ux0:data/VitaJPOverlay/text-%.11s.txt", title);
        diagnostic_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    }
    for (unsigned i = 0; i < VJO_NATIVE_HOOKS; i++) native_uids[i] = -1;
    version_ready = VJO_IMPORT_READY(vjoGetVersion);
    submit_ready = VJO_IMPORT_READY(vjoTextSubmit);
    version = version_ready ? vjoGetVersion() : -1;
    diagnostic("text startup version_ready=%d submit_ready=%d api=%d\n", version_ready, submit_ready, version);
    if (!version_ready || !submit_ready || version != VJO_API_VERSION || find_game_module(&module) < 0)
        goto done;
    diagnostic("game module=%08X name=%.27s\n", module.modid, module.module_name);
    for (unsigned i = 0; i < 4; i++)
        diagnostic("segment %u address=%08X bytes=%u\n", i, (unsigned)(uintptr_t)module.segments[i].vaddr, module.segments[i].memsz);
    enabled = 1;
    /* NIDs from the pinned VitaSDK SceLibKernel database. Import hooks affect
     * the main game module only, so our own clib calls cannot recurse. */
    uids[0] = taiHookFunctionImport(&refs[0], module.module_name, 0xCAE9ACE6, 0xAC595E68, strnlen_hook);
    uids[1] = taiHookFunctionImport(&refs[1], module.module_name, 0xCAE9ACE6, 0x2CDFCD1C, strlcpy_hook);
    uids[2] = taiHookFunctionImport(&refs[2], module.module_name, 0xCAE9ACE6, 0x14E9DBD7, memcpy_hook);
    uids[3] = taiHookFunctionImport(&refs[3], module.module_name, 0xCAE9ACE6, 0xC458D60A, strncpy_hook);
    diagnostic("import hooks strnlen=%08X strlcpy=%08X memcpy=%08X strncpy=%08X\n", uids[0], uids[1], uids[2], uids[3]);
    load_profile(&module);
    diagnostic("profile hooks=%u\n", profile.count);
    for (unsigned i = 0; i < profile.count; i++) diagnostic("profile %u result=%08X\n", i, native_uids[i]);
done:
    diagnostic("startup complete enabled=%d\n", enabled);
    if (diagnostic_fd >= 0) sceIoClose(diagnostic_fd);
    diagnostic_fd = -1;
    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
    (void)argc; (void)args;
    enabled = 0;
    for (unsigned i = 0; i < 4; i++) if (uids[i] >= 0) taiHookRelease(uids[i], refs[i]);
    for (unsigned i = 0; i < VJO_NATIVE_HOOKS; i++)
        if (native_uids[i] >= 0) taiHookRelease(native_uids[i], native_refs[i]);
    return SCE_KERNEL_STOP_SUCCESS;
}
