#include "cases.h"
#include "../../core/text_source.h"
#include "../../include/vjo_api.h"
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <taihen.h>
#include <stdint.h>
#include <string.h>

unsigned sceClibStrlcpy(char *dst, const char *src, unsigned cap);

uint32_t vjo_probe_expected_sp, vjo_probe_sp_captured;
uint32_t vjo_probe_gp_captured[14], vjo_probe_callback[18];
uint32_t vjo_probe_flags_captured, vjo_probe_fpscr_captured;
uint32_t vjo_probe_fp_initial[64] __attribute__((aligned(16)));
uint32_t vjo_probe_fp_captured[64] __attribute__((aligned(16)));
static int want_thumb;
static VjoTextSources captured;
static unsigned captures, call_captures, register_captures;
extern void vjo_probe_arm(void), vjo_probe_thumb(void);
extern void vjo_probe_continue_arm(void), vjo_probe_continue_thumb(void);
extern int vjo_profile_arm(const char *), vjo_profile_thumb(const char *), vjo_profile_rejected(const char *);

/* The production .suprx talks to these exported test transports. This proves
 * actual taiHEN interception, not the kernel's address-space isolation. */
int vjoGetVersion(void) { return VJO_API_VERSION; }
int vjoTextSubmit(const VjoTextEvent *e)
{
    char text[VJO_TEXT_BYTES];
    const void *raw = (const void *)(uintptr_t)e->address;
    unsigned bytes = e->bytes ? e->bytes : (unsigned)strlen(raw);
    captures++;
    if (e->kind == VJO_TEXT_CALL) call_captures++;
    if (e->kind == VJO_TEXT_REGISTER) register_captures++;
    if (vjo_text_decode((int)e->encoding, raw, bytes, text, sizeof(text)) > 0)
        vjo_text_offer(&captured, e->kind, e->encoding, e->stream, 0, text);
    return 0;
}

static int plugin_checks(FILE *out)
{
    unsigned checks = 0, failures = 0;
#define PLUGIN_CHECK(c) do { checks++; if (!(c)) { failures++; \
    fprintf(out, "FAIL plugin line=%d %s\n", __LINE__, #c); } } while (0)
    const char *line = "今日は新しい台詞を読む";
    char copy[128];
    unsigned copy_result = sceClibStrlcpy(copy, line, sizeof(copy));
    tai_module_info_t tai = {.size = sizeof(tai)};
    SceKernelModuleInfo module = {.size = sizeof(module)};
    PLUGIN_CHECK(taiGetModuleInfo("vjo-hook-test", &tai) == 0 && tai.module_nid != 0);
    PLUGIN_CHECK(sceKernelGetModuleInfo(tai.modid, &module) == 0);
    sceIoMkdir("ux0:data/VitaJPOverlay", 0777);
    sceIoMkdir("ux0:data/VitaJPOverlay/hooks", 0777);
    sceIoRemove("ux0:data/VitaJPOverlay/hooks/VJOHK0001.vjhook");
    FILE *profile = fopen("ux0:data/VitaJPOverlay/hooks/VJOHK0001.vjhook", "w");
    PLUGIN_CHECK(profile != NULL);
    if (!profile || failures) { if (profile) fclose(profile); return (int)failures; }
    fprintf(profile, "VJOHOOK1 VJOHK0001 %08X\n", tai.module_nid);
    uintptr_t functions[] = {(uintptr_t)vjo_profile_arm, (uintptr_t)vjo_profile_thumb,
                             (uintptr_t)vjo_profile_rejected};
    for (unsigned i = 0; i < 3; i++) {
        uintptr_t address = functions[i] & ~(uintptr_t)1;
        fprintf(profile, "0 %X %X 0 1 0 0 ",
                (unsigned)(address - (uintptr_t)module.segments[0].vaddr), (unsigned)(functions[i]&1));
        for (unsigned j = 0; j < 8; j++)
            fprintf(profile, "%02X", i == 2 ? 0xFF : ((const uint8_t *)address)[j]);
        fputc('\n', profile);
    }
    fclose(profile);
    vjo_text_sources_init(&captured);
    int status = -1;
    SceUID plugin = sceKernelLoadStartModule("app0:VitaJPOverlay_Text.suprx", 0, NULL, 0, NULL, &status);
    fprintf(out, "plugin uid=%08X start=%d\n", plugin, status);
    PLUGIN_CHECK(plugin >= 0 && status == 0);
    unsigned before = call_captures;
    PLUGIN_CHECK(sceClibStrnlen(line, 100) == strlen(line) && call_captures > before);
    before = call_captures;
    PLUGIN_CHECK(sceClibStrlcpy(copy, line, sizeof(copy)) == copy_result &&
                 !strcmp(copy, line) && call_captures > before);
    before = call_captures;
    PLUGIN_CHECK(sceClibMemcpy(copy, line, strlen(line)+1) == copy &&
                 !strcmp(copy, line) && call_captures > before);
    before = call_captures;
    PLUGIN_CHECK(sceClibStrncpy(copy, line, sizeof(copy)) == copy &&
                 !strcmp(copy, line) && call_captures > before);
    before = register_captures;
    PLUGIN_CHECK(vjo_profile_arm(line) == 17 && register_captures == before+1);
    before = register_captures;
    PLUGIN_CHECK(vjo_profile_thumb(line) == 19 && register_captures == before+1);
    before = register_captures;
    PLUGIN_CHECK(vjo_profile_rejected(line) == 23 && register_captures == before);
    unsigned japanese = 0;
    for (unsigned i = 0; i < VJO_TEXT_SOURCES; i++)
        if (captured.items[i].id && !strcmp(captured.items[i].text, line)) japanese++;
    PLUGIN_CHECK(japanese >= 6);
    PLUGIN_CHECK(sceKernelStopUnloadModule(plugin, 0, NULL, 0, NULL, &status) == 0 && status == 0);
    before = captures;
    PLUGIN_CHECK(sceClibStrnlen(line, 100) == strlen(line) &&
                 vjo_profile_arm(line) == 17 && vjo_profile_thumb(line) == 19 && captures == before);
    fprintf(out, "plugin checks=%u failures=%u call_events=%u register_events=%u matching_sources=%u\n",
            checks, failures, call_captures, register_captures, japanese);
#undef PLUGIN_CHECK
    return (int)failures;
}

uintptr_t vjo_text_observe(unsigned slot, const uint32_t *regs)
{
    (void)slot;
    memcpy(vjo_probe_callback, regs, sizeof(vjo_probe_callback));
    /* Force live caller FP registers and flags to change in the observer's
     * C callback. Callee-saved registers obey the C ABI. */
    __asm__ volatile(
        "veor q0, q0, q0\n veor q1, q1, q1\n veor q2, q2, q2\n veor q3, q3, q3\n"
        "veor q8, q8, q8\n veor q9, q9, q9\n veor q10, q10, q10\n veor q11, q11, q11\n"
        "veor q12, q12, q12\n veor q13, q13, q13\n veor q14, q14, q14\n veor q15, q15, q15\n"
        ::: "d0","d1","d2","d3","d4","d5","d6","d7","d16","d17","d18","d19",
            "d20","d21","d22","d23","d24","d25","d26","d27","d28","d29","d30","d31");
    uint32_t altered_fpscr = 0x01000003u, altered_flags = 0x80000000u;
    __asm__ volatile("vmsr fpscr, %0\n msr APSR_nzcvqg, %1"
                     :: "r"(altered_fpscr), "r"(altered_flags) : "cc");
    return (uintptr_t)(want_thumb ? vjo_probe_continue_thumb : vjo_probe_continue_arm);
}

static int observer_checks(FILE *out)
{
    unsigned checks = 0, failures = 0;
    for (int thumb = 0; thumb < 2; thumb++) {
        want_thumb = thumb;
        for (unsigned i = 0; i < 64; i++) vjo_probe_fp_initial[i] = 0x3F000000u + i;
        if (thumb) vjo_probe_thumb(); else vjo_probe_arm();
        for (unsigned i = 0; i < 13; i++) {
            checks += 2;
            if (vjo_probe_gp_captured[i] != i+1) failures++;
            if (vjo_probe_callback[i] != i+1) failures++;
        }
        for (unsigned i = 0; i < 64; i++) {
            checks++;
            if (vjo_probe_fp_captured[i] != vjo_probe_fp_initial[i]) failures++;
        }
        checks += 6;
        if (vjo_probe_sp_captured != vjo_probe_expected_sp) failures++;
        if (vjo_probe_callback[13] != vjo_probe_expected_sp) failures++;
        if (vjo_probe_callback[14] != vjo_probe_gp_captured[13]) failures++;
        if ((vjo_probe_flags_captured & 0xF80F0000u) != 0x600F0000u) failures++;
        /* Compare the state at observer entry. Vita3K can carry a host inexact
         * flag into the first probe despite the requested FPSCR seed. */
        if (vjo_probe_fpscr_captured != vjo_probe_callback[17]) failures++;
        if ((vjo_probe_callback[17] & 0x07C00000u) != 0x02000000u) failures++;
        fprintf(out, "observer mode=%s sp=%08X/%08X flags=%08X fpscr=%08X entry_fpscr=%08X failures=%u\n",
                thumb ? "thumb" : "arm", vjo_probe_sp_captured, vjo_probe_expected_sp,
                vjo_probe_flags_captured, vjo_probe_fpscr_captured, vjo_probe_callback[17], failures);
    }
    fprintf(out, "observer checks=%u failures=%u\n", checks, failures);
    return (int)failures;
}

int main(void)
{
    sceIoMkdir("ux0:data/VitaJPOverlay-hook-test", 0777);
    sceIoRemove("ux0:data/VitaJPOverlay-hook-test/results.txt");
    FILE *out = fopen("ux0:data/VitaJPOverlay-hook-test/results.txt", "w");
    if (!out) { sceKernelExitProcess(2); return 2; }
    int failures = vjo_text_run_tests(out);
    fflush(out);
    failures += observer_checks(out);
    fflush(out);
    failures += plugin_checks(out);
    fprintf(out, "RESULT %s\n", failures ? "FAIL" : "PASS");
    fclose(out);
    sceKernelExitProcess(failures != 0);
    return failures != 0;
}
