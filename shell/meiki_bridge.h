/* The caller owns buffers; all SDK C++ runs in the isolated module. */
#ifndef VJO_MEIKI_BRIDGE_H
#define VJO_MEIKI_BRIDGE_H
#include "../core/meiki_ocr.h"
#include "../core/net.h"
#include "module/api.h"
#include <psp2/kernel/threadmgr.h>

#define VJO_MEIKI_MODULE_PATH "ur0:data/VitaJPOverlay/meiki-engine.suprx"
typedef struct {
    void *ud;
    void *(*alloc)(void *ud, size_t bytes);
    /* Zero means released. Failure leaves the exact pointer owned and live. */
    int (*free)(void *ud, void *pointer);
    void (*report)(void *ud); /* optional, allocation-free */
} VjoMeikiAllocator;
typedef struct {
    void *metadata, *workspace, *preprocessing;
    float *input;
    void *scratch;
    SceUID module;
    MeikiModuleApi api;
    MeikiModuleStats module_stats;
    char model_path[256], detect_model_path[256];
    size_t input_elements;
    int stopped;
    VjoMeikiAllocator allocator;
} VjoMeikiBridge;

/* Serial worker only. A failed stop/unload keeps every borrowed buffer alive
 * and refuses another load; no callback can then access freed caller memory. */
int vjo_meiki_bridge_start(VjoMeikiBridge *bridge, const VjoPlatform *platform,
                           const char *model_dir, int (*cancelled)(void *), void *ud);
int vjo_meiki_bridge_start_mode(VjoMeikiBridge *bridge, const VjoPlatform *platform,
                                const char *model_dir, int dialogue_box,
                                int (*cancelled)(void *), void *ud);
VjoMeikiEngine vjo_meiki_bridge_engine(VjoMeikiBridge *bridge);
int vjo_meiki_bridge_finish(VjoMeikiBridge *bridge);
/* Set only while unloaded and every prior allocation has been released. The
 * copied callbacks/context remain the provenance for all three buffers. NULL
 * selects the normal Paf allocator; game builds require explicit callbacks. */
int vjo_meiki_bridge_set_allocator(VjoMeikiBridge *bridge,
                                   const VjoMeikiAllocator *allocator);
#endif
