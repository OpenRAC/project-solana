/* Ties (the level's instanced scenery: trees, walls, buildings), drawn by native code from
 * the game's tie data instead of by the PS2's VU1 tie microprogram (image 13507).
 *
 * The game sends, per tie class packet: its texture register blocks (A+D), an unpack header
 * (strip count, vertex counts), its strips (vertex count, place in the GS packet) and its
 * vertices; per instance: a 4x4 transform (object to screen), a morph factor and color
 * weights, the fog value, the instance's 64-color table and the class's color indexes.
 *   dinky vertex: x y z (16-bit), its place in the packet | s t q (4.12), a second place
 *   fat vertex:   a morph delta | a base position | s t q, place (a color blended from three)
 * native_tie_draw() builds the GS packet the microprogram built, word for word (checked
 * against the program itself by dev/vu_harness on every tie run of whole frames: identical
 * but for the unused fourth word of ST), and the caller sends it on. */
#include "native_tie.h"
#include <string.h>
static void tm_class(void);

typedef struct { s32 p[4]; s32 st[4]; } Dinky;            /* x y z place | s t q place2 */
typedef struct { s32 a[4]; s32 b[4]; s32 st[4]; } Fat;     /* delta, place | base | s t q place2 */
typedef struct {
    int nad; u32 ad_dest[5]; u32 ad[5][5][4];
    int nstrip; u32 strip_count[64], strip_off[64];
    int ndinky, nfat; Dinky dinky[256]; Fat fat[128];
    u32 tag_ad[4], tag_strip[4];
    u32 size;
    int valid;
} TieClass;
static TieClass cls;

static const u32 *Q(const u8 *mem, u32 a) { return (const u32 *)(mem + (a & 0x3FF) * 16); }

void native_tie_setup(const u8 *mem)
{
    TieClass *c = &cls;
    c->valid = 0;
    const u32 *d0 = Q(mem, 0);                    /* where the texture blocks after the first go */
    c->ad_dest[0] = 0; c->nad = 1;
    for (int k = 0; k < 4 && (s32)d0[k] > 0; k++) c->ad_dest[c->nad++] = d0[k];
    for (int a = 0; a < c->nad; a++) for (int r = 0; r < 5; r++) memcpy(c->ad[a][r], Q(mem, 1 + a * 5 + r), 16);
    u32 h = 1 + c->nad * 5, hb[12];               /* the unpack header: 12 bytes, one per word */
    for (int k = 0; k < 12; k++) hb[k] = Q(mem, h + k / 4)[k % 4];
    c->nstrip = hb[3] < 64 ? (int)hb[3] : 64;
    for (int s = 0; s < c->nstrip; s++) { const u32 *q = Q(mem, h + 3 + s); c->strip_count[s] = q[0]; c->strip_off[s] = q[2]; }
    c->ndinky = ((s32)hb[8] - 4) / 2;
    if (c->ndinky < 0) c->ndinky = 0;
    if (c->ndinky > 256) c->ndinky = 256;
    for (int j = 0; j < c->ndinky; j++) { memcpy(c->dinky[j].p, Q(mem, 50 + 2 * j), 16); memcpy(c->dinky[j].st, Q(mem, 51 + 2 * j), 16); }
    c->nfat = hb[11] < 128 ? (int)hb[11] : 128;
    u32 f0 = 50 + hb[8] - 4;
    for (int j = 0; j < c->nfat; j++) {
        memcpy(c->fat[j].a, Q(mem, f0 + 3 * j), 16); memcpy(c->fat[j].b, Q(mem, f0 + 3 * j + 1), 16); memcpy(c->fat[j].st, Q(mem, f0 + 3 * j + 2), 16);
    }
    memcpy(c->tag_ad, Q(mem, 966), 16); memcpy(c->tag_strip, Q(mem, 967), 16);
    c->size = 0;                                   /* the packet's length in qwords */
    for (int a = 0; a < c->nad; a++) if (c->ad_dest[a] + 6 > c->size) c->size = c->ad_dest[a] + 6;
    for (int s = 0; s < c->nstrip; s++) { u32 e = c->strip_off[s] + 1 + 3 * c->strip_count[s]; if (e > c->size) c->size = e; }
    if (c->size > 0x400) c->size = 0x400;
    c->valid = 1;
    tm_class();
}
u32 native_tie_size(void) { return cls.valid ? cls.size : 0; }

/* the VU's float to fixed point: truncation (the game runs with SSE rounding toward zero) */
static s32 ftoi4(float f) { float x = f * 16.0f; return x >= 2147483647.0f ? 0x7FFFFFFF : x <= -2147483648.0f ? (s32)0x80000000u : (s32)x; }
static float itof12(s32 i) { return (float)i * (1.0f / 4096.0f); }
static float fc(float x) { u32 b; memcpy(&b, &x, 4); if ((b & 0x7F800000u) == 0x7F800000u) { b = (b & 0x80000000u) | 0x7F7FFFFFu; memcpy(&x, &b, 4); } return x; }

static void put_vertex(u32 *out, u32 *n, u32 slot, const float st[3], const u32 rgba[4], const s32 xyz[3], s32 fog)
{
    slot &= 0x3FF;
    if (slot + 3 > 0x400) return;
    u32 *o = out + slot * 4;
    memcpy(o, st, 12); o[3] = 0;
    memcpy(o + 4, rgba, 16);
    o[8] = (u32)xyz[0]; o[9] = (u32)xyz[1]; o[10] = (u32)xyz[2]; o[11] = (u32)fog;
    if (slot + 3 > *n) *n = slot + 3;
}

/* object to screen: the program's own order of operations (ACC = row3, + row0 x, + row1 y, row2 z + ACC) */
static void transform(float R[6][4], float x, float y, float z, s32 xyz[3], float *q)
{
    float p[4];
    for (int k = 0; k < 4; k++) { float acc = fc(R[3][k]); acc = fc(acc + fc(R[0][k] * x)); acc = fc(acc + fc(R[1][k] * y)); p[k] = fc(fc(R[2][k] * z) + acc); }
    float w = p[3];
    *q = w == 0.0f ? 3.4028235e38f : 1.0f / w;
    xyz[0] = ftoi4(fc(p[0] * *q)); xyz[1] = ftoi4(fc(p[1] * *q)); xyz[2] = ftoi4(fc(p[2] * *q));
}

/* Before an instance's geometry, the program switches the alpha test when the instance's mode
 * (the low 16 bits of its fifth row's x) differs from the last one: a one-register packet,
 * TEST_1 from qword 974 (mode set) or 973 (mode clear), tag at 972. The last mode is kept in
 * VU memory (qword 971, y; x: the current one), as the program keeps it. */
u32 native_tie_state(u8 *mem, u32 top, u32 *out)
{
    u32 mode = Q(mem, 0xC6 + top + 5)[0] & 0xFFFF;
    u32 *st = (u32 *)(mem + 971 * 16);
    st[0] = (st[0] & 0xFFFF0000u) | mode;
    if ((st[1] & 0xFFFF) == mode) return 0;
    memcpy(out, Q(mem, 972), 16);
    memcpy(out + 4, Q(mem, mode ? 974 : 973), 16);
    st[1] = (st[1] & 0xFFFF0000u) | mode;
    return 2;
}

u32 native_tie_draw(const u8 *mem, u32 top, u32 *out)
{
    TieClass *c = &cls;
    if (!c->valid) return 0;
    float R[6][4];
    for (int k = 0; k < 6; k++) memcpy(R[k], Q(mem, 0xC6 + top + k), 16);
    const u32 *idx = Q(mem, 0xCC + top);            /* the class's color indexes, one per word */
    u32 n = 0;
    for (int a = 0; a < c->nad; a++) {               /* texture blocks */
        u32 d = c->ad_dest[a] & 0x3FF;
        if (d + 6 > 0x400) continue;
        u32 *o = out + d * 4; memcpy(o, c->tag_ad, 16); o[3] = 0;
        for (int r = 0; r < 5; r++) memcpy(o + 4 + r * 4, c->ad[a][r], 16);
        if (d + 6 > n) n = d + 6;
    }
    for (int s = 0; s < c->nstrip; s++) {            /* strip tags; the last one ends the packet */
        u32 d = c->strip_off[s] & 0x3FF, end = d + 1 + 3 * c->strip_count[s];
        if (end > 0x400) continue;
        u32 *o = out + d * 4; memcpy(o, c->tag_strip, 16);
        o[0] = c->strip_count[s] | (s == c->nstrip - 1 ? 0x8000 : 0);
        if (end > n) n = end;
    }
    s32 fog = ftoi4(R[5][3]);
    for (int j = 0; j < c->ndinky; j++) {
        const Dinky *v = &c->dinky[j];
        s32 xyz[3]; float q;
        transform(R, (float)v->p[0], (float)v->p[1], (float)v->p[2], xyz, &q);
        float st[3] = { fc(itof12(v->st[0]) * q), fc(itof12(v->st[1]) * q), fc(itof12(v->st[2]) * q) };
        const u32 *col = Q(mem, 838 + idx[j]);
        put_vertex(out, &n, v->p[3], st, col, xyz, fog);
        if (v->st[3] && v->st[3] != v->p[3]) put_vertex(out, &n, v->st[3], st, col, xyz, fog);
    }
    u32 fq = (c->ndinky + 3) / 4;                    /* fat vertices: three color indexes each, a word of four */
    for (int j = 0; j < c->nfat; j++) {
        const Fat *v = &c->fat[j];
        float x = fc(fc((float)v->a[0] * R[4][0]) + (float)v->b[0]);
        float y = fc(fc((float)v->a[1] * R[4][0]) + (float)v->b[1]);
        float z = fc(fc((float)v->a[2] * R[4][0]) + (float)v->b[2]);
        s32 xyz[3]; float q;
        transform(R, x, y, z, xyz, &q);
        float st[3] = { fc(itof12(v->st[0]) * q), fc(itof12(v->st[1]) * q), fc(itof12(v->st[2]) * q) };
        const u32 *iq = Q(mem, 0xCC + top + fq + j);
        const float *c0 = (const float *)Q(mem, 838 + iq[0]), *c1 = (const float *)Q(mem, 838 + iq[1]), *c2 = (const float *)Q(mem, 838 + iq[2]);
        u32 col[4];
        for (int k = 0; k < 4; k++) {   /* c0 weighted by row4.z, the mean of c1 and c2 by row4.w */
            float m = fc(fc(c2[k] * 0.5f) + fc(c1[k] * 0.5f));
            float f = fc(fc(c0[k] * R[4][2]) + fc(m * R[4][3]));
            memcpy(&col[k], &f, 4);
        }
        put_vertex(out, &n, v->a[3], st, col, xyz, fog);
        if (v->st[3] && v->st[3] != v->a[3]) put_vertex(out, &n, v->st[3], st, col, xyz, fog);
    }
    return n;
}

/* ---- ties as native meshes ----
 * A class's geometry is the same for every instance and every frame: its vertices (object
 * space), texture coordinates and strips become a mesh, built the first time the class packet
 * is seen (found again by its contents, the texture registers aside: the game patches those).
 * An instance is then drawn as that mesh with its transform, its colours (the instance's table)
 * and, when the class has fat vertices, their morphed positions (sent only when they change),
 * in place of the GS packet. The packet's shape is checked when the mesh is made: its items
 * (texture blocks, strips) follow one another to the end-of-packet strip, every strip vertex
 * is a vertex's place, and every q is 1; else the class goes as the packet. The vertices come
 * out as the program's (row 3 + row 0 x + row 1 y + row 2 z, 1/w, to the GS's 1/16 pixel;
 * the GS layer's CPU form, gs_mesh_vertex, gives the packet's values bit for bit).
 * UYA_NTIEMESH=0: the packets. */
#include "native_dlist.h"
#include <stdlib.h>
#include <stdio.h>
#define TM_ID0 16384u
#define TM_N 4096u
typedef struct { u32 first, count, blk; int strips; } TmRange;
typedef struct {
    u32 *key; u32 nkey; u64 h;
    int ok; u32 nv; float *v6; u8 *rgba;      /* (the mesh as last sent) */
    TmRange *r; int nr; u64 prim; int pre;
} TieMesh;
static TieMesh *tm; static u32 ntm;
static int tm_on = -1, tm_cur = -1;
static unsigned long long tm_n_mesh, tm_n_pk, tm_n_up;
#define TH 8192
static u64 th_k[TH]; static int th_v[TH];

static u32 tkey[16 + 256 * 8 + 128 * 12 + 64 * 2 + 16];
static u32 make_key(const TieClass *c)
{
    u32 n = 0;
    tkey[n++] = c->nad; for (int a = 0; a < c->nad; a++) tkey[n++] = c->ad_dest[a];
    for (int a = 0; a < c->nad; a++) tkey[n++] = c->ad[a][0][2] & 0xFF;   /* (the registers' addresses, not their values) */
    tkey[n++] = c->nstrip; for (int s = 0; s < c->nstrip; s++) { tkey[n++] = c->strip_count[s]; tkey[n++] = c->strip_off[s]; }
    tkey[n++] = c->ndinky; memcpy(tkey + n, c->dinky, c->ndinky * sizeof(Dinky)); n += c->ndinky * 8;
    tkey[n++] = c->nfat; memcpy(tkey + n, c->fat, c->nfat * sizeof(Fat)); n += c->nfat * 12;
    memcpy(tkey + n, c->tag_ad, 16); n += 4; memcpy(tkey + n, c->tag_strip, 16); n += 4;
    return n;
}
static void tm_drop(void)
{
    for (u32 i = 0; i < ntm; i++) { if (tm[i].ok) native_mesh_free(TM_ID0 + i); free(tm[i].key); free(tm[i].v6); free(tm[i].rgba); free(tm[i].r); }
    ntm = 0; memset(th_k, 0, sizeof th_k);
}

static int tm_build(const TieClass *c, TieMesh *m)
{
    u64 ta = ((u64)c->tag_ad[1] << 32) | c->tag_ad[0], ts = ((u64)c->tag_strip[1] << 32) | c->tag_strip[0];
    if ((ta & 0x7FFF) != 5 || ((ta >> 58) & 3) || ((ta >> 60) & 15) != 1 || (c->tag_ad[2] & 15) != 0xE) return 0;
    if (((ts >> 58) & 3) || ((ts >> 60) & 15) != 3 || (c->tag_strip[2] & 0xFFF) != 0x412) return 0;
    static s16 start[0x400]; static s16 vtx[0x400]; static s8 item[0x400]; static s16 itemk[0x400];
    memset(start, 0xFF, sizeof start); memset(vtx, 0xFF, sizeof vtx); memset(item, 0, sizeof item);
    for (int a = 0; a < c->nad; a++) {             /* the packet's items: 1 texture block, 2 strip */
        u32 d = c->ad_dest[a] & 0x3FF;
        if (d + 6 > 0x400) continue;
        for (u32 q = d; q < d + 6; q++) if (item[q]) return 0;
        item[d] = 1; itemk[d] = (s16)a; for (u32 q = d + 1; q < d + 6; q++) item[q] = 3;
    }
    for (int s = 0; s < c->nstrip; s++) {
        u32 d = c->strip_off[s] & 0x3FF;
        if (d + 1 + 3 * c->strip_count[s] > 0x400) continue;
        if (item[d]) return 0;
        item[d] = 2; itemk[d] = (s16)s;
    }
    int nv = c->ndinky + c->nfat;
    for (int j = 0; j < nv; j++) {                 /* each vertex's places, as put_vertex writes them */
        const s32 *st = j < c->ndinky ? c->dinky[j].st : c->fat[j - c->ndinky].st;
        u32 p1 = (u32)(j < c->ndinky ? c->dinky[j].p[3] : c->fat[j - c->ndinky].a[3]);
        if (st[2] != 4096) return 0;
        u32 pl[2] = { p1, (u32)st[3] }; int np = st[3] && st[3] != (s32)p1 ? 2 : 1;
        for (int k = 0; k < np; k++) {
            u32 sl = pl[k] & 0x3FF;
            if (sl + 3 > 0x400) continue;
            for (u32 q = sl; q < sl + 3; q++) {
                if (item[q]) return 0;
                if (start[q] >= 0 && (u32)start[q] != sl) return 0;
                start[q] = (s16)sl;
            }
            vtx[sl] = (s16)j;
        }
    }
    static float v6[384 * 6]; static u32 idx[0x400 * 3]; static TmRange rg[64];
    u32 ni = 0; int nr = 0, eop = 0;
    for (u32 q = 0; q < 0x400 && !eop;) {
        if (item[q] == 1) {
            if (nr == 64) return 0;
            rg[nr].first = ni; rg[nr].count = 0; rg[nr].blk = (u32)itemk[q]; rg[nr].strips = 0; nr++;
            q += 6;
        } else if (item[q] == 2) {
            int s = itemk[q]; u32 n = c->strip_count[s];
            if (!nr) return 0;
            u32 w[3] = { 0, 0, 0 };
            for (u32 k = 0; k < n; k++) {
                u32 sl = q + 1 + 3 * k;
                if (vtx[sl] < 0) return 0;
                w[0] = w[1]; w[1] = w[2]; w[2] = (u32)vtx[sl];
                if (k >= 2) { idx[ni++] = w[0]; idx[ni++] = w[1]; idx[ni++] = w[2]; rg[nr - 1].count += 3; }
            }
            rg[nr - 1].strips = 1;
            if (s == c->nstrip - 1) eop = 1;
            q += 1 + 3 * n;
        } else return 0;
    }
    if (!eop || !ni) return 0;
    for (int j = 0; j < nv; j++) {
        float *o = v6 + j * 6;
        if (j < c->ndinky) { const Dinky *v = &c->dinky[j]; o[0] = (float)v->p[0]; o[1] = (float)v->p[1]; o[2] = (float)v->p[2]; o[4] = itof12(v->st[0]); o[5] = itof12(v->st[1]); }
        else { const Fat *v = &c->fat[j - c->ndinky]; o[0] = (float)v->b[0]; o[1] = (float)v->b[1]; o[2] = (float)v->b[2]; o[4] = itof12(v->st[0]); o[5] = itof12(v->st[1]); }
        o[3] = 1.0f;
    }
    m->nv = (u32)nv;
    m->v6 = malloc(nv * 24); memcpy(m->v6, v6, nv * 24);
    m->rgba = calloc(nv, 4);
    m->r = malloc(nr * sizeof *rg); memcpy(m->r, rg, nr * sizeof *rg); m->nr = nr;
    m->pre = (int)((ts >> 46) & 1); m->prim = (ts >> 47) & 0x7FF;
    native_mesh_create(TM_ID0 + (u32)(m - tm), m->v6, m->rgba, m->nv, idx, ni);
    return 1;
}

static void tm_class(void)
{
    tm_cur = -1;
    if (tm_on < 0) { const char *e = getenv("UYA_NTIEMESH"), *n = getenv("UYA_NATIVE"); tm_on = !(e && !atoi(e)) && !(n && !atoi(n)); }
    if (!tm_on || !cls.valid) return;
    u32 nk = make_key(&cls);
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < nk; i++) h = (h ^ tkey[i]) * 1099511628211ull;
    if (!h) h = 1;
    u32 s = (u32)(h >> 51) & (TH - 1);
    while (th_k[s]) {
        if (th_k[s] == h) { TieMesh *m = &tm[th_v[s]]; if (m->nkey == nk && !memcmp(m->key, tkey, nk * 4)) { tm_cur = th_v[s]; return; } }
        s = (s + 1) & (TH - 1);
    }
    if (!tm) tm = calloc(TM_N, sizeof *tm);
    if (ntm == TM_N) { tm_drop(); tm_class(); return; }
    TieMesh *m = &tm[ntm];
    memset(m, 0, sizeof *m);
    m->key = malloc(nk * 4); memcpy(m->key, tkey, nk * 4); m->nkey = nk; m->h = h;
    m->ok = tm_build(&cls, m);
    if (!m->ok) { static int n; if (n++ < 8) uya_log("native tie meshes: a class goes as packets (%d strips, %d + %d vertices)", cls.nstrip, cls.ndinky, cls.nfat); }
    th_k[s] = h; th_v[s] = (int)ntm;
    tm_cur = (int)ntm++;
}

/* an instance at VU memory inst (rows 0..5, colour indexes at inst + 6) of class c (mesh mi);
 * sc, of: the screen scale and offset after the projection (clip: 0 none; 1 the frustum) */
static int tm_draw(TieClass *c, int mi, const u8 *mem, u32 inst, const float sc[3], const float of[3], int clip)
{
    if (mi < 0 || !tm[mi].ok) { tm_n_pk++; return 0; }
    TieMesh *m = &tm[mi]; int tm_cur = mi;
    float R[6][4];
    for (int k = 0; k < 6; k++) memcpy(R[k], Q(mem, inst + k), 16);
    s32 fog = ftoi4(R[5][3]);
    if (fog & 0x8000) { if (clip) { static int t; if (t++ < 3) uya_log("native tie clip meshes: fog %08X", (u32)fog); } if (++tm_n_pk % 10000 == 1) uya_log("native tie meshes: an instance as a packet (fog %08X)", (u32)fog); return 0; }      /* (the ADC bit: the packet's vertices would not draw) */
    static float v6[384 * 6]; static u8 rgba[384 * 4];
    const u32 *idx = Q(mem, inst + 6);
    for (int j = 0; j < c->ndinky; j++) { const u32 *col = Q(mem, 838 + idx[j]); for (int k = 0; k < 4; k++) rgba[j * 4 + k] = (u8)col[k]; }
    u32 fq = (c->ndinky + 3) / 4;
    int moved = 0;
    if (c->nfat) memcpy(v6, m->v6, m->nv * 24);
    for (int j = 0; j < c->nfat; j++) {
        const Fat *v = &c->fat[j];
        float *o = v6 + (c->ndinky + j) * 6;
        o[0] = fc(fc((float)v->a[0] * R[4][0]) + (float)v->b[0]);
        o[1] = fc(fc((float)v->a[1] * R[4][0]) + (float)v->b[1]);
        o[2] = fc(fc((float)v->a[2] * R[4][0]) + (float)v->b[2]);
        const u32 *iq = Q(mem, inst + 6 + fq + j);
        const float *c0 = (const float *)Q(mem, 838 + iq[0]), *c1 = (const float *)Q(mem, 838 + iq[1]), *c2 = (const float *)Q(mem, 838 + iq[2]);
        for (int k = 0; k < 4; k++) {
            float mm = fc(fc(c2[k] * 0.5f) + fc(c1[k] * 0.5f));
            float f = fc(fc(c0[k] * R[4][2]) + fc(mm * R[4][3]));
            u32 b; memcpy(&b, &f, 4); rgba[(c->ndinky + j) * 4 + k] = (u8)b;
        }
    }
    if (c->nfat && memcmp(v6, m->v6, m->nv * 24)) moved = 1;
    if (moved) { memcpy(m->v6, v6, m->nv * 24); memcpy(m->rgba, rgba, m->nv * 4); native_mesh_update(TM_ID0 + (u32)tm_cur, m->v6, m->rgba, m->nv); tm_n_up++; }
    else if (memcmp(rgba, m->rgba, m->nv * 4)) { memcpy(m->rgba, rgba, m->nv * 4); native_mesh_colors(TM_ID0 + (u32)tm_cur, m->rgba, m->nv); tm_n_up++; }
    float xf[32];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) xf[r * 4 + k] = k < 3 ? R[r][k] * sc[k] : R[r][k];
    float F = (float)((fog >> 4) & 0xFF);
    xf[16] = 1.0f; xf[17] = F; xf[18] = F; xf[19] = 0.0f;
    xf[20] = of[0]; xf[21] = of[1]; xf[22] = of[2]; xf[23] = -3.4028235e38f;   /* (no near limit on the CPU: the main program's instances are inside the view) */
    xf[24] = xf[25] = xf[26] = xf[27] = 0.0f;
    xf[28] = 0.0f; xf[29] = 0.0f; xf[30] = 1e-30f; xf[31] = 1.0f;   /* (the GPU: no near or far cut but w > 0) */
    if (clip) { if (sc[2] == 0.0f) { static int t; if (t++ < 3) uya_log("native tie clip meshes: screen scale %g %g %g", sc[0], sc[1], sc[2]); return 0; } xf[30] = 1.0f / sc[2]; }   /* (the clip program's box: |z| <= w; x y: the screen's edges) */
    extern const char *uya_gs_src; uya_gs_src = clip ? "native tie clip" : "native tie";
    for (int k = 0; k < m->nr; k++) {
        const TmRange *r = &m->r[k];
        u32 a[6]; u64 v[6]; int n = 0;
        for (int j = 0; j < 5; j++) { const u32 *q = c->ad[r->blk][j]; a[n] = q[2] & 0xFF; v[n++] = ((u64)q[1] << 32) | q[0]; }
        if (r->strips && m->pre) { a[n] = 0x00; v[n++] = m->prim; }
        native_regs(a, v, n);
        if (r->count) native_mesh_draw(TM_ID0 + (u32)tm_cur, r->first, r->count, xf);
    }
    if (++tm_n_mesh % 200000 == 0) { u32 bad = 0; for (u32 i = 0; i < ntm; i++) bad += !tm[i].ok; uya_log("native tie meshes: %u classes (%u as packets), instances %llu as meshes, %llu as packets, %llu mesh updates", ntm, bad, tm_n_mesh, tm_n_pk, tm_n_up); }
    return 1;
}

int native_tie_mesh(const u8 *mem, u32 top)
{
    static const float one[3] = { 1, 1, 1 }, zero[3] = { 0, 0, 0 };
    return tm_draw(&cls, tm_cur, mem, 0xC6 + top, one, zero, 0);
}

/* the tie clip program (224979): its class packets are the main program's; decoded here before
 * the program converts them in place, into a class of their own (the main program's is kept) */
static TieClass ccls; static int tm_clip_cur = -1;
void native_tie_clip_class(const u8 *mem)
{
    static TieClass keep; int keep_cur = tm_cur;
    keep = cls;
    native_tie_setup(mem);
    ccls = cls; tm_clip_cur = tm_cur;
    cls = keep; tm_cur = keep_cur;
}
int native_tie_clip_mesh(const u8 *mem, u32 inst)
{
    if (!ccls.valid || tm_clip_cur < 0 || !tm[tm_clip_cur].ok) { static int t; if (t++ < 3) uya_log("native tie clip meshes: no class mesh (valid %d, mesh %d)", ccls.valid, tm_clip_cur); return 0; }
    float sc[4], of[4];
    memcpy(sc, Q(mem, 974), 16); memcpy(of, Q(mem, 973), 16);
    return tm_draw(&ccls, tm_clip_cur, mem, inst, sc, of, 1);
}
