#ifndef VJO_GAMES_TEST_TAI_H
#define VJO_GAMES_TEST_TAI_H
#include <psp2kern/ctrl.h>
typedef unsigned tai_hook_ref_t;
#define KERNEL_PID 0x10005
#define TAI_ANY_LIBRARY 0xffffffffu
#define TAI_CONTINUE(type, ref, ...) ((type)1)
static inline SceUID taiHookFunctionExportForKernel(SceUID pid, tai_hook_ref_t *ref,
    const char *module, uint32_t lib, uint32_t nid, void *fn) { return 1; }
static inline int taiHookReleaseForKernel(SceUID id, tai_hook_ref_t ref) { return 0; }
#endif
