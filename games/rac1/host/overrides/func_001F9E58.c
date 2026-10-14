#include "common.h"
/* 0x001F9E58 (fastfunc, VU0): clamp the length of src.xyz to max into dst.
 * Returns 1 when it was scaled, 0 when it was already short enough (or zero);
 * dst is only written when scaled. */
s32 func_001F9E58(f32 *dst, const f32 *src, f32 max)
{
    f32 l2 = src[0] * src[0] + src[1] * src[1] + src[2] * src[2];
    f32 len = __builtin_sqrtf(l2);
    if (len == 0.0f) return 0;
    if (max - len > 0.0f) return 0;
    f32 q = max / len;
    dst[0] = src[0] * q; dst[1] = src[1] * q; dst[2] = src[2] * q; dst[3] = src[3];
    return 1;
}
