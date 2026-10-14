#include "common.h"
/* 0x001F9968 (fastfunc, hand-written): 30 - plzcw(x), the index of the
 * highest bit that differs from the sign bit. */
s32 func_001F9968(u32 x) { return 30 - uya_plzcw(x); }
