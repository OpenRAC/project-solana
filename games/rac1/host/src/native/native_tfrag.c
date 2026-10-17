/* Terrain (tfrags: the level's terrain chunks), drawn by native code from the game's tfrag data
 * instead of by the PS2's VU1 tfrag microprogram (image 55907).
 *
 * One run draws one tfrag packet the VIF unpacked at TOP (B, 0 or 328 qwords). All addresses
 * in the packet are relative to B. Its header:
 *   B+0  x vertices to transform, z level-1 morphs, w level-1 collapses
 *   B+1  x level-2 morphs, y level-2 collapses, z the vertex array
 *   B+2  y level-1 morph list, z level-1 collapse list, w level-2 morph list
 *   B+3  x level-2 collapse list, y the strip index list, z level-1 morph parents, w level-1 collapse parents
 *   B+4  x level-2 morph parents, y level-2 collapse parents, z the strip table, w the texture blocks
 *   B+5..8  object to clip space (rows x, y, z, translation)
 * A vertex is two qwords: position x y z (integers) | color (integers, already RGBA in the
 * plain starts; made floats by the transform in the LOD starts). An index entry (one qword) is
 * s t (biased floats: the bias is 657.z) | its vertex in w; the strip index list holds, one per
 * word, the index entries of the strips' vertices in order. The strip table has one qword per
 * strip: x the vertex count (count - 128: a new texture, whose block (5 A+D qwords) is z, at
 * B+4.w; 0 ends the list), y < 0: send the packet built so far first.
 * Globals: 656 (x the Q numerator, y/z fog clamp, w the ADC fog add), 657 (x/y morph factor
 * limits, z the s/t bias), 658 the strip GIF tag, 660 the texture block tag, 661 the screen
 * offset, 662 the clip scale, 664 the color scale, 666-669 the LOD morph parameters (level 1:
 * 666/668, level 2: 667/669; w: the collapse distance).
 *
 * Starts: 0x00 setup (output buffers 672/848, color scale); 0x10/0xB0/0xC0 color scale only;
 *   0x30/0x60/0xA0 plain, clipped (transform per vertex, ADC on triangles near the guard band)
 *   0x20 plain, not clipped
 *   0x40 / 0x50 / 0x70 LOD (transform the vertex array in place, morph level 1 (0x40 with
 *        collapses), 0x70 also level 2 with collapses), not clipped
 *   0x80 / 0x90 LOD (transform, level-2 morph), not clipped / clipped
 * The program builds its GS packets in two 176-qword buffers (672, 848) it alternates between
 * at each kick; this file builds them the same way, in VU memory, and copies each one out when
 * the program would kick it. Checked word for word against the program by dev/vu_harness on
 * every run of whole frames (all but the unused fourth word of ST, register garbage there). */
#include "native_tfrag.h"
#include <string.h>
#include <math.h>
#pragma STDC FP_CONTRACT OFF

/* registers the program keeps between runs: the two output buffers (vf3, ints) and the color
 * scale vf4 (664 as of the end of the last run) */
static struct { u32 buf[4]; float cscale[4]; } st = { { 672, 848, 672, 848 }, { 1, 1, 1, 1 } };

void native_tfrag_reset(void)
{
    st.buf[0] = 672; st.buf[1] = 848; st.buf[2] = 672; st.buf[3] = 848;
    for (int k = 0; k < 4; k++) st.cscale[k] = 1.0f;
}

void native_tfrag_state(u32 buf[4], float cscale[4]) { memcpy(buf, st.buf, 16); memcpy(cscale, st.cscale, 16); }

/* ---- VU arithmetic, as the translated program does it ---- */
static float fc(float x) { u32 b; memcpy(&b, &x, 4); if ((b & 0x7F800000u) == 0x7F800000u) { b = (b & 0x80000000u) | 0x7F7FFFFFu; memcpy(&x, &b, 4); } return x; }
static float vdiv(float a, float b)
{
    if (b == 0.0f) { u32 ua, ub; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4); return ((ua ^ ub) & 0x80000000u) ? -3.4028235e38f : 3.4028235e38f; }
    return a / b;
}
static s32 ftoi(float f, float scale)
{
    float x = f * scale;
    if (x >= 2147483647.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return (s32)0x80000000u;
    if (x != x) return 0;
    return (s32)x;
}
static int neg(float x) { u32 b; memcpy(&b, &x, 4); return b >> 31; }

/* ---- VU memory ---- */
static u32 *QW(u8 *mem, u32 a) { return (u32 *)(mem + (a & 0x3FF) * 16); }
static void ldf(u8 *mem, u32 a, float f[4]) { memcpy(f, QW(mem, a), 16); }
static void ldi2f(u8 *mem, u32 a, float f[4]) { const u32 *q = QW(mem, a); for (int k = 0; k < 4; k++) f[k] = (float)(s32)q[k]; }
static void stf(u8 *mem, u32 a, const float f[4]) { memcpy(QW(mem, a), f, 16); }
static u32 w16(u8 *mem, u32 a, int k) { return QW(mem, a)[k] & 0xFFFF; }

/* object to clip space: ACC = row3, + row0 x, + row1 y, row2 z + ACC */
static void xform(float R[4][4], const float v[4], float p[4])
{
    for (int k = 0; k < 4; k++) {
        float acc = fc(R[3][k] * 1.0f);
        acc = fc(acc + R[0][k] * v[0]);
        acc = fc(acc + R[1][k] * v[1]);
        p[k] = fc(acc + R[2][k] * v[2]);
    }
}
static void matrix(u8 *mem, u32 B, float R[4][4]) { for (int r = 0; r < 4; r++) ldf(mem, B + 5 + r, R[r]); }

/* ---- the run's context ---- */
typedef struct {
    u8 *mem; u32 B;
    u32 *out; u32 cap, used; NativeTfragPacket *pk; int npk, maxpk;
} Run;

/* XGKICK: the GIF packet at q, as the GIF reads it */
static void kick(Run *r, u32 q)
{
    u32 n = 0, a = q & 0x3FF, start = r->used;
    for (;;) {
        const u32 *t = QW(r->mem, a);
        u64 lo = ((u64)t[1] << 32) | t[0];
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        for (u32 k = 0; k < len && n < 0x400; k++, n++)
            if (r->used < r->cap) memcpy(r->out + 4 * r->used++, QW(r->mem, a + k), 16);
        a = (a + len) & 0x3FF;
        if (eop || n >= 0x400) break;
    }
    if (r->npk < r->maxpk) { r->pk[r->npk].off = start; r->pk[r->npk].n = r->used - start; r->npk++; }
}
static void rotate_buffers(void) { u32 x = st.buf[0]; st.buf[0] = st.buf[1]; st.buf[1] = st.buf[2]; st.buf[2] = st.buf[3]; st.buf[3] = x; }

/* ---- the transform of the vertex array (0x278, 0x290) ---- */
static void transform_array(Run *r, u32 count)
{
    u8 *m = r->mem; u32 B = r->B, a = B + w16(m, B + 1, 2);
    float R[4][4]; matrix(m, B, R);
    for (u32 i = 0; i < count; i++, a += 2) {
        float v[4], p[4], c[4];
        ldi2f(m, a, v); ldi2f(m, a + 1, c);
        xform(R, v, p);
        stf(m, a, p); stf(m, a + 1, c);
    }
}

/* ---- LOD morph (0x498/0x450 and, with collapses, 0xB00/0xAB0) ----
 * entry j of the list (an index entry: z parent B, w the vertex) and word j of the parents list
 * (an index entry whose w is parent A): the vertex, transformed, moves toward the middle of A
 * and B: f = clamp(c + d * w, 0, lim) (x for the midpoint, y for the vertex), A+B f.x + v f.y.
 * With collapses, when both parents are nearer than the collapse distance, the entry becomes
 * parent A's index entry instead (the vertex is left alone). */
static void morph(Run *r, int level2, int collapses)
{
    u8 *m = r->mem; u32 B = r->B;
    u32 count = w16(m, B + (level2 ? 1 : 0), level2 ? 0 : 2);
    if (!count) return;
    u32 list = B + w16(m, B + 2, level2 ? 3 : 1);
    u32 par = B + w16(m, B + (level2 ? 4 : 3), level2 ? 0 : 2);
    float c18[4], c19[4], lim[4], R[4][4];
    ldf(m, level2 ? 667 : 666, c18); ldf(m, level2 ? 669 : 668, c19); ldf(m, 657, lim);
    float tw = c18[3];
    matrix(m, B, R);
    for (u32 j = 0; j < count; j++) {
        u32 own = B + w16(m, list + j, 3), pb = B + w16(m, list + j, 2);
        u32 pidx = B + w16(m, par + j / 4, j % 4), pa = B + w16(m, pidx, 3);
        float v[4], p[4], s[4], A[4], A2[4], Bv[4], B2[4];
        ldi2f(m, own, v); ldi2f(m, own + 1, s);
        ldf(m, pa, A); ldf(m, pa + 1, A2); ldf(m, pb, Bv); ldf(m, pb + 1, B2);
        xform(R, v, p);
        float f[2];
        for (int k = 0; k < 2; k++) {
            float x = fc(fc(c19[k] * 1.0f) + c18[k] * p[3]);
            x = fminf(x, lim[k]);
            f[k] = fmaxf(x, 0.0f);
        }
        if (collapses && neg(A[3] - tw) && neg(Bv[3] - tw)) { memcpy(QW(m, list + j), QW(m, pidx), 16); continue; }
        float o[4], o2[4];
        for (int k = 0; k < 4; k++) {
            o[k] = fc(fc(fc(A[k] + Bv[k]) * f[0]) + p[k] * f[1]);
            o2[k] = fc(fc(fc(A2[k] + B2[k]) * f[0]) + s[k] * f[1]);
        }
        stf(m, own, o); stf(m, own + 1, o2);
    }
}

/* ---- LOD collapse (0x13E8/0x13C0): entry j (an index entry: z its vertex) becomes its parent's
 * index entry (word j of the parents list) when both vertices are nearer than the distance */
static void collapse(Run *r, int level2)
{
    u8 *m = r->mem; u32 B = r->B;
    u32 count = level2 ? w16(m, B + 1, 1) : w16(m, B, 3);
    if (!count) return;
    u32 list = B + w16(m, B + (level2 ? 3 : 2), level2 ? 0 : 2);
    u32 par = B + w16(m, B + (level2 ? 4 : 3), level2 ? 1 : 3);
    float t[4]; ldf(m, level2 ? 667 : 666, t);
    for (u32 j = 0; j < count; j++) {
        u32 pidx = B + w16(m, par + j / 4, j % 4);
        float own[4], pv[4];
        ldf(m, B + w16(m, list + j, 2), own);
        ldf(m, B + w16(m, pidx, 3), pv);
        if (neg(own[3] - t[3]) && neg(pv[3] - t[3])) memcpy(QW(m, list + j), QW(m, pidx), 16);
    }
}

/* ---- the strips: GS packets in the output buffers ---- */
enum { V_PLAIN_CLIP, V_PLAIN, V_LOD, V_LOD_CLIP };
typedef struct {
    int kind;
    float R[4][4], g656[4], g657[4], off[4], cs[4], one_b, bias;
    u32 hist; int lose;
} Verts;

static void put_st(u8 *m, u32 a, const float s[3]) { memcpy(QW(m, a), s, 12); }
static void put_xyzf(u8 *m, u32 a, const float x[4])
{
    u32 *o = QW(m, a);
    for (int k = 0; k < 4; k++) o[k] = (u32)ftoi(x[k], 16.0f);
}
/* clip judgement of a vertex (CLIPW), and whether one of the strip's last three was outside */
static int clip_adc(Verts *V, const float c[4])
{
    float a = fabsf(c[3]);
    u32 f = (c[0] > a) | ((c[0] < -a) << 1) | ((c[1] > a) << 2) | ((c[1] < -a) << 3) | ((c[2] > a) << 4) | ((c[2] < -a) << 5);
    if (V->lose) V->lose = 0;                       /* judged before the strip's flags were cleared */
    else V->hist = ((V->hist << 6) | f) & 0xFFFFFF;
    return (V->hist & 0x3FFFF) != 0;
}

static void vertex(Run *r, Verts *V, u32 o, u32 entry)
{
    u8 *m = r->mem; u32 B = r->B;
    const u32 *e = QW(m, entry);
    float E0, E1; memcpy(&E0, e + 0, 4); memcpy(&E1, e + 1, 4);
    u32 va = B + (e[3] & 0xFFFF);
    float p[4], s[3], x[4];
    int clipped = V->kind == V_PLAIN_CLIP || V->kind == V_LOD_CLIP;
    if (V->kind == V_PLAIN_CLIP || V->kind == V_PLAIN) { float v[4]; ldi2f(m, va, v); xform(V->R, v, p); }
    else ldf(m, va, p);
    float q = vdiv(V->g656[0], p[3]);
    s[0] = fc(fc(E0 - V->bias) * q); s[1] = fc(fc(E1 - V->bias) * q);
    s[2] = V->kind == V_PLAIN ? fc(1.0f * q) : fc(fc(V->one_b - V->bias) * q);
    for (int k = 0; k < 3; k++) x[k] = fc(fc(p[k] * q) + V->off[k]);
    x[3] = fc(p[3] + V->off[3]);
    if (clipped) {
        float c[4]; for (int k = 0; k < 4; k++) c[k] = fc(p[k] * V->cs[k]);
        int adc = clip_adc(V, c);
        x[3] = fminf(x[3], V->g656[2]); x[3] = fmaxf(x[3], V->g656[1]);
        if (adc) x[3] = fc(x[3] + V->g656[3]);
    } else {
        x[3] = fmaxf(x[3], V->g656[1]); x[3] = fminf(x[3], V->g656[2]);
    }
    put_st(m, o, s);
    if (V->kind == V_LOD || V->kind == V_LOD_CLIP) {
        float c[4]; ldf(m, va + 1, c);
        u32 *rgba = QW(m, o + 1);
        for (int k = 0; k < 4; k++) rgba[k] = (u32)ftoi(fc(c[k] * st.cscale[k]), 1.0f);
    } else memcpy(QW(m, o + 1), QW(m, va + 1), 16);
    put_xyzf(m, o + 2, x);
}

static void strips(Run *r, Verts *V)
{
    u8 *m = r->mem; u32 B = r->B;
    u32 tbl = B + w16(m, B + 4, 2), blocks = B + w16(m, B + 4, 3), idx = B + w16(m, B + 3, 1), iw = 0;
    u32 o = st.buf[0], pkt = o, tag = 0;
    s16 code = (s16)w16(m, tbl, 0);
    u32 count = (u16)(code + 128), hdr = blocks + w16(m, tbl, 2);
    V->hist = 0; V->lose = 0;
    for (;;) {
        if (hdr != 0xFFFFFFFFu) {                   /* the texture block: its tag, 5 A+D qwords */
            memcpy(QW(m, o++), QW(m, 660), 16);
            for (int k = 0; k < 5; k++) memcpy(QW(m, o++), QW(m, hdr + k), 16);
        }
        tag = o;                                     /* the strip's tag, NLOOP = its vertex count */
        memcpy(QW(m, o++), QW(m, 658), 16);
        QW(m, tag)[0] = count;
        for (u32 n = 0; n < count; n++, o += 3) {
            vertex(r, V, o, B + w16(m, idx + iw / 4, iw % 4));
            iw++;
        }
        V->hist = 0; V->lose = 1;                    /* FCSET 0 at the strip's end */
        code = (s16)w16(m, ++tbl, 0);
        hdr = 0xFFFFFFFFu;
        if (code > 0) { count = (u16)code; continue; }
        s16 y = (s16)w16(m, tbl, 1), z = (s16)w16(m, tbl, 2);
        if (code == 0 || y < 0) {                    /* end of packet: EOP on the last tag, kick, swap */
            QW(m, tag)[0] = (u16)(w16(m, tag, 0) + 0x8000);
            kick(r, pkt);
            rotate_buffers();
            if (code == 0) return;
            o = pkt = st.buf[0];
            if (z >= 0) hdr = blocks + (u16)z;
        } else hdr = blocks + (u16)z;
        count = (u16)(code + 128);
    }
}

static void draw_plain(Run *r, int clipped)
{
    u8 *m = r->mem; Verts V;
    V.kind = clipped ? V_PLAIN_CLIP : V_PLAIN;
    matrix(m, r->B, V.R);
    ldf(m, 656, V.g656); ldf(m, 657, V.g657); ldf(m, 661, V.off); ldf(m, 662, V.cs);
    V.bias = V.g657[2];
    V.one_b = fc(fc(0.0f + 1.0f) + V.bias);
    strips(r, &V);
}
static void draw_lod(Run *r, int clipped)
{
    u8 *m = r->mem; Verts V;
    V.kind = clipped ? V_LOD_CLIP : V_LOD;
    ldf(m, 656, V.g656); ldf(m, 657, V.g657); ldf(m, 661, V.off); ldf(m, 662, V.cs);
    V.bias = V.g657[2];
    if (clipped) {                                   /* the s/t bias: w of the texture blocks area + 1 (B+4.w + 1) */
        float h[4]; ldf(m, r->B + w16(m, r->B + 4, 3) + 1, h);
        V.bias = fc(0.0f + h[3]);
    }
    V.one_b = fc(fc(0.0f + 1.0f) + V.bias);
    strips(r, &V);
}

/* the LOD starts' work before the strips (native_terrain.c draws the strips itself) */
int native_tfrag_lod_vertices(u32 pc, u8 *mem, u32 top)
{
    Run r = { mem, top & 0x3FF, 0, 0, 0, 0, 0, 0 };
    switch (pc) {
    case 0x40: transform_array(&r, w16(mem, r.B, 0)); morph(&r, 0, 1); collapse(&r, 0); return 0;
    case 0x50: transform_array(&r, w16(mem, r.B, 0)); morph(&r, 0, 0); return 0;
    case 0x70: transform_array(&r, w16(mem, r.B, 0)); morph(&r, 0, 0); morph(&r, 1, 1); collapse(&r, 1); return 0;
    case 0x80: transform_array(&r, (w16(mem, r.B, 0) + w16(mem, r.B, 2)) & 0xFFFF); morph(&r, 1, 0); return 0;
    }
    return -1;
}
void native_tfrag_cscale(float c[4]) { memcpy(c, st.cscale, 16); }
void native_tfrag_set_cscale(const float c[4]) { memcpy(st.cscale, c, 16); }

int native_tfrag_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeTfragPacket *pk, int maxpk)
{
    Run r = { mem, top & 0x3FF, out, out_cap, 0, pk, 0, maxpk };
    switch (pc) {
    case 0x00: native_tfrag_reset(); break;
    case 0x10: case 0xB0: case 0xC0: break;
    case 0x20: draw_plain(&r, 0); break;
    case 0x30: case 0x60: case 0xA0: draw_plain(&r, 1); break;
    case 0x40: transform_array(&r, w16(mem, r.B, 0)); morph(&r, 0, 1); collapse(&r, 0); draw_lod(&r, 0); break;
    case 0x50: transform_array(&r, w16(mem, r.B, 0)); morph(&r, 0, 0); draw_lod(&r, 0); break;
    case 0x70: transform_array(&r, w16(mem, r.B, 0)); morph(&r, 0, 0); morph(&r, 1, 1); collapse(&r, 1); draw_lod(&r, 0); break;
    case 0x80: transform_array(&r, (w16(mem, r.B, 0) + w16(mem, r.B, 2)) & 0xFFFF); morph(&r, 1, 0); draw_lod(&r, 0); break;
    case 0x90: transform_array(&r, (w16(mem, r.B, 0) + w16(mem, r.B, 2)) & 0xFFFF); morph(&r, 1, 0); draw_lod(&r, 1); break;
    default: return -1;
    }
    float c[4]; ldf(mem, 664, c);                    /* every start ends reloading the color scale */
    memcpy(st.cscale, c, 16);
    return r.npk;
}
