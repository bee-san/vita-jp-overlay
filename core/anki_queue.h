/* On-disk queue records are UTF-8 JSON; JPEGs are separate files. */
#ifndef VJO_ANKI_QUEUE_H
#define VJO_ANKI_QUEUE_H
#include "anki.h"

#define VJO_QUEUE_ID_SIZE 33 /* 128 bits of SHA-256, lowercase hex + NUL */
#define VJO_QUEUE_TEXT_MAX (32u * 1024u)
#define VJO_QUEUE_PICTURE_MAX (192u * 1024u)

/* Content-derived ID: repeated saves of the same note keep the first screenshot. */
char *vjo_queue_encode(VjoArena *a, const VjoAnkiNote *note, int picture);
int vjo_queue_decode(VjoArena *a, const char *json, size_t len, VjoAnkiNote *note, int *picture);
void vjo_queue_id(const char *json, size_t len, char id[VJO_QUEUE_ID_SIZE]);
int vjo_queue_valid_id(const char *id);
#endif
