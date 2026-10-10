#include <psp2kern/host_stubs.h>
#if defined(VJO_TEST_INPUT_HOOKS) && !defined(VJO_TEST_TAIHEN_H)
#define VJO_TEST_TAIHEN_H
typedef uintptr_t tai_hook_ref_t;
#define KERNEL_PID 0x10005
#define TAI_ANY_LIBRARY UINT32_MAX
int test_tai_continue(tai_hook_ref_t, int, void *, unsigned, ...);
#define TAI_CONTINUE(type, ref, ...) ((type)test_tai_continue((ref), __VA_ARGS__))
SceUID taiHookFunctionExportForKernel(SceUID, tai_hook_ref_t *, const char *, uint32_t, uint32_t, void *);
int taiHookReleaseForKernel(SceUID, tai_hook_ref_t);
#endif
