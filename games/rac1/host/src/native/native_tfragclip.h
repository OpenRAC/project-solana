/* Terrain fragments that cross the view frustum, drawn by native code (host/src/native/
 * native_tfragclip.c) instead of by the PS2's VU1 tfrag clip microprogram (image 903379) */
#ifndef NATIVE_TFRAGCLIP_H
#define NATIVE_TFRAGCLIP_H
#include "uya_host.h"

/* one GS packet the program kicked: n qwords at out + off * 4 */
typedef struct { u32 off, n; } NativeTfragclipPacket;

/* power-on state of the registers the program keeps between runs */
void native_tfragclip_reset(void);
/* the VU1 registers this program carries from run to run: vf3 (its two output buffers, ints),
 * the clip flags (24 bits), vi15 (the strip loop's return address; the program stores it in
 * VU memory) and the w of vf26 and vf27 (never written by the program; it stores them as the
 * unused fourth word of every fourth vertex's ST). Other VU1 programs change these, so a
 * caller that runs them in between seeds these from the VU1 state before a run. */
typedef struct { u32 vf3[4]; u32 clip, vi15, vf26w, vf27w; } NativeTfragclipRegs;
void native_tfragclip_seed(const NativeTfragclipRegs *r);
void native_tfragclip_state(NativeTfragclipRegs *r);

/* one run of the program, started at pc (the VIF's MSCAL/MSCNT address) with VU1 data memory
 * mem (16 KB, read and written as the program does) and the VIF's TOP. The packets it kicks,
 * in order, are copied to out (at most out_cap qwords in all) and listed in pk (at most maxpk).
 * Returns the number of packets, or -1 for a start this file does not know. */
int native_tfragclip_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeTfragclipPacket *pk, int maxpk);
#endif
