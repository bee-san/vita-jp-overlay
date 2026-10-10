#ifndef VJO_GAME_OCR_WORKER_MAIN_H
#define VJO_GAME_OCR_WORKER_MAIN_H
#include "../include/vjo_game_ocr.h"
/* One serialized caller. run_request performs the same owned USER allocation,
 * model validation, preprocessing, engine and cleanup path as the worker.
 * It fills result and returns its VJO_E_* status; it does not publish transport
 * results. The loader owns the start/stop lifecycle. */
int vjo_game_ocr_run_request(const VjoGameOcrRequest *, VjoGameOcrResult *);
int vjo_game_ocr_worker_start(void);
/* Failure preserves every still-live UID/pointer and refuses module unload. */
int vjo_game_ocr_worker_stop(void);
#endif
