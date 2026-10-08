/* Hachidori Relay v0.0.5 Yomitan-compatible HTTP dictionary lookup. */
#ifndef VJO_HACHIDORI_H
#define VJO_HACHIDORI_H

#include "client.h"

#define VJO_HACHIDORI_MAX_TEXT 4096u
#define VJO_HACHIDORI_MAX_UNITS 1024
#define VJO_HACHIDORI_MAX_MEANINGS 64
#define VJO_HACHIDORI_MAX_GLOSS 16384u

int vjo_hachidori_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                         const char *text, VjoDictResult *res, VjoErr *err);
#endif
