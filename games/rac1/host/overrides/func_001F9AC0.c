#include "common.h"
/* 0x001F9AC0 (fastfunc, hand-written): dst = a | b over n bytes, in quadwords (at least one). */
void func_001F9AC0(u32 *dst, const u32 *a, const u32 *b, s32 n)
{
    do {
        for (int i = 0; i < 4; i++) dst[i] = a[i] | b[i];
        dst += 4; a += 4; b += 4; n -= 16;
    } while (n > 0);
}
