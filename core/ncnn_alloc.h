/* Include the platform declarations before remapping ncnn allocations.
 * Defining these on the compiler command line breaks C++ <cstdlib>. */
#ifndef VJO_NCNN_ALLOC_H
#define VJO_NCNN_ALLOC_H
#include <stdlib.h>
#include "ocr_heap.h"
#define malloc vjo_ncnn_malloc
#define free vjo_ncnn_free
#define posix_memalign vjo_ncnn_posix_memalign
#endif
