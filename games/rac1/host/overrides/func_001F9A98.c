#include "common.h"
/* 0x001F9A98 (fastfunc, hand-written): copy n bytes in quadwords (at least one). */
void func_001F9A98(void *dst, const void *src, s32 n)
{
    u8 *d = dst; const u8 *s = src;
    do { __builtin_memcpy(d, s, 16); d += 16; s += 16; n -= 16; } while (n > 0);
}
