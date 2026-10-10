#ifndef VJO_CATALOG_H
#define VJO_CATALOG_H
#include "../core/games.h"
typedef struct {
    char id[VJO_TITLE_ID_SIZE];
    char title[256];
    char icon[160];
} VjoInstalledGame;
/* Installed ux0/ur0 titles and inserted gro0 cartridge, deduplicated by ID. */
int vjo_catalog_scan(VjoInstalledGame *out, int capacity, int *truncated);
#endif
