#include "common.h"
/* 0x001F9A00 (fastfunc, hand-written): copy n bytes, by quadwords, words or
 * bytes depending on alignment. */
void func_001F9A00(void *dst, const void *src, s32 n)
{
    if (n > 0) __builtin_memmove(dst, src, (u32)n);
}
