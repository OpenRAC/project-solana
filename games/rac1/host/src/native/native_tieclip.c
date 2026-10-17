/* Ties crossing the screen's edges, drawn by native code from the game's tie data instead of
 * by the PS2's VU1 tie clip microprogram (image 224979). The game's TieProc sends the tie
 * instances that are not wholly inside the view to this program (the others go to the main
 * tie program, image 13507, host/src/native/native_tie.c); it uploads it each frame after the
 * main pass.
 *
 * Starts:
 *   0x0040  setup: 971 the output buffers (vf21, floats 2^23 + address), 970 their sum (vf23.xz,
 *           3 * 2^23 + the three buffers), 972 the instance buffers (vf22: 198, 242, 198, 242);
 *           the clipper's six plane stages (976 + 7k) are cleared, with their plane in z.
 *   0x0020  nothing (the game's MSCAL before each class)
 *   0x0030  a class packet (as for image 13507): 0 the A+D block destinations, 1.. the A+D
 *           blocks (5 qwords each), then the unpack header (3 qwords, bytes hb[0..11], one per
 *           word) and the strips (count, -, offset, -); 30.. the strip walk (below); 50.. the
 *           dinky vertices (2 qwords: x y z place | s t q place2) and after them the fat ones
 *           (3 qwords: delta xyz place | base xyz | s t q place2). The setup writes the A+D
 *           blocks (GIF tag 966, w cleared) and the strip GIF tags (967, count in x, the last
 *           with EOP) into both output buffers, converts the vertices to floats in place
 *           (positions and places itof0, s t q itof12) and kicks 969 (an empty packet).
 *   0x0000  one instance at vf22.x (rotated each instance: 198 / 242): rows 0..3 object to
 *           clip space, row 4 (x the fat morph factor, z / w the fat color weights), row 5.w
 *           fog, 6.. the color indexes into the table at 838 (dinky: one per word; fat: one
 *           qword each, three indexes).
 *   0x0010  an instance skipped (registers only; not used by the game).
 *
 * An instance:
 * 1. The vertices are transformed (ACC = row3, + row0 x, + row1 y, row2 z + ACC), and each is
 *    written, unprojected, into the output buffer (vf21.x, the two buffers alternate) at its
 *    place (+ base): s t q * 1/w | color | x y z w. The header's words say which vertices also
 *    go to a second place (place2 + base, in the s t q's w): dinky: the vertices up to the one
 *    at place hb[4], and three more, go to one place; if hb[0] is 0 the next ones up to the one
 *    at hb[5], and two more, to two. Fat (if hb[1] is 0): up to the one at hb[6] to one place,
 *    then up to hb[7] to two. A fat vertex is delta * morph + base; its color c0 * row4.z +
 *    (c1 / 2 + c2 / 2) * row4.w (colors are floats 2^23 + c).
 * 2. The strip walk: from 30, signed offsets, four per qword, from one vertex to the next in
 *    the output buffer (the first from the buffer). An offset <= 0 starts a new strip at the
 *    vertex - offset (-10: after a texture switch, an A+D block; -10 then 0 ends the walk). Each
 *    vertex's screen position (x y z * scale (974) * the vertex's q, + offset (973, w the fog))
 *    replaces its x y z w (ftoi4). A triangle (the vertex and the two before it) with a vertex
 *    outside the guard band (CLIPW against w) gets its last vertex's ADC bit (fog + 3072); unless
 *    the vertex starts a strip, it is then tested against the screen (the vertices scaled by
 *    975): all three outside one plane, it is dropped; else it goes to the clipper.
 * 3. The clipper (a Sutherland-Hodgman pipeline, one vertex at a time through six plane stages
 *    in VU memory, 976 + 7k: x the first vertex, y the last one, z the plane test, then two
 *    crossing vertices; pending vertices on a stack at 1018) clips the triangle (clip space
 *    position, color, s t q * w) to the guard band and writes a triangle fan into the third
 *    buffer (vf23.x - vf21.x - vf21.y): the texture's A+D block (when it is not the last one
 *    written there), the fan GIF tag (968) and the vertices, kicked then, with 969 after it.
 * 4. The output buffer is kicked.
 *
 * The program's ways, kept here:
 *   - an instance's first test compares hb[4] (+ base) with a register left over (50 + hb[8]
 *     after a class packet, vf23.w = 0 after an instance): equal, no vertex is transformed and
 *     the walk works on what the buffer holds;
 *   - the color index qwords are read ahead; the last one read leaves its w in vf24, which the
 *     clipper's first input carries (s t q's w) until a plane test writes vf24.w;
 *   - the walk: the offset right after a strip start is always added (another strip start
 *     there is not seen); a third offset <= 0 starts no strip (the vertex is at the second
 *     - offset, but its triangle counts and the clip flags are not reset); the clip flags of a
 *     triangle sent to the screen test are issued again, so a strip start right before it no
 *     longer clears them; the last triangle is tested without the strip start test; and when
 *     the walk ends with its last vertex in pipeline slot 0, that vertex's place gets the
 *     screen position of the vertex before it (0x2388 stores vf4);
 *   - a vertex's triangle with a vertex outside the guard band sets the vertex's ADC bit even
 *     when it starts a strip;
 *   - the clipper's first vertex at a stage is tested against VU memory 0 (vf18 from address 0);
 *     its inputs' s t q w words are vf24.w, vf25.w (last plane test, kept across runs) and
 *     vf26.w (never written by the program); its stack of pending vertices is at 1018.., below
 *     its return address in 1022.x (1579: 0x3158);
 *   - the GIF tags' w in the buffers: vf1.w - vf1.w and vf7.w - vf7.w (0, or MAX for inf/NaN);
 *     a fan vertex's s t q leaves the fourth word as it was.
 * Where the program would jump to an address it read from VU memory that something overwrote
 * (a plane test's address, the clip call's saved return address, a stack five deep) or loop
 * forever (header ends no vertex has), this file does not follow it.
 *
 * native_tieclip_run() does what the program does, word for word, and reads and writes VU
 * memory as the program does (the buffers, the clipper's stages and stack, the saved
 * registers in the third buffer). Checked against the program itself by dev/vu_harness
 * (native_tieclip_h.c on captures, tieclip_fuzz.c on made-up classes and moved instances). */
#include "native_tieclip.h"
#include <string.h>

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
static u32 clipw(const u32 *c)
{
    float x = F(c[0]), y = F(c[1]), z = F(c[2]), a = __builtin_fabsf(F(c[3]));
    return (x > a) | ((x < -a) << 1) | ((y > a) << 2) | ((y < -a) << 3) | ((z > a) << 4) | ((z < -a) << 5);
}
static u16 lo16(u32 w) { return (u16)w; }
static u32 sx(u16 v) { return (u32)(s32)(s16)v; }          /* MFIR: sign extended */
static u32 sub_self(u32 w) { return U(fc(F(w) - F(w))); }   /* SUB x, x: 0 (MAX for inf / NaN) */

/* ---- the registers the program keeps from one run to the next ---- */
static NativeTieclipRegs R;

void native_tieclip_reset(void) { memset(&R, 0, sizeof R); }
void native_tieclip_seed(const NativeTieclipRegs *r) { R = *r; }
void native_tieclip_state(NativeTieclipRegs *r) { *r = R; }

static void mr32(u32 *v) { u32 x = v[0]; v[0] = v[1]; v[1] = v[2]; v[2] = v[3]; v[3] = x; }

typedef struct {
    u8 *mem;
    u32 *out, out_cap, nq;
    NativeTieclipPacket *pk;
    int maxpk, npk;
    u32 pending;                 /* a kick goes when the next one comes or the run ends */
    /* the strip walk */
    u16 vi[16];
    u32 S[4][4], P[4][4], T[4][4];  /* per pipeline slot: screen (vf1..4), clip space (vf5..8), s t q | strip start flag (vf9..12) */
    u32 vf13[4], vf14[4], vf15[4], vf16[4], vf17[4];
    u32 hist;                    /* the clip flags of the last four CLIPWs */
    u32 vf24w, vf25w;            /* the w words the clipper's inputs carry along */
    u16 clip;                    /* vi9: the clip buffer */
} Ctx;

static u32 *QW(Ctx *c, u32 a) { return (u32 *)(c->mem + (a & 0x3FF) * 16); }

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

/* ======================= 0x0040: setup ======================= */
static void run_init(Ctx *c)
{
    for (int k = 0; k < 6; k++) { u32 *n = QW(c, 976 + 7 * k); n[0] = 0; n[1] = 0; n[2] = 0; n[3] = 0x3F800000u; }
    memcpy(R.vf21, QW(c, 971), 16);
    R.vf23[0] = QW(c, 970)[0]; R.vf23[2] = QW(c, 970)[2];
    memcpy(R.vf22, QW(c, 972), 16);
    for (int k = 0; k < 6; k++) QW(c, 976 + 7 * k)[2] = 1751 + 2 * k;
    R.vi[1] = 1761;
}

/* ======================= 0x0030: a class packet ======================= */
static void run_class(Ctx *c)
{
    u16 *vi = R.vi;
    /* (the delay slot of the start) vf21.xz = vf23.xz - vf21.x, then - vf21.y */
    float x = F(R.vf21[0]);
    R.vf21[0] = U(fc(F(R.vf23[0]) - x)); R.vf21[2] = U(fc(F(R.vf23[2]) - x));
    u32 tag[4]; memcpy(tag, QW(c, 966), 12);                 /* vf1: the A+D GIF tag */
    u32 d0[4]; memcpy(d0, QW(c, 0), 16);                    /* vf7: the next blocks' places */
    vi[4] = 0;
    vi[6] = lo16(R.vf21[1]);
    float y = F(R.vf21[1]);
    R.vf21[0] = U(fc(F(R.vf21[0]) - y)); R.vf21[2] = U(fc(F(R.vf21[2]) - y));
    u32 blk[5][4];
    u16 src = 1;
    for (int r = 0; r < 5; r++) memcpy(blk[r], QW(c, src + r), 16);
    src += 5;
    vi[5] = lo16(R.vf21[0]);
    tag[3] = R.vf1w = sub_self(R.vf1w);
    vi[1] = 969;
    for (int guard = 0; guard < 4096; guard++) {           /* the A+D blocks, into both buffers */
        vi[3] = vi[4] + vi[5];
        vi[4] = vi[4] + vi[6];
        memcpy(QW(c, vi[3]), tag, 16);
        for (int r = 0; r < 5; r++) memcpy(QW(c, vi[3] + 1 + r), blk[r], 16);
        memcpy(QW(c, vi[4]), tag, 16);
        for (int r = 0; r < 5; r++) memcpy(QW(c, vi[4] + 1 + r), blk[r], 16);
        vi[4] = lo16(d0[0]);
        for (int r = 0; r < 5; r++) memcpy(blk[r], QW(c, src + r), 16);
        src += 5;
        d0[0] = d0[1]; d0[1] = d0[2]; d0[2] = d0[3];          /* MR32.xyz */
        if ((s16)vi[4] <= 0) break;
    }
    /* the header: the last five qwords read are it and the first two strips */
    u32 hb[3][4]; memcpy(hb, blk, sizeof hb);
    vi[9] = lo16(hb[0][3]);
    d0[3] = sub_self(d0[3]);
    vi[2] = src - 2;
    vi[7] = lo16(QW(c, vi[2])[0]);
    vi[4] = lo16(QW(c, vi[2])[2]);
    vi[9]--;
    u32 st[4]; memcpy(st, QW(c, 967), 12); st[3] = d0[3];     /* vf7: the strip GIF tag */
    if (vi[9] != 0) {
        for (int guard = 0; guard < 65536; guard++) {
            vi[2]++;
            vi[3] = vi[4] + vi[5];
            vi[4] = vi[4] + vi[6];
            vi[9]--;
            memcpy(QW(c, vi[3]), st, 16); QW(c, vi[3])[0] = vi[7];
            memcpy(QW(c, vi[4]), st, 16); QW(c, vi[4])[0] = vi[7];
            vi[7] = lo16(QW(c, vi[2])[0]);
            vi[4] = lo16(QW(c, vi[2])[2]);
            if (vi[9] == 0) break;
        }
    }
    vi[2]++;
    vi[7] += 0x8000;
    vi[3] = vi[4] + vi[5];
    vi[4] = vi[4] + vi[6];
    memcpy(QW(c, vi[3]), st, 16); QW(c, vi[3])[0] = vi[7];
    memcpy(QW(c, vi[4]), st, 16); QW(c, vi[4])[0] = vi[7];
    /* the dinky vertices to floats, in place: as many as the loop's end (50 + hb[8]) says */
    vi[12] = lo16(hb[2][0]) + 50;
    vi[13] = lo16(hb[2][1]);
    vi[11] = 54;
    u32 nd = 0;
    for (int guard = 0; guard < 4096 && vi[11] != vi[12]; guard++) { nd++; vi[11] += 2; }
    for (u32 j = 0; j < nd; j++) {
        u32 *p = QW(c, 50 + 2 * j), *t = QW(c, 51 + 2 * j);
        u32 a[4] = { itof(p[0], 1.0f), itof(p[1], 1.0f), itof(p[2], 1.0f), itof(p[3], 1.0f) };
        u32 b[4] = { itof(t[0], 1.0f / 4096.0f), itof(t[1], 1.0f / 4096.0f), itof(t[2], 1.0f / 4096.0f), itof(t[3], 1.0f) };
        memcpy(t, b, 16); memcpy(p, a, 16);
    }
    vi[11] -= 4;
    /* the fat ones: the loop's end is hb[9] on from them, two vertices more than it converts */
    vi[13] = vi[13] + vi[11];
    vi[2] = vi[11];
    u16 e = vi[11] + 3;
    u32 n3 = 0;
    for (int guard = 0; guard < 4096 && e != vi[13]; guard++) { n3++; e += 3; }
    u32 nf = n3 >= 2 ? n3 - 1 : 0;   /* exits at +3 and +6 convert none, +9 one, ... */
    for (u32 j = 0; j < nf; j++) {
        u32 *q = QW(c, vi[2]);
        u32 a[4] = { itof(q[0], 1.0f), itof(q[1], 1.0f), itof(q[2], 1.0f), itof(q[3], 1.0f) };
        q = QW(c, vi[2] + 1);
        u32 b[4] = { itof(q[0], 1.0f), itof(q[1], 1.0f), itof(q[2], 1.0f), itof(q[3], 1.0f) };
        q = QW(c, vi[2] + 2);
        u32 t[4] = { itof(q[0], 1.0f / 4096.0f), itof(q[1], 1.0f / 4096.0f), itof(q[2], 1.0f / 4096.0f), itof(q[3], 1.0f) };
        memcpy(QW(c, vi[2]), a, 16); memcpy(QW(c, vi[2] + 1), b, 16); memcpy(QW(c, vi[2] + 2), t, 16);
        vi[2] += 3;
    }
    vi[11] = e;
    /* the class's registers for its instances */
    vi[5] = lo16(hb[0][0]); vi[14] = lo16(hb[0][1]);
    vi[4] = lo16(hb[1][0]); vi[6] = lo16(hb[1][1]); vi[7] = lo16(hb[1][2]); vi[8] = lo16(hb[1][3]);
    vi[15] = 0;
    vi[3] = lo16(R.vf22[0]);
    vi[2] = 50;
    kick(c, vi[1]);
    mr32(R.vf21);
    R.vf23[1] = 0; R.vf23[3] = 0;
}

/* ======================= the clipper ======================= */
/* a vertex in VU memory: clip space position | color (floats) | s t q * w. A stage's plane
 * test (its z: the program's address of it) gives the distances w + x, w - x, w + y, ... of the
 * stage's last vertex and the new one; a vertex is outside when its distance's sign bit is set */
static void plane(u32 fn, const u32 *prev, const u32 *cur, u32 *dprev, u32 *dcur)
{
    int k = (int)(fn - 1751) / 2, comp = k >> 1;
    if (fn < 1751 || fn > 1761 || (fn & 1) == 0) comp = 0, k = 0;   /* (the program would jump elsewhere) */
    float sp = F(prev[comp]), sc = F(cur[comp]);
    if (k & 1) { *dprev = U(fc(F(prev[3]) - sp)); *dcur = U(fc(F(cur[3]) - sc)); }
    else { *dprev = U(fc(F(prev[3]) + sp)); *dcur = U(fc(F(cur[3]) + sc)); }
}
static int neg(u32 w) { return (int)(w >> 31); }

/* the crossing from vertex a (whose distance is da) toward b (db): Q = da / (da - db) */
static void cross(Ctx *c, u32 a, u32 b, float q, u32 dst)
{
    const u32 *pa = QW(c, a), *pb = QW(c, b), *ca = QW(c, a + 1), *cb = QW(c, b + 1), *ta = QW(c, a + 2), *tb = QW(c, b + 2);
    u32 p[4], col[4], t[3];
    for (int k = 0; k < 4; k++) {
        float d = fc(F(pb[k]) - F(pa[k])); d = fc(d * q); p[k] = U(fc(F(pa[k]) + d));
        float e = fc(F(cb[k]) - F(ca[k])); e = fc(e * q); col[k] = U(fc(F(ca[k]) + e));
    }
    for (int k = 0; k < 3; k++) { float d = fc(F(tb[k]) - F(ta[k])); d = fc(d * q); t[k] = U(fc(F(ta[k]) + d)); }
    memcpy(QW(c, dst), p, 16); memcpy(QW(c, dst + 1), col, 16); memcpy(QW(c, dst + 2), t, 12);
}

typedef struct { u16 out, n; } Fan;

/* a clipped vertex out: s t q / w, color (integers), screen x y z, fog */
static void emit(Ctx *c, Fan *f, u32 v)
{
    u32 p[4], col[4], t[3];
    memcpy(p, QW(c, v), 16); memcpy(col, QW(c, v + 1), 16); memcpy(t, QW(c, v + 2), 12);
    f->n++;
    float q = vdiv(1.0f, F(p[3]));
    float s[3];
    for (int k = 0; k < 3; k++) s[k] = fc(F(p[k]) * F(c->vf14[k]));
    for (int k = 0; k < 4; k++) col[k] = (u32)ftoi(F(col[k]), 1.0f);
    f->out += 3;
    for (int k = 0; k < 3; k++) t[k] = U(fc(F(t[k]) * q));
    for (int k = 0; k < 3; k++) s[k] = fc(s[k] * q);
    u32 o[4];
    for (int k = 0; k < 3; k++) o[k] = (u32)ftoi(fc(s[k] + F(c->vf16[k])), 16.0f);
    o[3] = (u32)ftoi(fc(0.0f + F(c->vf16[3])), 16.0f);
    memcpy(QW(c, f->out - 2), col, 16);
    memcpy(QW(c, f->out - 3), t, 12);
    memcpy(QW(c, f->out - 1), o, 16);
}

/* one vertex into the stages from stage s on (and the vertices it leaves on the stack) */
static void feed(Ctx *c, Fan *f, u32 v, u32 s)
{
    u16 sp = 0;
    for (int guard = 0; guard < 100000; guard++) {
        u16 vs = (u16)s, vv = (u16)v;
        if (vs == 1018) {
            emit(c, f, vv);
        } else {
            u32 *node = QW(c, vs);
            u16 prev = lo16(node[1]);
            u32 fn = lo16(node[2]);
            u32 cur[4], pv[4]; memcpy(cur, QW(c, vv), 16); memcpy(pv, QW(c, prev), 16);
            node[1] = vv;
            u32 dp, dc;
            if (prev == 0) {
                node[0] = vv;
                plane(fn, pv, cur, &dp, &dc);
                c->vf24w = dp; c->vf25w = dc;
                if (!neg(dc)) { s = vs + 7; continue; }
            } else {
                plane(fn, pv, cur, &dp, &dc);
                c->vf24w = dp;
                int op = neg(dp), oc = neg(dc);
                c->vf25w = U(fc(F(dp) - F(dc)));
                float q = vdiv(F(dp), F(c->vf25w));
                if (op) {
                    if (!oc) {
                        cross(c, prev, vv, q, vs + 4);
                        s = vs + 7;
                        u32 *k = QW(c, 1018 + sp);
                        k[0] = (u16)s; k[1] = vv; sp++;
                        v = (u16)(s - 3);
                        continue;
                    }
                } else {
                    if (oc) { cross(c, prev, vv, q, vs + 1); v = (u16)(vs + 1); s = vs + 7; continue; }
                    s = vs + 7; continue;
                }
            }
        }
        /* the next vertex from the stack, or back */
        if ((s16)sp <= 0) return;
        const u32 *k = QW(c, 1017 + sp);
        s = lo16(k[0]); v = lo16(k[1]); sp--;
    }
}

/* the triangle (vertices at a b c, their positions and s t q as the walk has them) to the
 * clipper, which kicks the fan it makes, if any */
static void clip_tri(Ctx *c, u16 a, u16 b, u16 cc, const u32 *pa, const u32 *pb, const u32 *pc,
                     const u32 *ta, const u32 *tb, const u32 *tc, u16 ret)
{
    u16 *vi = c->vi, C = c->clip;
    u32 *w = QW(c, C + 44);
    w[0] = vi[2]; w[1] = vi[3]; w[2] = vi[4]; w[3] = vi[5];
    w = QW(c, C + 45);
    w[0] = vi[6]; w[1] = vi[7]; w[2] = vi[8]; w[3] = vi[1];
    QW(c, C + 46)[0] = ret;
    u16 tex = lo16(QW(c, 1022)[2]), last = lo16(QW(c, 1022)[1]);
    u32 v24[4] = { ta[0], ta[1], ta[2], c->vf24w };
    u32 c0[4];
    c0[0] = sx(lo16(QW(c, a + 1)[0])); c0[1] = sx(lo16(QW(c, a + 1)[1]));
    if (last != tex) {
        if ((s16)(u16)(tex - a) > 0) tex = lo16(QW(c, 1022)[3]);   /* the walk is past a switch this triangle is before */
        memcpy(QW(c, C + 47), pa, 16); memcpy(QW(c, C + 48), pb, 16); memcpy(QW(c, C + 49), pc, 16); memcpy(QW(c, C + 50), v24, 16);
        u32 tg[4], bl[5][4];
        memcpy(tg, QW(c, 966), 16);
        for (int r = 0; r < 4; r++) memcpy(bl[r], QW(c, tex + r), 16);
        memcpy(QW(c, C), tg, 16); memcpy(QW(c, C + 1), bl[0], 16);
        memcpy(bl[4], QW(c, tex + 4), 16);
        for (int r = 1; r < 5; r++) memcpy(QW(c, C + 1 + r), bl[r], 16);
        QW(c, 1022)[1] = tex;
    }
    c0[2] = sx(lo16(QW(c, a + 1)[2])); c0[3] = sx(lo16(QW(c, a + 1)[3]));
    u32 v25[4] = { tb[0], tb[1], tb[2], c->vf25w }, v26[4] = { tc[0], tc[1], tc[2], R.vf26w };
    for (int k = 0; k < 3; k++) {
        v24[k] = U(fc(F(v24[k]) * F(pa[3]))); v25[k] = U(fc(F(v25[k]) * F(pb[3]))); v26[k] = U(fc(F(v26[k]) * F(pc[3])));
    }
    u32 f0[4], f1[4], f2[4], c1[4], c2[4];
    for (int k = 0; k < 4; k++) f0[k] = itof(c0[k], 1.0f);
    for (int k = 0; k < 4; k++) c1[k] = sx(lo16(QW(c, b + 1)[k]));
    for (int k = 0; k < 4; k++) f1[k] = itof(c1[k], 1.0f);
    memcpy(QW(c, C + 35), f0, 16);
    for (int k = 0; k < 4; k++) c2[k] = sx(lo16(QW(c, cc + 1)[k]));
    memcpy(QW(c, C + 38), f1, 16);
    u32 tag968[4]; memcpy(tag968, QW(c, 968), 16);
    for (int k = 0; k < 4; k++) f2[k] = itof(c2[k], 1.0f);
    memcpy(QW(c, C + 34), pa, 16); memcpy(QW(c, C + 36), v24, 16); memcpy(QW(c, C + 37), pb, 16);
    memcpy(QW(c, C + 39), v25, 16); memcpy(QW(c, C + 40), pc, 16); memcpy(QW(c, C + 42), v26, 16);
    memcpy(QW(c, C + 41), f2, 16); memcpy(QW(c, C + 43), R.vf29, 16); memcpy(QW(c, C + 6), tag968, 16);
    /* the clipper */
    Fan f = { (u16)(C + 7), 0 };
    QW(c, 1022)[0] = 1579;                         /* (its return address: 0x3158) */
    feed(c, &f, (u16)(C + 34), 976);
    feed(c, &f, (u16)(C + 37), 976);
    feed(c, &f, (u16)(C + 40), 976);
    for (u16 s = 976;; s += 7) {                   /* close the polygon at each stage */
        u32 *node = QW(c, s);
        u16 first = lo16(node[0]), last2 = lo16(node[1]);
        u32 fn = lo16(node[2]);
        if (first != 0 && last2 != first) {
            u32 pl[4], pf[4]; memcpy(pl, QW(c, last2), 16); memcpy(pf, QW(c, first), 16);
            u32 dl, df;
            plane(fn, pl, pf, &dl, &df);
            c->vf24w = dl;
            int ol = neg(dl), of = neg(df);
            c->vf25w = U(fc(F(dl) - F(df)));
            if (ol != of) {
                float q = vdiv(F(dl), F(c->vf25w));
                if (!ol) { cross(c, last2, first, q, s + 1); feed(c, &f, (u16)(s + 1), (u16)(s + 7)); }
                else { cross(c, last2, first, q, s + 4); feed(c, &f, (u16)(s + 4), (u16)(s + 7)); }
            }
        }
        node = QW(c, s);
        node[0] = 0; node[1] = 0;
        if (s >= 1011) break;
    }
    u16 n = f.n;
    if (n) n += 0x8000;
    QW(c, C + 6)[0] = n;
    if (n) { kick(c, C); kick(c, 969); }
    /* the walk's registers back (from VU memory, as the program has them) */
    w = QW(c, C + 44);
    vi[2] = lo16(w[0]); vi[3] = lo16(w[1]); vi[4] = lo16(w[2]); vi[5] = lo16(w[3]);
    w = QW(c, C + 45);
    vi[6] = lo16(w[0]); vi[7] = lo16(w[1]); vi[8] = lo16(w[2]); vi[1] = lo16(w[3]);
    memcpy(R.vf29, QW(c, C + 43), 16);
}

/* ======================= the strip walk ======================= */
#define SL(k) ((k) & 3)
static void push(Ctx *c, const u32 *p) { c->hist = ((c->hist << 6) | clipw(p)) & 0xFFFFFF; }
static u16 fcand(Ctx *c) { return (c->hist & 0x3FFFF) != 0; }
static u16 *A(Ctx *c, int s) { return &c->vi[3 + SL(s)]; }
static void mul14(Ctx *c, int s) { for (int k = 0; k < 3; k++) c->S[s][k] = U(fc(F(c->P[s][k]) * F(c->vf14[k]))); }
static void ldT(Ctx *c, int s) { memcpy(c->T[s], QW(c, *A(c, s)), 12); }
static void mulq(Ctx *c, int s) { float q = F(c->T[s][2]); for (int k = 0; k < 3; k++) c->S[s][k] = U(fc(F(c->S[s][k]) * q)); }
static void w0(Ctx *c, int s) { c->S[s][3] = U(fc(1.0f - 1.0f)); }
static void addoff(Ctx *c, int s, const u32 *o) { for (int k = 0; k < 4; k++) c->S[s][k] = U(fc(F(c->S[s][k]) + F(o[k]))); }
static void toi(Ctx *c, int s) { for (int k = 0; k < 4; k++) c->S[s][k] = (u32)ftoi(F(c->S[s][k]), 16.0f); }
static void stS(Ctx *c, int s) { memcpy(QW(c, *A(c, s) + 2), c->S[s], 16); }
static void ldP(Ctx *c, int s) { memcpy(c->P[s], QW(c, *A(c, s) + 2), 16); }
static void flag(Ctx *c, int s, int one) { c->T[s][3] = one ? 0x3F800000u : 0; }
static void scale(Ctx *c, const u32 *p, u32 *o) { for (int k = 0; k < 4; k++) o[k] = U(fc(F(p[k]) * F(c->vf17[k]))); }
static void plus0(const u32 *t, u32 *o) { for (int k = 0; k < 3; k++) o[k] = U(fc(F(t[k]) + 0.0f)); }

/* return addresses of the triangle blocks (saved with the walk's registers) */
static const u16 RET_N[4] = { 1046, 1054, 1030, 1038 }, RET_R[4] = { 1117, 1144, 1063, 1090 }, RET_E[4] = { 1133, 1161, 1079, 1106 };

/* a triangle whose vertices are not all inside the guard band, found at step m (slot s) of
 * the walk (the triangle ends at m - 1); restart: the step starts a new strip */
static void tri_block(Ctx *c, int s, int restart, const u16 *oth)
{
    int st = SL(s - 1);
    int proceed = neg(U(fc(F(c->T[st][3]) - 1.0f)));      /* not a strip's first two vertices */
    u16 a0 = *A(c, s + 1);
    u32 x0[4], x1[4], x2[4];
    scale(c, c->P[SL(s + 1)], x0); scale(c, c->P[SL(s + 2)], x1); scale(c, c->P[SL(s + 3)], x2);
    if (restart) *A(c, s + 1) = *A(c, s) - *oth; else *A(c, s + 1) = *A(c, s) + *oth;
    addoff(c, st, c->vf15);
    if (restart) flag(c, SL(s + 1), 1); else flag(c, SL(s + 2), 0);
    stS(c, SL(s - 2));
    if (!proceed) { mulq(c, s); c->vi[1] = fcand(c); w0(c, s); ldP(c, SL(s + 1)); return; }
    u32 k0 = clipw(x0), k1 = clipw(x1), k2 = clipw(x2);
    c->hist = ((c->hist << 18) | (k0 << 12) | (k1 << 6) | k2) & 0xFFFFFF;
    u32 p0[4], p1[4], p2[4], t0[3], t1[3], t2[3];
    memcpy(p0, c->P[SL(s + 1)], 16); memcpy(p1, c->P[SL(s + 2)], 16); memcpy(p2, c->P[SL(s + 3)], 16);
    plus0(c->T[SL(s + 1)], t0); plus0(c->T[SL(s + 2)], t1); plus0(c->T[SL(s + 3)], t2);
    push(c, c->P[SL(s + 2)]); push(c, c->P[SL(s + 3)]); push(c, c->P[s]);
    ldP(c, SL(s + 1));
    mulq(c, s); w0(c, s);
    c->vi[1] = fcand(c);
    if (k0 & k1 & k2 & 63) return;                          /* all outside one plane of the screen */
    clip_tri(c, a0, *A(c, s + 2), *A(c, s + 3), p0, p1, p2, t0, t1, t2, restart ? RET_R[s] : RET_N[s]);
}

/* the walk's end at restart step r (slot s): its last triangle and vertices */
static void walk_end(Ctx *c, int s)
{
    if (c->vi[1]) {
        u32 x0[4], x1[4], x2[4];
        scale(c, c->P[SL(s + 2)], x0); scale(c, c->P[SL(s + 3)], x1); scale(c, c->P[s], x2);
        addoff(c, s, c->vf15);
        u16 a0 = *A(c, s + 2);
        u32 k0 = clipw(x0), k1 = clipw(x1), k2 = clipw(x2);
        c->hist = ((c->hist << 18) | (k0 << 12) | (k1 << 6) | k2) & 0xFFFFFF;
        u32 t0[3], t1[3], t2[3];
        plus0(c->T[SL(s + 2)], t0); plus0(c->T[SL(s + 3)], t1); plus0(c->T[s], t2);
        stS(c, SL(s - 1));
        if (!(k0 & k1 & k2 & 63))
            clip_tri(c, a0, *A(c, s + 3), *A(c, s), c->P[SL(s + 2)], c->P[SL(s + 3)], c->P[s], t0, t1, t2, RET_E[s]);
    } else {
        addoff(c, s, c->vf16);
        stS(c, SL(s - 1));
    }
    toi(c, s);
    /* (in slot 0 the program stores the vertex before's screen position there: vf4, 0x2388) */
    if (s == 0) memcpy(QW(c, *A(c, 0) + 2), c->S[3], 16); else stS(c, s);
}

static void walk(Ctx *c)
{
    u16 *vi = c->vi;
    memcpy(c->vf13, QW(c, 30), 16);
    u16 inst = lo16(R.vf22[0]);
    vi[1] = lo16(R.vf21[0]);
    vi[7] = 31;
    float cb = fc(F(R.vf23[0]) - F(R.vf21[0])); cb = fc(cb - F(R.vf21[1]));
    memcpy(c->vf16, QW(c, 973), 16);
    vi[2] = lo16(c->vf13[0]);
    c->vf16[3] = QW(c, inst + 5)[3];
    memcpy(c->vf14, QW(c, 974), 16);
    vi[3] = vi[1] + vi[2];
    vi[8] = lo16(c->vf13[1]);
    ldP(c, 0);
    for (int k = 0; k < 4; k++) c->vf15[k] = U(fc(F(c->vf16[k]) + 0.0f));
    c->clip = lo16(U(cb)); vi[9] = c->clip;
    vi[1] = vi[1] + 1;
    QW(c, 1022)[1] = 0;
    QW(c, 1022)[2] = vi[1];
    flag(c, 0, 1); c->hist = 0;
    mul14(c, 0); ldT(c, 0);
    push(c, c->P[0]);
    c->vf15[3] = U(fc(F(c->vf15[3]) + 3072.0f));
    vi[4] = vi[3] + vi[8];
    flag(c, 1, 1); vi[2] = lo16(c->vf13[2]);
    ldP(c, 1);
    mulq(c, 0); memcpy(c->vf17, QW(c, 975), 16);
    w0(c, 0);
    flag(c, 2, 0);
    int init2 = (s16)vi[2] <= 0;
    mul14(c, 1); ldT(c, 1);
    push(c, c->P[1]); vi[1] = 0;
    if (init2) vi[5] = vi[4] - vi[2]; else vi[5] = vi[4] + vi[2];
    addoff(c, 0, c->vf16); vi[8] = lo16(c->vf13[3]);
    flag(c, 3, 0); ldP(c, 2);
    mulq(c, 1); vi[1] = fcand(c);
    w0(c, 1); memcpy(c->vf13, QW(c, vi[7]), 16); vi[7]++;
    toi(c, 0);
    int restart = !init2 && (s16)vi[8] <= 0;
    mul14(c, 2); ldT(c, 2);

    for (int m = 2; m < 100000; m++) {
        int s = SL(m);
        u16 *own = (m & 1) ? &vi[8] : &vi[2], *oth = (m & 1) ? &vi[2] : &vi[8];
        push(c, c->P[s]);
        int blk = vi[1] != 0;
        *own = lo16(c->vf13[SL(m + 2)]);
        if (!restart) {
            if (blk) tri_block(c, s, 0, oth);
            else {
                addoff(c, SL(s - 1), c->vf16); *A(c, s + 1) = *A(c, s) + *oth;
                flag(c, SL(s + 2), 0); ldP(c, SL(s + 1));
                mulq(c, s); vi[1] = fcand(c);
                w0(c, s); stS(c, SL(s - 2));
            }
            if (s == 1) { memcpy(c->vf13, QW(c, vi[7]), 16); vi[7]++; }
            toi(c, SL(s - 1));
            restart = (s16)*own <= 0;
            mul14(c, SL(s + 1)); ldT(c, SL(s + 1));
            continue;
        }
        if (blk) tri_block(c, s, 1, oth);
        else {
            flag(c, SL(s + 1), 1); *A(c, s + 1) = *A(c, s) - *oth;
            addoff(c, SL(s - 1), c->vf16); ldP(c, SL(s + 1));
            mulq(c, s); vi[1] = fcand(c);
            w0(c, s); stS(c, SL(s - 2));
        }
        if (s == 1) { memcpy(c->vf13, QW(c, vi[7]), 16); vi[7]++; }
        vi[10] = *oth + 10;
        flag(c, SL(s + 2), 1); c->hist = 0;
        toi(c, SL(s - 1));
        mul14(c, SL(s + 1)); ldT(c, SL(s + 1));
        restart = 0;
        if (vi[10] != 0) continue;
        if (*own == 0) { walk_end(c, s); break; }
        vi[11] = *A(c, s + 1) - 6;                              /* a texture switch */
        vi[10] = lo16(QW(c, 1022)[2]);
        QW(c, 1022)[2] = vi[11];
        QW(c, 1022)[3] = vi[10];
    }
}

/* ======================= the vertices ======================= */
typedef struct {
    float R[4][4];               /* rows 0..3 */
    float m, cw0, cw1;           /* row 4: x the fat morph, z / w the color weights */
    float base;                  /* vf21.x */
    u32 inst;                    /* vi3 at the start */
} Inst;

static void transform(const Inst *in, float x, float y, float z, u32 *p)
{
    for (int k = 0; k < 4; k++) {
        float acc = fc(in->R[3][k] * 1.0f);
        acc = fc(acc + fc(in->R[0][k] * x));
        acc = fc(acc + fc(in->R[1][k] * y));
        p[k] = U(fc(acc + fc(in->R[2][k] * z)));
    }
}

/* one vertex out to its place (and its second place); returns its place */
static u16 put(Ctx *c, const u32 *st, const u32 *col, const u32 *pos, float placef, int twice, const Inst *in)
{
    u32 t[4];
    float q = vdiv(1.0f, F(pos[3]));
    for (int k = 0; k < 3; k++) t[k] = U(fc(F(st[k]) * q));
    t[3] = st[3];
    u16 a = lo16(U(fc(placef + in->base)));
    if (twice) t[3] = U(fc(F(st[3]) + in->base));
    memcpy(QW(c, a), t, 16); memcpy(QW(c, a + 1), col, 16); memcpy(QW(c, a + 2), pos, 16);
    if (twice) { u16 b = lo16(t[3]); memcpy(QW(c, b), t, 16); memcpy(QW(c, b + 1), col, 16); memcpy(QW(c, b + 2), pos, 16); }
    return a;
}

static u16 dinky(Ctx *c, const Inst *in, u32 j, int twice, u16 vi2)
{
    const u32 *p = QW(c, vi2 + 2 * j), *st = QW(c, vi2 + 2 * j + 1);
    u32 pos[4];
    transform(in, F(p[0]), F(p[1]), F(p[2]), pos);
    u32 idx = QW(c, in->inst + 6 + j / 4)[j & 3];
    u32 col[4]; memcpy(col, QW(c, 838 + lo16(idx)), 16);
    u32 s[4]; memcpy(s, st, 16);
    return put(c, s, col, pos, F(p[3]), twice, in);
}

static u16 fat(Ctx *c, const Inst *in, u32 f0, u32 iq0, u32 j, int twice)
{
    const u32 *a = QW(c, f0 + 3 * j), *b = QW(c, f0 + 3 * j + 1), *st = QW(c, f0 + 3 * j + 2);
    float x = fc(fc(F(a[0]) * in->m) + F(b[0])), y = fc(fc(F(a[1]) * in->m) + F(b[1])), z = fc(fc(F(a[2]) * in->m) + F(b[2]));
    u32 pos[4];
    transform(in, x, y, z, pos);
    const u32 *iq = QW(c, in->inst + 6 + iq0 + j);
    const u32 *c0 = QW(c, 838 + lo16(iq[0])), *c1 = QW(c, 838 + lo16(iq[1])), *c2 = QW(c, 838 + lo16(iq[2]));
    u32 col[4];
    for (int k = 0; k < 4; k++) {
        float acc = fc(F(c1[k]) * 0.5f);
        float mm = fc(acc + fc(F(c2[k]) * 0.5f));
        acc = fc(mm * in->cw1);
        col[k] = U(fc(acc + fc(F(c0[k]) * in->cw0)));
    }
    u32 s[4]; memcpy(s, st, 16);
    return put(c, s, col, pos, F(a[3]), twice, in);     /* (the place: a.w + base) */
}

static void run_instance(Ctx *c)
{
    u16 *vi = R.vi;
    mr32(R.vf21);
    Inst in;
    in.inst = vi[3];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) in.R[r][k] = F(QW(c, vi[3] + r)[k]);
    const u32 *r4 = QW(c, vi[3] + 4);
    in.m = F(r4[0]); in.cw0 = F(r4[2]); in.cw1 = F(r4[3]);
    in.base = F(R.vf21[0]);
    u16 d1 = lo16(R.vf21[0]) - vi[15];
    vi[4] += d1; vi[6] += d1; vi[7] += d1; vi[8] += d1; vi[15] += d1;
    u32 nidx = 1;                                  /* idx qwords loaded (row 6 is the first) */
    u16 vi2 = vi[2];
    if (vi[4] != vi[12]) {
        /* dinky: one place up to the vertex at hb[4], and three more */
        u32 j = 0, k;
        for (;; j++) { if (dinky(c, &in, j, 0, vi2) == vi[4] || j > 4096) break; }
        k = j;
        for (j = k + 1; j <= k + 3; j++) dinky(c, &in, j, 0, vi2);
        nidx = k / 4 + 2;
        u32 n = k + 4;
        if (vi[5] == 0) {                          /* two places up to the vertex at hb[5], and two more */
            for (j = k + 4;; j++) { if (dinky(c, &in, j, 1, vi2) == vi[6] || j > 4096) break; }
            dinky(c, &in, j + 1, 1, vi2); dinky(c, &in, j + 2, 1, vi2);
            n = j + 3;
            /* (index qwords: one more on the way in from the single loop's slots 2 and 3, one
             * with each double vertex in slot 1) */
            nidx += (k & 3) >= 2;
            for (u32 i = k + 4; i <= j; i++) nidx += (i & 3) == 1;
        }
        if (vi[14] == 0) {                         /* fat: one place up to hb[6], two up to hb[7] */
            u32 f0 = vi2 + 2 * n, iq0 = (n + 3) / 4;
            for (j = 0;; j++) { if (fat(c, &in, f0, iq0, j, 0) == vi[7] || j > 4096) break; }
            for (j = j + 1;; j++) { if (fat(c, &in, f0, iq0, j, 1) == vi[8] || j > 4096) break; }
            nidx = iq0 + j + 5;
        }
    }
    /* the registers the walk saves (vf29..vf31) */
    R.vf29[0] = sx(vi[4]); R.vf29[1] = sx(vi[5]); R.vf29[2] = sx(vi[6]); R.vf29[3] = sx(vi[7]);
    R.vf30[0] = sx(vi[8]); R.vf30[1] = sx(vi[9]); R.vf30[2] = sx(vi[10]); R.vf30[3] = sx(vi[11]);
    R.vf31[0] = sx(vi[13]); R.vf31[1] = sx(vi[14]); R.vf31[2] = sx(vi[15]);
    c->vf24w = QW(c, in.inst + 6 + nidx - 1)[3];
    c->vf25w = R.vf25w;
    walk(c);
    R.vf25w = c->vf25w;
    R.vf1w = c->S[0][3];
    for (int k = 4; k < 16; k++) if (k != 12) {
        const u32 *src = k < 8 ? &R.vf29[k - 4] : k < 12 ? &R.vf30[k - 8] : &R.vf31[k - 13];
        vi[k] = lo16(*src);
    }
    vi[1] = lo16(R.vf21[0]);
    vi[12] = lo16(R.vf23[3]);
    mr32(R.vf22);
    vi[2] = 50;
    R.vf23[1] = 0;
    kick(c, vi[1]);
    vi[3] = lo16(R.vf22[0]);
}

/* 0x0010: an instance skipped */
static void run_skip(void)
{
    u16 *vi = R.vi;
    for (int k = 4; k < 16; k++) if (k != 12) {
        const u32 *src = k < 8 ? &R.vf29[k - 4] : k < 12 ? &R.vf30[k - 8] : &R.vf31[k - 13];
        vi[k] = lo16(*src);
    }
    vi[1] = lo16(R.vf23[1]);
    vi[12] = lo16(R.vf23[3]);
    mr32(R.vf22);
    vi[2] = 50;
    vi[1] = vi[1] + vi[15];
    R.vf23[1] = sx(vi[12]);
    vi[3] = lo16(R.vf22[0]);
}

int native_tieclip_run(u32 pc, u8 *mem, u32 top, u32 *out, u32 out_cap, NativeTieclipPacket *pk, int maxpk)
{
    (void)top;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.mem = mem; c.out = out; c.out_cap = out_cap; c.pk = pk; c.maxpk = maxpk; c.pending = 0xFFFFFFFFu;
    switch (pc) {
    case 0x40: run_init(&c); break;
    case 0x20: break;
    case 0x30: run_class(&c); break;
    case 0x00: run_instance(&c); break;
    case 0x10: run_skip(); break;
    default: return -1;
    }
    if (c.pending != 0xFFFFFFFFu) send(&c, c.pending);
    return c.npk;
}
