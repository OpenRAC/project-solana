/* Terrain fragments that cross the view frustum, drawn by native code from the game's tfrag data
 * instead of by the PS2's VU1 tfrag clip microprogram (image 903379). The game sends a tfrag
 * here, instead of to the main tfrag program (55907), when it is near the edge of the view.
 *
 * One run draws one tfrag packet the VIF unpacked at TOP (0 or 328 qwords). The packet is the
 * main tfrag program's plain format (native_tfrag.c): header at B (= TOP)
 *   B+3.y the strip index list, B+4.z the strip table, B+4.w the texture blocks, B+5..8 object
 *   to clip space (rows x, y, z, translation).
 * The index list, the index entries (s t | vertex in w) and the vertices (position x y z ints
 * | color RGBA ints, copied as they are) are addressed from 0 when TOP is 0 and from 328 for any
 * other TOP (the program has one copy of its loop for each). The s/t bias is the w of the
 * qword after the first texture block's tag (B+4.w + 1): s, t and 1 + bias, less the bias,
 * times Q are STQ.
 * Globals: 656 (x the Q numerator, y/z fog clamp, w the fog add for vertices next to a clipped
 * triangle), 658 the strip GIF tag, 659 the clipped triangle's tag, 660 the texture block tag,
 * 661 the screen offset, 662 the guard band scale (clip space -> CLIPW), 663 its inverse (the
 * clipper's output), 665 the frustum scale (trivial rejection), 670 a GIF packet kicked after
 * each packet (a GS state reset).
 *
 * Starts: 0x00 setup (output buffers 672/823 in vf3, the clipper's plane table at 974);
 *   0x20 (and 0x30) reloads vf4 only; 0x10 draws.
 * Drawing is the plain tfrag strip loop with a clip test per vertex: when one of the strip's
 * last three vertices is outside the guard band (CLIPW against 662, the clip flags of the last
 * three judgements) the vertex gets the fog add (so the GS does not draw that strip triangle)
 * and, from the strip's third vertex on, the triangle is clipped: unless all three vertices are
 * outside the same plane of the frustum (665), the packet so far is kicked with the strip cut
 * before the vertex, the triangle is clipped against the six guard band planes
 * (Sutherland-Hodgman, one vertex at a time through the plane stages at 974 + 7 k, with a
 * stack at 1016) into the other output buffer and kicked as its own packet, and the strip
 * restarts with the last two vertices. The clipper works in VU memory and leaves its records
 * there (as do the saved registers at 1020-1022 and the register save area after the clipped
 * packet); this file does the same, so the memory is as the program leaves it.
 * The program is software pipelined four vertices deep, and this file reads index words,
 * entries and vertices at the same points between its stores as the pipeline does (it reads
 * past the end of the run). Quirks reproduced: the register save area holds what the pipeline
 * had in flight (the next two vertices); the fourth word of ST is the w of vf24..27 by vertex
 * (vf24's is not stored, vf25's is the bias, vf26/vf27's come from outside the program); the
 * clipped vertices' ST w is the w of the registers the records were loaded into; the clipper
 * output's Q is 1 / the guard band w (the DIV reads w before the MUL by 663 in the same pair);
 * with TOP 0 a vertex 3 mod 4 whose triangle was clipped gets the fog add twice (a delay slot);
 * the last strip end's delay slot judges the vertex after the run (clip flags after the run).
 * Checked word for word against the program (packets and all of VU memory) by dev/vu_harness. */
#include "native_tfragclip.h"
#include <string.h>
#include <math.h>
#pragma STDC FP_CONTRACT OFF

/* registers the program keeps between runs: the output buffers (vf3, rotated at each packet),
 * the clip flags, vi15, vf26.w and vf27.w */
static struct { u32 buf[4]; u32 clip, vi15, vf26w, vf27w; } st = { { 672, 823, 672, 823 }, 0, 0, 0, 0 };

void native_tfragclip_reset(void)
{
    st.buf[0] = 672; st.buf[1] = 823; st.buf[2] = 672; st.buf[3] = 823;
    st.clip = 0; st.vi15 = 0; st.vf26w = st.vf27w = 0;
}
void native_tfragclip_seed(const NativeTfragclipRegs *r)
{
    memcpy(st.buf, r->vf3, 16); st.clip = r->clip & 0xFFFFFF; st.vi15 = r->vi15 & 0xFFFF;
    st.vf26w = r->vf26w; st.vf27w = r->vf27w;
}
void native_tfragclip_state(NativeTfragclipRegs *r)
{
    memcpy(r->vf3, st.buf, 16); r->clip = st.clip; r->vi15 = st.vi15; r->vf26w = st.vf26w; r->vf27w = st.vf27w;
}

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
static u32 sgn(float x) { u32 b; memcpy(&b, &x, 4); return b >> 31; }
/* CLIPW: x y z against |w| */
static u32 judge(const float c[4])
{
    float a = fabsf(c[3]);
    return (c[0] > a) | ((c[0] < -a) << 1) | ((c[1] > a) << 2) | ((c[1] < -a) << 3) | ((c[2] > a) << 4) | ((c[2] < -a) << 5);
}

/* ---- VU memory ---- */
static u32 *QW(u8 *mem, u32 a) { return (u32 *)(mem + (a & 0x3FF) * 16); }
static void ldf(u8 *mem, u32 a, float f[4]) { memcpy(f, QW(mem, a), 16); }
static void stf(u8 *mem, u32 a, const float f[4]) { memcpy(QW(mem, a), f, 16); }
static u32 w16(u8 *mem, u32 a, int k) { return QW(mem, a)[k] & 0xFFFF; }
static void cpq(u8 *mem, u32 to, u32 from) { memcpy(QW(mem, to), QW(mem, from), 16); }

/* one vertex as the pipeline has it. Its index word, index entry and vertex are read from
 * memory at the points the program reads them (ld_idx, ld_ent, ld_vtx), the rest follows */
typedef struct {
    u32 v, valid;
    u32 word; u32 e[2];     /* its index word; s t of its entry */
    u32 va;                 /* its vertex, as an address */
    float raw[4];           /* the position (ITOF0) */
    u32 col[4];
    float p[4];             /* clip space */
    float cs[4];            /* guard band space (p * 662) */
    float proj[4];          /* p xyz * Q, p w */
    float stq[3];
    float scr[4];           /* screen, fog clamped (before the fog add) */
    u32 f;                  /* CLIPW of cs */
} Vtx;

typedef struct {
    u8 *mem; u32 bh, bd, idx;
    float R[4][4], g656[4], off[4], gb[4], fr[4], inv[4], bw, one_b;
    u32 *out; u32 cap, used; NativeTfragclipPacket *pk; int npk, maxpk;
    u32 cn, co;             /* the clipper: vertices out (vi4), output pointer (vi14) */
    Vtx vc[8];
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

/* object to clip space: ACC = row3, + row0 x, + row1 y, row2 z + ACC */
static void xform(float R[4][4], const float v[4], float p[4])
{
    for (int k = 0; k < 4; k++) {
        float acc = fc(R[3][k] * 1.0f);
        acc = fc(acc + fc(R[0][k] * v[0]));
        acc = fc(acc + fc(R[1][k] * v[1]));
        p[k] = fc(acc + fc(R[2][k] * v[2]));
    }
}

/* vertex v of the run: word v of the strip index list (the pipeline reads ahead of the strips,
 * past the end at the end of the run) */
static u32 idx_word(Run *r, u32 v) { return w16(r->mem, r->bd + ((r->idx + v / 4) & 0xFFFF), v % 4); }
static void ld_idx(Run *r, u32 v) { Vtx *x = &r->vc[v & 7]; x->v = v; x->valid = 0; x->word = idx_word(r, v); }
static void ld_ent(Run *r, u32 v) { Vtx *x = &r->vc[v & 7]; const u32 *e = QW(r->mem, r->bd + x->word); x->e[0] = e[0]; x->e[1] = e[1]; x->va = e[3] & 0xFFFF; }
static void ld_vtx(Run *r, u32 v)
{
    Vtx *x = &r->vc[v & 7];
    const u32 *pv = QW(r->mem, r->bd + x->va);
    for (int k = 0; k < 4; k++) x->raw[k] = (float)(s32)pv[k];
    memcpy(x->col, QW(r->mem, r->bd + x->va + 1), 16);
}
static Vtx *vtx(Run *r, u32 v)
{
    Vtx *x = &r->vc[v & 7];
    if (x->valid) return x;
    x->valid = 1;
    xform(r->R, x->raw, x->p);
    for (int k = 0; k < 4; k++) x->cs[k] = fc(x->p[k] * r->gb[k]);
    float q = vdiv(r->g656[0], x->p[3]), e0, e1;
    memcpy(&e0, &x->e[0], 4); memcpy(&e1, &x->e[1], 4);
    x->stq[0] = fc(fc(e0 - r->bw) * q); x->stq[1] = fc(fc(e1 - r->bw) * q); x->stq[2] = fc(fc(r->one_b - r->bw) * q);
    for (int k = 0; k < 3; k++) x->proj[k] = fc(x->p[k] * q);
    x->proj[3] = x->p[3];
    for (int k = 0; k < 4; k++) x->scr[k] = fc(x->proj[k] + r->off[k]);
    x->scr[3] = fc(fminf(x->scr[3], r->g656[2]));
    x->scr[3] = fc(fmaxf(x->scr[3], r->g656[1]));
    x->f = judge(x->cs);
    return x;
}

/* ---- the clipper (0x1738..0x1C38) ---- */
/* the plane function at a stage (table z, a code address): w +- x/y/z of the position at a,
 * and the sign flag it leaves */
static float pdist(Run *r, u32 fn, u32 a, u32 *s)
{
    float v[4]; ldf(r->mem, a, v);
    float d;
    switch ((fn << 3) & 0x3FF8) {
    default:
    case 0x1B98: d = v[3] + v[0]; break;
    case 0x1BA8: d = v[3] - v[0]; break;
    case 0x1BB8: d = v[3] + v[1]; break;
    case 0x1BC8: d = v[3] - v[1]; break;
    case 0x1BD8: d = v[3] + v[2]; break;
    case 0x1BE8: d = v[3] - v[2]; break;
    }
    *s = sgn(d);
    return fc(d);
}
/* the point t of the way from record a to record b (position, color, ST), to record d */
static void interp(Run *r, u32 a, u32 b, float t, u32 d)
{
    float A[3][4], B[3][4], o[3][4];
    for (int i = 0; i < 3; i++) { ldf(r->mem, a + i, A[i]); ldf(r->mem, b + i, B[i]); }
    for (int i = 0; i < 3; i++) for (int k = 0; k < 4; k++) o[i][k] = fc(A[i][k] + fc(fc(B[i][k] - A[i][k]) * t));
    for (int i = 0; i < 3; i++) stf(r->mem, d + i, o[i]);
}
/* a vertex through all six planes: to the output buffer (0x18D8) */
static void clip_out(Run *r, u32 a)
{
    u8 *m = r->mem;
    float p[4], c[4], s[4];
    ldf(m, a, p); ldf(m, a + 1, c); ldf(m, a + 2, s);
    r->cn = (r->cn + 1) & 0xFFFF;
    float q = vdiv(1.0f, p[3]);                      /* the DIV reads w before the MUL (same pair) */
    for (int k = 0; k < 4; k++) p[k] = fc(p[k] * r->inv[k]);
    u32 ci[4], xi[4];
    for (int k = 0; k < 4; k++) ci[k] = (u32)ftoi(c[k], 1.0f);
    r->co = (r->co + 3) & 0xFFFF;
    for (int k = 0; k < 3; k++) { s[k] = fc(s[k] * q); p[k] = fc(p[k] * q); }
    memcpy(QW(m, r->co - 2), ci, 16);
    for (int k = 0; k < 4; k++) p[k] = fc(p[k] + r->off[k]);
    p[3] = fc(fmaxf(p[3], r->g656[1]));
    p[3] = fc(fminf(p[3], r->g656[2]));
    for (int k = 0; k < 4; k++) xi[k] = (u32)ftoi(p[k], 16.0f);
    stf(m, r->co - 3, s);
    memcpy(QW(m, r->co - 1), xi, 16);
}
/* one vertex (record at cur) into the plane stages from stage s on (0x1740) */
static void feed(Run *r, u32 s, u32 cur)
{
    u8 *m = r->mem;
    u32 depth = 0;
    for (int guard = 0; guard < 4096; guard++) {
        if (s == 1016) { clip_out(r, cur); goto pop; }
        {
            u32 prev = w16(m, s, 1), fn = w16(m, s, 2), sp, sc;
            QW(m, s)[1] = cur;
            if (prev == 0) {                         /* the first vertex at this stage */
                QW(m, s)[0] = cur;
                pdist(r, fn, cur, &sc);
                if (sc) goto pop;
                s = (s + 7) & 0xFFFF;
                continue;
            }
            float dp = pdist(r, fn, prev, &sp), dc = pdist(r, fn, cur, &sc);
            if (!sp) {
                if (!sc) { s = (s + 7) & 0xFFFF; continue; }
                interp(r, prev, cur, vdiv(dp, fc(dp - dc)), s + 1);   /* in to out: the crossing goes on */
                cur = (s + 1) & 0xFFFF; s = (s + 7) & 0xFFFF;
                continue;
            }
            if (sc) goto pop;
            interp(r, prev, cur, vdiv(dp, fc(dp - dc)), s + 4);       /* out to in: the crossing, then the vertex */
            s = (s + 7) & 0xFFFF;
            QW(m, 1016 + depth)[0] = s; QW(m, 1016 + depth)[1] = cur;
            depth = (depth + 1) & 0xFFFF;
            cur = (s - 3) & 0xFFFF;
            continue;
        }
    pop:
        if ((s16)depth <= 0) return;
        s = w16(m, 1015 + depth, 0); cur = w16(m, 1015 + depth, 1);
        depth = (depth - 1) & 0xFFFF;
    }
}
/* the closing edges of every stage, in order (0x1998) */
static void clip_close(Run *r)
{
    u8 *m = r->mem;
    for (u32 s = 974;;) {
        u32 first = w16(m, s, 0), last = w16(m, s, 1), fn = w16(m, s, 2);
        if (first != 0 && last != first) {
            u32 sl, sf;
            float dl = pdist(r, fn, last, &sl), df = pdist(r, fn, first, &sf);
            if (sl != sf) {
                u32 d = sl ? s + 4 : s + 1;
                interp(r, last, first, vdiv(dl, fc(dl - df)), d);
                feed(r, (s + 7) & 0xFFFF, d & 0xFFFF);
            }
        }
        u32 done = (s - 1009) & 0xFFFF;
        QW(m, s)[0] = 0; QW(m, s)[1] = 0;
        s = (s + 7) & 0xFFFF;
        if ((s16)done >= 0) break;
    }
}

/* ---- the run ---- */
/* where the program's strip loop resumes after a strip ends at a vertex (vi15), and where the
 * clip path returns from the clipper (stored at 1022.x), by vertex mod 4, for TOP 0 / 328 */
static const u16 ret_strip[2][4] = { { 0xC5, 0xD3, 0xE1, 0xB7 }, { 0x1F5, 0x203, 0x211, 0x1E7 } };
static const u16 ret_clip[2][4] = { { 0x13D, 0x163, 0x189, 0x1AF }, { 0x246, 0x26C, 0x292, 0x2B8 } };

typedef struct {
    u32 o, tag, send, pkt, tbl, blocks, vi12, vi7;
    u32 lastxyz[4];
} Loop;

/* the clip path of vertex v (0x08F0 and its three rotations); scr is its screen position
 * (with the fog add) */
static void clip_triangle(Run *r, Loop *L, u32 v, float scr[4])
{
    u8 *m = r->mem;
    int c = v & 3, hi = r->bd != 0;
    QW(m, 1021)[3] = st.vi15;
    if ((s16)((L->vi7 - 2) & 0xFFFF) < 0) return;
    Vtx *a = vtx(r, v - 2), *b = vtx(r, v - 1), *x = vtx(r, v);
    /* trivial rejection: all three outside one plane of the frustum (665) */
    u32 f2[3];
    const Vtx *t3[3] = { a, b, x };
    for (int i = 0; i < 3; i++) { float s[4]; for (int k = 0; k < 4; k++) s[k] = fc(t3[i]->cs[k] * r->fr[k]); f2[i] = judge(s); }
    st.clip = (f2[2] << 18) | (a->f << 12) | (b->f << 6) | x->f;
    if (f2[0] & f2[1] & f2[2] & 0x3F) return;

    Vtx *n1 = vtx(r, v + 1), *n2 = vtx(r, v + 2);
    u32 nlo = w16(m, L->tag, 0);
    QW(m, 1022)[1] = nlo; QW(m, 1021)[2] = L->send;
    QW(m, 1020)[0] = L->o; QW(m, 1020)[1] = L->vi7; QW(m, 1020)[2] = L->blocks; QW(m, 1020)[3] = L->tbl;
    QW(m, 1021)[0] = L->tag; QW(m, 1021)[1] = L->vi12;
    QW(m, L->tag)[0] = (L->vi7 + 0x8000) & 0xFFFF;  /* the strip ends before the vertex */
    kick(r, L->pkt);
    kick(r, 670);

    u32 ob = st.buf[1];                              /* the clipped triangle's packet */
    cpq(m, ob, 659); QW(m, ob)[0] = 0x8000;
    /* the registers the pipeline holds, by register: positions (vf12..), colors (vf20..),
     * guard band positions (vf16..); d = (register - vertex) mod 4 */
    float pos[3][4], cs[3][4]; u32 col[3][4];
    for (int j = 0; j < 3; j++) {
        int d = (j - c) & 3;
        const Vtx *pv = d == 0 ? x : d == 1 ? n1 : d == 2 ? n2 : b;
        const Vtx *cv = d == 0 ? x : d == 1 ? n1 : d == 2 ? a : b;
        if (d == 0) memcpy(pos[j], scr, 16);
        else if (d == 1) memcpy(pos[j], n1->proj, 16);
        else if (d == 2) memcpy(pos[j], n2->raw, 16);
        else memcpy(pos[j], L->lastxyz, 16);
        memcpy(col[j], pv->col, 16);
        memcpy(cs[j], cv->cs, 16);
    }
    /* the three records: guard band position, color (float), ST (s t 1 less the bias; w as was) */
    stf(m, ob + 28, x->cs); memcpy(QW(m, ob + 29), x->col, 16);
    stf(m, ob + 31, b->cs); memcpy(QW(m, ob + 32), b->col, 16);
    stf(m, ob + 34, a->cs);
    for (int j = 0; j < 3; j++) stf(m, ob + 45 + j, cs[j]);
    float stv[3][4];
    u32 ew[3] = { idx_word(r, v), idx_word(r, v - 1), idx_word(r, v - 2) };   /* read again */
    for (int j = 0; j < 3; j++) {
        float e[4]; ldf(m, r->bd + ew[j], e);
        stv[j][0] = fc(e[0] - r->bw); stv[j][1] = fc(e[1] - r->bw); stv[j][2] = 1.0f; stv[j][3] = cs[j][3];
    }
    for (int j = 0; j < 3; j++) memcpy(QW(m, ob + 42 + j), col[j], 16);
    u32 c2[4]; memcpy(c2, QW(m, r->bd + (QW(m, r->bd + ew[2])[3] & 0xFFFF) + 1), 16);   /* the oldest vertex's color, read again */
    u32 cc[3][4];
    memcpy(cc[0], QW(m, ob + 29), 16); memcpy(cc[1], QW(m, ob + 32), 16); memcpy(cc[2], c2, 16);
    stf(m, ob + 39, pos[0]); stf(m, ob + 40, pos[1]);
    stf(m, ob + 30, stv[0]); stf(m, ob + 33, stv[1]); stf(m, ob + 36, stv[2]);
    stf(m, ob + 41, pos[2]);
    for (int j = 0; j < 3; j++) { float f[4]; for (int k = 0; k < 4; k++) f[k] = (float)(s32)cc[j][k]; stf(m, ob + 29 + 3 * j, f); }

    ldf(m, 663, r->inv);
    QW(m, 1022)[0] = ret_clip[hi][c];
    r->cn = 0; r->co = (ob + 1) & 0xFFFF;
    feed(r, 974, ob + 28);
    feed(r, 974, ob + 31);
    feed(r, 974, ob + 34);
    clip_close(r);
    if (r->cn) QW(m, ob)[0] = (r->cn + 0x8000) & 0xFFFF;

    /* the registers back (0x0850), from memory as the clipper left it */
    u32 nl = w16(m, 1022, 1);
    L->send = w16(m, 1021, 2); L->o = w16(m, 1020, 0); L->vi7 = w16(m, 1020, 1);
    L->blocks = w16(m, 1020, 2); L->tbl = w16(m, 1020, 3); L->tag = w16(m, 1021, 0); L->vi12 = w16(m, 1021, 1);
    u32 nt = (L->o - 4) & 0xFFFF;
    nl = (nl - L->vi7 + 1) & 0xFFFF;
    kick(r, ob);
    kick(r, 670);
    cpq(m, nt, 658); QW(m, nt)[0] = nl;               /* the strip again from the vertex before */
    L->tag = nt; L->pkt = nt; L->vi7 = 1;
    st.vi15 = w16(m, 1021, 3);
    /* TOP 0, vertex 3 mod 4: the branch back (0x0DA0) has the fog add in its delay slot too */
    if (!hi && c == 3) scr[3] = fc(scr[3] + r->g656[3]);
}

/* the texture block: its tag and 5 A+D qwords, read and written in the program's order
 * (3 read, the tag and 3 written, 2 read, 2 written); in the prologue the pipeline's first
 * reads of vertices 1 and 2 fall in between */
static void tex_block(Run *r, Loop *L, u32 z, int prologue)
{
    u8 *m = r->mem;
    u32 h = (z + L->blocks) & 0xFFFF, q[5][4], o = L->o;
    for (int k = 0; k < 3; k++) memcpy(q[k], QW(m, h + k), 16);
    cpq(m, o, 660);
    for (int k = 0; k < 3; k++) memcpy(QW(m, o + 1 + k), q[k], 16);
    if (prologue) { ld_idx(r, 2); ld_vtx(r, 1); }
    for (int k = 3; k < 5; k++) memcpy(q[k], QW(m, h + k), 16);
    if (prologue) ld_ent(r, 2);
    for (int k = 3; k < 5; k++) memcpy(QW(m, o + 1 + k), q[k], 16);
    L->o = (o + 6) & 0xFFFF;
}
static void strip_tag(u8 *m, Loop *L, u32 count)
{
    L->tag = L->o;
    L->send = (L->o + 3 * count) & 0xFFFF;
    cpq(m, L->o, 658); QW(m, L->o)[0] = count & 0xFFFF;
    L->o = (L->o + 1) & 0xFFFF;
}

static void draw(Run *r)
{
    u8 *m = r->mem;
    u32 bh = r->bh;
    Loop L;
    L.blocks = (w16(m, bh + 4, 3) + bh) & 0xFFFF;
    L.tbl = (w16(m, bh + 4, 2) + bh) & 0xFFFF;
    r->idx = w16(m, bh + 3, 1);
    for (int i = 0; i < 4; i++) ldf(m, bh + 5 + i, r->R[i == 3 ? 3 : i]);
    ldf(m, 656, r->g656); ldf(m, 661, r->off); ldf(m, 662, r->gb); ldf(m, 665, r->fr);
    float bias; memcpy(&bias, QW(m, L.blocks + 1) + 3, 4);
    r->bw = fc(bias + 0.0f);
    r->one_b = fc(1.0f + bias);
    for (int i = 0; i < 8; i++) r->vc[i].valid = 0;

    /* the prologue: the first vertices into the pipeline around the first texture block */
    L.o = L.pkt = st.buf[0];
    ld_idx(r, 0); ld_ent(r, 0);
    s16 code = (s16)w16(m, L.tbl, 0);
    u32 z0 = w16(m, L.tbl, 2);
    ld_idx(r, 1); ld_vtx(r, 0); ld_ent(r, 1);
    tex_block(r, &L, z0, 1);
    ld_idx(r, 3); ld_vtx(r, 2);
    strip_tag(m, &L, (code + 128) & 0xFFFF);
    ld_ent(r, 3);
    L.tbl = (L.tbl + 1) & 0xFFFF;
    L.vi12 = w16(m, L.tbl, 0);
    L.vi7 = 0xFFFF;
    memset(L.lastxyz, 0, 16);

    for (u32 v = 0; v < 0x100000; v++) {
        L.vi7 = (L.vi7 + 1) & 0xFFFF;
        ld_idx(r, v + 4);                            /* in the delay slot of the clip test's branch */
        Vtx *x = vtx(r, v);
        float scr[4]; memcpy(scr, x->scr, 16);
        st.clip = ((st.clip << 6) | x->f) & 0xFFFFFF;
        if (st.clip & 0x3FFFF) {
            scr[3] = fc(scr[3] + r->g656[3]);
            clip_triangle(r, &L, v, scr);
        }
        ld_vtx(r, v + 3);
        memcpy(QW(m, L.o), x->stq, 12);         /* ST: vf24..27 by vertex; vf24 keeps its w */
        if (v & 3) { u32 *w = QW(m, L.o) + 3; float b = r->bw; if ((v & 3) == 1) memcpy(w, &b, 4); else *w = (v & 3) == 2 ? st.vf26w : st.vf27w; }
        memcpy(QW(m, L.o + 1), x->col, 16);
        ld_ent(r, v + 4);
        int end = ((L.o + 2) & 0xFFFF) == L.send;
        u32 *xyz = QW(m, L.o + 2);
        for (int k = 0; k < 4; k++) xyz[k] = (u32)ftoi(scr[k], 16.0f);
        memcpy(L.lastxyz, xyz, 16);
        L.o = (L.o + 3) & 0xFFFF;
        if (!end) continue;

        /* the strip's end (0x0110) */
        st.vi15 = ret_strip[r->bd != 0][v & 3];
        L.vi7 = 0xFFFF;
        code = (s16)L.vi12;
        L.tbl = (L.tbl + 1) & 0xFFFF;
        if (code > 0) { strip_tag(m, &L, (u32)code); L.vi12 = w16(m, L.tbl, 0); continue; }
        s16 y = (s16)w16(m, L.tbl - 1, 1);
        u32 z = w16(m, L.tbl - 1, 2), nl = w16(m, L.tag, 0);
        if (code == 0 || y < 0) {                    /* end of packet: EOP on the last tag, kick, swap */
            QW(m, L.tag)[0] = (nl + 0x8000) & 0xFFFF;
            u32 nb = st.buf[1];
            rotate_buffers();
            kick(r, L.pkt);
            kick(r, 670);
            if (code == 0) {                         /* the strip end branch's delay slot judged the next vertex */
                st.clip = ((st.clip << 6) | vtx(r, v + 1)->f) & 0xFFFFFF;
                return;
            }
            L.o = L.pkt = nb;
            if ((s16)z >= 0) tex_block(r, &L, z, 0);
        } else tex_block(r, &L, z, 0);
        strip_tag(m, &L, (code + 128) & 0xFFFF);
        L.vi12 = w16(m, L.tbl, 0);
    }
}

int native_tfragclip_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeTfragclipPacket *pk, int maxpk)
{
    Run r;
    r.mem = mem; r.bh = top & 0xFFFF; r.bd = r.bh ? 328 : 0;
    r.out = out; r.cap = out_cap; r.used = 0; r.pk = pk; r.npk = 0; r.maxpk = maxpk;
    switch (pc) {
    case 0x00:                                        /* setup: the buffers, the plane table */
        st.buf[0] = 672; st.buf[1] = 823; st.buf[2] = 672; st.buf[3] = 823;
        for (int k = 0; k < 6; k++) {
            u32 *q = QW(mem, 974 + 7 * k);
            q[0] = q[1] = q[2] = 0; q[3] = 0x3F800000u;
            q[2] = 0x373 + 2 * k;
        }
        break;
    case 0x20: case 0x30: break;
    case 0x10: draw(&r); break;
    default: return -1;
    }
    return r.npk;
}
