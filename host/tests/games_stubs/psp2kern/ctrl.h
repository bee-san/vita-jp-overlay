#ifndef VJO_GAMES_TEST_CTRL_H
#define VJO_GAMES_TEST_CTRL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef struct {
    uint64_t timeStamp;
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t reserved[16];
} SceCtrlData;
#define SCE_CTRL_SELECT 0x0001u
#define SCE_CTRL_START 0x0008u
#define SCE_CTRL_LTRIGGER 0x0100u
#define SCE_CTRL_RTRIGGER 0x0200u
#define SCE_CTRL_CROSS 0x4000u
#define SCE_CTRL_MODE_DIGITAL 0
#define SCE_CTRL_MODE_ANALOG 1
int ksceCtrlGetSamplingMode(int *);
int ksceCtrlSetSamplingMode(int);
int ksceCtrlPeekBufferPositive(int, SceCtrlData *, int);
SceUID ksceKernelGetProcessId(void);
SceUID ksceKernelGetThreadId(void);
uint64_t ksceKernelGetSystemTimeWide(void);
int ksceKernelMemcpyUserToKernel(void *, const void *, SceSize);
int ksceKernelMemcpyKernelToUser(void *, const void *, SceSize);
int ksceKernelSetEventFlag(SceUID, unsigned);
#endif
