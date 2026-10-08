/* Random-access files. The Vita implementation uses sceIo, never newlib or mmap. */
#ifndef VJO_FILE_H
#define VJO_FILE_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx;
    uint64_t size;
    /* Read exactly n bytes at off; 0 on success, -1 on short read/I/O error. */
    int (*read)(void *ctx, uint64_t off, void *dst, size_t n);
    void (*close)(void *ctx);
} VjoFile;
#endif
