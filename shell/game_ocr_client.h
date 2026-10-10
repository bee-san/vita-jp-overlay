#ifndef VJO_GAME_OCR_CLIENT_H
#define VJO_GAME_OCR_CLIENT_H
#include "../include/vjo_game_ocr.h"

typedef struct {
    void *ud;
    int (*submit)(void *, const VjoGameOcrRequest *);
    int (*read)(void *, uint32_t, VjoGameOcrResult *);
    int (*cancel)(void *, uint32_t);
    int64_t (*now_us)(void *);
    void (*delay_us)(void *, uint32_t);
} VjoGameOcrClient;

/* On abort, Cancel only signals the worker. The kernel keeps its raw capture
 * pinned until the worker has stopped the engine and released owned memory. */
int vjo_game_ocr_exchange(const VjoGameOcrClient *client,
                          const VjoGameOcrRequest *request,
                          VjoGameOcrResult *result,
                          int (*cancelled)(void *), void *cancel_ud,
                          int64_t timeout_us);
#endif
