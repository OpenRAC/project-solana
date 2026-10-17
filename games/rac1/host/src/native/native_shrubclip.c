/* Shrubs that cross the screen's edges, drawn by native code instead of by the PS2's VU1 shrub
 * clip microprogram (image 912339). The game sends a shrub instance that crosses the frustum to
 * this program instead of the main shrub program (56467, host/src/native/native_shrub.c); the
 * data formats are the same.
 *
 * Starts:
 *   0x0000  once after the program is loaded: the constants in qwords 0..5 go to registers
 *           (vf31 the A+D GIF tag, vf30 x: 3*2^23+2064 (the three output buffers' sum), y: 2^23
 *           (the integer-in-float bias), z/w fog scale/offset, vf29 x/y fog clamp, vf22/vf23
 *           screen scale/offset, vf24 a one-qword GIF tag sent before each clipped polygon);
 *           vf17 = the output buffers (856, 520, 856, 520) + 2^23; vi11 = 120.
 *   0x0088 / 0x00A8  a shrub class, uploaded at qword 122 - vi11 (2 or 120, alternating): the
 *           next instance group goes to 238 (0x00A8) or 379 (0x0088), kept in qword 1 y.
 *           vf17.x/z become the buffer neither of x, y used (2064 - x - y); the strip GIF tags and
 *           the A+D blocks (each preceded by vf31) are written to buffers x and y at the offsets in
 *           their w words; the class's vertices are converted in place (positions int -> float,
 *           ST 4.12 -> float).
 *           Class: header (x: A+D blocks, y: strips, z: vertices, w: vertex offset), the strip
 *           tags (w: offset in the packet), the A+D blocks (4 qwords, w of the first: offset),
 *           then the positions (w: the vertex's slot in the packet) and the ST (w: colour index).
 *   0x0318  a group of instances at 617 - qword 1 y (toggled each run): x the count, then per
 *           instance 28 qwords: the 4x4 transform (rows 0..3), a colour table (w of its first
 *           qword: alpha; colours at table + (ST w & 0x7FFF)). Per instance, pass 1 writes every
 *           vertex's ST (z *= 1/w), RGBA and clip-space position to its slot in buffer vf17.w;
 *           pass 2 walks the strips: XYZ = pos * q * vf22 + vf23 (ftoi4), ST *= q, F = fog; a
 *           vertex that ends a triangle with a vertex outside the clip box gets ADC (skipped by
 *           the GS); unless all three are outside one plane (x+-, y+-, z-) the triangle is
 *           clipped (Sutherland-Hodgman: z-, x-, x+, y-, y+) in the third buffer and sent as its
 *           own packet: vf24, then A+D tag + the strip's texture block (the last one at or before
 *           the strip's offset) + a fan tag (vf24.x + n) + n vertices. The instance's buffer is
 *           kicked at the end and vf17 rotated (x y z w <- y z w x).
 *
 * native_shrubclip_run() does what the program does, word for word: the same loads and stores of
 * VU memory in the same order (the buffers are reused stale by later runs), the same float
 * operations in the same order with the VU's clamp of inf/NaN results to +-FLT_MAX (the host runs
 * SSE rounding toward zero), ftoi/itof as the VU. Pipeline effects the program sees: none (every
 * clip flag and Q read comes after the result is ready), so the clip flags are a plain 24-bit
 * history and Q the last DIV. Checked by dev/vu_harness (native_shrubclip_h.c, shrubclip_fuzz.c). */
#include "native_shrubclip.h"
#include <string.h>
#include <math.h>

typedef union { f32 f[4]; u32 u[4]; } V4;

/* the registers the program keeps from one run to the next */
static struct {
    V4 vf17, vf28;                       /* output buffers (+2^23); vf28.x/y: added to the kick address */
    V4 c22, c23, c24, c29, c30, c31;     /* the constants loaded by 0x0000 */
    u16 vi11, vi12;                      /* where the class is; its vertex count */
} S;

void native_shrubclip_reset(void) { memset(&S, 0, sizeof S); }

void native_shrubclip_seed(const u32 vf[32][4], const u32 vi[16])
{
    memcpy(S.vf17.u, vf[17], 16); memcpy(S.vf28.u, vf[28], 16);
    memcpy(S.c22.u, vf[22], 16); memcpy(S.c23.u, vf[23], 16); memcpy(S.c24.u, vf[24], 16);
    memcpy(S.c29.u, vf[29], 16); memcpy(S.c30.u, vf[30], 16); memcpy(S.c31.u, vf[31], 16);
    S.vi11 = (u16)vi[11]; S.vi12 = (u16)vi[12];
}

/* the carried registers back into a register file (for a caller that keeps the VU1's registers
 * shared with programs that are not native) */
void native_shrubclip_state(u32 vf[32][4], u32 vi[16])
{
    memcpy(vf[17], S.vf17.u, 16); memcpy(vf[28], S.vf28.u, 16);
    memcpy(vf[22], S.c22.u, 16); memcpy(vf[23], S.c23.u, 16); memcpy(vf[24], S.c24.u, 16);
    memcpy(vf[29], S.c29.u, 16); memcpy(vf[30], S.c30.u, 16); memcpy(vf[31], S.c31.u, 16);
    vi[11] = S.vi11; vi[12] = S.vi12;
}

static u8 *M;
static u32 *Q(u32 a) { return (u32 *)(M + (a & 0x3FF) * 16); }
static void ld(V4 *d, u32 a) { memcpy(d->u, Q(a), 16); }
static void ld3(V4 *d, u32 a) { memcpy(d->u, Q(a), 12); }
static void st(u32 a, const V4 *s) { memcpy(Q(a), s->u, 16); }
static void st3(u32 a, const V4 *s) { memcpy(Q(a), s->u, 12); }
static f32 F(u32 u) { f32 f; memcpy(&f, &u, 4); return f; }
static u32 U(f32 f) { u32 u; memcpy(&u, &f, 4); return u; }
static f32 fc(f32 x) { u32 b = U(x); if ((b & 0x7F800000u) == 0x7F800000u) x = F((b & 0x80000000u) | 0x7F7FFFFFu); return x; }
static f32 vdiv(f32 a, f32 b)
{
    if (b == 0.0f) return ((U(a) ^ U(b)) & 0x80000000u) ? -3.4028235e38f : 3.4028235e38f;
    return a / b;
}
static s32 ftoi(f32 f, f32 scale)
{
    f32 x = f * scale;
    if (x >= 2147483647.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return (s32)0x80000000u;
    if (x != x) return 0;
    return (s32)x;
}
static u32 itof(u32 i, f32 inv) { return U((f32)(s32)i * inv); }
/* the 6 clip flags of a vertex (CLIPw): x+ x- y+ y- z+ z- */
static u32 clipf(const V4 *v)
{
    f32 a = fabsf(v->f[3]);
    return (v->f[0] > a) | ((v->f[0] < -a) << 1) | ((v->f[1] > a) << 2) | ((v->f[1] < -a) << 3) | ((v->f[2] > a) << 4) | ((v->f[2] < -a) << 5);
}

/* XGKICK as the host does it: a packet goes to the GIF when the next is kicked or the run ends,
 * read from VU memory then */
static void (*E)(const u32 *, u32);
static u32 kick_pending;
static void kick_send(u32 qaddr)
{
    static u32 pk[0x400 * 4];
    u32 q = qaddr & 0x3FF, n = 0;
    while (n < 0x400) {
        const u32 *t = Q(q);
        u64 lo = ((u64)t[1] << 32) | t[0];
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        for (u32 k = 0; k < len && n < 0x400; k++) { memcpy(pk + n * 4, Q(q + k), 16); n++; }
        q = (q + len) & 0x3FF;
        if (eop) break;
    }
    E(pk, n);
}
static void kick(u32 qaddr) { if (kick_pending != 0xFFFFFFFFu) kick_send(kick_pending); kick_pending = qaddr & 0x3FF; }

/* 0x0088 / 0x00A8: a shrub class */
static void run_class(u32 grp)
{
    V4 vf1, vf2, vf5, vf18, vf19, vf20, vf21;
    u16 vi1, vi2, vi3, vi4, vi5, vi6, vi8, vi9, vi14, vi15;
    f32 x0 = S.vf17.f[0];
    S.vf17.f[0] = fc(0.0f - x0); S.vf17.f[2] = fc(0.0f - x0);
    S.vf28.f[0] = fc(0.0f + S.c30.f[1]); S.vf28.f[1] = fc(0.0f + S.c30.f[1]);
    S.vf28.f[2] = fc(0.0f + S.c30.f[1]); S.vf28.f[3] = fc(1.0f + S.c30.f[1]);
    S.vi11 = (u16)(122 - S.vi11);
    Q(1)[1] = grp;
    S.vf17.f[0] = fc(S.vf17.f[0] - S.vf17.f[1]); S.vf17.f[2] = fc(S.vf17.f[2] - S.vf17.f[1]);
    vi9 = S.vi11 + 1;
    vi5 = (u16)Q(S.vi11)[1]; vi15 = (u16)Q(S.vi11)[0];
    ld(&vf1, vi9++);
    S.vf17.f[0] = fc(S.vf17.f[0] + S.c30.f[0]); S.vf17.f[2] = fc(S.vf17.f[2] + S.c30.f[0]);
    vi15--;
    vi4 = (u16)vf1.u[3]; vi2 = (u16)S.vf17.u[0]; vi3 = (u16)S.vf17.u[1];
    if (vi15 == 0) S.vf28.f[1] = fc(S.vf28.f[1] + S.c31.f[2]);
    /* the strip tags to both buffers (two at a time, one read ahead) */
    for (;;) {
        ld(&vf2, vi9++);
        vi1 = vi4 + vi2; vi14 = vi4 + vi3; vi5--; vi4 = (u16)vf2.u[3];
        st(vi1, &vf1); st(vi14, &vf1);
        if (vi5 == 0) break;
        ld(&vf1, vi9++);
        vi1 = vi4 + vi2; vi14 = vi4 + vi3; vi5--; vi4 = (u16)vf1.u[3];
        st(vi1, &vf2); st(vi14, &vf2);
        if (vi5 == 0) break;
    }
    vi9--;
    /* the A+D blocks, each after an A+D tag */
    vi5 = (u16)Q(S.vi11)[0];
    do {
        ld(&vf18, vi9++); ld(&vf19, vi9++); ld(&vf20, vi9++); ld(&vf21, vi9++);
        vi4 = (u16)vf18.u[3]; vi5--;
        vi1 = vi4 + vi2;
        st(vi1, &S.c31); st(vi1 + 1, &vf18); st(vi1 + 2, &vf19); st(vi1 + 3, &vf20); st(vi1 + 4, &vf21);
        vi1 = vi4 + vi3;
        st(vi1, &S.c31); st(vi1 + 1, &vf18); st(vi1 + 2, &vf19); st(vi1 + 3, &vf20); st(vi1 + 4, &vf21);
    } while (vi5 != 0);
    /* the vertices, in place, two at a time */
    S.vi12 = (u16)Q(S.vi11)[2]; vi6 = (u16)Q(S.vi11)[3];
    vi5 = 1 + S.vi12; vi6 += S.vi11; vi8 = vi6 + S.vi12;
    for (;;) {
        ld(&vf1, vi6); ld3(&vf5, vi8); ld(&vf18, vi6 + 1);
        vi5 -= 2;
        for (int k = 0; k < 4; k++) vf1.u[k] = itof(vf1.u[k], 1.0f);
        ld3(&vf19, vi8 + 1);
        for (int k = 0; k < 3; k++) vf5.u[k] = itof(vf5.u[k], 1.0f / 4096.0f);
        for (int k = 0; k < 4; k++) vf18.u[k] = itof(vf18.u[k], 1.0f);
        for (int k = 0; k < 3; k++) vf19.u[k] = itof(vf19.u[k], 1.0f / 4096.0f);
        st(vi6++, &vf1);
        st3(vi8++, &vf5);
        if (vi5 == 0) break;
        st(vi6++, &vf18);
        st3(vi8++, &vf19);
        if (vi5 == 1) break;
    }
}

/* the clipper's per plane formulas: the distances' difference and the first vertex's distance */
enum { PZN, PXN, PXP, PYN, PYP };
static const u32 plane_bit[5] = { 0x20, 0x02, 0x01, 0x08, 0x04 };

/* the intersection of the edge vf1 -> vf5 (and its ST, colour) at q, to *vi8 */
static void edge_point(u16 *vi8, const V4 *vf1, const V4 *vf18, const V4 *vf19, const V4 *vf5, const V4 *vf20, const V4 *vf21, f32 q)
{
    const V4 *a[3] = { vf1, vf18, vf19 }, *b[3] = { vf5, vf20, vf21 };
    for (int j = 0; j < 3; j++) {
        V4 d, acc;
        for (int k = 0; k < 4; k++) d.f[k] = fc(b[j]->f[k] - a[j]->f[k]);
        for (int k = 0; k < 4; k++) acc.f[k] = fc(d.f[k] * q);
        for (int k = 0; k < 4; k++) d.f[k] = fc(acc.f[k] + fc(a[j]->f[k] * 1.0f));
        st((*vi8)++, &d);
    }
}

/* the triangle ending at the vertex before vi6 is clipped (0x06B0 .. 0x10B8); the registers it
 * keeps are saved in and restored from the third buffer as the program does */
static void clip_tri(u16 vi6, u16 *pvi2, u16 *pvi3, u16 *pvi9, u16 *pvi5, u16 *pvi7,
                     const V4 *vf25, const V4 *vf26, const V4 *vf27, const V4 *vf8, const V4 *vf2w, u32 vf4w)
{
    V4 vf1, vf2, vf4, vf5, vf18, vf19, vf20, vf21, vf6;
    u16 vi2, vi3, vi4, vi5, vi7, vi8, vi9, vi15;
    f32 q;
    f32 t = fc(0.0f - S.vf17.f[0]); t = fc(t - S.vf17.f[1]); t = fc(t + S.c30.f[0]);
    vi15 = (u16)U(t);
    Q(vi15)[0] = *pvi2; Q(vi15)[1] = *pvi3; Q(vi15)[2] = *pvi9; Q(vi15)[3] = *pvi5;
    Q(vi15 + 1)[0] = *pvi7;
    vi15 += 4;
    /* the triangle's three vertices (newest first): clip-space position, ST / q, colour */
    const V4 *pos[3] = { vf25, vf26, vf27 };
    for (int j = 0; j < 3; j++) {
        ld(&vf5, vi6 - 3 - 3 * j);
        q = vdiv(1.0f, vf5.f[2]);
        ld(&vf1, vi6 - 2 - 3 * j);
        for (int k = 0; k < 4; k++) vf1.u[k] = itof(vf1.u[k], 1.0f);
        st(vi15 + 3 * j, pos[j]);
        st(vi15 + 3 * j + 2, &vf1);
        for (int k = 0; k < 3; k++) vf5.f[k] = fc(vf5.f[k] * q);
        st(vi15 + 3 * j + 1, &vf5);
    }
    vi7 = 3; vi9 = 0; vi8 = vi15 + 36; vi5 = vi8;
    for (int p = 0; p < 5; p++) {
        u32 bit = plane_bit[p];
        vi4 = vi7 - 1; vi3 = vi4 + vi4; vi3 = vi3 + vi4; vi3 = vi3 + vi15; vi4 = vi15;
        do {
            ld(&vf1, vi3); ld(&vf5, vi4); ld(&vf18, vi3 + 1); ld(&vf19, vi3 + 2); ld(&vf20, vi4 + 1); ld(&vf21, vi4 + 2);
            u32 o1 = clipf(&vf1) & bit, o5 = clipf(&vf5) & bit;
            switch (p) {
            case PZN:
                vf6.f[2] = fc(vf5.f[2] - vf1.f[2]); vf6.f[3] = fc(vf5.f[3] - vf1.f[3]);
                vf6.f[3] = fc(vf6.f[3] + vf6.f[2]);
                vf6.f[2] = fc(0.0f - vf1.f[3]); vf6.f[2] = fc(vf6.f[2] - vf1.f[2]);
                q = vdiv(vf6.f[2], vf6.f[3]);
                break;
            case PXN: case PYN: {
                int c = p == PXN ? 0 : 1;
                vf6.f[c] = fc(vf5.f[c] - vf1.f[c]); vf6.f[3] = fc(vf5.f[3] - vf1.f[3]);
                vf6.f[3] = fc(vf6.f[3] + vf6.f[c]);
                vf6.f[c] = fc(0.0f - vf1.f[3]); vf6.f[c] = fc(vf6.f[c] - vf1.f[c]);
                q = vdiv(vf6.f[c], vf6.f[3]);
                break;
            }
            default: {
                int c = p == PXP ? 0 : 1;
                vf6.f[c] = fc(vf5.f[c] - vf1.f[c]); vf6.f[3] = fc(vf1.f[3] - vf5.f[3]);
                vf6.f[3] = fc(vf6.f[3] + vf6.f[c]);
                vf6.f[c] = fc(0.0f + vf1.f[3]); vf6.f[c] = fc(vf6.f[c] - vf1.f[c]);
                q = vdiv(vf6.f[c], vf6.f[3]);
                break;
            }
            }
            if (o1 && o5) {
                /* both outside: nothing */
            } else if (o1) {            /* coming in: the intersection, then the vertex */
                edge_point(&vi8, &vf1, &vf18, &vf19, &vf5, &vf20, &vf21, q);
                st(vi8++, &vf5); st(vi8++, &vf20); st(vi8++, &vf21);
                vi9 += 2;
            } else if (o5) {            /* going out: the intersection */
                edge_point(&vi8, &vf1, &vf18, &vf19, &vf5, &vf20, &vf21, q);
                vi9 += 1;
            } else {                    /* inside: the vertex */
                st(vi8++, &vf5); st(vi8++, &vf20); st(vi8++, &vf21);
                vi9 += 1;
            }
            vi7--; vi3 = vi4; vi4 += 3;
        } while (vi7 != 0);
        vi8 = vi15; vi15 = vi5; vi5 = vi8; vi7 = vi9; vi9 = 0;
        if ((s16)(u16)(vi7 - 3) < 0) goto restore;
    }
    /* the polygon's packet at the third buffer + 76: vf24 (kicked alone), then A+D tag, the
       strip's texture block, the fan's tag, the vertices */
    t = fc(0.0f - S.vf17.f[0]); t = fc(t - S.vf17.f[1]); t = fc(t + S.c30.f[0]);
    vi2 = (u16)U(t);
    vi8 = vi2 + 76; vi3 = vi8;
    st(vi8++, &S.c24);
    kick(vi3);
    vi5 = (u16)Q(S.vi11)[1]; vi3 = (u16)Q(S.vi11)[0];
    vi9 = S.vi11 + 1; vi9 += vi5;
    for (;;) {                          /* the last A+D block at or before the strip's offset */
        ld(&vf18, vi9);
        vi4 = (u16)vf8->u[3]; vi2 = (u16)vf18.u[3];
        vi4 -= vi2; vi3--;
        if ((s16)vi4 < 0) break;
        vi9 += 4;
        if (vi3 == 0) break;
    }
    vi9 -= 4;
    ld(&vf18, vi9++); ld(&vf19, vi9++); ld(&vf20, vi9++); ld(&vf21, vi9++);
    vi3 = vi8;
    st(vi8++, &S.c31); st(vi8++, &vf18); st(vi8++, &vf19); st(vi8++, &vf20); st(vi8++, &vf21);
    vf1.u[1] = S.c24.u[1]; vf1.u[2] = S.c24.u[2]; vf1.u[3] = S.c24.u[3];
    vi2 = (u16)S.c24.u[0]; vi2 += vi7;
    vf1.u[0] = (u32)(s32)(s16)vi2;
    st(vi8++, &vf1);
    vf2 = *vf2w; vf4.u[3] = vf4w;
    do {
        ld(&vf1, vi15++); ld(&vf18, vi15++); ld(&vf19, vi15++);
        q = vdiv(1.0f, vf1.f[3]);
        for (int k = 0; k < 3; k++) vf18.f[k] = fc(vf18.f[k] * q);
        for (int k = 0; k < 3; k++) vf2.f[k] = fc(vf1.f[k] * q);
        f32 acc[3];
        for (int k = 0; k < 3; k++) acc[k] = fc(vf2.f[k] * S.c22.f[k]);
        for (int k = 0; k < 3; k++) vf2.f[k] = fc(acc[k] + fc(S.c23.f[k] * 1.0f));
        for (int k = 0; k < 3; k++) vf2.u[k] = (u32)ftoi(vf2.f[k], 16.0f);
        vi7--;
        for (int k = 0; k < 3; k++) vf4.u[k] = (u32)ftoi(vf19.f[k], 1.0f);
        st(vi8++, &vf18); st(vi8++, &vf4); st(vi8++, &vf2);
    } while (vi7 != 0);
    kick(vi3);
restore:
    t = fc(0.0f - S.vf17.f[0]); t = fc(t - S.vf17.f[1]); t = fc(t + S.c30.f[0]);
    vi15 = (u16)U(t);
    *pvi2 = (u16)Q(vi15)[0]; *pvi3 = (u16)Q(vi15)[1]; *pvi9 = (u16)Q(vi15)[2]; *pvi5 = (u16)Q(vi15)[3];
    *pvi7 = (u16)Q(vi15 + 1)[0];
}

/* 0x0318: a group of instances */
static void run_group(void)
{
    V4 vf1, vf2, vf4, vf5, vf6, vf7, vf8, vf14, vf15, vf16, vf18, vf19, vf20, vf21, vf25, vf26, vf27, acc;
    u16 vi1, vi2, vi3, vi4, vi5, vi6, vi7, vi8, vi9, vi13, vi14, vi15;
    const u16 vi10 = 0x7FFF;
    u32 vf3w, clip = 0;
    f32 q;
    memset(&vf25, 0, sizeof vf25); vf26 = vf25; vf27 = vf25; vf2 = vf25;
    vi13 = (u16)(617 - (u16)Q(1)[1]);
    Q(1)[1] = vi13;
    vi14 = (u16)Q(vi13)[0];
    vi13 += 5;
    do {
        /* pass 1: every vertex's ST (z / w), colour and clip-space position to its slot */
        ld(&vf21, vi13 - 1);
        vi6 = (u16)Q(S.vi11)[3];
        ld(&vf18, vi13 - 4); ld(&vf19, vi13 - 3);
        f32 accw = fc(vf21.f[3] * S.c30.f[2]);
        ld(&vf20, vi13 - 2);
        vf2.f[3] = fc(accw + fc(S.c30.f[3] * 1.0f));
        vf4.u[3] = Q(vi13)[3];
        vi6 += S.vi11;
        vi8 = vi6 + S.vi12;
        vf2.f[3] = fc(fmaxf(vf2.f[3], S.c29.f[0]));
        vi3 = S.vi12;
        vf2.f[3] = fc(fminf(vf2.f[3], S.c29.f[1]));
        ld(&vf14, vi6++); ld(&vf15, vi8++);
        for (;;) {
            for (int k = 0; k < 4; k++) acc.f[k] = fc(vf21.f[k] * 1.0f);
            for (int k = 0; k < 4; k++) vf16.f[k] = fc(vf14.f[k] + S.vf17.f[k]);
            ld(&vf1, vi6++);
            for (int k = 0; k < 4; k++) acc.f[k] = fc(acc.f[k] + fc(vf18.f[k] * vf14.f[0]));
            vi4 = (u16)vf15.u[3];
            for (int k = 0; k < 4; k++) acc.f[k] = fc(acc.f[k] + fc(vf19.f[k] * vf14.f[1]));
            ld(&vf5, vi8++);
            for (int k = 0; k < 4; k++) vf6.f[k] = fc(acc.f[k] + fc(vf20.f[k] * vf14.f[2]));
            for (int k = 0; k < 4; k++) acc.f[k] = fc(vf21.f[k] * 1.0f);
            ld(&vf14, vi6++);
            for (int k = 0; k < 4; k++) vf7.f[k] = fc(vf1.f[k] + S.vf17.f[k]);
            for (int k = 0; k < 4; k++) acc.f[k] = fc(acc.f[k] + fc(vf18.f[k] * vf1.f[0]));
            vi1 = (u16)vf5.u[3];
            for (int k = 0; k < 4; k++) acc.f[k] = fc(acc.f[k] + fc(vf19.f[k] * vf1.f[1]));
            q = vdiv(1.0f, vf6.f[3]);
            { f32 z = vf1.f[2]; for (int k = 0; k < 4; k++) vf1.f[k] = fc(acc.f[k] + fc(vf20.f[k] * z)); }
            vi7 = (u16)vf16.u[3];
            vi4 &= vi10; vi4 += vi13; vi1 &= vi10;
            ld3(&vf4, vi4);
            vi3--;
            vi2 = (u16)vf7.u[3];
            vf15.f[2] = fc(vf15.f[2] * q);
            q = vdiv(1.0f, vf1.f[3]);
            st(vi7 + 1, &vf4); st(vi7 + 2, &vf6); st(vi7, &vf15);
            if (vi3 == 0) break;
            vi1 += vi13;
            ld3(&vf4, vi1);
            vi3--;
            vf5.f[2] = fc(vf5.f[2] * q);
            ld(&vf15, vi8++);
            st(vi2 + 2, &vf1); st(vi2 + 1, &vf4); st(vi2, &vf5);
            if (vi3 == 0) break;
        }
        /* pass 2: the strips */
        vf2.u[3] = (u32)ftoi(vf2.f[3], 16.0f);
        vi2 = (u16)S.vf17.u[3];
        vi9 = S.vi11 + 1;
        vi5 = (u16)Q(S.vi11)[1];
        vi1 = vi10 + 1;
        ld(&vf8, vi9++);
        vi4 = (u16)vf2.u[3]; vi4 |= vi1;
        vf3w = (u32)(s32)(s16)vi4;
        do {
            vi4 = (u16)vf8.u[3]; vi3 = (u16)vf8.u[0];
            vi5--;
            vi7 = 0xFFFD;
            vi6 = vi4 + vi2;
            ld(&vf16, vi6 + 3); ld(&vf15, vi6 + 1);
            vi3 &= vi10;
            clip = 0;
            vi6++;
            do {
                f32 qz = vf15.f[2];
                for (int k = 0; k < 3; k++) vf2.f[k] = fc(vf16.f[k] * qz);
                vf27 = vf26;
                clip = ((clip << 6) | clipf(&vf16)) & 0xFFFFFF;
                vf26 = vf25;
                vf15.f[0] = fc(vf15.f[0] * qz); vf15.f[1] = fc(vf15.f[1] * qz);
                vf25 = vf16;
                vi3--;
                for (int k = 0; k < 3; k++) acc.f[k] = fc(vf2.f[k] * S.c22.f[k]);
                vf14.u[3] = vf2.u[3];
                for (int k = 0; k < 3; k++) vf2.f[k] = fc(acc.f[k] + fc(S.c23.f[k] * 1.0f));
                vi1 = (clip & 0x3FFFF) != 0;
                st(vi6, &vf15);
                vi6 += 3;
                for (int k = 0; k < 3; k++) vf14.u[k] = (u32)ftoi(vf2.f[k], 16.0f);
                vi7++;
                if (vi1) {
                    /* a vertex of the triangle is outside: no drawing kick (ADC) for this one */
                    vf14.u[3] = vf3w;
                    if ((s16)vi7 >= 0 &&
                        (clip | 0xFDF7DF) != 0xFFFFFF && (clip | 0xFF7DF7) != 0xFFFFFF &&
                        (clip | 0xFFBEFB) != 0xFFFFFF && (clip | 0xFFDF7D) != 0xFFFFFF &&
                        (clip | 0xFFEFBE) != 0xFFFFFF) {
                        clip_tri(vi6, &vi2, &vi3, &vi9, &vi5, &vi7, &vf25, &vf26, &vf27, &vf8, &vf2, vf4.u[3]);
                        /* the clip flags of the last three vertices again */
                        clip = ((clip << 18) | (clipf(&vf27) << 12) | (clipf(&vf26) << 6) | clipf(&vf25)) & 0xFFFFFF;
                    }
                }
                ld(&vf15, vi6); ld(&vf16, vi6 + 2);
                st(vi6 - 1, &vf14);
            } while (vi3 != 0);
            ld(&vf8, vi9++);
        } while (vi5 != 0);
        /* the instance's packet is kicked from buffer y (+ vf28.x) and the buffers rotate */
        vi15 = (u16)S.vf17.u[1];
        { u32 x = S.vf17.u[0]; S.vf17.u[0] = S.vf17.u[1]; S.vf17.u[1] = S.vf17.u[2]; S.vf17.u[2] = S.vf17.u[3]; S.vf17.u[3] = x; }
        vi9 = (u16)S.vf28.u[0];
        vi14--;
        vi13 += 28;
        vi15 += vi9;
        S.vf28.f[0] = fc(0.0f + S.vf28.f[1]);
        kick(vi15);
    } while (vi14 != 0);
}

int native_shrubclip_run(u32 pc, u8 *vumem, u32 top, void (*emit)(const u32 *pk, u32 qwc))
{
    (void)top;
    M = vumem; E = emit; kick_pending = 0xFFFFFFFFu;
    switch (pc & 0x3FF8) {
    case 0x0000:
        ld(&S.c31, 0); ld(&S.c30, 1); ld(&S.c29, 2);
        S.vi11 = 120;
        S.vf17.u[0] = 856; S.vf17.u[2] = 856; S.vf17.u[1] = 520; S.vf17.u[3] = 520;
        ld(&S.c22, 3); ld(&S.c23, 4);
        for (int k = 0; k < 4; k++) S.vf17.u[k] = itof(S.vf17.u[k], 1.0f);
        ld(&S.c24, 5);
        for (int k = 0; k < 4; k++) S.vf17.f[k] = fc(S.vf17.f[k] + S.c30.f[1]);
        return 0;
    case 0x0088: run_class(379); return 0;
    case 0x00A8: run_class(238); return 0;
    case 0x0318:
        run_group();
        if (kick_pending != 0xFFFFFFFFu) { u32 k = kick_pending; kick_pending = 0xFFFFFFFFu; kick_send(k); }
        return 0;
    }
    return -1;
}
