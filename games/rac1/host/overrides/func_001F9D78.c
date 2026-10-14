#include "common.h"
/* 0x001F9D78 (fastfunc, VU0): do two spheres (xyz centre, w radius) overlap?
 * 1 when the squared distance is less than the squared sum of the radii. */
s32 func_001F9D78(const f32 *a, const f32 *b)
{
    f32 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], r = a[3] + b[3];
    return dx * dx + dy * dy + dz * dz - r * r < 0.0f ? 1 : 0;
}
