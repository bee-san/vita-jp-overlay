/* Disk-backed Yomitan term dictionaries; see docs/local-dictionaries.md. */
#ifndef VJO_LOCAL_DICT_H
#define VJO_LOCAL_DICT_H
#include "client.h"

#define VJO_LOCAL_MAX_DICTS 8
#define VJO_LOCAL_MAX_KEY 192
#define VJO_LOCAL_MAX_ENTRY 8192
#define VJO_LOCAL_MAX_TEXT 4096
#define VJO_LOCAL_MAX_RESULTS 128
#define VJO_LOCAL_MAX_MATCHES 8
#define VJO_LOCAL_RESULT_BUDGET (64u * 1024u)
#define VJO_LOCAL_SCRATCH_BUDGET (48u * 1024u)

/* Validate comma-separated, relative .vjdict filenames (no directory traversal). */
int vjo_local_names_valid(const char *names);
int vjo_local_lookup(VjoArena *a, const VjoPlatform *p, const VjoConfig *cfg,
                     const char *text, VjoDictResult *res, VjoErr *err);
#endif
