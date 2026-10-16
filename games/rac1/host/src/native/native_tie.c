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
