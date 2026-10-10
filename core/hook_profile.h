#ifndef VJO_HOOK_PROFILE_H
#define VJO_HOOK_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#define VJO_NATIVE_HOOKS 8
typedef struct {
    uint32_t segment, offset, thumb, reg, encoding, indirections;
    int32_t padding;
    uint8_t signature[16];
    unsigned signature_bytes;
} VjoNativeHook;
typedef struct {
    char title[12];
    uint32_t module_nid;
    unsigned count;
    VjoNativeHook hooks[VJO_NATIVE_HOOKS];
} VjoNativeProfile;
/* Strict local profile: VJOHOOK1 TITLE MODULE_NID, followed by at most eight
 * seg offset thumb register encoding dereferences padding signature lines.
 * Numbers are hexadecimal (padding may be negative). Require an 8..16-byte
 * original-instruction signature; never blindly install emulator addresses. */
int vjo_hook_profile_parse(VjoNativeProfile *profile, const char *text, size_t bytes);
#endif
