#ifndef VJO_TEST_PAF_SYSMEM_H
#define VJO_TEST_PAF_SYSMEM_H
struct SceKernelFreeMemorySizeInfo { int size, size_user, size_cdram, size_phycont; };
extern "C" int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info);
#endif
