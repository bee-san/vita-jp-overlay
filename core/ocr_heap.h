#ifndef VJO_OCR_HEAP_H
#define VJO_OCR_HEAP_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int vjo_ocr_heap_begin(void *memory, size_t size);
size_t vjo_ocr_heap_end(void);
int vjo_ocr_heap_failed(void);
void *vjo_ncnn_malloc(size_t size);
void vjo_ncnn_free(void *ptr);
int vjo_ncnn_posix_memalign(void **ptr, size_t alignment, size_t size);
#ifdef __cplusplus
}
#endif
#endif
