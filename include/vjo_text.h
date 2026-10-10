/* Bounded, platform-neutral text-source protocol. No framebuffer coordinates. */
#ifndef VJO_TEXT_H
#define VJO_TEXT_H
#include <stdint.h>

#define VJO_TEXT_BYTES 1024
#define VJO_TEXT_SOURCES 24
#define VJO_TEXT_CHOICES 5
#define VJO_TEXT_MATCH_PERCENT 80

enum { VJO_TEXT_AUTO = 0, VJO_TEXT_UTF8 = 1, VJO_TEXT_UTF16LE = 2, VJO_TEXT_CP932 = 3 };
enum { VJO_TEXT_MEMORY = 1, VJO_TEXT_CALL = 2, VJO_TEXT_GLYPHS = 3, VJO_TEXT_REGISTER = 4,
       VJO_TEXT_POINTER = 5 };
enum { VJO_TEXT_OFF = 0, VJO_TEXT_DISCOVER = 1, VJO_TEXT_FOLLOW = 2, VJO_TEXT_LISTEN = 3 };

typedef struct {
    uint32_t id;             /* opaque, valid only in this process session */
    uint32_t kind, encoding;
    uint32_t address;        /* memory buffer, or hook call site */
    uint32_t locator;        /* mapped allocation identity, or pointer slot */
    uint32_t updates;        /* different complete strings observed */
    uint32_t score;          /* Unicode edit similarity, 0..100 */
    uint32_t japanese;       /* Japanese characters in the normalized text */
    uint32_t length;         /* normalized code points */
    char text[VJO_TEXT_BYTES];
} VjoTextCandidate;

typedef struct {
    uint32_t size;
    int32_t pid;
    uint32_t session;        /* changes on foreground/process lifecycle */
    uint32_t sequence;
    uint32_t selected;
    uint32_t scanning;       /* discovery pass still in progress */
    uint32_t scan_address;
    uint32_t count;
    VjoTextCandidate candidates[VJO_TEXT_CHOICES];
    VjoTextCandidate current; /* selected stream, even when outside top five */
} VjoTextSnapshot;

/* Native game plugin -> kernel. The kernel copies/decodes from the caller's
 * address space. 0 bytes means a bounded NUL-terminated string. */
typedef struct {
    uint32_t size, kind, encoding, stream;
    uint32_t address, bytes;
    uint32_t indirections;
    int32_t padding;         /* added after the pointer dereferences */
} VjoTextEvent;

#if !defined(VJO_HOST)
#ifdef __cplusplus
extern "C" {
#endif
int vjoTextSubmit(const VjoTextEvent *event);
int vjoTextControl(uint32_t session, int mode, uint32_t selected);
int vjoTextReference(uint32_t session, const char *text);
int vjoTextRead(VjoTextSnapshot *snapshot);
#ifdef __cplusplus
}
#endif
#endif
#endif
