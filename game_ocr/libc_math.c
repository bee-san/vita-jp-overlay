/* IEEE-754 binary32 floor, without newlib/Paf initialization or integer
 * conversion overflow. Preserve signed zero, infinities and NaN payloads. */
#include <stdint.h>

float floorf(float value)
{
    union { float f; uint32_t u; } bits = {value};
    int exponent = (int)((bits.u >> 23) & 255u) - 127;
    if (exponent >= 23 || !(bits.u & 0x7fffffffu)) return value;
    if (exponent < 0) return (bits.u >> 31) ? -1.0f : 0.0f;
    uint32_t fraction = (1u << (23 - exponent)) - 1u;
    if (!(bits.u & fraction)) return value;
    if (bits.u >> 31) bits.u += fraction;
    bits.u &= ~fraction;
    return bits.f;
}
