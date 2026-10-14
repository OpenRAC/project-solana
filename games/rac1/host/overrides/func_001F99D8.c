#include "common.h"
/* 0x001F99D8 (fastfunc, hand-written): clear n bytes in quadwords (at least one). */
void func_001F99D8(void *p, s32 n)
{
    u8 *q = p;
    do { __builtin_memset(q, 0, 16); q += 16; n -= 16; } while (n > 0);
}
