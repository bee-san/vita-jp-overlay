/* One private workspace, shared by ncnn's model/tensor allocations. 64-byte
 * blocks satisfy its SIMD alignment, without touching SceShell's libc heap. */
#include "ocr_heap.h"
#include <stdint.h>

#define ALIGN 64u
typedef struct Block { size_t size; struct Block *prev, *next; int free; } Block;
static Block *head;
static size_t used, peak;
static int failed, busy;

int vjo_ocr_heap_begin(void *memory, size_t size)
{
    if (!memory || ((uintptr_t)memory & (ALIGN - 1)) || size < 2 * ALIGN ||
        __atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return -1;
    head = (Block *)memory;
    head->size = (size & ~(size_t)(ALIGN - 1)) - ALIGN;
    head->prev = head->next = NULL; head->free = 1;
    used = peak = 0; failed = 0;
    return 0;
}

void *vjo_ncnn_malloc(size_t n)
{
    if (!head || n > SIZE_MAX - ALIGN) { failed = 1; return NULL; }
    n = n ? (n + ALIGN - 1) & ~(size_t)(ALIGN - 1) : ALIGN;
    for (Block *b = head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + 2 * ALIGN) {
            Block *tail = (Block *)((unsigned char *)b + ALIGN + n);
            tail->size = b->size - n - ALIGN;
            tail->prev = b; tail->next = b->next; tail->free = 1;
            if (tail->next) tail->next->prev = tail;
            b->next = tail; b->size = n;
        }
        b->free = 0; used += b->size + ALIGN;
        if (used > peak) peak = used;
        return (unsigned char *)b + ALIGN;
    }
    failed = 1; return NULL;
}

void vjo_ncnn_free(void *ptr)
{
    if (!ptr) return;
    Block *b = (Block *)((unsigned char *)ptr - ALIGN);
    used -= b->size + ALIGN; b->free = 1;
    if (b->next && b->next->free) {
        b->size += ALIGN + b->next->size; b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size += ALIGN + b->size; b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
}

int vjo_ncnn_posix_memalign(void **ptr, size_t alignment, size_t n)
{
    if (!ptr || !alignment || alignment > ALIGN || (alignment & (alignment - 1))) return 22;
    void *p = vjo_ncnn_malloc(n);
    if (!p) return 12;
    *ptr = p; return 0;
}
int vjo_ocr_heap_failed(void) { return failed; }
size_t vjo_ocr_heap_end(void)
{
    size_t result = peak;
    head = NULL;
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return result;
}
