#include "common.h"
/* 0x001F9938 (fastfunc, hand-written): func_001F9908 on a 16-bit timer. */
s32 func_001F9938(s16 *p)
{
    s32 x = *p;
    if (x == 0) return 1;
    x = (x > 1 ? x : 1) - 1;
    *p = (s16)x;
    return x > 0 ? 0 : 2;
}
