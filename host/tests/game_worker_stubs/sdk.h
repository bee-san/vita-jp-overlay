/* Host-only SDK boundary for the actual OCR worker/bridge. No Vita build uses
 * these declarations; fake functions model only owned UIDs and file handles. */
#ifndef VJO_GAME_WORKER_TEST_SDK_H
#define VJO_GAME_WORKER_TEST_SDK_H
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef int32_t SceUID;
typedef uint32_t SceSize, SceUInt, SceUInt32;
typedef int64_t SceOff;
typedef struct SceKernelLMOption SceKernelLMOption;
typedef struct SceKernelULMOption SceKernelULMOption;
typedef struct { int size, size_user, size_cdram, size_phycont; } SceKernelFreeMemorySizeInfo;
typedef struct { SceSize size; void *mappedBase; SceSize mappedSize; int memoryType; uint32_t access; int type; } SceKernelMemBlockInfo;
typedef struct { int64_t st_size; } SceIoStat;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 0x0C20D060
#define SCE_KERNEL_MEMORY_TYPE_NORMAL 0xD0
#define SCE_KERNEL_MEMORY_ACCESS_R 4
#define SCE_KERNEL_MEMORY_ACCESS_W 2
#define SCE_KERNEL_ERROR_INVALID_UID ((int32_t)0x80024501)
#define SCE_KERNEL_ERROR_UNKNOWN_UID ((int32_t)0x80028001)
#define SCE_KERNEL_START_SUCCESS 0
#define SCE_KERNEL_START_FAILED 2
#define SCE_KERNEL_STOP_SUCCESS 0
#define SCE_KERNEL_STOP_FAIL 1
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_CREAT 0x200
#define SCE_O_APPEND 0x100
#define SCE_SEEK_SET 0
#define SCE_SEEK_END 2
#define sceClibMemset memset
#define sceClibMemcpy memcpy
#define sceClibMemcmp memcmp
#define sceClibMemchr memchr
#define sceClibSnprintf snprintf
#define sceClibVsnprintf vsnprintf
#define sceClibStrcmp strcmp
#define sceClibStrncmp strncmp
SceUID sceKernelAllocMemBlock(const char *, int, SceSize, void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelGetMemBlockInfoByRange(void *, SceSize, SceKernelMemBlockInfo *);
int sceKernelFreeMemBlock(SceUID);
int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *);
SceUID sceKernelCreateThread(const char *, int (*)(SceSize, void *), int, SceSize, unsigned, int, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelWaitThreadEnd(SceUID, int *, SceUInt *);
int sceKernelDeleteThread(SceUID);
int sceKernelDelayThread(SceUInt);
SceUID sceKernelLoadStartModule(const char *, SceSize, void *, int, SceKernelLMOption *, int *);
int sceKernelStopUnloadModule(SceUID, SceSize, void *, int, SceKernelULMOption *, int *);
SceUID sceIoOpen(const char *, int, unsigned);
int sceIoClose(SceUID);
int sceIoGetstat(const char *, SceIoStat *);
SceOff sceIoLseek(SceUID, SceOff, int);
int sceIoRead(SceUID, void *, SceSize);
int sceIoWrite(SceUID, const void *, SceSize);
#endif
