/* Only the module-manager boundary used by the real Meiki Shell bridge. */
#ifndef VJO_TEST_MODULEMGR_H
#define VJO_TEST_MODULEMGR_H
#include <psp2/host_stubs.h>
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_NO_RESIDENT 1
#define SCE_KERNEL_START_FAILED 2
#define SCE_KERNEL_STOP_SUCCESS 0
#define SCE_KERNEL_STOP_FAIL 1
typedef struct SceKernelLMOption SceKernelLMOption;
typedef struct SceKernelULMOption SceKernelULMOption;
SceUID sceKernelLoadStartModule(const char *path, SceSize args, void *argp,
                               int flags, SceKernelLMOption *option, int *status);
int sceKernelStopUnloadModule(SceUID uid, SceSize args, void *argp,
                              int flags, SceKernelULMOption *option, int *status);
#endif
