#ifndef VJO_TEST_RTC_H
#define VJO_TEST_RTC_H
#include "host_stubs.h"
typedef struct { uint64_t tick; } SceRtcTick;
int sceRtcGetCurrentTick(SceRtcTick *tick);
#endif
