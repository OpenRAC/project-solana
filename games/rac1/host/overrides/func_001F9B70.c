#include "common.h"
/* 0x001F9B70 (fastfunc, hand-written): abs(x) */
s32 func_001F9B70(s32 x) { return x < 0 ? -x : x; }
