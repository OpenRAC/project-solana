/* vu.h: runtime for the VU microprograms translated by tools/vu_recomp.py.
 *
 * Each microprogram the game uploads (VU0 through VIF0, VU1 through VIF1) is
 * translated to C at build time from the user's executable. A translation is a
 * function over an RcVu0 (VU0 uses rc_vu0, the registers it shares with the
 * EE's macro mode; VU1 has rc_vu1). Data memory is the real address range:
 * VU0 at 0x11004000 (4 KB), VU1 at 0x1100C000 (16 KB).
 *
 * The VU pipeline is visible to these programs in three places and modelled
 * here: MAC/status and clip flags written by an op are seen by flag reads 4
 * cycles later, and Q from DIV/SQRT (RSQRT) 7 (13) cycles later. The
 * translator counts cycles (with the stalls it can see statically) in a local
 * `cyc`; results are queued with the cycle they become visible at. */
#ifndef VU_H
#define VU_H
#include "rc.h"

extern RcVu0 rc_vu1;

#ifndef VU_TRACE
#define VU_TRACE(pc)   /* testing hook: each instruction pair's address */
#endif

/* VU data memory, by quadword address */
#define VU0_MEM(qa) ((u32 *)(uintptr_t)(0x11004000u + (((u32)(qa) & 0xFFu) << 4)))
#define VU1_MEM(qa) ((u32 *)(uintptr_t)(0x1100C000u + (((u32)(qa) & 0x3FFu) << 4)))

RC_INLINE u32 vu_u(f32 f) { u32 u; __builtin_memcpy(&u, &f, 4); return u; }
RC_INLINE f32 vu_f(u32 u) { f32 f; __builtin_memcpy(&f, &u, 4); return f; }

/* MAC flag bits for one component (x = 0 .. w = 3) of a result */
/* an FMAC result as the VU has it: no inf or NaN, +-MAX instead (counted: FPSLOG) */
extern u32 vu_nonfinite;
RC_INLINE f32 vu_fc(f32 x)
{
    u32 b = vu_u(x);
    if (__builtin_expect((b & 0x7F800000u) == 0x7F800000u, 0)) { vu_nonfinite++; b = (b & 0x80000000u) | 0x7F7FFFFFu; __builtin_memcpy(&x, &b, 4); }
    return x;
}
RC_INLINE u32 vu_macbits(f32 r, int c)
{
    u32 u = vu_u(r), m = 0;
    if ((u & 0x7FFFFFFFu) == 0) m |= 8u >> c;
    if (u >> 31) m |= 0x80u >> c;
    return m;
}

/* ---- MAC/status flag pipeline ---- */
RC_INLINE void vu_fl_sync(RcVu0 *v, u32 cyc)
{
    while (v->fp_n && (s32)(cyc - v->fp_rdy[v->fp_h]) >= 0) {
        v->mac = v->fp_mac[v->fp_h]; v->status = v->fp_st[v->fp_h];
        v->fp_h = (v->fp_h + 1) & 3; v->fp_n--;
    }
}
RC_INLINE void vu_fl_push(RcVu0 *v, u32 cyc, u32 mac)
{
    u32 st = (v->st_int & 0xFC0u) | ((mac & 0xF) ? 1 : 0) | ((mac & 0xF0) ? 2 : 0);
    st |= (st & 3) << 6;
    v->st_int = st;
    if (v->fp_n == 4) { v->mac = v->fp_mac[v->fp_h]; v->status = v->fp_st[v->fp_h]; v->fp_h = (v->fp_h + 1) & 3; v->fp_n--; }
    u32 k = (v->fp_h + v->fp_n) & 3;
    v->fp_rdy[k] = cyc + 4; v->fp_mac[k] = mac; v->fp_st[k] = st; v->fp_n++;
}
RC_INLINE u32 vu_status(RcVu0 *v, u32 cyc) { vu_fl_sync(v, cyc); return v->status; }
RC_INLINE u32 vu_mac(RcVu0 *v, u32 cyc) { vu_fl_sync(v, cyc); return v->mac; }
RC_INLINE void vu_fsset(RcVu0 *v, u32 cyc, u32 imm)
{
    vu_fl_sync(v, cyc + 8);
    v->status = (v->status & 0x3F) | (imm & 0xFC0); v->st_int = v->status;
}

/* ---- clip flag pipeline ---- */
RC_INLINE void vu_cl_sync(RcVu0 *v, u32 cyc)
{
    while (v->cp_n && (s32)(cyc - v->cp_rdy[v->cp_h]) >= 0) {
        v->clip = v->cp_val[v->cp_h]; v->cp_h = (v->cp_h + 1) & 3; v->cp_n--;
    }
}
RC_INLINE void vu_clipw(RcVu0 *v, u32 cyc, const f32 *fs, f32 w)
{
    f32 a = __builtin_fabsf(w);
    u32 f = (fs[0] > a) | ((fs[0] < -a) << 1) | ((fs[1] > a) << 2) | ((fs[1] < -a) << 3) | ((fs[2] > a) << 4) | ((fs[2] < -a) << 5);
    v->cl_int = ((v->cl_int << 6) | f) & 0xFFFFFF;
    if (v->cp_n == 4) { v->clip = v->cp_val[v->cp_h]; v->cp_h = (v->cp_h + 1) & 3; v->cp_n--; }
    u32 k = (v->cp_h + v->cp_n) & 3;
    v->cp_rdy[k] = cyc + 4; v->cp_val[k] = v->cl_int; v->cp_n++;
}
RC_INLINE u32 vu_clip(RcVu0 *v, u32 cyc) { vu_cl_sync(v, cyc); return v->clip; }
RC_INLINE void vu_fcset(RcVu0 *v, u32 cyc, u32 imm)
{
    vu_cl_sync(v, cyc + 8);
    v->clip = v->cl_int = imm & 0xFFFFFF;
}

/* ---- Q ---- */
RC_INLINE f32 vu_q(RcVu0 *v, u32 cyc)
{
    if (v->q_pend && (s32)(cyc - v->q_rdy) >= 0) { v->q = v->q_new; v->q_pend = 0; }
    return v->q;
}
/* WAITQ, or a new DIV while one is in flight: stall until Q is written */
RC_INLINE u32 vu_qwait(RcVu0 *v, u32 cyc)
{
    if (v->q_pend) { if ((s32)(cyc - v->q_rdy) < 0) cyc = v->q_rdy; v->q = v->q_new; v->q_pend = 0; }
    return cyc;
}
RC_INLINE void vu_qset(RcVu0 *v, u32 cyc, int lat, f32 val) { v->q_new = val; v->q_pend = 1; v->q_rdy = cyc + lat; }

/* end of a microprogram: everything in flight lands */
RC_INLINE void vu_settle(RcVu0 *v)
{
    while (v->fp_n) { v->mac = v->fp_mac[v->fp_h]; v->status = v->fp_st[v->fp_h]; v->fp_h = (v->fp_h + 1) & 3; v->fp_n--; }
    while (v->cp_n) { v->clip = v->cp_val[v->cp_h]; v->cp_h = (v->cp_h + 1) & 3; v->cp_n--; }
    if (v->q_pend) { v->q = v->q_new; v->q_pend = 0; }
    v->fp_h = v->cp_h = 0;
}

RC_INLINE f32 vu_div(f32 a, f32 b)
{
    if (b == 0.0f) return ((vu_u(a) ^ vu_u(b)) & 0x80000000u) ? -3.4028235e38f : 3.4028235e38f;
    return a / b;
}
RC_INLINE f32 vu_sqrt(f32 a) { return __builtin_sqrtf(__builtin_fabsf(a)); }
RC_INLINE f32 vu_rsqrt(f32 a, f32 b)
{
    f32 s = __builtin_sqrtf(__builtin_fabsf(b));
    if (s == 0.0f) return (vu_u(a) & 0x80000000u) ? -3.4028235e38f : 3.4028235e38f;
    return a / s;
}
RC_INLINE s32 vu_ftoi(f32 f, f32 scale)
{
    f32 x = f * scale;
    if (x >= 2147483647.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return (s32)0x80000000u;
    if (x != x) return 0;
    return (s32)x;
}
RC_INLINE u32 vu_itof(u32 i, f32 inv) { return vu_u((f32)(s32)i * inv); }

/* host side (host/src/gfx/vu.c) */
void vu_xgkick(u32 qaddr);
u32 vu_vif_top(int vu);
u32 vu_vif_itop(int vu);
void vu_bad(int vu, u32 pc, const char *what);
#endif
