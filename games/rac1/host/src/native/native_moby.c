/* Mobys (the game's characters and objects) drawn by native code from the game's moby data
 * instead of by the PS2's VU1 moby microprogram (image 13859). Starts: 0x0000 sets up the two
 * output buffers; 0x0070 draws one packet of a moby; 0x0050 the same with per vertex fog,
 * culling and clipping (for mobys the game finds crossing the screen's edges).
 *
 * The game uploads per packet, double buffered at TOP (0x000 or 0x164), relative to TOP:
 *   0x000+i-1  vertex i: x y z (16-bit, already skinned by the EE)
 *   0x060+i    vertex i: r g b a (one word each), the colour as it goes out
 *   0x0C1+i    vertex i: s t q (4.12)
 *   0x12D      header: x the culled winding (0 or 2; another value: no culling), y where the
 *              texture table is (from 0x12D), z the vertex of the first texture switch
 *   0x12E..    the strip: signed index words, four per qword (the first four in 0x12E)
 *   0x123..0x12C  per moby: A+D GIF tag (x: fog minimum), strip GIF tag (x: projection
 *              factor), guard band, clip scale, unclip scale, screen offset (w: fog offset),
 *              object to screen matrix (rows x y z w)
 * Strip words: i > 0 vertex i; i < 0 vertex i + 128, starting a new strip (ADC); 0 a texture
 * switch: the next vertex comes from the switch list (the header's z, then the w words of the
 * texture table in turn), a new strip starts with it, preceded by the texture's A+D block from
 * the table (0x0070: four qwords, the first one's register a float; 0x0050: as many as the low
 * 16 bits of that float say, a negative count flipping the culled winding); a switch to vertex
 * 0 ends the packet (the three words before it are padding the program's pipeline reads but
 * never draws).
 * Per vertex: the matrix (the program sums its rows in a fixed order per pipeline place),
 * Q = projection factor / w, x y z * Q + offset, s t q * Q; fog: 0x0070 one for the packet
 * (the matrix's w translation + offset), 0x0050 per vertex (w + offset), clamped to the fog
 * minimum and 255. Out as ST RGBAQ XYZF2 into the two output buffers (712, 840), filled in
 * turn: one is kicked when full (the strip goes on in the other with its last two vertices
 * again) and the last at the end. 0x0050 also culls (ADC) triangles of the given winding and
 * sends triangles with a vertex outside the clip box to the clipper (see clip_tri).
 * native_moby_run() does what the program does, word for word, and reads and writes VU memory
 * as the program does (the output buffers included, so words the program leaves alone stay). */
#include "native_moby.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef unsigned short u16;
typedef short s16;
#include "native_dlist.h"

/* the VU registers the program keeps from one run to the next */
static struct {
    u32 vf27[4];     /* the output buffers, rotated at each kick */
    u32 vf12w;       /* left in the ST of a vertex in slot 3 at a texture switch or a buffer change */
    float e29[2], e30[2];   /* the last two screen edges (vf29.xy, vf30.xy), for the culling test */
    u32 clip;        /* the clip flags of the last vertices */
} S = { { 712, 840, 712, 840 }, 0, { 0, 0 }, { 0, 0 }, 0 };

void native_moby_reset(void)
{
    S.vf27[0] = 712; S.vf27[1] = 840; S.vf27[2] = 712; S.vf27[3] = 840;
    S.vf12w = 0; S.e29[0] = S.e29[1] = S.e30[0] = S.e30[1] = 0; S.clip = 0;
}

void native_moby_seed(const u32 vf27[4], u32 vf12w) { memcpy(S.vf27, vf27, sizeof S.vf27); S.vf12w = vf12w; }

static u32 *Q(u8 *mem, u32 a) { return (u32 *)(mem + (a & 0x3FF) * 16); }
static float F(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static u32 U(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static float fc(float x) { u32 b = U(x); if ((b & 0x7F800000u) == 0x7F800000u) x = F((b & 0x80000000u) | 0x7F7FFFFFu); return x; }
/* float to fixed: truncation (the game runs SSE rounding toward zero) */
static s32 ftoi(float f, float scale)
{
    float x = f * scale;
    if (x >= 2147483647.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return (s32)0x80000000u;
    if (x != x) return 0;
    return (s32)x;
}
static float vdiv(float a, float b)
{
    if (b == 0.0f) return ((U(a) ^ U(b)) & 0x80000000u) ? -3.4028235e38f : 3.4028235e38f;
    return a / b;
}
/* the VU's CLIPW flags of one point: +x -x +y -y +z -z */
static u32 clipw(const float c[4])
{
    float a = fabsf(c[3]);
    return (c[0] > a) | ((c[0] < -a) << 1) | ((c[1] > a) << 2) | ((c[1] < -a) << 3) | ((c[2] > a) << 4) | ((c[2] < -a) << 5);
}

typedef struct {
    u8 *mem;
    int clipping;               /* the 0x0050 start: per vertex fog, culling, clipping */
    u32 top, B;                 /* B = TOP - 356 (vi13) */
    float M[4][4];              /* rows: x y z w (vf17..vf20) */
    float proj;                 /* vf23.x */
    float off[4];               /* vf22 */
    float cscale[4];            /* vf21: object to clip space scale */
    float guard[4];             /* vf28: the guard band, as a scale of clip space */
    float unclip[4];            /* clip space back to the matrix's space (0x0050 clipping) */
    float fogmin, fog;          /* vf24.x; the fog of the whole packet (0x0070) */
    u32 vf27[4];
    u32 vf29w;                  /* the open strip tag */
    u32 vi6, vi7, vi9, vi10, vi11;  /* output pointer, room left, strip count, culled winding, texture table */
    u32 ctr;                    /* the switch list (vf10.w) */
    u32 vi12;                   /* the next switch's vertex */
    u32 ctr_out;                /* the switch list pointer as the output stage sees it */
    int end;                    /* the entry of the switch to vertex 0 */
    NativeMobyKicks *k;
} Ctx;

typedef struct {
    s32 idx;                    /* the strip word */
    float p[4];                 /* screen x y z (after the offset); w: fog (0x0050) */
    float c[4];                 /* clip space (0x0050) */
    float st[3], q;
    u32 rgba[4];
    u32 ctr_after;              /* the switch list pointer after this front step */
    u32 cl;                     /* clip flags (0x0050) */
    u32 wb;                     /* 0x0050: the vertex its zero strip word is replaced with (0: none) */
    float P[4];                 /* the transformed position, before the projection (for a mesh) */
    float s0[3];                /* s t q before the multiply by Q (for a mesh) */
} Vtx;

#define MAXE 2048
static Vtx V[MAXE];

static void kick(Ctx *c, u32 qaddr)
{
    NativeMobyKicks *k = c->k;
    if (k->npk >= NATIVE_MOBY_MAXPK) return;
    u32 q = qaddr & 0x3FF, n = 0, cap = (u32)(sizeof k->d / 16) - k->nq;
    u32 *dst = k->d + k->nq * 4;
    while (n < 0x400 && n < cap) {
        const u32 *t = Q(c->mem, q);
        unsigned long long lo = ((unsigned long long)t[1] << 32) | t[0];
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        for (u32 j = 0; j < len && n < 0x400 && n < cap; j++) { memcpy(dst + n * 4, Q(c->mem, q + j), 16); n++; }
        q = (q + len) & 0x3FF;
        if (eop) break;
    }
    k->off[k->npk] = k->nq; k->len[k->npk] = n; k->npk++; k->nq += n;
}
/* the texture switches' A+D blocks of the packet being drawn (0x0070), for a mesh */
static int mm_nsw;
static u32 mm_ad[256][4][4];
static void mm_switch(Ctx *c, const u32 a0[4])
{
    if (mm_nsw >= 256) { mm_nsw = 257; return; }
    memcpy(mm_ad[mm_nsw][0], a0, 16);
    for (int j = 0; j < 3; j++) memcpy(mm_ad[mm_nsw][1 + j], Q(c->mem, c->vi11 + j), 16);
    mm_nsw++;
}
static int mm_draw(Ctx *c);
static unsigned long long mm_n50;   /* (clipping packets: GIF data) */
static void rot(Ctx *c) { u32 a = c->vf27[0]; c->vf27[0] = c->vf27[1]; c->vf27[1] = c->vf27[2]; c->vf27[2] = c->vf27[3]; c->vf27[3] = a; }
static void sq(Ctx *c, u32 a, const u32 *w, int n) { memcpy(Q(c->mem, a), w, 4 * n); }

/* the front of the pipeline: a vertex loaded, transformed and projected. The program sums the
 * matrix rows in three orders: 0: w x y z (the first two vertices), 1: w z y x (0x0070, the
 * others), 2: x y z w (0x0050, the others) */
static void front(Ctx *c, Vtx *v, u32 vtx, int order)
{
    const s32 *pi = (const s32 *)Q(c->mem, c->top + vtx - 1);
    const s32 *ui = (const s32 *)Q(c->mem, c->top + 0xC1 + vtx);
    memcpy(v->rgba, Q(c->mem, c->top + 0x60 + vtx), 16);
    float x = (float)pi[0], y = (float)pi[1], z = (float)pi[2], p[4];
    for (int i = 0; i < 4; i++) {
        float acc;
        if (order == 0) { acc = fc(c->M[3][i]); acc = fc(acc + fc(c->M[0][i] * x)); acc = fc(acc + fc(c->M[1][i] * y)); p[i] = fc(acc + fc(c->M[2][i] * z)); }
        else if (order == 1) { acc = fc(c->M[3][i]); acc = fc(acc + fc(c->M[2][i] * z)); acc = fc(acc + fc(c->M[1][i] * y)); p[i] = fc(acc + fc(c->M[0][i] * x)); }
        else { acc = fc(c->M[0][i] * x); acc = fc(acc + fc(c->M[1][i] * y)); acc = fc(acc + fc(c->M[2][i] * z)); p[i] = fc(acc + fc(c->M[3][i])); }
    }
    float q = vdiv(c->proj, p[3]);
    v->q = q;
    memcpy(v->P, p, 16);
    for (int i = 0; i < 3; i++) v->s0[i] = (float)ui[i] * (1.0f / 4096.0f);
    if (c->clipping) {
        for (int i = 0; i < 4; i++) v->c[i] = fc(p[i] * c->cscale[i]);
        v->cl = clipw(v->c);
        float w = fc(p[3] + c->off[3]);
        w = fmaxf(w, c->fogmin);
        v->p[3] = fminf(w, 255.0f);
    } else v->p[3] = c->fog;
    for (int i = 0; i < 3; i++) v->p[i] = fc(fc(p[i] * q) + c->off[i]);
    for (int i = 0; i < 3; i++) v->st[i] = fc((float)ui[i] * (1.0f / 4096.0f) * q);
}

/* the ST register's fourth word as the program has it at a texture switch in slot s:
 * 3072 (vf9.w), the switch list pointer (vf10.w), 255 (vf11.w), whatever vf12.w holds */
static u32 st_w(Ctx *c, int s) { return s == 0 ? 0x45400000u : s == 1 ? 0x4B000000u + c->ctr_out : s == 2 ? 0x437F0000u : S.vf12w; }

/* a texture switch at the output of the strip word in slot s: the open strip's tag closed,
 * maybe a kick, the texture's A+D block, a new strip tag; then the vertex's ST */
static void tex_switch(Ctx *c, int s, const float st[3])
{
    u8 *mem = c->mem;
    u32 n, a0[4];
    memcpy(a0, Q(mem, c->vi11), 16); c->vi11++;
    if (!c->clipping) {                 /* four registers; the first one's address a float */
        n = 4;
        c->vi7 = (c->vi7 - 6) & 0xFFFF;
    } else {                            /* the count in the low bits of the first one's float address */
        n = a0[2] & 0xFFFF;
        c->vi7 = (c->vi7 - n - 2) & 0xFFFF;
        if ((s16)n < 0) {               /* and the winding of the strips that follow flips */
            n = (n + 256) & 0xFFFF;
            c->vi7 = (c->vi7 - 256) & 0xFFFF;
            c->vi10 = c->vi10 != 0 ? (c->vi10 - 2) & 0xFFFF : 2;
        }
    }
    a0[2] = (u32)ftoi(F(a0[2]), 1.0f);
    if (!c->clipping) mm_switch(c, a0);
    Q(mem, c->vf29w)[0] = c->vi9;
    if ((s16)(s == 3 ? c->vi7 + 12 : c->vi7) < 0) {
        static const u32 room[2][4] = { { 110, 113, 116, 107 }, { 114, 117, 120, 111 } };
        c->vi9 = (c->vi9 + 0x8000) & 0xFFFF;
        Q(mem, c->vf29w)[0] = c->vi9;
        u32 b = c->vf27[0]; c->vi6 = c->vf27[1]; rot(c);
        c->vi7 = (room[c->clipping][s] - (c->clipping ? n : 0)) & 0xFFFF;
        kick(c, b);
    }
    if (!c->clipping) {
        u32 a[3][4];
        for (int j = 0; j < 3; j++) { memcpy(a[j], Q(mem, c->vi11), 16); c->vi11++; }
        sq(c, c->vi6, Q(mem, c->B + 647), 3); c->vi6++;          /* the A+D tag */
        c->vi9 = 0;
        Q(mem, c->vi6 - 1)[0] = 4;
        sq(c, c->vi6, a0, 4); c->vi6++;
        for (int j = 0; j < 3; j++) { sq(c, c->vi6, a[j], 4); c->vi6++; }
    } else {
        sq(c, c->vi6, Q(mem, c->B + 647), 4); c->vi6++;          /* the A+D tag */
        Q(mem, c->vi6 - 1)[0] = n;
        n = (n - 1) & 0xFFFF;
        c->vi9 = 0;
        sq(c, c->vi6, a0, 4); c->vi6++;
        while (n != 0) { sq(c, c->vi6, Q(mem, c->vi11), 4); c->vi11++; c->vi6++; n = (n - 1) & 0xFFFF; }
    }
    c->vf29w = c->vi6;
    sq(c, c->vi6, Q(mem, c->B + 648), 4); c->vi6++;              /* the strip tag */
    u32 w[4] = { U(st[0]), U(st[1]), U(st[2]), st_w(c, s) };
    sq(c, c->vi6, w, 4); c->vi6++;
}

/* the front of the pipeline for the strip words, in order, until the switch to vertex 0;
 * returns that word's place (the vertices of the three words before it are never drawn) */
static int front_all(Ctx *c)
{
    u8 *mem = c->mem;
    u32 P = (c->B + 659) & 0xFFFF;                 /* vi8: 0x12F + TOP */
    const u32 *h0 = Q(mem, P - 2), *h1 = Q(mem, P - 1);
    c->vi10 = h0[0] & 0xFFFF;
    c->vi11 = ((h0[1] & 0xFFFF) + P - 2) & 0xFFFF;  /* the texture table */
    c->vi12 = h0[2] & 0xFFFF;
    c->ctr = c->vi11;
    s32 w[4] = { (s32)h1[0], (s32)h1[1], (s32)h1[2], (s32)h1[3] };
    u32 first;
    if ((w[0] & 0xFFFF) != 0) first = (w[0] + 128) & 0xFFFF;
    else {                                          /* the packet starts with a texture switch */
        first = c->vi12;
        Q(mem, P - 1)[0] = c->vi12;
        c->vi12 = Q(mem, c->ctr)[3] & 0xFFFF;
        c->ctr = (c->ctr + 1) & 0xFFFF;
    }
    int ord = c->clipping ? 2 : 1;
    V[0].wb = V[1].wb = V[2].wb = 0;
    V[0].idx = w[0]; front(c, &V[0], first, 0); V[0].ctr_after = c->ctr;
    V[1].idx = w[1]; front(c, &V[1], (w[1] + 128) & 0xFFFF, 0); V[1].ctr_after = c->ctr;
    V[2].idx = w[2]; front(c, &V[2], w[2] & 0xFFFF, ord); V[2].ctr_after = c->ctr;
    if (!c->clipping && (s16)w[3] < 0)
        /* the program's slip: when the fourth word starts a strip, the second vertex's ST is
         * multiplied by its Q twice (the prologue does it, then the slot 3 start again) */
        for (int i = 0; i < 3; i++) { V[1].s0[i] = V[1].st[i]; V[1].st[i] = fc(V[1].st[i] * V[1].q); }
    for (int n = 3; n < MAXE; n++) {
        u32 wa = n == 3 ? P - 1 : P + (n - 4) / 4;  /* where the word is */
        int wi = n == 3 ? 3 : (n - 4) & 3;
        s32 word = (s32)Q(mem, wa)[wi];
        Vtx *v = &V[n];
        v->idx = word;
        v->wb = 0;
        s16 i = (s16)word;
        if (i > 0) front(c, v, (u16)i, ord);
        else if (i < 0) front(c, v, (u16)(i + 128), ord);
        else {
            u32 vx = c->vi12;
            v->wb = c->clipping ? vx : 0;           /* 0x0050 puts the vertex in the strip (see draw) */
            c->vi12 = Q(mem, c->ctr)[3] & 0xFFFF;
            c->ctr = (c->ctr + 1) & 0xFFFF;
            v->ctr_after = c->ctr;
            if (vx == 0) return n;
            front(c, v, vx, ord);
        }
        v->ctr_after = c->ctr;
    }
    return -1;
}


/* ---- clipping (0x0050) ----
 * A triangle with a vertex outside the clip box goes here (its last vertex gets ADC, so the
 * strip draws nothing). If all three are outside one plane of the guard band (the clip box
 * scaled by vf28) that is all. Otherwise the program cuts the strip and clips the triangle
 * in clip space (x y z against w; a vertex behind the eye counts as outside in x and y too)
 * against the planes its vertices are outside of, in the order -z +z -y +y -x +x, building
 * the polygon in VU memory at 979 (five qwords a vertex: clip position; s t q fog; r g b a as
 * floats; the packet's ST (w: clip flags); the packet's XYZF), draws it as a fan of its own
 * (the strip tag with the fan PRIM) and reopens the strip with the triangle's last two
 * vertices. With culling on, a triangle with a vertex behind the eye is first culled in 3D
 * (x y w). Interpolated colours get +0.01 (and the fog coming into a plane), and an
 * intersection whose parameter falls outside 0..1 is done again with it clamped. */
typedef struct { float v[4]; } F4;
static F4 ld(u8 *mem, u32 a) { F4 r; memcpy(r.v, Q(mem, a), 16); return r; }
static void st4(u8 *mem, u32 a, const F4 *r) { memcpy(Q(mem, a), r->v, 16); }
static void st3(u8 *mem, u32 a, const F4 *r) { memcpy(Q(mem, a), r->v, 12); }
static u32 fl(float x) { return (U(x) >> 31) ? 2 : 0; }          /* the sign flag as the program reads it */
static void push_clip(u32 f) { S.clip = ((S.clip << 6) | f) & 0xFFFFFF; }
static u32 clipw4(const F4 *a) { return clipw(a->v); }

/* the intersection with plane k (0..5: -z +z -y +y -x +x) of the polygon edge from the vertex
 * at p (prev) to the one at cq (cur), written as a new vertex at o. Variant 1 (0x2188): prev
 * outside, cur inside, the point prev + (cur - prev) t; variant 2 (0x2308): prev inside, cur
 * outside, the point cur + (prev - cur) t. Register names are the program's, so is every
 * operation's order (and what a retry with a clamped t sees). */
static void intersect(Ctx *c, int k, int variant, u32 p, u32 cq, u32 o)
{
    u8 *mem = c->mem;
    int j = k < 2 ? 2 : k < 4 ? 1 : 0;           /* the coordinate the plane cuts */
    F4 vf1 = ld(mem, p), vf18 = ld(mem, cq), vf20 = ld(mem, cq + 2);
    F4 vf2 = ld(mem, p + 1), vf3 = ld(mem, p + 2), vf19 = ld(mem, cq + 1), vf14, vf15 = { { 0, 0, 0, 0 } }, vf16, vf25 = { { 0, 0, 0, 0 } };
    float num, den;
    /* a: where the point starts from (vf1 / vf18), b: the other end */
    F4 *a = variant == 1 ? &vf1 : &vf18, *b = variant == 1 ? &vf18 : &vf1;
    for (int i = 0; i < 4; i++) vf14.v[i] = fc(b->v[i] - a->v[i]);
    /* the plane routine (vi3, vi3 + 5): t = num / den */
    if (!(k & 1)) {            /* -plane: where c_j = -w */
        vf25.v[j] = fc(b->v[j] - a->v[j]); vf25.v[3] = fc(b->v[3] - a->v[3]);
        num = fc(fc(0.0f - a->v[3]) - fc(a->v[j]));
    } else {                   /* +plane: where c_j = w */
        vf25.v[j] = fc(a->v[j] - b->v[j]); vf25.v[3] = fc(a->v[3] - b->v[3]);
        num = fc(a->v[3] - a->v[j]);
    }
    for (int i = 0; i < 4; i++) if (i != 2) vf15.v[i] = variant == 1 ? fc(vf19.v[i] - vf2.v[i]) : fc(vf2.v[i] - vf19.v[i]);
    den = !(k & 1) ? fc(vf25.v[3] + vf25.v[j]) : fc(vf25.v[3] - vf25.v[j]);
    for (;;) {
        float t = vdiv(num, den);
        F4 un, acc, pt;
        for (int i = 0; i < 4; i++) { acc.v[i] = fc(a->v[i] * c->unclip[i]); un.v[i] = fc(vf14.v[i] * c->unclip[i]); }
        if (variant == 1) {
            vf2 = ld(mem, p + 1);
            for (int i = 0; i < 4; i++) vf16.v[i] = fc(vf20.v[i] - vf3.v[i]);
            vf3 = ld(mem, p + 2);
            vf2.v[3] = fc(vf2.v[3] + 0.01f);                               /* fog and colour +0.01 */
            for (int i = 0; i < 4; i++) vf3.v[i] = fc(vf3.v[i] + 0.01f);
        } else {
            for (int i = 0; i < 4; i++) vf16.v[i] = fc(vf3.v[i] - vf20.v[i]);
            vf2.v[3] = fc(vf19.v[3] + 0.01f);                              /* (overwritten below) */
            for (int i = 0; i < 4; i++) vf3.v[i] = fc(vf20.v[i] + 0.01f);
        }
        for (int i = 0; i < 4; i++) un.v[i] = fc(acc.v[i] + fc(un.v[i] * t));            /* the point, unclipped */
        for (int i = 0; i < 4; i++) pt.v[i] = fc(fc(a->v[i] + 0.0f) + fc(vf14.v[i] * t));  /* in clip space */
        u32 s1 = fl(t);
        {                                                                    /* s t (q kept) fog */
            const F4 *bs = variant == 1 ? &vf2 : &vf19;
            float x = fc(bs->v[0] + 0.0f), y = fc(bs->v[1] + 0.0f), w = fc(bs->v[3] + 0.0f);
            vf2.v[0] = fc(x + fc(vf15.v[0] * t)); vf2.v[1] = fc(y + fc(vf15.v[1] * t)); vf2.v[3] = fc(w + fc(vf15.v[3] * t));
        }
        for (int i = 0; i < 4; i++) vf16.v[i] = fc(vf16.v[i] * t);
        float q = vdiv(c->proj, un.v[3]);
        u32 s2 = fl(fc(1.0f - t));
        float tc = fmaxf(t, 0.0f);
        st4(mem, o, &pt);
        push_clip(clipw4(&pt));
        if (variant == 1) vf1 = ld(mem, p);                                  /* read again: it may be the point just written */
        else vf1 = pt;
        for (int i = 0; i < 4; i++) vf3.v[i] = fc(vf3.v[i] + vf16.v[i]);
        st4(mem, o + 1, &vf2);
        un.v[3] = fc(vf2.v[3] + 0.0f);
        for (int i = 0; i < 3; i++) un.v[i] = fc(un.v[i] * q);
        u32 f10 = S.clip & 0xFFF;
        tc = fminf(tc, 1.0f);
        F4 stq; for (int i = 0; i < 4; i++) stq.v[i] = fc(vf2.v[i] * q);
        st4(mem, o + 2, &vf3);
        Q(mem, o + 3)[3] = f10;
        for (int i = 0; i < 3; i++) un.v[i] = fc(un.v[i] + c->off[i]);
        u32 xyzf[4]; for (int i = 0; i < 4; i++) xyzf[i] = (u32)ftoi(un.v[i], 16.0f);
        st3(mem, o + 3, &stq);
        if (s1 | s2) { num = tc; den = 1.0f; continue; }   /* t outside 0..1: again with it clamped */
        memcpy(Q(mem, o + 4), xyzf, 16);
        break;
    }
}

/* the strip word of entry n, as the program has it in VU memory (0x0050 puts the vertex of a
 * texture switch in its zero) */
static u32 strip_word(Ctx *c, int n)
{
    u32 P = (c->B + 659) & 0xFFFF;
    return n < 4 ? Q(c->mem, P - 1)[n] : Q(c->mem, P + (n - 4) / 4)[(n - 4) & 3];
}
static u32 flagfix(u32 f) { if (!(f & 3)) f += 3; if (!(f & 12)) f += 12; return f; }   /* behind the eye: outside in x and y */

/* the triangle ending at entry e (slot s) clipped and drawn as a fan; the strip reopened.
 * f1, f14, f13: the clip flag words the program has for its three vertices. Returns 0. */
static int clip_tri(Ctx *c, int e, int s, u32 f1, u32 f14, u32 f13)
{
    static const u32 room[4] = { 111, 114, 117, 108 };
    u8 *mem = c->mem;
    u32 vi6_in = c->vi6;
    u32 *m978 = Q(mem, 978);
    m978[0] = c->vi10; m978[1] = c->vi6; m978[2] = s == 3 ? 12 : 0; m978[3] = room[s];
    u32 sta[4], stb[4], xa[4], xb[4];
    memcpy(sta, Q(mem, c->vi6 - 7), 16); memcpy(stb, Q(mem, c->vi6 - 4), 16);
    memcpy(xa, Q(mem, c->vi6 - 5), 16); memcpy(xb, Q(mem, c->vi6 - 2), 16);
    memcpy(Q(mem, 982), sta, 16);
    u32 iw[3];
    for (int k = 0; k < 3; k++) {
        u32 w = strip_word(c, e - 2 + k) & 0xFFFF;
        if ((s16)w <= 0) w = (w + 128) & 0xFFFF;
        iw[k] = w;
    }
    s32 uv[3][4];
    for (int k = 0; k < 3; k++) memcpy(uv[k], Q(mem, c->top + 0xC1 + iw[k]), 16);
    F4 colc, colb, cola;
    for (int i = 0; i < 4; i++) colc.v[i] = (float)(s16)(Q(mem, 991)[i] & 0xFFFF);
    for (int i = 0; i < 4; i++) colb.v[i] = (float)(s16)(Q(mem, vi6_in - 3)[i] & 0xFFFF);
    st4(mem, 991, &colc);
    for (int i = 0; i < 4; i++) cola.v[i] = (float)(s16)(Q(mem, vi6_in - 6)[i] & 0xFFFF);
    F4 uvf[3];
    for (int k = 0; k < 3; k++) for (int i = 0; i < 4; i++) uvf[k].v[i] = (float)uv[k][i] * (1.0f / 4096.0f);
    u32 wa = xa[3] & 0xFF0, wb = xb[3] & 0xFF0, wc = Q(mem, 993)[3] & 0xFF0;
    memcpy(Q(mem, 987), stb, 16);
    st3(mem, 980, &uvf[0]); st3(mem, 985, &uvf[1]); st3(mem, 990, &uvf[2]);
    Q(mem, 993)[3] = wc;
    st4(mem, 981, &cola);
    xa[3] = wa; memcpy(Q(mem, 983), xa, 16);
    st4(mem, 986, &colb);
    xb[3] = wb; memcpy(Q(mem, 988), xb, 16);
    Q(mem, 980)[3] = U((float)(s32)wa * (1.0f / 16.0f));
    Q(mem, 985)[3] = U((float)(s32)wb * (1.0f / 16.0f));
    Q(mem, 990)[3] = U((float)(s32)wc * (1.0f / 16.0f));
    /* the clip flags of the three: a vertex behind the eye counts as outside in x and y */
    if (f1 & 32) f1 = flagfix(f1);
    Q(mem, 982)[3] = f1 & 0xFFFF;
    u32 all = f1;
    if (f14 & 32) f14 = flagfix(f14);
    Q(mem, 987)[3] = f14 & 0xFFFF;
    all |= f14;
    if (f13 & 32) f13 = flagfix(f13);
    Q(mem, 992)[3] = f13 & 0xFFFF;
    all |= f13;
    u32 planes = (all & 63) + 512;
    int n = 3;
    if (planes & 32) {
        u32 cullv = m978[0];
        if (cullv == 0 || cullv == 2) {      /* culling on: the winding in 3D (x y w) */
            S.vf12w = (u32)ftoi(F(S.vf12w), 16.0f);
            F4 A = ld(mem, 979), Bv = ld(mem, 984), Cv = ld(mem, 989), N;
            A.v[2] = A.v[3]; Bv.v[2] = Bv.v[3]; Cv.v[2] = Cv.v[3];
            for (int i = 0; i < 4; i++) { Bv.v[i] = fc(Bv.v[i] - A.v[i]); Cv.v[i] = fc(Cv.v[i] - A.v[i]); }
            N.v[0] = fc(fc(Bv.v[1] * Cv.v[2]) - fc(Cv.v[1] * Bv.v[2]));
            N.v[1] = fc(fc(Bv.v[2] * Cv.v[0]) - fc(Cv.v[2] * Bv.v[0]));
            N.v[2] = fc(fc(Bv.v[0] * Cv.v[1]) - fc(Cv.v[0] * Bv.v[1]));
            float d0 = fc(A.v[0] * N.v[0]), d1 = fc(A.v[1] * N.v[1]), d2 = fc(A.v[2] * N.v[2]);
            float d = fc(fc(d0 + d1) + d2);
            u32 expect = (S.vf12w & 0xFFFF) == 0 ? cullv : cullv != 0 ? 0 : 2;
            if (fl(d) == expect) { c->vi6 = (c->vi6 - 1) & 0xFFFF; return 0; }   /* facing away */
        }
    }
    float half = 32.5f;
    for (int k = 0; k < 6; k++) {
        u32 mask = (u32)ftoi(half, 1.0f);
        u32 need = planes & 32;
        planes = (planes + planes) & 0xFFFF;
        if (need) {
            if (n == 0) break;
            for (int j = 0; j < 5; j++) memcpy(Q(mem, 979 + 5 * n + j), Q(mem, 979 + j), 16);   /* closed */
            u32 vi4 = 989, vi5 = 979;
            int cnt = n; n = 0;
            for (int t = 0; t < cnt; t++, vi4 += 5) {
                u32 p = vi4 - 10, cq = vi4 - 5;
                u32 fp = (Q(mem, p + 3)[3] & 0xFFFF) & mask, fcur = (Q(mem, cq + 3)[3] & 0xFFFF) & mask;
                if (!fp) {
                    if (!fcur) {            /* both inside: the vertex stays */
                        u32 tmp[5][4];
                        for (int j = 0; j < 5; j++) memcpy(tmp[j], Q(mem, cq + j), 16);
                        for (int j = 0; j < 5; j++) memcpy(Q(mem, vi5 + j), tmp[j], 16);
                        vi5 += 5; n++;
                    } else {                /* going out: the crossing only */
                        vi5 += 5; n++;
                        intersect(c, k, 2, p, cq, vi5 - 5);
                    }
                } else if (!fcur) {         /* coming in: the crossing, then the vertex */
                    u32 tmp[5][4];
                    for (int j = 0; j < 5; j++) memcpy(tmp[j], Q(mem, cq + j), 16);
                    vi5 += 5; n++;
                    intersect(c, k, 1, p, cq, vi5 - 5);
                    if ((vi5 & 0xFFFF) != (vi4 & 0xFFFF)) {
                        for (int j = 0; j < 5; j++) memcpy(Q(mem, vi5 + j), tmp[j], 16);
                        vi5 += 5; n++;
                    }
                }
            }
        }
        half = fc(half * 0.5f);
        if ((s16)planes < 0) break;
    }
    c->vi6 = (c->vi6 - 1) & 0xFFFF;               /* the ST of e is written again after */
    if (n == 0) return 0;
    /* the polygon as a fan, the open strip closed first */
    u32 v15 = (u32)(3 * n);
    c->vi7 = (c->vi7 - 5 - v15) & 0xFFFF;
    Q(mem, c->vf29w)[0] = c->vi9;
    if ((s16)((c->vi7 + m978[2]) & 0xFFFF) < 0) {
        c->vi9 = (c->vi9 + 0x8000) & 0xFFFF;
        Q(mem, c->vf29w)[0] = c->vi9;
        c->vi7 = (m978[3] - v15) & 0xFFFF;
        u32 b = c->vf27[0]; c->vi6 = c->vf27[1]; rot(c);
        kick(c, b);
    }
    const u32 *tag = Q(mem, c->B + 648);
    u32 t4[4]; memcpy(t4, tag, 16);
    sq(c, c->vi6, t4, 4); c->vi6++;
    Q(mem, c->vi6 - 1)[0] = (u32)n;
    Q(mem, c->vi6 - 1)[1] = t4[3];                  /* the fan's PRIM */
    for (int j = 0; j < n; j++) {
        u32 a = 979 + 5 * j, rgba[4], st[4], xyz[4];
        memcpy(st, Q(mem, a + 3), 16); memcpy(xyz, Q(mem, a + 4), 16);
        for (int i = 0; i < 4; i++) rgba[i] = (u32)ftoi(F(Q(mem, a + 2)[i]), 1.0f);
        sq(c, c->vi6, st, 4); c->vi6++;
        sq(c, c->vi6, rgba, 4); c->vi6++;
        sq(c, c->vi6, xyz, 4); c->vi6++;
    }
    /* the strip again, from the triangle's second vertex */
    u32 prev[3][4], vi14 = m978[1];
    for (int j = 0; j < 3; j++) memcpy(prev[j], Q(mem, vi14 - 4 + j), 16);
    c->vf29w = c->vi6;
    sq(c, c->vi6, t4, 4); c->vi6++;
    for (int j = 0; j < 3; j++) { sq(c, c->vi6, prev[j], 4); c->vi6++; }
    c->vi9 = 1;
    return 0;
}

/* runs the clipper ended early: none any more (the program's edge-going-out path is done
 * natively); kept for the host's diagnostics */
u32 native_moby_bails;

static int draw(Ctx *c)
{
    int end = front_all(c);
    if (end < 0) return -1;
    c->end = end;
    u8 *mem = c->mem;
    c->vi7 = 115;
    c->vi9 = 0xFFFF;
    c->vi6 = c->vf27[0];
    c->vf29w = c->vi6;
    sq(c, c->vi6, Q(mem, c->B + 648), 4); c->vi6++;
    for (int e = 0; e + 4 <= end; e++) {
        const Vtx *v = &V[e];
        if (V[e + 3].wb) {   /* the front of the pipeline is three words ahead when this one goes out */
            u32 P = (c->B + 659) & 0xFFFF, k = (u32)e + 3;
            if (k < 4) Q(mem, P - 1)[k] = V[k].wb; else Q(mem, P + (k - 4) / 4)[(k - 4) & 3] = V[k].wb;
        }
        int s = e & 3;
        s16 i = (s16)v->idx;
        float fw = v->p[3];
        int culled = 0, clipping_now = 0;
        u32 cl = 0;
        if (c->clipping) {
            /* the winding of the triangle this vertex ends, from the last two screen edges */
            float *n = (s & 1) ? S.e29 : S.e30, *o = (s & 1) ? S.e30 : S.e29;   /* vf29 is the older at even slots */
            float a = (s & 1) ? fc(n[0] * o[1]) : fc(o[0] * n[1]);
            float b = (s & 1) ? fc(o[0] * n[1]) : fc(n[0] * o[1]);
            float x = a - b;
            u32 sgn = (U(x) >> 31) ? 2 : 0;
            culled = sgn == c->vi10;
            cl = S.clip = ((S.clip << 6) | v->cl) & 0xFFFFFF;
            float *ne = (s & 1) ? S.e30 : S.e29;
            ne[0] = fc(V[e + 1].p[0] - v->p[0]); ne[1] = fc(V[e + 1].p[1] - v->p[1]);
        }
        u32 xyz[4];
        c->vi9 = (c->vi9 + 1) & 0xFFFF;
        if (i <= 0) fw = fc(fw + 3072.0f);              /* ADC: a strip starts here */
        else if (c->clipping) {
            if (culled) fw = fc(fw + 3072.0f);          /* a triangle facing away */
            if ((cl & (culled ? 0x20820u : 0x3FFFFu)) != 0) clipping_now = 1;
        }
        xyz[0] = (u32)ftoi(v->p[0], 16.0f); xyz[1] = (u32)ftoi(v->p[1], 16.0f); xyz[2] = (u32)ftoi(v->p[2], 16.0f); xyz[3] = (u32)ftoi(fw, 16.0f);
        if (i == 0) { c->ctr_out = V[e + 3].ctr_after; tex_switch(c, s, v->st); }
        else { u32 st[3] = { U(v->st[0]), U(v->st[1]), U(v->st[2]) }; sq(c, c->vi6, st, 3); c->vi6++; }
        if (clipping_now) {
            /* a triangle to clip: no strip triangle ends here, whatever happens */
            fw = fc(fw + 3072.0f);
            xyz[3] = (u32)ftoi(fw, 16.0f);
            const Vtx *va = &V[e - 2], *vb = &V[e - 1];
            u32 g[3];
            for (int k = 0; k < 3; k++) {
                const Vtx *vk = k == 0 ? va : k == 1 ? vb : v;
                float gv[4]; for (int j = 0; j < 4; j++) gv[j] = fc(vk->c[j] * c->guard[j]);
                g[k] = clipw(gv);
            }
            push_clip(g[0]); u32 r1 = S.clip & 0xFFF;
            push_clip(g[1]); u32 r14 = S.clip & 0xFFF;
            push_clip(g[2]); u32 r15 = S.clip & 0xFFF;
            push_clip(va->cl); u32 f1 = S.clip & 0xFFF;
            push_clip(vb->cl); u32 f14 = S.clip & 0xFFF;
            push_clip(v->cl); u32 f13 = S.clip & 0xFFF;
            if ((r1 & r14 & r15 & 63) == 0) {             /* not all outside one guard plane: clip */
                st4(mem, 979, (const F4 *)va->c); st4(mem, 984, (const F4 *)vb->c); st4(mem, 989, (const F4 *)v->c);
                memcpy(Q(mem, 991), v->rgba, 16);
                c->ctr_out = V[e + 3].ctr_after;
                u32 w992[4] = { U(v->st[0]), U(v->st[1]), U(v->st[2]), s == 3 ? 0x3F800000u : st_w(c, s) };
                memcpy(Q(mem, 992), w992, 16);
                memcpy(Q(mem, 993), xyz, 16);
                S.vf12w = (s & 1) ? 0x3F800000u : 0;     /* the slot's parity, for the 3D culling */
                clip_tri(c, e, s, f1, f14, f13);
                u32 st[4] = { U(v->st[0]), U(v->st[1]), U(v->st[2]), st_w(c, s) };
                sq(c, c->vi6, st, 4); c->vi6++;
                push_clip(vb->cl); push_clip(v->cl);
            }
        }
        sq(c, c->vi6, v->rgba, 4); c->vi6++;
        if (s == 2) c->vi7 = (c->vi7 - 12) & 0xFFFF;
        if (s == 3 && (s16)c->vi7 < 0) {                 /* the buffer is full: kick it, go on in the other */
            sq(c, c->vi6, xyz, 4);
            Q(mem, c->vf29w)[0] = (c->vi9 + 1 + 0x8000) & 0xFFFF;
            u32 b = c->vf27[0], nb = c->vf27[1]; rot(c);
            c->vi7 = 109;
            kick(c, b);
            c->vf29w = nb;
            u32 a = nb;
            sq(c, a, Q(mem, c->B + 648), 4); a++;
            if (c->vi9 != 0) {                           /* the strip's last two vertices again */
                u32 t[3][4];
                for (int j = 0; j < 3; j++) memcpy(t[j], Q(mem, c->vi6 - 5 + j), 16);
                for (int j = 0; j < 3; j++) { sq(c, a, t[j], 4); a++; }
                c->vi9 = 1;
            }
            u32 st[4] = { U(v->st[0]), U(v->st[1]), U(v->st[2]), S.vf12w };
            sq(c, a, st, 4); a++;
            sq(c, a, v->rgba, 4); a++;
            c->vi6 = a;
        }
        sq(c, c->vi6, xyz, 4); c->vi6++;
    }
    c->vi9 = (c->vi9 + 1) & 0xFFFF;
    Q(mem, c->vf29w)[0] = (c->vi9 + 0x8000) & 0xFFFF;
    u32 b = c->vf27[0]; rot(c);
    kick(c, b);
    return 0;
}

int native_moby_run(u32 pc, u8 *mem, u32 top, NativeMobyKicks *k)
{
    k->npk = 0; k->nq = 0;
    if (pc == 0x0000) {                     /* setup: the two output buffers */
        S.vf27[0] = 712; S.vf27[1] = 840; S.vf27[2] = 712; S.vf27[3] = 840;
        return 0;
    }
    if (pc != 0x0070 && pc != 0x0050) return -1;
    static Ctx C;
    Ctx *c = &C;
    memset(c, 0, sizeof *c);
    c->mem = mem; c->top = top; c->B = (top - 356) & 0xFFFF; c->k = k;
    c->clipping = pc == 0x0050;
    memcpy(c->vf27, S.vf27, sizeof c->vf27);
    for (int r = 0; r < 4; r++) for (int i = 0; i < 4; i++) c->M[r][i] = F(Q(mem, c->B + 653 + r)[i]);
    c->proj = F(Q(mem, c->B + 648)[0]);
    for (int i = 0; i < 4; i++) c->off[i] = F(Q(mem, c->B + 652)[i]);
    for (int i = 0; i < 4; i++) c->cscale[i] = F(Q(mem, c->B + 650)[i]);
    for (int i = 0; i < 4; i++) c->guard[i] = F(Q(mem, c->B + 649)[i]);
    for (int i = 0; i < 4; i++) c->unclip[i] = F(Q(mem, c->B + 651)[i]);
    c->fogmin = F(Q(mem, c->B + 647)[0]);
    c->fog = fminf(fmaxf(fc(c->M[3][3] + c->off[3]), c->fogmin), 255.0f);
    Q(mem, 976)[0] = c->B;
    mm_nsw = 0;
    int r = draw(c);
    if (c->clipping) mm_n50++;
    if (r < 0) return -1;
    if (!c->clipping && mm_draw(c)) k->npk = 0;   /* (drawn as a native mesh: the packets stay in VU memory, unsent) */
    memcpy(S.vf27, c->vf27, sizeof S.vf27);
    return 0;
}

/* ---- mobys as native meshes (0x0070) ----
 * A packet's vertices change every frame (the EE skins them), so its mesh gets them with each
 * draw: one mesh vertex per strip entry the program sends, positions as the program transforms
 * them (its row orders), before the projection; the mesh's transform is the identity and its
 * constants the program's (projection factor, screen offset, the packet's fog), so the GS layer's
 * CPU form of the mesh vertex gives the packet's values bit for bit. The triangles are the GS's:
 * one at each entry without ADC once the strip tag (reset at a texture switch) has three vertices;
 * the texture switches' A+D blocks are sent between the parts. Meshes are kept by the shape of the
 * entry list (count, strip starts, switches), so packets of the same shape share one. The clipping
 * start (0x0050) goes as packets. UYA_NMOBYMESH=0: all as packets. */
#define MM_ID0 24576u
#define MM_N 8192u
typedef struct { u64 h; u32 nv; u8 *kind; u32 *seg; int nseg; } MobyMesh;  /* seg: first, count per part */
static MobyMesh *mmt; static u32 nmm;
static int mm_on = -1;
static unsigned long long mm_n_mesh, mm_n_pk;
#define MH 16384
static u64 mh_k[MH]; static int mh_v[MH];
static void mm_drop(void)
{
    for (u32 i = 0; i < nmm; i++) { native_mesh_free(MM_ID0 + i); free(mmt[i].kind); free(mmt[i].seg); }
    nmm = 0; memset(mh_k, 0, sizeof mh_k);
}
static int mm_fail(int why) { mm_n_pk++; static int n; if (n++ < 4) uya_log("native moby meshes: a packet goes as GIF data (%d)", why); return 0; }
static int mm_draw(Ctx *c)
{
    if (mm_on < 0) { const char *e = getenv("UYA_NMOBYMESH"), *n = getenv("UYA_NATIVE"); mm_on = !(e && !atoi(e)) && !(n && !atoi(n)); }
    if (!mm_on) return 0;
    u8 *mem = c->mem;
    int nv = c->end - 3;
    if (nv < 1 || mm_nsw > 256) return mm_fail(1);
    const u32 *t8 = Q(mem, c->B + 648), *t7 = Q(mem, c->B + 647);
    if (((t8[1] >> 26) & 3) || (t8[1] >> 28) != 3 || (t8[2] & 0xFFF) != 0x412) return mm_fail(2);
    if (((t7[1] >> 26) & 3) || (t7[1] >> 28) != 1 || (t7[2] & 0xF) != 0xE) return mm_fail(3);
    int pre = (t8[1] >> 14) & 1; u64 prim = (t8[1] >> 15) & 0x7FF;
    s32 fn = ftoi(c->fog, 16.0f), fa = ftoi(fc(c->fog + 3072.0f), 16.0f);
    if ((fn & 0x8000) || ((fn >> 4) & 0xFF) != ((fa >> 4) & 0xFF)) return mm_fail(4);
    static float v6[MAXE * 6]; static u8 rgba[MAXE * 4], kind[MAXE];
    int nsw = 0, head = 0;
    u64 h = 1469598103934665603ull ^ (u64)nv;
    for (int e = 0; e < nv; e++) {
        const Vtx *v = &V[e];
        s16 i = (s16)v->idx;
        kind[e] = i == 0 ? 2 : i < 0 ? 1 : 0;
        nsw += i == 0;
        h = (h ^ kind[e]) * 1099511628211ull;
        if (U(v->st[0]) != U(fc(v->s0[0] * v->q)) || U(v->st[1]) != U(fc(v->s0[1] * v->q)) || U(v->st[2]) != U(fc(v->s0[2] * v->q))) return mm_fail(5);
        if (U(v->s0[2]) != 0x3F800000u) {           /* q not 1: the GS's Q is not the mesh's */
            if (e != 1) return mm_fail(8);
            head = 1;                                /* (the program's slip, entry 1: its triangles go as a packet) */
        }
        if (v->P[3] == 0.0f) return mm_fail(6);
        float *o = v6 + e * 6;
        o[0] = v->P[0]; o[1] = v->P[1]; o[2] = v->P[2]; o[3] = v->P[3]; o[4] = v->s0[0]; o[5] = v->s0[1];
        for (int j = 0; j < 4; j++) rgba[e * 4 + j] = (u8)v->rgba[j];
    }
    if (nsw != mm_nsw) return mm_fail(7);
    if (head && (nv < 4 || kind[1] == 2 || kind[2] == 2 || kind[3] == 2)) return mm_fail(9);
    if (!h) h = 1;
    u32 sl = (u32)(h >> 50) & (MH - 1);
    int id = -1;
    while (mh_k[sl]) {
        if (mh_k[sl] == h) { MobyMesh *m = &mmt[mh_v[sl]]; if (m->nv == (u32)nv && !memcmp(m->kind, kind, nv)) { id = mh_v[sl]; break; } }
        sl = (sl + 1) & (MH - 1);
    }
    if (id < 0) {
        if (!mmt) mmt = calloc(MM_N, sizeof *mmt);
        if (nmm == MM_N) { mm_drop(); sl = (u32)(h >> 50) & (MH - 1); }
        static u32 idx[MAXE * 3], seg[2 * 258];
        u32 ni = 0; int ns = 1, qn = 0;
        seg[0] = 0; seg[1] = 0;
        for (int e = 0; e < nv; e++) {
            if (kind[e] == 2) { seg[2 * ns] = ni; seg[2 * ns + 1] = 0; ns++; qn = 0; }
            qn++;
            if (kind[e] == 0 && qn >= 3) { idx[ni++] = (u32)e - 2; idx[ni++] = (u32)e - 1; idx[ni++] = (u32)e; seg[2 * ns - 1] += 3; }
        }
        MobyMesh *m = &mmt[nmm];
        m->h = h; m->nv = (u32)nv; m->kind = malloc(nv); memcpy(m->kind, kind, nv);
        m->seg = malloc(ns * 8); memcpy(m->seg, seg, ns * 8); m->nseg = ns;
        if (!ni) idx[ni++] = 0, idx[ni++] = 0, idx[ni++] = 0;   /* (a mesh needs triangles: never drawn) */
        native_mesh_create(MM_ID0 + nmm, v6, rgba, (u32)nv, idx, ni);
        while (mh_k[sl]) sl = (sl + 1) & (MH - 1);
        mh_k[sl] = h; mh_v[sl] = (int)nmm;
        id = (int)nmm++;
    } else native_mesh_update(MM_ID0 + (u32)id, v6, rgba, (u32)nv);
    const MobyMesh *m = &mmt[id];
    float xf[32] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    float F = (float)((fn >> 4) & 0xFF);
    xf[16] = c->proj; xf[17] = F; xf[18] = F; xf[19] = 0.0f;
    xf[20] = c->off[0]; xf[21] = c->off[1]; xf[22] = c->off[2]; xf[23] = -3.4028235e38f;   /* (no near limit: these packets are inside the view) */
    xf[28] = 0.0f; xf[29] = 0.0f; xf[30] = 1e-30f; xf[31] = 1.0f;   /* (the GPU: no near or far cut but w > 0) */
    extern const char *uya_gs_src; uya_gs_src = "native moby";
    u32 pa = 0; u64 pv = prim;
    for (int k = 0; k < m->nseg; k++) {
        if (k > 0) {
            u32 a[4]; u64 v[4];
            for (int j = 0; j < 4; j++) { const u32 *q = mm_ad[k - 1][j]; a[j] = q[2] & 0xFF; v[j] = ((u64)q[1] << 32) | q[0]; }
            native_regs(a, v, 4);
        }
        if (pre) native_regs(&pa, &pv, 1);
        u32 first = m->seg[2 * k], count = m->seg[2 * k + 1];
        if (head && k == (kind[0] == 2 ? 1 : 0)) {
            /* entries 0..3 as the program sends them (a strip of four), then the mesh's triangles after them */
            static u32 pk[13 * 4];
            u64 tg = 4 | (1ull << 15) | ((u64)pre << 46) | (prim << 47) | (3ull << 60);
            pk[0] = (u32)tg; pk[1] = (u32)(tg >> 32); pk[2] = 0x412; pk[3] = 0;
            for (int e = 0; e < 4; e++) {
                const Vtx *v = &V[e]; u32 *o = pk + 4 + e * 12;
                float fw = v->p[3]; if ((s16)v->idx <= 0) fw = fc(fw + 3072.0f);
                o[0] = U(v->st[0]); o[1] = U(v->st[1]); o[2] = U(v->st[2]); o[3] = 0;
                memcpy(o + 4, v->rgba, 16);
                o[8] = (u32)ftoi(v->p[0], 16.0f); o[9] = (u32)ftoi(v->p[1], 16.0f); o[10] = (u32)ftoi(v->p[2], 16.0f); o[11] = (u32)ftoi(fw, 16.0f);
            }
            native_kick(pk, 13);
            u32 skip = 0, qn = 0;
            for (int e = kind[0] == 2 ? 0 : 0; e < 4; e++) { if (kind[e] == 2) qn = 0; qn++; if (e >= 2 && kind[e] == 0 && qn >= 3) skip += 3; }
            first += skip; count -= skip;
        }
        if (count) native_mesh_draw(MM_ID0 + (u32)id, first, count, xf);
    }
    if (++mm_n_mesh % 100000 == 0) uya_log("native moby meshes: %u shapes, packets %llu as meshes, %llu as GIF data, %llu clipping packets (GIF data)", nmm, mm_n_mesh, mm_n_pk, mm_n50);
    return 1;
}
