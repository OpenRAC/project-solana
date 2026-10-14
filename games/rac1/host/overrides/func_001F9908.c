#include "common.h"
/* 0x001F9908 (fastfunc, hand-written): count down the timer *p.
 * 0 stays 0 and returns 1; otherwise it goes down by one and the result is 2
 * when it reaches 0, else 0. */
s32 func_001F9908(s32 *p)
{
    s32 x = *p;
    if (x == 0) return 1;
    x = (x > 1 ? x : 1) - 1;
    *p = x;
    return x > 0 ? 0 : 2;
}
