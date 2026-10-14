#include <stdint.h>
#include <stdlib.h>
/* func_0020C468(src, dst): unpack a "WAD" compressed block (Insomniac's
 * LZ77 variant, close to LZO1X) and return the unpacked size.
 *
 * Ported from the UYA Wrapper (UYA's copy is func_0039B760). RAC1's retail
 * version is the same hand-written asm: it checks the "WAD" magic and streams
 * the input through the scratchpad in 0x2000-byte halves by DMA. Here the
 * input is read straight from memory; a far match with distance 0 and length
 * above 1 is the encoder's "go to the next 0x2000-byte chunk" marker, which in
 * a linear read means skipping to the next chunk boundary of the data (which
 * starts 0x10 bytes into the block). */
#include "common.h"

s32 func_0020C468(u8 *src, u8 *dst)
{
    u8 *start = dst;
    {   /* testing: UYA_LOADLOG=1 */
        static int on = -1;
        if (on < 0) on = getenv("UYA_LOADLOG") != 0;
        if (on) { uint32_t uya_frame_count(void); const char *rc_stack_scan(void); uya_log("v%u WAD unpack %08X -> %08X: %.3s; stack%s", uya_frame_count(), (u32)(uintptr_t)src, (u32)(uintptr_t)dst, (const char *)src, rc_stack_scan()); }
    }
    if (src[0] != 'W' || src[1] != 'A' || src[2] != 'D') return 0;
    u32 size = src[3] | (src[4] << 8) | (src[5] << 16) | ((u32)src[6] << 24);
    u8 *data = src + 0x10, *in = data, *end = src + size;
    u32 flag, n;
    u8 *m;

    if (in[0] > 0x11) {                           /* leading literal run */
        n = *in++ - 0x11;
        while (n--) *dst++ = *in++;
        goto after_literals;
    }
    for (;;) {
        /* state A: a literal run may come next */
        if (in >= end) break;
        flag = *in++;
        if (flag < 0x10) {
            n = flag ? flag : *in++ + 0xF;
            n += 3;
            while (n--) *dst++ = *in++;
after_literals:
            if (in >= end) break;
            flag = *in++;
        }
        /* a match */
        for (;;) {
            if (flag >= 0x40) {
                m = dst - 1 - ((flag >> 2) & 7);
                m -= *in++ << 3;
                n = (flag >> 5) - 1;
            } else if (flag >= 0x20) {
                n = flag & 0x1F;
                if (!n) n = *in++ + 0x1F;
                m = dst - 1 - ((in[0] >> 2) + (in[1] << 6));
                in += 2;
            } else if (flag >= 0x10) {
                m = dst - ((flag & 8) << 11);
                n = flag & 7;
                if (!n) n = *in++ + 7;
                m -= (in[0] >> 2) + (in[1] << 6);
                in += 2;
                if (m == dst) {
                    if (n != 1) {                 /* next-chunk marker */
                        u32 off = (u32)(in - data);
                        in = data + ((off + 0x1FFF) & ~0x1FFFu);
                        goto state_a;
                    }
                    goto trailing;                /* distance 0, length 1: no copy */
                }
                m -= 0x4000;
            } else {
                uya_unimpl("func_0020C468: bad WAD stream");
                return (s32)(dst - start);
            }
            *dst++ = *m++;
            *dst++ = *m++;
            do { *dst++ = *m++; } while ((s32)--n > 0);
trailing:
            n = in[-2] & 3;                       /* up to 3 literals after a match */
            if (!n) goto state_a;
            while (n--) *dst++ = *in++;
            if (in >= end) return (s32)(dst - start);
            flag = *in++;
        }
state_a:;
    }
    return (s32)(dst - start);
}
