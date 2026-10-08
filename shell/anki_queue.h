#ifndef VJO_SHELL_ANKI_QUEUE_H
#define VJO_SHELL_ANKI_QUEUE_H
#include "../core/anki_queue.h"

/* All calls are serialized by the Anki thread. No whole-queue allocation. */
int vjo_queue_count(void); /* -1 on I/O error */
int vjo_queue_first(char id[VJO_QUEUE_ID_SIZE]); /* 1 = found, 0 = empty, -1 = error */
/* JSON is committed last. Returns 0 saved, 1 already queued, 2 archived duplicate, -1 on failure. */
int vjo_queue_save(VjoArena *a, const VjoAnkiNote *note, const VjoAnkiMedia *media);
int vjo_queue_load(VjoArena *a, const char *id, VjoAnkiNote *note, VjoAnkiMedia *media);
/* Only after confirmed addNote/findNotes success. JSON removed before JPEG. */
int vjo_queue_remove(const char *id);
/* Anki refused a duplicate: retain its JSON and JPEG for inspection. */
int vjo_queue_archive_duplicate(const char *id);
#endif
