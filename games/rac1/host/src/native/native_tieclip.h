/* Ties crossing the screen's edges, drawn by native code (host/src/native/native_tieclip.c)
 * instead of by the PS2's VU1 tie clip microprogram (image 224979) */
#ifndef NATIVE_TIECLIP_H
#define NATIVE_TIECLIP_H
#include "uya_host.h"

/* one GS packet the program kicked: n qwords at out + off * 4 */
typedef struct { u32 off, n; } NativeTieclipPacket;

/* the registers the program keeps from one run to the next (their bits) */
typedef struct {
    u16 vi[16];
    u32 vf21[4], vf22[4], vf23[4];      /* the output buffers, the instance buffers, their sums */
    u32 vf29[4], vf30[4], vf31[4];      /* vi4..vi15 as an instance left them (for 0x0010) */
    u32 vf1w, vf25w, vf26w;             /* words the program carries into VU memory */
} NativeTieclipRegs;

/* power-on state of the registers */
void native_tieclip_reset(void);
/* the registers as a captured VU state has them, for a test that starts from one */
void native_tieclip_seed(const NativeTieclipRegs *r);
/* the registers now, for checks */
void native_tieclip_state(NativeTieclipRegs *r);

/* one run of the program, started at pc (0x0000 an instance, 0x0010 an instance skipped,
 * 0x0020 nothing, 0x0030 a class packet, 0x0040 the program's own setup) with VU1 data memory
 * mem (16 KB, read and written as the program does) and the VIF's TOP (unused: the program
 * keeps its own instance buffers). The packets it kicks, in order, are copied to out (at most
 * out_cap qwords in all) and listed in pk (at most maxpk). Returns the number of packets, or
 * -1 for a start this file does not know (nothing done). */
int native_tieclip_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeTieclipPacket *pk, int maxpk);
#endif
