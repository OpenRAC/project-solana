/* Glints and sparkles, drawn by native code from the game's particle buffers instead of by the
 * PS2's VU1 effect microprogram (image 221571). The game uploads the program, runs its setup
 * once, then sends particle buffers (double buffered at 0 / 512) and starts the program on each.
 *
 * Starts:
 *   0x0000  setup: VU memory 0..3 the world to clip space matrix (vf28..vf31: rows x, y, z and
 *           the translation), 4 vf25 (x: the projection's numerator, Q = vf25.x / w), 5 vf26
 *           (the screen offset; z the depth offset, w the fog / last word added to every vertex
 *           of a screen quad), 6 vf27 (xy: a screen quad's size scale), 7..10 vf21..vf24 (the
 *           ST qwords of the four corners), 11 vf16 (the GIF tag of a quad), 12 vf17 (of a
 *           line), 13 vf18 (of a streak), 14 vf19 (A+D: zw the register, ALPHA_1), 15 vf20 (A+D:
 *           zw TEX0_1). vi14 = 1023, vi15 = 512, vi8 = -2. vf12.zw and vf13.zw are multiplied by
 *           0 (they stay +-0 as the sign was, -MAX if they were inf). Nothing kicked; the program
 *           ends at 0x0098 (its next MSCNT would be 0x00A0, the game uses MSCAL 0x00A0 instead).
 *   0x00A0  a particle buffer: vi15 = (vi15 + 512) & vi14 (the buffers alternate), the
 *           particles from vi15, the packet built at vi15 + 128 and kicked. The particle count
 *           (in qwords) is the first particle's qword 2's w.
 *
 * A particle starts with its position (xyz) and its mode in w (low 16 bits); the output for
 * each is a GIF tag and its registers (the tags have EOP clear; the last particle's tag gets
 * 0x8001 in its first word, after the kick is issued, before the packet goes):
 *   mode 0 (4 qwords) a screen-space quad: +1 RGBA, +2 size (x, y; z the ALPHA_1 data, w the
 *           count in the first particle), +3 TEX0_1 data (xy). The center is transformed and
 *           projected (xyz * Q, Q = vf25.x / w); the corners are vf26 +- vf12 and vf26 +- vf13,
 *           all four words (vf12 = vf27.xy * size.xy, vf13 = (-size.y, vf27.y * size.x): the
 *           second offset's x is not scaled), + the projected center (xyz), ftoi4. Out (12): vf16,
 *           A+D (x = size.z sign extended from 16 bits, yzw vf19's), A+D (xy = +3, zw vf20's),
 *           RGBA, then ST, XYZ for the four corners (vf21 c0, vf22 c1, vf23 c2, vf24 c3; c0 =
 *           +vf12, c1 = +vf13, c2 = -vf13, c3 = -vf12).
 *   mode > 0 (4 qwords) a world-space quad: as mode 0's inputs; the corners are the position
 *           +- size in x and y, (x - size.y, y + size.x), (x + size.y, y - size.x), each
 *           transformed and projected on its own (+ vf26.xyz, w 1, ftoi4). Out (12) as mode 0.
 *   mode < 0, not vi8 (4 qwords) a line: +1 RGBA, +2 the second point (xyz), +3 its RGBA; out
 *           (6): vf17, A+D (x = 68, yzw vf19's), RGBA, XYZ, RGBA, XYZ.
 *   mode = vi8 (-2) (6 qwords) a streak: +1 the second point (xyz; w its width scale), +2 the
 *           half width (xyz), +3 RGBA, +4 the second RGBA, +5 TEX0_1 data (xy; z the ALPHA_1
 *           data): corners p + d, p - d, p2 + d * s, p2 - d * s, each transformed and projected;
 *           out (13): vf18, A+D, A+D, RGBA, ST vf21, XYZ, ST vf22, XYZ, RGBA, ST vf23, XYZ, ST
 *           vf24, XYZ.
 *
 * The program's ways, kept here:
 *   - a transform is ACC = vf31 * 1, ACC += vf28 * x, ACC += vf29 * y, vf30 * z + ACC, each
 *     product and sum rounded (toward zero) and clamped (inf / NaN to +-MAX) in that order; a
 *     corner's xyz is the transformed xyz * Q, + vf26, its w 1 (a screen quad's: vf26.w +
 *     vf12.w / vf13.w); the DIVs are not clamped (Q can be inf; its products are);
 *   - Q is always the fresh one where the program reads it (each read is 7 cycles after its DIV);
 *   - a world quad's corners all take the position's z, the A+D qword
 *     of a line has x = 68 (the line's own ALPHA_1 data), and vf19.x / vf20.xy keep the last
 *     particle's values (registers carried on);
 *   - the next particle's first qword is read ahead (in a world quad, a line and a streak after
 *     some of this particle's output is written: an output buffer that runs into the input is
 *     read as the program reads it), and its mode is taken from that read;
 *   - the walk ends only when the input address hits the count's end exactly (4 or 6 qwords a
 *     particle, 16-bit addresses, VU memory wrapping at 16 KB); a walk that never does would
 *     loop forever on the VU: here it is left after 65536 particles, unkicked;
 *   - the last particle's tag's first word is replaced by 0x8001 (vi2 stored as a word);
 *   - a NaN bit pattern read as a float (only output read back as input) gives +-MAX with the
 *     sign of the first NaN operand in the program's order (SSE's rule), not the compiler's;
 *   - the program stops at an E bit, so its TPC is left where it would go on: 0x00A0 after the
 *     setup, 0x01F8 / 0x03E0 / 0x0508 / 0x06C8 after a buffer whose last particle was a screen
 *     quad / world quad / line / streak.
 *
 * native_fx221571_run() does what the program does, word for word, and reads and writes VU
 * memory as the program does; native_fx221571_state() then has the whole register file as the
 * program leaves it (every register it writes, the scratch ones too, ACC, Q, TPC), which
 * native_fx221571_regs_to_vu() puts back into an RcVu0 with the pipeline as vu_settle leaves
 * it. Checked against the program itself by dev/vu_harness (native_fx221571_h.c on captures,
 * fx221571_fuzz.c on made-up particles of every mode): packets, VU memory and the whole RcVu0. */
#include "native_fx221571.h"
#include <string.h>

#define AI static inline __attribute__((always_inline))

/* ---- the VU's arithmetic as the translated program has it (SSE, rounding toward zero) ---- */
AI float F(u32 u) { float f; memcpy(&f, &u, 4); return f; }
AI u32 U(float f) { u32 u; memcpy(&u, &f, 4); return u; }
/* an FMAC result: +-MAX instead of inf or NaN */
AI float fc(float x) { u32 b = U(x); if ((b & 0x7F800000u) == 0x7F800000u) x = F((b & 0x80000000u) | 0x7F7FFFFFu); return x; }
/* two operands, a NaN among them (only where an output buffer has run over input not read yet):
 * SSE keeps the first operand's NaN, and the program's operands are in this order (a sum's
 * compiled order is the compiler's choice: fixed here so the result does not depend on it) */
AI int isnan_u(u32 u) { return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu); }
AI u32 nanmax(u32 a, u32 b) { return ((isnan_u(a) ? a : b) & 0x80000000u) | 0x7F7FFFFFu; }
AI u32 add(u32 a, u32 b) { if (isnan_u(a) | isnan_u(b)) return nanmax(a, b); return U(fc(F(a) + F(b))); }
AI u32 sub(u32 a, u32 b) { if (isnan_u(a) | isnan_u(b)) return nanmax(a, b); return U(fc(F(a) - F(b))); }
AI u32 mul(u32 a, u32 b) { if (isnan_u(a) | isnan_u(b)) return nanmax(a, b); return U(fc(F(a) * F(b))); }
AI u32 madd(u32 acc, u32 a, u32 b) { return U(fc(F(acc) + F(mul(a, b)))); }   /* (ACC and the product are never NaN) */
AI u32 ftoi4(u32 a)
{
    float x = F(a) * 16.0f;
    if (x >= 2147483647.0f) return 0x7FFFFFFFu;
    if (x <= -2147483648.0f) return 0x80000000u;
    if (x != x) return 0;
    return (u32)(s32)x;
}
AI u32 vdiv(u32 a, u32 b)
{
    if (F(b) == 0.0f) return ((a ^ b) & 0x80000000u) ? 0xFF7FFFFFu : 0x7F7FFFFFu;
    return U(F(a) / F(b));
}
AI u16 lo16(u32 w) { return (u16)w; }
AI u32 sx(u16 v) { return (u32)(s32)(s16)v; }          /* MFIR: sign extended */

#define ONE  0x3F800000u                               /* vf0.w */
#define ZERO 0x00000000u                               /* vf0.x */

/* ---- the registers: the program's whole register file as it leaves it ---- */
static NativeFx221571Regs R;
#define RF(n) R.vf[n]
#define RI(n) R.vi[n]

void native_fx221571_reset(void) { memset(&R, 0, sizeof R); R.vf[0][3] = ONE; }
void native_fx221571_seed(const NativeFx221571Regs *r) { R = *r; }
void native_fx221571_state(NativeFx221571Regs *r) { *r = R; }

void native_fx221571_regs_from_vu(NativeFx221571Regs *r, const RcVu0 *v)
{
    memset(r, 0, sizeof *r);
    memcpy(r->vf, v->vf, sizeof r->vf);
    memcpy(r->acc, v->acc, 16);
    memcpy(&r->q, v->q_pend ? &v->q_new : &v->q, 4);       /* Q as it lands (the program reads none before its DIV) */
    for (int k = 0; k < 16; k++) r->vi[k] = (u16)v->vi[k];
    r->tpc = v->tpc;
}

void native_fx221571_regs_to_vu(const NativeFx221571Regs *r, RcVu0 *v)
{
    /* the registers the program writes (vf0, vi0, I, P, R are never written) */
    for (int n = 1; n < 32; n++) memcpy(v->vf[n], r->vf[n], 16);
    memcpy(v->acc, r->acc, 16);
    for (int k = 1; k < 16; k++) v->vi[k] = r->vi[k];
    memcpy(&v->q, &r->q, 4);
    v->tpc = r->tpc;
    /* the flags: the program sets none; its start latches status / clip into st_int / cl_int
     * and its end lands whatever was in flight (vu_settle), the last DIV included */
    v->st_int = v->status; v->cl_int = v->clip;
    while (v->fp_n) { v->mac = v->fp_mac[v->fp_h]; v->status = v->fp_st[v->fp_h]; v->fp_h = (v->fp_h + 1) & 3; v->fp_n--; }
    while (v->cp_n) { v->clip = v->cp_val[v->cp_h]; v->cp_h = (v->cp_h + 1) & 3; v->cp_n--; }
    if (r->qdiv) memcpy(&v->q_new, &r->q, 4);
    v->q_pend = 0;
    v->fp_h = v->cp_h = 0;
}

typedef struct {
    u8 *mem;
    u32 *out, out_cap, nq;
    NativeFx221571Packet *pk;
    int maxpk, npk;
} Ctx;

static u32 *QW(Ctx *c, u32 a) { return (u32 *)(c->mem + (a & 0x3FF) * 16); }
static void ld(Ctx *c, u32 *d, u32 a) { memcpy(d, QW(c, a), 16); }
static void st(Ctx *c, u32 a, const u32 *s) { memcpy(QW(c, a), s, 16); }

/* XGKICK: the packet as the GIF reads it (tag by tag to EOP, at most 1024 qwords) */
static void send(Ctx *c, u32 qaddr)
{
    if (c->npk >= c->maxpk) return;
    u32 q = qaddr & 0x3FF, n = 0;
    u32 *dst = c->out + c->nq * 4;
    while (n < 0x400 && c->nq + n < c->out_cap) {
        const u32 *t = QW(c, q);
        u64 lo = ((u64)t[1] << 32) | t[0];
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        for (u32 j = 0; j < len && n < 0x400 && c->nq + n < c->out_cap; j++) { memcpy(dst + n * 4, QW(c, q + j), 16); n++; }
        q = (q + len) & 0x3FF;
        if (eop) break;
    }
    c->pk[c->npk].off = c->nq; c->pk[c->npk].n = n; c->npk++; c->nq += n;
}

/* ACC = vf31 * vf0.w */
static void mula31(void) { for (int i = 0; i < 4; i++) R.acc[i] = mul(RF(31)[i], ONE); }
static void madda(int n, u32 s) { for (int i = 0; i < 4; i++) R.acc[i] = madd(R.acc[i], RF(n)[i], s); }
/* ACC = vf31; ACC += vf28 * x; ACC += vf29 * y; d = ACC + vf30 * z (ACC left as before the last) */
static void xform(u32 *d, u32 x, u32 y, u32 z)
{
    mula31(); madda(28, x); madda(29, y);
    for (int i = 0; i < 4; i++) d[i] = madd(R.acc[i], RF(30)[i], z);
}
static void div_w(u32 w) { R.q = vdiv(RF(25)[0], w); R.qdiv = 1; }
static void mulq(u32 *d) { for (int i = 0; i < 3; i++) d[i] = mul(d[i], R.q); }
static void add26(u32 *d) { for (int i = 0; i < 3; i++) d[i] = add(d[i], RF(26)[i]); }
static void toi4(u32 *d) { for (int i = 0; i < 4; i++) d[i] = ftoi4(d[i]); }
static void ldxy(Ctx *c, u32 *d, u32 a) { const u32 *m = QW(c, a); d[0] = m[0]; d[1] = m[1]; }

/* ======================= 0x0000: setup ======================= */
static void run_setup(Ctx *c)
{
    static const u8 dst[16] = { 28, 29, 30, 31, 25, 26, 27, 21, 22, 23, 24, 16, 17, 18, 19, 20 };
    RF(12)[2] = mul(RF(12)[2], ZERO); RF(12)[3] = mul(RF(12)[3], ZERO);
    RF(13)[2] = mul(RF(13)[2], ZERO); RF(13)[3] = mul(RF(13)[3], ZERO);
    for (int k = 0; k < 16; k++) ld(c, RF(dst[k]), k);
    RI(14) = 1023; RI(15) = 512; RI(8) = 0xFFFE;
    R.tpc = 0x00A0;
}

/* ======================= 0x00A0: a particle buffer ======================= */
/* registers: vf1 the particle's first qword (read ahead), vf14 / vf15 its other qwords,
 * vi1 its output, vi9 its mode, vi10 the input, vi11 the next output, vi12 the end */

/* mode 0: a screen-space quad (0x00F8..0x01E0) */
static void p_screen(Ctx *c)
{
    u32 *v1 = RF(1), *v2 = RF(2), *v5 = RF(5), *v6 = RF(6), *v7 = RF(7), *v8 = RF(8), *s = RF(15);
    mula31();                                                       /* 0x00F0 (the branch's delay slot) */
    madda(28, v1[0]); ld(c, RF(14), RI(10) + 1);
    madda(29, v1[1]); ldxy(c, RF(20), RI(10) + 3);
    for (int i = 0; i < 4; i++) v2[i] = madd(R.acc[i], RF(30)[i], v1[2]);
    RI(10) += 4;
    RI(1) = RI(11); RI(11) += 12;
    RF(12)[0] = mul(RF(27)[0], s[0]); RF(12)[1] = mul(RF(27)[1], s[1]);
    RF(13)[1] = mul(RF(27)[1], s[0]); RI(2) = lo16(s[2]);
    RF(13)[0] = sub(ZERO, s[1]); div_w(v2[3]);
    ld(c, v1, RI(10));                                              /* the next particle */
    st(c, RI(1) + 0, RF(16));
    RF(19)[0] = sx(RI(2));
    for (int i = 0; i < 4; i++) v5[i] = add(RF(26)[i], RF(12)[i]);  /* (the w = 1 before is overwritten) */
    for (int i = 0; i < 4; i++) v8[i] = sub(RF(26)[i], RF(12)[i]);
    st(c, RI(1) + 3, RF(14));
    mulq(v2);
    st(c, RI(1) + 2, RF(20));
    for (int i = 0; i < 4; i++) v6[i] = add(RF(26)[i], RF(13)[i]);
    for (int i = 0; i < 4; i++) v7[i] = sub(RF(26)[i], RF(13)[i]);
    st(c, RI(1) + 1, RF(19));
    for (int i = 0; i < 3; i++) v5[i] = add(v5[i], v2[i]);
    st(c, RI(1) + 4, RF(21));
    for (int i = 0; i < 3; i++) v6[i] = add(v6[i], v2[i]);
    for (int i = 0; i < 3; i++) v7[i] = add(v7[i], v2[i]);
    st(c, RI(1) + 6, RF(22));
    for (int i = 0; i < 3; i++) v8[i] = add(v8[i], v2[i]);
    toi4(v5); st(c, RI(1) + 8, RF(23));
    toi4(v6); RI(2) = 0x8001;
    toi4(v7); st(c, RI(1) + 10, RF(24));
    toi4(v8); RI(9) = lo16(v1[3]);
    st(c, RI(1) + 5, v5);
    st(c, RI(1) + 7, v6);
    st(c, RI(1) + 9, v7);
    st(c, RI(1) + 11, v8);
    R.tpc = 0x01F8;
}

/* mode > 0: a world-space quad (0x0208..0x03C8) */
static void p_world(Ctx *c)
{
    u32 *p = RF(1), *s = RF(15), *v2 = RF(2), *v3 = RF(3), *v4 = RF(4), *v5 = RF(5);
    ldxy(c, RF(20), RI(10) + 3);
    RI(10) += 4;
    v2[0] = add(p[0], s[0]); v2[1] = add(p[1], s[1]); RI(2) = lo16(s[2]);
    v5[0] = sub(p[0], s[0]); v5[1] = sub(p[1], s[1]); RI(1) = RI(11);
    v3[0] = sub(p[0], s[1]); RI(11) += 12;
    v4[0] = add(p[0], s[1]);
    v3[1] = add(p[1], s[0]);
    v4[1] = sub(p[1], s[0]); RF(19)[0] = sx(RI(2));
    u32 z = p[2];
    st(c, RI(1) + 0, RF(16));
    st(c, RI(1) + 3, RF(14));
    xform(v2, v2[0], v2[1], z);
    st(c, RI(1) + 2, RF(20));
    st(c, RI(1) + 1, RF(19));
    xform(v3, v3[0], v3[1], z); div_w(v2[3]);
    xform(v4, v4[0], v4[1], z);
    mulq(v2); div_w(v3[3]);
    xform(v5, v5[0], v5[1], z);
    mulq(v3); div_w(v4[3]);
    mulq(v4); div_w(v5[3]);
    v2[3] = ONE; st(c, RI(1) + 4, RF(21));
    v3[3] = ONE;
    v5[3] = ONE; st(c, RI(1) + 6, RF(22));
    v4[3] = ONE; st(c, RI(1) + 8, RF(23));
    mulq(v5); st(c, RI(1) + 10, RF(24));
    add26(v2); ld(c, p, RI(10));                                    /* the next particle */
    add26(v3); add26(v4); add26(v5);
    toi4(v2); RI(2) = 0x8001;
    toi4(v3); toi4(v4);
    toi4(v5); RI(9) = lo16(p[3]);
    st(c, RI(1) + 5, v2);
    st(c, RI(1) + 7, v3);
    st(c, RI(1) + 9, v4);
    st(c, RI(1) + 11, v5);
    R.tpc = 0x03E0;
}

/* mode < 0, not vi8: a line (0x03F0..0x04F0) */
static void p_line(Ctx *c)
{
    u32 *p = RF(1), *v3 = RF(3), *v4 = RF(4), *s = RF(15);
    mula31(); madda(28, p[0]); RI(1) = RI(11);                      /* 0x03E0 / 0x03E8 */
    madda(29, p[1]); RI(11) += 6;
    for (int i = 0; i < 4; i++) v3[i] = madd(R.acc[i], RF(30)[i], p[2]);
    RI(2) = 68;
    xform(v4, s[0], s[1], s[2]); div_w(v3[3]);
    v3[3] = ONE; ld(c, s, RI(10) + 3);
    RF(19)[0] = sx(RI(2));
    RI(10) += 4;
    mulq(v3); div_w(v4[3]);
    v4[3] = ONE; st(c, RI(1) + 0, RF(17));
    st(c, RI(1) + 1, RF(19));
    st(c, RI(1) + 2, RF(14));
    mulq(v4); st(c, RI(1) + 4, s);
    ld(c, p, RI(10));                                               /* the next particle */
    add26(v3); add26(v4);
    toi4(v3);
    toi4(v4); RI(9) = lo16(p[3]);
    RI(2) = 0x8001;
    st(c, RI(1) + 3, v3);
    st(c, RI(1) + 5, v4);
    R.tpc = 0x0508;
}

/* mode = vi8: a streak (0x0508..0x06B0) */
static void p_streak(Ctx *c)
{
    u32 *p = RF(1), *s = RF(15), *e = RF(14), *v2 = RF(2), *v3 = RF(3), *v4 = RF(4), *v5 = RF(5);
    mula31(); madda(28, p[0]); RI(1) = RI(11);                      /* 0x03E0 / 0x03E8 (ACC discarded) */
    for (int i = 0; i < 3; i++) v2[i] = add(p[i], s[i]);
    ldxy(c, RF(20), RI(10) + 5);
    for (int i = 0; i < 3; i++) v3[i] = sub(p[i], s[i]);
    for (int i = 0; i < 3; i++) s[i] = mul(s[i], e[3]);
    RI(2) = lo16(QW(c, RI(10) + 5)[2]);
    xform(v2, v2[0], v2[1], v2[2]);
    for (int i = 0; i < 3; i++) v4[i] = add(e[i], s[i]);
    RI(11) += 13;
    for (int i = 0; i < 3; i++) v5[i] = sub(e[i], s[i]);
    RF(19)[0] = sx(RI(2));
    xform(v3, v3[0], v3[1], v3[2]); div_w(v2[3]);
    ld(c, e, RI(10) + 3);
    ld(c, s, RI(10) + 4);
    xform(v4, v4[0], v4[1], v4[2]);
    RI(10) += 6;
    mulq(v2); div_w(v3[3]);
    st(c, RI(1) + 0, RF(18));
    xform(v5, v5[0], v5[1], v5[2]);
    st(c, RI(1) + 3, e);
    st(c, RI(1) + 8, s);
    mulq(v3); div_w(v4[3]);
    st(c, RI(1) + 1, RF(19));
    st(c, RI(1) + 2, RF(20));
    mulq(v4); div_w(v5[3]);
    v2[3] = ONE; st(c, RI(1) + 4, RF(21));
    v3[3] = ONE;
    v5[3] = ONE; st(c, RI(1) + 6, RF(22));
    v4[3] = ONE; st(c, RI(1) + 9, RF(23));
    mulq(v5); st(c, RI(1) + 11, RF(24));
    add26(v2); ld(c, p, RI(10));                                    /* the next particle */
    add26(v3); add26(v4); add26(v5);
    toi4(v2); RI(2) = 0x8001;
    toi4(v3); toi4(v4);
    toi4(v5); RI(9) = lo16(p[3]);
    st(c, RI(1) + 5, v2);
    st(c, RI(1) + 7, v3);
    st(c, RI(1) + 10, v4);
    st(c, RI(1) + 12, v5);
    R.tpc = 0x06C8;
}

static void run_particles(Ctx *c)
{
    RI(15) = (u16)((RI(15) + 512) & RI(14));
    u16 b = RI(15);
    RI(12) = lo16(QW(c, b + 2)[3]);
    RI(9) = lo16(QW(c, b)[3]);
    ld(c, RF(1), b);
    RI(10) = b; RI(11) = (u16)(b + 128);
    RI(13) = (u16)(b + 128);
    RI(12) = (u16)(RI(12) + RI(10));
    for (u32 n = 0;; n++) {
        if (n >= 65536) return;                                     /* would loop forever on the VU */
        ld(c, RF(15), RI(10) + 2);                                  /* 0x00F0 */
        if (RI(9) == 0) p_screen(c);
        else {
            ld(c, RF(14), RI(10) + 1);                              /* 0x0200 */
            if ((s16)RI(9) >= 0) p_world(c);
            else if (RI(9) == RI(8)) p_streak(c);
            else p_line(c);
        }
        if (RI(10) == RI(12)) break;
    }
    /* XGKICK vi13; its delay slot (ISW.x vi2) lands before the GIF reads the packet */
    QW(c, RI(1))[0] = RI(2);
    send(c, RI(13));
}

int native_fx221571_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeFx221571Packet *pk, int maxpk)
{
    (void)top;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.mem = mem; c.out = out; c.out_cap = out_cap; c.pk = pk; c.maxpk = maxpk;
    R.qdiv = 0;
    switch (pc) {
    case 0x0000: run_setup(&c); break;
    case 0x00A0: run_particles(&c); break;
    default: return -1;
    }
    return c.npk;
}
