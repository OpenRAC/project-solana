/* Terrain (tfrag) drawn by native code (host/src/native/native_tfrag.c) instead of by the
 * PS2's VU1 tfrag microprogram (image 55907) */
#ifndef NATIVE_TFRAG_H
#define NATIVE_TFRAG_H
#include "uya_host.h"

/* one GS packet the program kicked: n qwords at out + off * 4 */
typedef struct { u32 off, n; } NativeTfragPacket;

/* power-on state of the registers the program keeps between runs */
void native_tfrag_reset(void);
/* those registers (the two output buffers, the color scale), for checks */
void native_tfrag_state(u32 buf[4], float cscale[4]);

/* one run of the program, started at pc (the VIF's MSCAL/MSCNT address) with VU1 data memory
 * mem (16 KB, read and written as the program does) and the VIF's TOP. The packets it kicks,
 * in order, are copied to out (at most out_cap qwords in all) and listed in pk (at most maxpk).
 * Returns the number of packets, or -1 for a start this file does not know. */
int native_tfrag_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeTfragPacket *pk, int maxpk);
#endif
