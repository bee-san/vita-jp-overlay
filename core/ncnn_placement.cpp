/* NCNN_SIMPLESTL declares placement operators out of line. Do not replace
 * ordinary new/delete: the shell imports those from Paf. */
#include <stddef.h>
void *operator new(size_t, void *p) { return p; }
void *operator new[](size_t, void *p) { return p; }
void operator delete(void *, void *) {}
void operator delete[](void *, void *) {}
