/* Mobys drawn natively (host/src/native/native_moby.c) */
#ifndef NATIVE_MOBY_H
#define NATIVE_MOBY_H
#include "uya_host.h"
#define NATIVE_MOBY_MAXPK 64
typedef struct {
    u32 npk;                          /* GIF packets kicked, in order */
    u32 len[NATIVE_MOBY_MAXPK];       /* each one's length in qwords */
    u32 off[NATIVE_MOBY_MAXPK];       /* where it starts in d[] (qwords) */
    u32 nq;                           /* qwords used in d[] */
    u32 d[NATIVE_MOBY_MAXPK * 0x100 * 4];
} NativeMobyKicks;
/* one start of the moby microprogram (VU1 image 13859) at pc (0x0000 setup, 0x0050 / 0x0070
 * draw one packet, with / without clipping), with VU1 data memory vumem (16 KB, read and
 * written as the program does) and the VIF TOP; the GIF packets it kicks go to *k.
 * Returns 0, or -1 for a start it does not know (nothing done). */
int native_moby_run(u32 pc, u8 *vumem, u32 top, NativeMobyKicks *k);
void native_moby_reset(void);
/* the VU registers the program carries from one run to the next, for a test that starts from
 * a captured VU state: vf27 (the output buffers), vf12.w (a word the GS never reads) */
void native_moby_seed(const u32 vf27[4], u32 vf12w);
#endif
