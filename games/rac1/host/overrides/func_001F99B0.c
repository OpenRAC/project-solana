#include <stdint.h>
#include <stdlib.h>
#include "common.h"
/* 0x001F99B0 (fastfunc, hand-written): FastMemSet, a 32-bit pattern over n bytes (at least one word). */
void func_001F99B0(u32 *p, u32 v, s32 n)
{
    {   /* testing: UYA_LOADLOG=1 logs the scrubs of freed load buffers (0xDEADBEEF) */
        static int on = -1;
        if (on < 0) on = getenv("UYA_LOADLOG") != 0;
        if (on && v == 0xDEADBEEFu) { uint32_t uya_frame_count(void); uya_log("v%u fill %08X-%08X with DEADBEEF", uya_frame_count(), (u32)(uintptr_t)p, (u32)(uintptr_t)p + n); }
    }
    do { *p++ = v; n -= 4; } while (n > 0);
}
