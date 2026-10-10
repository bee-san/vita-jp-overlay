/* Bounded CPU-data allocation diagnostics. No OCR, GPU or foreign mappings. */
#ifndef VJO_MEMORY_PROBE_H
#define VJO_MEMORY_PROBE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VJO_MEMORY_PROBE_USER 0x0C20D060u
#define VJO_MEMORY_PROBE_PHYCONT 0x0C80D060u
#define VJO_MEMORY_PROBE_CDRAM 0x09408060u
#define VJO_MEMORY_PROBE_HEADROOM (2u * 1024u * 1024u)
/* Frozen ABI2: 16 MiB model/tensors + 4 MiB metadata + single-line input/scratch. */
#define VJO_MEMORY_PROBE_ENGINE_BYTES (20u * 1024u * 1024u + 368640u + 13440u)

enum {
    VJO_MEMORY_PROBE_OK = 0,
    VJO_MEMORY_PROBE_UNAVAILABLE = -1,
    VJO_MEMORY_PROBE_CLEANUP = -2,
    VJO_MEMORY_PROBE_INVALID = -3,
    VJO_MEMORY_PROBE_CANARY = -4,
    VJO_MEMORY_PROBE_BUSY = -5
};
enum {
    VJO_MEMORY_PROBE_SNAPSHOT, VJO_MEMORY_PROBE_ALLOC,
    VJO_MEMORY_PROBE_BASE, VJO_MEMORY_PROBE_INFO,
    VJO_MEMORY_PROBE_TOUCH, VJO_MEMORY_PROBE_FREE,
    VJO_MEMORY_PROBE_UID, VJO_MEMORY_PROBE_SKIP,
    VJO_MEMORY_PROBE_COMPLETE, VJO_MEMORY_PROBE_RETAINED
};
typedef struct {
    int32_t user, cdram, phycont;
} VjoMemoryProbeFree;
typedef struct {
    void *base;
    uint32_t bytes, memory_type, access, type;
} VjoMemoryProbeInfo;
typedef struct {
    unsigned stage;
    const char *name;
    uint32_t type, bytes;
    int32_t uid, rc;
    VjoMemoryProbeInfo info;
    VjoMemoryProbeFree free;
} VjoMemoryProbeEvent;
typedef struct {
    void *ud;
    int (*snapshot)(void *, VjoMemoryProbeFree *);
    int32_t (*alloc)(void *, const char *, uint32_t, uint32_t);
    int (*base)(void *, int32_t, void **);
    int (*info)(void *, void *, uint32_t, VjoMemoryProbeInfo *);
    int (*free)(void *, int32_t);
    void (*report)(void *, const VjoMemoryProbeEvent *);
} VjoMemoryProbeBackend;
typedef struct {
    int started, free_attempted, free_accepted;
    int32_t held_uid;
    void *held_base;
    uint32_t held_type, held_bytes;
    const char *held_name;
} VjoMemoryProbeState;

/* Serialized caller. Initialize held_uid to -1. Negative cleanup status keeps
 * the exact UID/base; never restart allocations or free foreign/reused UIDs. */
int vjo_memory_probe_once_with(const VjoMemoryProbeBackend *, VjoMemoryProbeState *);
int vjo_memory_probe_release_with(const VjoMemoryProbeBackend *, VjoMemoryProbeState *);
/* Native wrappers use guarded SDK imports, fixed bounds and static ownership.
 * Invoke explicitly while OCR/Anki work is idle. A repeat only reports state. */
int vjo_memory_probe_once(void);
int vjo_memory_probe_release(void);

#ifdef __cplusplus
}
#endif
#endif
