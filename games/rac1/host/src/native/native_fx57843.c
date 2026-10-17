/* An effect's triangle strips, drawn by native code instead of by the PS2's VU1 microprogram
 * image 57843 (588 instruction pairs). The game uploads it now and then for an effect (Kerwan:
 * a strip of a few hundred vertices at a time); the strips are transformed in place in VU
 * memory and kicked as they are, and triangles that cross the view's edges are clipped into
 * fans of their own.
 *
 * Starts:
 *   0x0000  setup: vf18..vf25 = 932..939, vf31 = 940, vf29 = 941, vf30 = 942, vf7 = 943.
 *           932..935 the plain strip's object to screen matrix (rows x, y, z, translation),
 *           936..939 the clipped strip's object to clip space matrix, 940 the clipped fans' GIF
 *           tag (x NLOOP 0 | EOP, w the plain strip's Q numerator), 941 the screen scale
 *           (vf29, w the fog scale; its w is the plain strip's last Q numerator), 942 the screen
 *           offset (vf30, w the fog offset), 943 the fog clamp (vf7: x low, y high).
 *   0x0060  (and 0x0078) a plain strip at TOP: the GIF tag (NLOOP = N vertices), then per vertex
 *           3 qwords: s t q | color (ints, untouched) | x y z. Each vertex is transformed by
 *           vf18..21 (ACC = vf21 * vf0.w, + vf18 x, + vf19 y; + vf20 z), Q = vf31.w / w, and
 *           written over its input: s t q * Q (w: vf13.w for even vertices, vf6.w for odd ones,
 *           registers this program never writes), and ftoi4 of (x y z * Q, w) + vf30, w clamped
 *           (max vf7.x, then min vf7.y). The strip is kicked.
 *   0x0070  (and 0x0068) a clipped strip at TOP, same layout. vf31 is reloaded from 940 first.
 *           Each vertex: P = vf22..25 * xyz (clip space), Q = vf0.w / P.w, s t q * Q as above,
 *           screen = (P.xyz * Q, P.w) * vf29 + vf30 (w clamped: max vf7.x then min vf7.y, the
 *           first vertex min then max), CLIPW P (the last three positions kept in vf28 vf27
 *           vf26). From the second vertex on, when the triangle of the last three has a vertex
 *           outside the view (clip flags & 0x2FBEF: all planes but far) the vertex's screen w
 *           gets + 2048 (its XYZ's ADC bit: the GS does not draw that strip triangle); unless all
 *           three are outside one plane (tested in the order -z, -y, +y, -x, +x), it goes to
 *           the clipper when it is a whole one (third vertex on, not the run's extra last one).
 *   The clipper (0x0838): if the three vertices with x and y times 3 are all outside one side
 *   (CLIPW: -y, +y, -x, +x), the triangle is dropped. Else a polygon at 948 (3 qwords a vertex:
 *   clip space position, s t q, color as floats; the first two vertices' s t q read back from the strip
 *   output and divided by their q, the third's taken as the strip input has it) is clipped
 *   (Sutherland-Hodgman, edge by edge from the last vertex) against -w = z, then x = -w, x = w,
 *   y = -w, y = w, ping-ponging between 948 and 973, and written as a fan at 998:
 *   vf31 (kicked by itself: an EOP tag in the game's 940), a tag (vf31, x + the vertex count) and per vertex
 *   s t q * 1/w | ftoi0 color | ftoi4 of (xyz / w, w) * vf29 + vf30 (w min vf7.y then max
 *   vf7.x), kicked. Fewer than three vertices left after a plane: nothing kicked. VU memory
 *   947 keeps the strip's registers over the call (vi6 vi7 vi15, Q in w); the clip flags are
 *   rebuilt from vf28 vf27 vf26 after it.
 *
 * The program's ways, kept here:
 *   - it is software pipelined: it reads the vertex three ahead of the one it writes (and reads
 *     past the strip's end), and works in place; this file reads and writes at those points;
 *   - the strip counters are 16 bits: a plain strip of N < 4 vertices draws N + 65536, a clipped
 *     strip of 0 draws 65536 (the clipper then runs only where the count left is positive);
 *   - the plain strip's last vertex takes vf29.w as its Q numerator when N is even (the
 *     epilogue's DIV), and then vf31 is reloaded from 940 (the end's delay slot);
 *   - the clipped strip handles one vertex past its end (CLIPW, the triangle test: the clip
 *     flags, vf26..28 after the run have it) and its second vertex's triangle test reads the
 *     clip flags the last run left (the ADC bit);
 *   - the STQ w words: vf13.w / vf6.w of the VU (never written here), carried into the clipper's
 *     polygon and interpolated with it.
 * The VU's DIV latency (Q) and flag latency never show: every read comes after the result (the
 * fuzzer checks this on the translated program).
 *
 * native_fx57843_run() does what the program does, word for word, and reads and writes VU
 * memory as the program does. Checked against the program by dev/vu_harness
 * (native_fx57843_h.c on captures, fx57843_fuzz.c on mutated and made-up strips). */
#include "native_fx57843.h"
#include <string.h>
#include <math.h>
#pragma STDC FP_CONTRACT OFF

/* ---- the VU's arithmetic as the translated program has it (SSE, rounding toward zero) ---- */
static float F(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static u32 U(float f) { u32 u; memcpy(&u, &f, 4); return u; }
/* an FMAC result: +-MAX instead of inf or NaN */
static float fc(float x) { u32 b = U(x); if ((b & 0x7F800000u) == 0x7F800000u) x = F((b & 0x80000000u) | 0x7F7FFFFFu); return x; }
static s32 ftoi(float f, float scale)
{
    float x = f * scale;
    if (x >= 2147483647.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return (s32)0x80000000u;
    if (x != x) return 0;
    return (s32)x;
}
static u32 itof(u32 i, float inv) { return U((float)(s32)i * inv); }
static float vdiv(float a, float b)
{
    if (b == 0.0f) return ((U(a) ^ U(b)) & 0x80000000u) ? -3.4028235e38f : 3.4028235e38f;
    return a / b;
}
/* the CLIPW flags of a point: +x -x +y -y +z -z */
static u32 clipflags(const u32 *c)
{
    float x = F(c[0]), y = F(c[1]), z = F(c[2]), a = __builtin_fabsf(F(c[3]));
    return (x > a) | ((x < -a) << 1) | ((y > a) << 2) | ((y < -a) << 3) | ((z > a) << 4) | ((z < -a) << 5);
}


/* ---- the registers ---- */
static NativeFx57843Regs R;
#define RF(n) R.vf[n]   /* a register of the file */

void native_fx57843_reset(void) { memset(&R, 0, sizeof R); R.vf[0][3] = 0x3F800000u; }
void native_fx57843_seed(const NativeFx57843Regs *r) { R = *r; }
void native_fx57843_state(NativeFx57843Regs *r) { *r = R; }
void native_fx57843_regs_from_vu(NativeFx57843Regs *r, const RcVu0 *v)
{
    memcpy(r->vf, v->vf, sizeof r->vf);
    memcpy(r->vi, v->vi, sizeof r->vi);
    memcpy(r->acc, v->acc, 16);
    memcpy(&r->q, &v->q, 4); memcpy(&r->q_new, &v->q_new, 4); memcpy(&r->i, &v->i, 4);
    r->clip = v->clip & 0xFFFFFF; r->tpc = v->tpc;
}
void native_fx57843_regs_to_vu(const NativeFx57843Regs *r, RcVu0 *v)
{
    memcpy(v->vf, r->vf, sizeof r->vf);
    memcpy(v->vi, r->vi, sizeof r->vi);
    memcpy(v->acc, r->acc, 16);
    memcpy(&v->q, &r->q, 4); memcpy(&v->q_new, &r->q_new, 4); memcpy(&v->i, &r->i, 4);
    v->clip = v->cl_int = r->clip;              /* (a run starts with cl_int = clip, st_int = status) */
    v->st_int = v->status;
    v->tpc = r->tpc;
    v->q_pend = 0; v->cp_n = 0; v->cp_h = 0;    /* (a run ends with its pipeline empty) */
    if (!v->fp_n) v->fp_h = 0;
}

typedef struct {
    u8 *mem;
    u32 *out, out_cap, nq;
    NativeFx57843Packet *pk;
    int maxpk, npk;
    u32 pending;                 /* a kick goes when the next one comes or the run ends */
    u32 clip;                    /* the clip flags */
    float q;                     /* Q */
} Ctx;

static u32 *QW(Ctx *c, u32 a) { return (u32 *)(c->mem + (a & 0x3FF) * 16); }
static void ld(Ctx *c, u32 *r, u32 a) { memcpy(r, QW(c, a), 16); }
static void sq(Ctx *c, u32 a, const u32 *r) { memcpy(QW(c, a), r, 16); }

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
static void kick(Ctx *c, u32 qaddr) { if (c->pending != 0xFFFFFFFFu) send(c, c->pending); c->pending = qaddr & 0x3FF; }

static void clipw(Ctx *c, const u32 *v) { c->clip = ((c->clip << 6) | clipflags(v)) & 0xFFFFFF; }
/* FCOR: every flag imm leaves out is set */
static int fcor(Ctx *c, u32 imm) { return ((c->clip | imm) & 0xFFFFFF) == 0xFFFFFF; }
/* FCORs each followed by a branch on vi1, the next FCOR in its delay slot: whether one hit;
 * *vi1 is what the chain leaves in vi1 */
static int chain(Ctx *c, const u32 *imm, int n, u32 *vi1)
{
    for (int i = 0; i < n; i++) if (fcor(c, imm[i])) { *vi1 = i + 1 < n ? (u32)fcor(c, imm[i + 1]) : 1; return 1; }
    *vi1 = 0;
    return 0;
}

/* ACC = t * vf0.w; ACC += x * p.x; ACC += y * p.y; d = ACC + z * p.z (each product and sum
 * rounded; ACC keeps the sum before z) */
static void xform(u32 *d, int x, int y, int z, int t, const u32 *p)
{
    float w1 = F(RF(0)[3]), px = F(p[0]), py = F(p[1]), pz = F(p[2]);
    for (int k = 0; k < 4; k++) {
        float a = fc(F(RF(t)[k]) * w1);
        a = fc(a + fc(F(RF(x)[k]) * px));
        a = fc(a + fc(F(RF(y)[k]) * py));
        R.acc[k] = U(a);
        d[k] = U(fc(a + fc(F(RF(z)[k]) * pz)));
    }
}
/* d.xyz = s.xyz * q, d.w = w */
static void mulq3(u32 *d, const u32 *s, float q, u32 w)
{
    for (int k = 0; k < 3; k++) d[k] = U(fc(F(s[k]) * q));
    d[3] = w;
}
/* the clipped strip's screen position: ACC = s * vf29; s = ACC + vf30 * vf0.w */
static void scale(u32 *s)
{
    float w1 = F(RF(0)[3]);
    for (int k = 0; k < 4; k++) {
        float a = fc(F(s[k]) * F(RF(29)[k]));
        R.acc[k] = U(a);
        s[k] = U(fc(a + fc(F(RF(30)[k]) * w1)));
    }
}
static u32 wmax(u32 w) { return U(fc(fmaxf(F(w), F(RF(7)[0])))); }
static u32 wmin(u32 w) { return U(fc(fminf(F(w), F(RF(7)[1])))); }
static void ftoi4(u32 *d, const u32 *s) { for (int k = 0; k < 4; k++) d[k] = (u32)ftoi(F(s[k]), 16.0f); }
static void shift(const u32 *p) { memcpy(RF(28), RF(27), 16); memcpy(RF(27), RF(26), 16); memcpy(RF(26), p, 16); }

/* the strips' registers by vertex parity (even, odd): transformed position, s t q as read,
 * screen position, s t q * Q */
static const int PREG[2] = { 8, 1 }, RREG[2] = { 12, 5 }, SREG[2] = { 9, 2 }, TREG[2] = { 13, 6 };

/* ======================= 0x0000: setup ======================= */
static void run_setup(Ctx *c)
{
    for (int r = 18; r <= 25; r++) ld(c, RF(r), 932 + r - 18);
    ld(c, RF(31), 940); ld(c, RF(29), 941); ld(c, RF(30), 942); ld(c, RF(7), 943);
    R.tpc = 0x0060;
}

/* ======================= 0x0060: a plain strip ======================= */
static void run_plain(Ctx *c, u32 top)
{
    u16 vi15 = (u16)top;
    u16 n = (u16)(QW(c, vi15)[0] & 0x7FFF);
    u32 d = (u16)(n - 3);
    if (!d) d = 65536;                 /* the counter (n - 3, one down per vertex) runs to 0 */
    u32 nv = d + 3;
    int odd = d & 1;                   /* the loop leaves by its first half: the other epilogue */
    u16 rd = vi15 + 1, wr = vi15 + 1;
    u32 pos[4][4], stq[4][4], P[4], S[4], T[4];
    for (int k = 0; k < 3; k++) { ld(c, pos[k], rd + 2); ld(c, stq[k], rd); rd += 3; }
    for (u32 m = 0; m < nv; m++) {
        if (m + 3 <= nv) { ld(c, pos[(m + 3) & 3], rd + 2); ld(c, stq[(m + 3) & 3], rd); rd += 3; }
        xform(P, 18, 19, 20, 21, pos[m & 3]);
        float q = vdiv(F(odd && m == nv - 1 ? RF(29)[3] : RF(31)[3]), F(P[3]));
        mulq3(S, P, q, P[3]);
        for (int k = 0; k < 4; k++) S[k] = U(fc(F(S[k]) + F(RF(30)[k])));
        S[3] = wmax(S[3]);
        S[3] = wmin(S[3]);
        mulq3(T, stq[m & 3], q, RF(TREG[m & 1])[3]);
        sq(c, wr, T);
        ftoi4(S, S);
        sq(c, wr + 2, S);
        wr += 3;
    }
    /* the registers as the pipeline leaves them: vertex nv (read ahead) transformed and projected
     * with vf29.w (the epilogue's DIV), vertex nv - 1 as written */
    int p = nv & 1;
    u32 PT[4];
    xform(PT, 18, 19, 20, 21, pos[nv & 3]);
    float qT = vdiv(F(RF(29)[3]), F(PT[3]));
    memcpy(RF(PREG[p]), PT, 16);
    memcpy(RF(PREG[!p]), P, 16);
    memcpy(RF(RREG[p]), stq[nv & 3], 16);
    memcpy(RF(RREG[!p]), stq[(nv - 1) & 3], 16);
    mulq3(RF(SREG[p]), PT, qT, PT[3]);
    memcpy(RF(SREG[!p]), S, 16);
    mulq3(RF(TREG[p]), stq[nv & 3], qT, RF(TREG[p])[3]);
    memcpy(RF(TREG[!p]), T, 16);
    R.q = R.q_new = U(qT);
    R.vi[1] = 0x7FFF; R.vi[6] = (u16)(rd - 3); R.vi[7] = (u16)(wr - 3); R.vi[12] = 0; R.vi[15] = vi15;
    kick(c, vi15);
    if (odd) { ld(c, RF(31), 940); R.tpc = 0x04A0; }       /* (the end's delay slots) */
    else { ld(c, RF(8), (u16)(rd - 3) + 2); R.tpc = 0x0390; }
}

/* ======================= the clipper (0x0838) ======================= */
/* ENTER (0x1158): the crossing, then the vertex */
static void emit_enter(Ctx *c, u16 *o)
{
    float w1 = F(RF(0)[3]);
    u32 *v11 = RF(11);
    static const int a[3] = { 3, 14, 15 }, b[3] = { 10, 16, 17 };
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 4; k++) {
            v11[k] = U(fc(F(RF(b[r])[k]) - F(RF(a[r])[k])));
            float acc = fc(F(v11[k]) * c->q);
            R.acc[k] = U(acc);
            v11[k] = U(fc(acc + fc(F(RF(a[r])[k]) * w1)));
        }
        sq(c, (*o)++, v11);
    }
    sq(c, (*o)++, RF(10)); sq(c, (*o)++, RF(16)); sq(c, (*o)++, RF(17));
}
/* EXIT (0x11E0): the crossing only (left in vf10 vf16 vf17) */
static void emit_exit(Ctx *c, u16 *o)
{
    float w1 = F(RF(0)[3]);
    static const int a[3] = { 3, 14, 15 }, b[3] = { 10, 16, 17 };
    for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) RF(b[r])[k] = U(fc(F(RF(b[r])[k]) - F(RF(a[r])[k])));
    for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) {
        float acc = fc(F(RF(b[r])[k]) * c->q);
        R.acc[k] = U(acc);
        RF(b[r])[k] = U(fc(acc + fc(F(RF(a[r])[k]) * w1)));
    }
    for (int r = 0; r < 3; r++) sq(c, (*o)++, RF(b[r]));
}

static void clipper(Ctx *c, u16 *vi6, u16 *vi7, u16 *vi15)
{
    const float w1 = F(RF(0)[3]);
    u32 *v3 = RF(3), *v10 = RF(10), *v14 = RF(14), *v15 = RF(15), *v11 = RF(11);
    v3[3] = U(fc(w1 * c->q));                      /* Q, kept over the call */
    R.i = 0x40400000u;                             /* 3 */
    for (int r = 0; r < 3; r++) {                  /* vf14..16: vf28 vf27 vf26, x and y times I */
        const u32 *s = RF(28 - r); u32 *g = RF(14 + r);
        g[2] = s[2]; g[3] = s[3];
        g[0] = U(fc(F(s[0]) * F(R.i))); g[1] = U(fc(F(s[1]) * F(R.i)));
    }
    u32 *s = QW(c, 947);
    s[0] = *vi6; s[1] = *vi7; s[2] = *vi15; s[3] = v3[3];
    clipw(c, RF(14)); clipw(c, RF(15)); clipw(c, RF(16));
    static const u32 XY[4] = { 0xFF7DF7, 0xFFBEFB, 0xFFDF7D, 0xFFEFBE };
    u32 vi1;
    if (chain(c, XY, 4, &vi1)) goto restore;
    {
        /* the polygon: positions vf28 vf27 vf26, s t q from the strip */
        u16 a = *vi7 - 3;
        sq(c, 948, RF(28));
        ld(c, v10, a); c->q = vdiv(w1, F(v10[2])); mulq3(v10, v10, c->q, v10[3]); sq(c, 949, v10);
        ld(c, v3, a + 1); for (int k = 0; k < 4; k++) v3[k] = itof(v3[k], 1.0f); sq(c, 950, v3);
        sq(c, 951, RF(27));
        ld(c, v10, a + 3); c->q = vdiv(w1, F(v10[2])); mulq3(v10, v10, c->q, v10[3]); sq(c, 952, v10);
        ld(c, v3, a + 4); for (int k = 0; k < 4; k++) v3[k] = itof(v3[k], 1.0f); sq(c, 953, v3);
        sq(c, 954, RF(26));
        ld(c, v10, a + 6); sq(c, 955, v10);
        ld(c, v3, a + 7); for (int k = 0; k < 4; k++) v3[k] = itof(v3[k], 1.0f); sq(c, 956, v3);

        /* the planes: component, w + or - it (plus), its clip flag; each one's return address */
        static const struct { int comp, plus, bit; u16 ret; } pl[5] = {
            { 2, 0, 5, 341 }, { 0, 0, 1, 381 }, { 0, 1, 0, 422 }, { 1, 0, 3, 462 }, { 1, 1, 2, 503 } };
        u16 n = 3, cnt = 0, in = 948, out = in + 25, nb = out, i10 = 0, i11 = 0, i12 = 0;   /* vi7 vi9 vi15 vi8 vi5 vi10..12 */
        for (int p = 0; p < 5; p++) {
            int cc = pl[p].comp;
            u32 fb = 1u << pl[p].bit, fp = fb << 6;
            i12 = n - 1; i11 = i12 + i12; i11 = i11 + i12; i11 = i11 + in; i12 = in;
            do {
                ld(c, v3, i11); ld(c, v10, i12); ld(c, v14, i11 + 1); ld(c, v15, i11 + 2);
                clipw(c, v3);
                ld(c, RF(16), i12 + 1); ld(c, RF(17), i12 + 2);
                v11[cc] = U(fc(F(v10[cc]) - F(v3[cc])));
                v11[3] = pl[p].plus ? U(fc(F(v3[3]) - F(v10[3]))) : U(fc(F(v10[3]) - F(v3[3])));
                v11[3] = U(fc(F(v11[3]) + F(v11[cc])));
                v11[cc] = pl[p].plus ? U(fc(F(RF(0)[cc]) + F(v3[3]))) : U(fc(F(RF(0)[cc]) - F(v3[3])));
                v11[cc] = U(fc(F(v11[cc]) - F(v3[cc])));
                c->q = vdiv(F(v11[cc]), F(v11[3]));
                clipw(c, v10);
                i10 = pl[p].ret;
                if (fcor(c, 0xFFFFFF & ~(fb | fp))) ;                       /* both outside */
                else if (fcor(c, 0xFFFFFF & ~fp)) { emit_enter(c, &out); cnt += 2; }
                else if (fcor(c, 0xFFFFFF & ~fb)) { cnt += 1; emit_exit(c, &out); }
                else { sq(c, out++, v10); sq(c, out++, RF(16)); sq(c, out++, RF(17)); cnt += 1; }
                n--; i11 = i12; i12 = i12 + 3;
            } while (n != 0);
            i10 = cnt - 3;
            u16 o = in;
            in = nb;
            if ((s16)i10 < 0) { out = o; goto done; }   /* fewer than three vertices: nothing drawn */
            nb = o; n = cnt; cnt = 0; out = o;
        }
        /* the fan */
        out = 998; i11 = 998;
        sq(c, out++, RF(31));
        kick(c, 998);
        i11 = 999;
        v3[1] = RF(31)[1]; v3[2] = RF(31)[2]; v3[3] = RF(31)[3];
        i10 = (u16)RF(31)[0];
        i10 = i10 + n;
        v3[0] = (u32)(s32)(s16)i10;
        sq(c, out++, v3);
        do {
            ld(c, v3, in++); ld(c, v14, in++); ld(c, v15, in++);
            c->q = vdiv(w1, F(v3[3]));
            mulq3(v3, v3, c->q, v3[3]);
            mulq3(v14, v14, c->q, v14[3]);
            for (int k = 0; k < 4; k++) v15[k] = (u32)ftoi(F(v15[k]), 1.0f);
            scale(v3);
            n--;
            v3[3] = wmin(v3[3]);
            v3[3] = wmax(v3[3]);
            sq(c, out++, v14);
            ftoi4(v3, v3);
            sq(c, out++, v15);
            sq(c, out++, v3);
        } while (n != 0);
        kick(c, 999);
    done:
        R.vi[5] = nb; R.vi[8] = out; R.vi[9] = cnt; R.vi[10] = i10; R.vi[11] = i11; R.vi[12] = i12;
    }
restore:
    clipw(c, RF(28));
    v3[3] = QW(c, 947)[3];
    c->q = vdiv(F(v3[3]), w1);
    clipw(c, RF(27));
    clipw(c, RF(26));
    *vi6 = (u16)QW(c, 947)[0]; *vi7 = (u16)QW(c, 947)[1]; *vi15 = (u16)QW(c, 947)[2];
}

/* ======================= 0x0070: a clipped strip ======================= */
static void run_clipped(Ctx *c, u32 top)
{
    static const u32 TESTS[5] = { 0xFDF7DF, 0xFF7DF7, 0xFFBEFB, 0xFFDF7D, 0xFFEFBE };   /* -z -y +y -x +x */
    const float w1 = F(RF(0)[3]);
    ld(c, RF(31), 940);
    u16 vi15 = (u16)top;
    u16 left = (u16)(QW(c, vi15)[0] & 0x7FFF);      /* vi13 */
    u16 rd = vi15 + 1, wr = vi15 + 1;              /* vi6 (reads), vi7 (writes) */
    R.i = 0x45000000u;                             /* 2048 */
    RF(4)[3] = U(fc(w1 * F(R.i)));                 /* + 2048 to a screen w is the ADC bit */
    const float adc = F(RF(4)[3]);
    u32 pos[4], stq[4][4], P[4][4];
    u32 Sa[4], Sb[4], Ta[4], Tb[4];                /* screen / s t q of the vertex before, the vertex */
    u32 vi1 = 0x7FFF;
    ld(c, pos, rd + 2); ld(c, stq[0], rd); rd += 3;
    xform(P[0], 22, 23, 24, 25, pos);
    ld(c, pos, rd + 2); ld(c, stq[1], rd); rd += 3;
    c->q = vdiv(w1, F(P[0][3]));
    xform(P[1], 22, 23, 24, 25, pos);
    mulq3(Sa, P[0], c->q, P[0][3]);
    mulq3(Ta, stq[0], c->q, RF(13)[3]);
    clipw(c, P[0]); shift(P[0]);
    ld(c, pos, rd + 2); ld(c, stq[2], rd); rd += 3;
    c->q = vdiv(w1, F(P[1][3]));
    scale(Sa);
    xform(P[2], 22, 23, 24, 25, pos);
    Sa[3] = wmin(Sa[3]);                          /* the first vertex: min, then max */
    mulq3(Sb, P[1], c->q, P[1][3]);
    mulq3(Tb, stq[1], c->q, RF(6)[3]);
    R.vi[2] = 0; R.vi[3] = 0;
    Sa[3] = wmax(Sa[3]);
    u32 k;
    for (k = 1;; k++) {
        clipw(c, P[k & 3]); shift(P[k & 3]);
        u32 xyz[4]; ftoi4(xyz, Sa);
        ld(c, pos, rd + 2);
        c->q = vdiv(w1, F(P[(k + 1) & 3][3]));
        ld(c, stq[(k + 2) & 3], rd); rd += 3;
        scale(Sb);
        int out = (c->clip & 0x2FBEF) != 0;        /* the triangle has a vertex outside (but far) */
        vi1 = (u32)out;
        left--;
        Sb[3] = wmax(Sb[3]);
        R.vi[k & 1 ? 3 : 2] = vi1;
        xform(P[(k + 2) & 3], 22, 23, 24, 25, pos);
        sq(c, wr, Ta); sq(c, wr + 2, xyz);
        Sb[3] = wmin(Sb[3]);
        if (out) {
            Sb[3] = U(fc(F(Sb[3]) + adc));
            if (!chain(c, TESTS, 5, &vi1)) {
                R.vi[14] = k & 1 ? 204 : 220;      /* (the return address: 0x0660 / 0x06E0) */
                if ((s16)left > 0) {
                    vi1 = (u16)(wr - vi15 - 3);
                    if ((s16)vi1 > 0) clipper(c, &rd, &wr, &vi15);
                }
            }
        }
        memcpy(Sa, Sb, 16); memcpy(Ta, Tb, 16);
        mulq3(Sb, P[(k + 1) & 3], c->q, P[(k + 1) & 3][3]);
        mulq3(Tb, stq[(k + 1) & 3], c->q, RF(TREG[(k + 1) & 1])[3]);
        wr += 3;
        if (left == 0) break;
    }
    /* the registers as the pipeline leaves them (vertex k last; k + 1 and k + 2 read ahead) */
    int p = k & 1;
    memcpy(RF(PREG[p]), P[(k + 2) & 3], 16);
    memcpy(RF(PREG[!p]), P[(k + 1) & 3], 16);
    memcpy(RF(RREG[p]), stq[(k + 2) & 3], 16);
    memcpy(RF(RREG[!p]), stq[(k + 1) & 3], 16);
    memcpy(RF(SREG[p]), Sa, 16);
    memcpy(RF(SREG[!p]), Sb, 16);
    memcpy(RF(TREG[p]), Ta, 16);
    memcpy(RF(TREG[!p]), Tb, 16);
    R.q = R.q_new = U(c->q);
    R.vi[1] = vi1; R.vi[6] = rd; R.vi[7] = wr; R.vi[13] = left; R.vi[15] = vi15;
    kick(c, vi15);
    R.tpc = 0x0708;
}

int native_fx57843_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeFx57843Packet *pk, int maxpk)
{
    Ctx c;
    memset(&c, 0, sizeof c);
    c.mem = mem; c.out = out; c.out_cap = out_cap; c.pk = pk; c.maxpk = maxpk; c.pending = 0xFFFFFFFFu;
    c.clip = R.clip & 0xFFFFFF;
    switch (pc) {
    case 0x0000: run_setup(&c); break;
    case 0x0060: case 0x0078: run_plain(&c, top); break;
    case 0x0068: case 0x0070: run_clipped(&c, top); break;
    default: return -1;
    }
    R.clip = c.clip;
    if (c.pending != 0xFFFFFFFFu) send(&c, c.pending);
    return c.npk;
}
