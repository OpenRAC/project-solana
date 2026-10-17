/* Screen-space and clipped triangle strips of an effect (VU1 microprogram image 57843), drawn
 * by native code (host/src/native/native_fx57843.c) instead of by the PS2's VU1 */
#ifndef NATIVE_FX57843_H
#define NATIVE_FX57843_H
#include "uya_host.h"
#include "rc.h"

/* one GS packet the program kicked: n qwords at out + off * 4 */
typedef struct { u32 off, n; } NativeFx57843Packet;

/* the VU1 register file as the program sees it (bits). A run reads the registers from here and
 * leaves every register the program writes as the program leaves it, so that copying them back
 * (native_fx57843_regs_to_vu) makes the VU1 registers what running the program makes them: other
 * programs read what this one leaves (vf12 / vf13 z w, for one).
 * Written by the program: setup vf7 vf18..25 vf29..31; plain strip vf1 vf2 vf5 vf6 vf8 vf9 vf12
 * vf13 (vf6 / vf13 xyz only), ACC, Q, vi1 vi6 vi7 vi12 vi15, vf31 (even counts); clipped strip
 * the same and vf3 vf4.w vf10 vf11 vf14..17 vf26..28 vf31, I, vi1..3 vi5..15, the clip flags. */
typedef struct {
    u32 vf[32][4];
    u32 vi[16];                  /* 16 bit values */
    u32 acc[4];
    u32 q, q_new, i;             /* Q; the last DIV's value as RcVu0 keeps it (q_new); I */
    u32 clip;                    /* the clip flags (24 bits: the last four CLIPWs) */
    u32 tpc;                     /* where the run ended (0x0060 / 0x0390 / 0x04A0 / 0x0708) */
} NativeFx57843Regs;

/* power-on state of the registers (all zero, vf0 = 0 0 0 1) */
void native_fx57843_reset(void);
/* the registers as a VU state has them (the game: rc_vu1 before each run, since other programs
 * share and change them) */
void native_fx57843_seed(const NativeFx57843Regs *r);
/* the registers after the run */
void native_fx57843_state(NativeFx57843Regs *r);
/* the mapping to and from the VU1 state: from_vu takes vf vi acc q q_new i clip tpc; to_vu
 * writes them back with what the end of a translated run leaves in the flag fields (cl_int =
 * clip, st_int = status; status and mac are not written by the program); the pipeline queues
 * (fp_*, cp_*, q_pend / q_rdy) are left as they are (empty after a run) */
void native_fx57843_regs_from_vu(NativeFx57843Regs *r, const RcVu0 *v);
void native_fx57843_regs_to_vu(const NativeFx57843Regs *r, RcVu0 *v);

/* one run of the program, started at pc (0x0000 the setup; 0x0060 / 0x0078 a plain strip,
 * 0x0068 / 0x0070 a clipped strip, both at TOP) with VU1 data memory mem (16 KB, read and written
 * as the program does) and the VIF's TOP. The packets it kicks, in order, are copied to out (at
 * most out_cap qwords in all) and listed in pk (at most maxpk). Returns the number of packets, or
 * -1 for a start this file does not know (nothing done). */
int native_fx57843_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeFx57843Packet *pk, int maxpk);
#endif
