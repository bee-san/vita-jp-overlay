#ifndef VJO_TEST_PAF_STDLIB_H
#define VJO_TEST_PAF_STDLIB_H
#include <cstddef>
extern "C" void *sce_paf_memalign(size_t alignment, size_t bytes);
extern "C" void sce_paf_free(void *pointer);
#endif
