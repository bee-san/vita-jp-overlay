/* Host-only SDK boundary for exercising the real shell control policy.
 * No threads or captures are started; time and job notifications are supplied
 * by the regression harness. The Vita build never includes these headers. */
#ifndef VJO_TEST_VITA_STUBS_H
#define VJO_TEST_VITA_STUBS_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int SceUID;
typedef unsigned int SceSize;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0
#define SCE_EVENT_WAITOR 1
#define SCE_EVENT_WAITCLEAR_PAT 2
#define SCE_KERNEL_POWER_TICK_DEFAULT 0
#define sceClibMemcpy memcpy
#define sceClibMemset memset
#define sceClibSnprintf snprintf
#define sceClibStrnlen strnlen
#define sceClibStrcmp strcmp
#define sceClibStrncmp strncmp
uint64_t sceKernelGetProcessTimeWide(void);
int sceKernelSetEventFlag(SceUID uid, unsigned int bits);
int sceKernelPowerTick(int type);
static inline int sceKernelLockMutex(SceUID uid, int count, void *timeout) { return 0; }
static inline int sceKernelUnlockMutex(SceUID uid, int count) { return 0; }
#ifdef VJO_TEST_MEMORY_STUBS
SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned int size, void *opt);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);
#else
static inline SceUID sceKernelAllocMemBlock(const char *name, int type, unsigned int size, void *opt) { return -1; }
static inline int sceKernelGetMemBlockBase(SceUID uid, void **base) { return -1; }
static inline int sceKernelFreeMemBlock(SceUID uid) { return 0; }
#endif
static inline int sceKernelWaitEventFlag(SceUID uid, unsigned int bits, unsigned int mode,
                                        unsigned int *out, void *timeout) { return -1; }
static inline SceUID sceKernelCreateMutex(const char *name, unsigned int attr, int count, void *opt) { return -1; }
static inline SceUID sceKernelCreateEventFlag(const char *name, unsigned int attr, unsigned int bits, void *opt) { return -1; }
static inline SceUID sceKernelCreateThread(const char *name, int (*entry)(SceSize, void *), int priority,
                                          unsigned int stack, unsigned int attr, int affinity, void *opt) { return -1; }
static inline int sceKernelStartThread(SceUID uid, SceSize size, void *args) { return -1; }
#ifdef VJO_TEST_WORKER_LIFECYCLE
int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout);
int sceKernelDeleteThread(SceUID uid);
int sceKernelDeleteEventFlag(SceUID uid);
int sceKernelDeleteMutex(SceUID uid);
#else
static inline int sceKernelWaitThreadEnd(SceUID uid, int *status, void *timeout) { return -1; }
static inline int sceKernelDeleteThread(SceUID uid) { return 0; }
static inline int sceKernelDeleteEventFlag(SceUID uid) { return 0; }
static inline int sceKernelDeleteMutex(SceUID uid) { return 0; }
#endif
static inline int sceAppMgrAppParamGetString(int pid, int param, char *out, SceSize cap) { return -1; }
#endif
