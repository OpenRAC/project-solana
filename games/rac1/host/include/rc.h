/* rc.h: runtime for code produced by tools/recomp.py.
 *
 * recomp.py translates the game's assembly-only functions (MIPS R5900, with
 * MMI and VU0 macro instructions) into C ahead of time. Each becomes a C
 * function over an RcCtx (the EE's registers); it is compiled with the rest of
 * the program, so nothing is translated or interpreted while the game runs.
 * Memory is the PS2's own address space, mapped at the same addresses. */
#ifndef RC_H
#define RC_H
#include <stdint.h>
#include <string.h>
#include <math.h>

/* the helpers below are tiny: always inline them (the generated code is built
 * with -fno-builtin -fno-inline-functions, which otherwise leaves each one,
 * and the memcpy in it, a call) */
#define RC_INLINE static inline __attribute__((always_inline))

typedef int8_t s8; typedef uint8_t u8; typedef int16_t s16; typedef uint16_t u16;
typedef int32_t s32; typedef uint32_t u32; typedef int64_t s64; typedef uint64_t u64;
typedef float f32;

typedef union {
    u64 d[2]; s64 sd[2]; u32 w[4]; s32 sw[4]; u16 h[8]; s16 sh[8]; u8 b[16]; s8 sb[16];
} R128;

typedef struct RcCtx {
    R128 r[32];
    R128 hi, lo;            /* hi/lo in the low doubleword, hi1/lo1 in the high one */
    u32 sa;
    f32 f[32];
    f32 facc;               /* the EE FPU accumulator */
    int fcc;                /* FPU condition */
} RcCtx;

typedef struct {
    f32 vf[32][4];
    f32 acc[4];
    f32 q, i, p, r;
    u32 vi[16];
    u32 clip, status, mac;
    /* micro mode (tools/vu_recomp.py, host/include/vu.h): CMSAR0, where the
     * last microprogram ended (MSCNT), and the results still in the pipeline:
     * MAC/status and clip flags become visible 4 cycles after the op issues,
     * Q 7 (13 for RSQRT) cycles after the DIV. */
    u32 cmsar0, tpc;
    int susp;               /* VU0: parked in a wait loop for the EE (tpc = where to resume) */
    u32 st_int, cl_int;
    u32 fp_n, fp_h, fp_rdy[4], fp_mac[4], fp_st[4];
    u32 cp_n, cp_h, cp_rdy[4], cp_val[4];
    u32 q_pend, q_rdy; f32 q_new;
} RcVu0;
extern RcVu0 rc_vu0;

/* the shared stack pointer for nested C <-> recompiled calls */
extern u32 rc_sp;
#define RC_GP 0x00166D00u

void rc_dispatch(RcCtx *c, u32 addr);           /* call any function by PS2 address */
void rc_vcallms(RcCtx *c, u32 addr);
void rc_vu0_resume(void);            /* a VU0 microprogram (host/src/gfx/vu.c) */
void vu_run(int vu, u32 pc);                    /* run the translated microprogram at pc */
void rc_syscall(RcCtx *c);
void rc_break(u32 pc);
void rc_bad(u32 pc, const char *what);
void rc_enter(RcCtx *c);
u32 rc_cop0(int r);

/* ---- memory: KSEG aliases of RAM (0x20000000-0x3FFFFFFF uncached/accelerated) fold back ---- */
RC_INLINE uintptr_t RC_A(u32 a) { return (uintptr_t)((((a >> 28) == 2 || (a >> 28) == 3) && (a & 0x0FFFFFFFu) < 0x02000000u) ? (a & 0x0FFFFFFFu) : a); }
#define M8(a)   (*(u8  *)RC_A(a))
#define M16(a)  (*(u16 *)RC_A(a))
#define M32(a)  (*(u32 *)RC_A(a))
#define M64(a)  (*(u64 *)RC_A(a))
#define MF(a)   (*(f32 *)RC_A(a))
/* The EE's hardware registers (0x10000000-0x1000FFFF): a store that starts a
 * DMA channel runs the transfer at once; FIFO writes go to the VIF/GIF. */
void rc_io_write32(u32 a, u32 v);
void rc_io_write128(u32 a, const void *v);
RC_INLINE int rc_is_io(u32 a) { return (a & 0xFFFF0000u) == 0x10000000u; }
RC_INLINE void rc_sw(u32 a, u32 v) { if (rc_is_io(a)) rc_io_write32(a, v); else M32(a) = v; }
RC_INLINE void rc_sd(u32 a, u64 v) { if (rc_is_io(a)) { rc_io_write32(a, (u32)v); rc_io_write32(a + 4, (u32)(v >> 32)); } else M64(a) = v; }
/* Quadword accesses (lq, sq, lqc2, sqc2) ignore the low 4 address bits on the
 * EE, and the game relies on it. The one exception on the host: a local array
 * of the C code on the x86 stack, which is not 16-byte aligned as the EE's
 * would be, is used where it is (rc_cstack_lo/hi: the x86 stack, set by
 * rc_enter). */
extern u32 rc_cstack_lo, rc_cstack_hi;
RC_INLINE uintptr_t RC_QA(u32 a)
{
    return RC_A(a - rc_cstack_lo < rc_cstack_hi - rc_cstack_lo ? a : (a & ~15u));
}
RC_INLINE void rc_lq(R128 *d, u32 a) { __builtin_memcpy(d, (void *)RC_QA(a), 16); }
RC_INLINE void rc_sq(const R128 *s, u32 a) { if (rc_is_io(a)) rc_io_write128(a & ~15u, s); else __builtin_memcpy((void *)RC_QA(a), s, 16); }

#define SE32(x) ((u64)(s64)(s32)(u32)(x))

/* unaligned loads/stores (little-endian EE) */
RC_INLINE u64 rc_lwl(u64 rt, u32 a) { u32 k = (a & 3) * 8; u32 w = M32(a & ~3u);
    u32 keep = k == 24 ? 0 : (0x00FFFFFFu >> k); return SE32(((u32)rt & keep) | (w << (24 - k))); }
RC_INLINE u64 rc_lwr(u64 rt, u32 a) { u32 k = (a & 3) * 8; u32 w = M32(a & ~3u);
    if (k == 0) return SE32(w);
    u32 keep = ~(0xFFFFFFFFu >> k); return (rt & 0xFFFFFFFF00000000ull) | (((u32)rt & keep) | (w >> k)); }
RC_INLINE void rc_swl(u64 rt, u32 a) { u32 k = (a & 3) * 8; u32 *p = (u32 *)RC_A(a & ~3u);
    u32 keep = k == 24 ? 0 : (0xFFFFFF00u << k); *p = (*p & keep) | ((u32)rt >> (24 - k)); }
RC_INLINE void rc_swr(u64 rt, u32 a) { u32 k = (a & 3) * 8; u32 *p = (u32 *)RC_A(a & ~3u);
    u32 keep = k == 0 ? 0 : (0x00FFFFFFu >> (24 - k)); *p = (*p & keep) | ((u32)rt << k); }
RC_INLINE u64 rc_ldl(u64 rt, u32 a) { u32 k = (a & 7) * 8; u64 w = M64(a & ~7u);
    u64 keep = k == 56 ? 0 : (0x00FFFFFFFFFFFFFFull >> k); return (rt & keep) | (w << (56 - k)); }
RC_INLINE u64 rc_ldr(u64 rt, u32 a) { u32 k = (a & 7) * 8; u64 w = M64(a & ~7u);
    u64 keep = k == 0 ? 0 : ~(0xFFFFFFFFFFFFFFFFull >> k); return (rt & keep) | (w >> k); }
RC_INLINE void rc_sdl(u64 rt, u32 a) { u32 k = (a & 7) * 8; u64 *p = (u64 *)RC_A(a & ~7u);
    u64 keep = k == 56 ? 0 : (0xFFFFFFFFFFFFFF00ull << k); *p = (*p & keep) | (rt >> (56 - k)); }
RC_INLINE void rc_sdr(u64 rt, u32 a) { u32 k = (a & 7) * 8; u64 *p = (u64 *)RC_A(a & ~7u);
    u64 keep = k == 0 ? 0 : (0x00FFFFFFFFFFFFFFull >> (56 - k)); *p = (*p & keep) | (rt << k); }

/* ---- integer helpers ---- */
RC_INLINE s32 rc_plzcw1(u32 x) { u32 s = x >> 31; s32 n = 0; for (int b = 31; b >= 0 && ((x >> b) & 1) == s; b--) n++; return n - 1; }
RC_INLINE s32 rc_clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* ---- floats ---- */
RC_INLINE u32 rc_fbits(f32 f) { u32 u; __builtin_memcpy(&u, &f, 4); return u; }
RC_INLINE f32 rc_bitsf(u32 u) { f32 f; __builtin_memcpy(&f, &u, 4); return f; }
RC_INLINE s32 rc_cvtws(f32 f) {   /* EE: truncates, saturates */
    if (f >= 2147483647.0f) return 0x7FFFFFFF;
    if (f <= -2147483648.0f) return (s32)0x80000000u;
    return (s32)f;
}
RC_INLINE f32 rc_fsqrt(f32 x) { return __builtin_sqrtf(__builtin_fabsf(x)); }
/* The EE FPU and the VUs have no infinities: a result that overflows is
 * +-MAX (0x7F7FFFFF). Without this, MAX * MAX (FastArcSin of a huge value
 * after a division by zero) becomes inf and then NaN, which a camera's
 * matrices keep for good. */
RC_INLINE f32 rc_fc(f32 x) { u32 b = rc_fbits(x); return ((b & 0x7F800000u) == 0x7F800000u) ? rc_bitsf((b & 0x80000000u) | 0x7F7FFFFFu) : x; }
RC_INLINE f32 rc_fdiv(f32 a, f32 b) { if (b == 0.0f) return (rc_fbits(a) ^ rc_fbits(b)) & 0x80000000u ? -3.4028235e38f : 3.4028235e38f; return a / b; }
RC_INLINE s32 rc_ftoi(f32 f, int shift) { return rc_cvtws(f * (f32)(1 << shift)); }
RC_INLINE f32 rc_itof(s32 i, int shift) { return (f32)i / (f32)(1 << shift); }

/* COP2 macro mode: an FMAC op (ADD/SUB/MUL/MADD/MSUB/OPMULA/OPMSUB and their
 * ACC forms) sets the MAC flags (cfc2 $vi17) from its result, per written
 * component (Z bits 3..0 = x..w, S bits 7..4), the others cleared, and the
 * status flag's Z/S/U/O bits (cfc2 $vi16) from them, with the sticky copies
 * (bits 6..9). The collision code (coll_sphere, func_L00_001F10E0) tests the
 * signs of a VSUB this way: `cfc2 $at, $vi17; andi $at, 0xE0`. */
RC_INLINE void rc_vu0_flags(const f32 *v, u32 dest)
{
    u32 mac = 0;
    for (int i = 0; i < 4; i++) {
        if (!(dest & (8u >> i))) continue;
        u32 b; __builtin_memcpy(&b, &v[i], 4);
        if ((b & 0x7FFFFFFFu) == 0) mac |= 8u >> i;
        if (b >> 31) mac |= 0x80u >> i;
        if ((b & 0x7F800000u) == 0x7F800000u) mac |= 0x8000u >> i;      /* overflow */
    }
    rc_vu0.mac = mac;
    u32 st = (mac & 0xF ? 1u : 0) | (mac & 0xF0 ? 2u : 0) | (mac & 0xF00 ? 4u : 0) | (mac & 0xF000 ? 8u : 0);
    rc_vu0.status = (rc_vu0.status & ~0xFu) | st | (st << 6);
}

#define VF(n) rc_vu0.vf[n]
#define VACC rc_vu0.acc
#endif
