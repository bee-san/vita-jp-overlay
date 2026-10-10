#include <psp2kern/host_stubs.h>
#ifndef VJO_TEST_CTRL_H
#define VJO_TEST_CTRL_H
typedef struct {
    uint64_t timeStamp;
    uint32_t buttons;
    uint8_t lx, ly, rx, ry;
    uint8_t reserved[16];
} SceCtrlData;
#define SCE_CTRL_SELECT 0x0001u
#define SCE_CTRL_START 0x0008u
#define SCE_CTRL_UP 0x0010u
#define SCE_CTRL_DOWN 0x0040u
#define SCE_CTRL_LTRIGGER 0x0100u
#define SCE_CTRL_RTRIGGER 0x0200u
#define SCE_CTRL_CIRCLE 0x2000u
#define SCE_CTRL_CROSS 0x4000u
#define SCE_CTRL_MODE_DIGITAL 0
#define SCE_CTRL_MODE_ANALOG 1
int ksceCtrlGetSamplingMode(int *);
int ksceCtrlSetSamplingMode(int);
int ksceCtrlPeekBufferPositive(int, SceCtrlData *, int);
#endif
