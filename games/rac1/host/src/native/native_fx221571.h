/* Glints / sparkles (billboard quads, streaks and lines), drawn by native code
 * (host/src/native/native_fx221571.c) instead of by the PS2's VU1 effect microprogram
 * (image 221571) */
#ifndef NATIVE_FX221571_H
#define NATIVE_FX221571_H
#include "uya_host.h"
#include "rc.h"

/* one GS packet the program kicked: n qwords at out + off * 4 */
typedef struct { u32 off, n; } NativeFx221571Packet;

/* the VU1 register file as the program leaves it (bits). What carries from one run of this
 * program to the next: the setup (0x0000) loads vf16..vf31 from VU memory 0..15, sets vi8 / vi14
 * / vi15 and multiplies vf12.zw and vf13.zw by 0 (keeping their signs, or -MAX for inf); a
 * particle run (0x00A0) reads those, flips vi15 (the double buffer). Everything the program
 * writes is kept too, for the VU1 programs that run after it:
 *   setup:     vf12.zw vf13.zw vf16..vf31, vi8 vi14 vi15, TPC 0x00A0;
 *   particles: vf1..vf8 (which of vf2..vf8 depends on the modes), vf12.xy vf13.xy (screen
 *              quads), vf14 vf15, vf19.x vf20.xy, vi1 vi2 vi9..vi13 vi15, ACC, Q, TPC (where the
 *              program stopped: 0x01F8 / 0x03E0 / 0x0508 / 0x06C8 after a screen quad / world
 *              quad / line / streak).
 * The program sets no MAC / status / clip flags and does not touch I, P, R, vf0, vi0. */
typedef struct {
    u32 vf[32][4];
    u32 acc[4];
    u32 q;                             /* Q: the last DIV (vf25.x / w) */
    u32 tpc;
    u16 vi[16];
    u32 qdiv;                          /* (this run did a DIV: Q's pipeline value is Q) */
} NativeFx221571Regs;

/* the mapping to the translated programs' register file. regs_from_vu takes every register;
 * regs_to_vu writes back what a run of the program changes: vf1..vf31, vi1..vi15, ACC, Q, TPC,
 * and the pipeline as the program's end leaves it (st_int / cl_int latched from status / clip,
 * flags in flight landed, Q's DIV landed: q_pend 0, q_new; q_rdy, a cycle count, is left). A
 * hook: regs_from_vu(rc_vu1) + seed before a run, state + regs_to_vu(rc_vu1) after it. */
void native_fx221571_regs_from_vu(NativeFx221571Regs *r, const RcVu0 *v);
void native_fx221571_regs_to_vu(const NativeFx221571Regs *r, RcVu0 *v);

/* power-on state of the registers (all zero but vf0.w = 1) */
void native_fx221571_reset(void);
/* the registers as a captured VU state has them, for a test that starts from one */
void native_fx221571_seed(const NativeFx221571Regs *r);
/* the registers now, for checks */
void native_fx221571_state(NativeFx221571Regs *r);

/* one run of the program, started at pc (0x0000 the setup, 0x00A0 a particle buffer) with VU1
 * data memory mem (16 KB, read and written as the program does) and the VIF's TOP (unused: the
 * program keeps its own double buffer). The packets it kicks, in order, are copied to out (at
 * most out_cap qwords in all) and listed in pk (at most maxpk). Returns the number of packets
 * (0 or 1), or -1 for a start this file does not know (nothing done). A buffer whose walk never
 * reaches its end (the program would loop forever) is left after 65536 particles, unkicked. */
int native_fx221571_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeFx221571Packet *pk, int maxpk);
#endif
