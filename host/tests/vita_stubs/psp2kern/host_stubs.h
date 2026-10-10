/* Host boundary for testing kernel/text.c's actual mapping/lifetime policy. */
#ifndef VJO_TEST_KERNEL_STUBS_H
#define VJO_TEST_KERNEL_STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int SceUID;
typedef unsigned int SceSize;
/* Preserve the Vita structure's size and type offset on a 64-bit host. */
typedef struct {
    SceSize size;
    struct { unsigned int type; uint32_t remaining[10]; } core_info;
    uint32_t remaining[34];
} SceKernelMemBlockInfoEx;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW 1
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_R 2
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_GAME_RW 3
#define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW 4
extern unsigned syscall_depth;
#define ENTER_SYSCALL(s) do { (s) = syscall_depth++; } while (0)
#define EXIT_SYSCALL(s) do { syscall_depth = (s); } while (0)
SceUID ksceKernelGetProcessId(void);
uint64_t ksceKernelGetSystemTimeWide(void);
SceUID ksceKernelCreateMutex(const char *, unsigned int, int, void *);
int ksceKernelDeleteMutex(SceUID);
int ksceKernelLockMutex(SceUID, int, void *);
int ksceKernelTryLockMutex(SceUID, int);
int ksceKernelUnlockMutex(SceUID, int);
SceUID ksceKernelProcUserMap(SceUID, const char *, int, const void *, SceSize, void **, SceSize *, uint32_t *);
int ksceKernelMemBlockRelease(SceUID);
SceUID ksceKernelFindProcMemBlockByAddr(SceUID, const void *, SceSize);
int ksceKernelGetMemBlockBase(SceUID, void **);
int ksceKernelGetMemBlockAllocMapSize(SceUID, SceSize *);
int ksceKernelMemBlockGetInfoEx(SceUID, SceKernelMemBlockInfoEx *);
SceUID ksceKernelAllocMemBlock(const char *, unsigned int, SceSize, void *);
int ksceKernelFreeMemBlock(SceUID);
int ksceKernelMemcpyUserToKernel(void *, const void *, SceSize);
int ksceKernelMemcpyKernelToUser(void *, const void *, SceSize);
int ksceKernelStrncpyUserToKernel(char *, const char *, SceSize);
#endif
