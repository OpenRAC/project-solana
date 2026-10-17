/* native_dlist.c: the native renderers' draws as a native draw list.
 *
 * The native code (native_tie.c, native_shrub.c, native_tfrag.c, native_moby.c)
 * builds its draws in VU1 data memory exactly as the PS2's microprograms did
 * (later runs read what it leaves there), then hands them on here. Each draw
 * becomes a native draw list for the renderer (host/src/gs/gs.c, gs_dlist_):
 * register writes, and strips of plain vertices (position, colour, texture
 * coordinates, whether the vertex closes a triangle), with the same meaning as
 * the GIF data, so the same primitives are drawn without decoding PS2 GIF
 * packets. Data of another shape (none of the four renderers sends any) goes
 * on as it is. UYA_DLIST=0 sends everything as GIF packets (to compare). */
#include "native_dlist.h"
#include <stdlib.h>
#include <string.h>

void uya_gif_packet(const u32 *data, u32 qwc);
void uya_gs_dlist(const u32 *d, u32 qwc);

/* the list for a PATH1 packet (as XGKICK sends it: from a tag, to the EOP);
 * returns its qwords, or 0 if the packet has data of another shape */
static u32 convert(const u32 *pk, u32 qwc, u32 *out, u32 cap)
{
    u32 q = 0, o = 0;
#define PUT4(a, b, c, d) do { if (o + 1 > cap) return 0; out[o * 4] = (a); out[o * 4 + 1] = (b); out[o * 4 + 2] = (c); out[o * 4 + 3] = (d); o++; } while (0)
    while (q < qwc) {
        const u32 *t = pk + q * 4;
        u64 lo = ((u64)t[1] << 32) | t[0], hi = ((u64)t[3] << 32) | t[2];
        u32 nloop = (u32)(lo & 0x7FFF), eop = (u32)(lo >> 15) & 1, pre = (u32)(lo >> 46) & 1, primv = (u32)(lo >> 47) & 0x7FF;
        u32 flg = (u32)(lo >> 58) & 3, nreg = (u32)(lo >> 60) & 15;
        if (!nreg) nreg = 16;
        q++;
        if (pre) PUT4(1, 0x00, primv, 0);
        PUT4(3, 0, 0, 0);
        if (nloop) {
            if (flg != 0) return 0;
            if (q + nloop * nreg > qwc) return 0;
            u32 r[16];
            for (u32 i = 0; i < nreg; i++) r[i] = (u32)(hi >> (i * 4)) & 15;
            int all_ad = 1;
            for (u32 i = 0; i < nreg; i++) if (r[i] != 0xE) all_ad = 0;
            if (all_ad) {
                for (u32 i = 0; i < nloop * nreg; i++, q++) {
                    const u32 *v = pk + q * 4;
                    PUT4(1, v[2] & 0xFF, v[0], v[1]);
                }
            } else if (nreg == 3 && (r[2] == 0x4 || r[2] == 0x5) &&
                       ((r[0] == 0x2 && r[1] == 0x1) || (r[0] == 0x1 && r[1] == 0x2))) {
                /* ST and RGBAQ (either order), then XYZF2 or XYZ2: a strip of vertices */
                if (o + 1 + 2 * nloop > cap) return 0;
                u32 *h = out + o * 4;
                h[0] = 2; h[1] = nloop; h[2] = h[3] = 0;
                u32 *w = h + 4;
                u32 ist = r[0] == 0x2 ? 0 : 1, irg = 1 - ist;
                for (u32 l = 0; l < nloop; l++, q += 3, w += 8) {
                    const u32 *s_ = pk + (q + ist) * 4, *c = pk + (q + irg) * 4, *x = pk + (q + 2) * 4;
                    w[0] = (x[0] & 0xFFFF) | ((x[1] & 0xFFFF) << 16);
                    w[2] = (c[0] & 255) | ((c[1] & 255) << 8) | ((c[2] & 255) << 16) | ((c[3] & 255) << 24);
                    if (r[2] == 0x4) { w[1] = (x[2] >> 4) & 0xFFFFFF; w[3] = ((x[3] >> 4) & 0xFF) | (((x[3] >> 15) & 1) << 8); }
                    else { w[1] = x[2]; w[3] = (((x[3] >> 15) & 1) << 8) | 0x200; }
                    w[4] = s_[0]; w[5] = s_[1]; w[6] = s_[2]; w[7] = 0;
                }
                o += 1 + 2 * nloop;
            } else return 0;
        }
        if (eop) break;
    }
    return o;
#undef PUT4
}

void native_kick(const u32 *pk, u32 qwc)
{
    static int on = -1;
    static u32 *buf; static u32 bufq;
    if (on < 0) { const char *e = getenv("UYA_DLIST"); on = !e || atoi(e) != 0; }
    if (!on || !qwc) { uya_gif_packet(pk, qwc); return; }
    u32 cap = qwc * 2 + 16;
    if (cap > bufq) { free(buf); buf = malloc((size_t)cap * 16); bufq = cap; }
    u32 n = buf ? convert(pk, qwc, buf, cap) : 0;
    if (n) uya_gs_dlist(buf, n);
    else {
        static int told;
        if (!told++) uya_log("native draw list: a packet of another shape went as GIF data");
        uya_gif_packet(pk, qwc);
    }
}

/* ---- native meshes ---- */
static u32 *mbuf; static size_t mcap;
static u32 *mroom(size_t qw)
{
    if (qw * 4 > mcap) { free(mbuf); mcap = qw * 4 + 1024; mbuf = malloc(mcap * 4); }
    memset(mbuf, 0, qw * 16);
    return mbuf;
}
void native_mesh_create(u32 id, const float *v6, const u8 *rgba, u32 nv, const u32 *idx, u32 ni)
{
    u32 q6 = (nv * 6 + 3) / 4, qc = (nv + 3) / 4, qi = (ni + 3) / 4, n = 1 + q6 + qc + qi;
    u32 *b = mroom(n);
    b[0] = 4; b[1] = id; b[2] = nv; b[3] = ni;
    memcpy(b + 4, v6, (size_t)nv * 24);
    memcpy(b + 4 + q6 * 4, rgba, (size_t)nv * 4);
    memcpy(b + 4 + (q6 + qc) * 4, idx, (size_t)ni * 4);
    uya_gs_dlist(b, n);
}
void native_mesh_colors(u32 id, const u8 *rgba, u32 nv)
{
    u32 n = 1 + (nv + 3) / 4;
    u32 *b = mroom(n);
    b[0] = 5; b[1] = id; b[2] = nv;
    memcpy(b + 4, rgba, (size_t)nv * 4);
    uya_gs_dlist(b, n);
}
void native_mesh_update(u32 id, const float *v6, const u8 *rgba, u32 nv)
{
    u32 q6 = (nv * 6 + 3) / 4, n = 1 + q6 + (nv + 3) / 4;
    u32 *b = mroom(n);
    b[0] = 8; b[1] = id; b[2] = nv;
    memcpy(b + 4, v6, (size_t)nv * 24);
    memcpy(b + 4 + q6 * 4, rgba, (size_t)nv * 4);
    uya_gs_dlist(b, n);
}
void native_mesh_update_idx(u32 id, const float *v6, const u8 *rgba, u32 nv, const u32 *idx, u32 ni)
{
    u32 q6 = (nv * 6 + 3) / 4, qc = (nv + 3) / 4, qi = (ni + 3) / 4, n = 1 + q6 + qc + qi;
    u32 *b = mroom(n);
    b[0] = 9; b[1] = id; b[2] = nv; b[3] = ni;
    memcpy(b + 4, v6, (size_t)nv * 24);
    memcpy(b + 4 + q6 * 4, rgba, (size_t)nv * 4);
    memcpy(b + 4 + (q6 + qc) * 4, idx, (size_t)ni * 4);
    uya_gs_dlist(b, n);
}
void native_mesh_free(u32 id)
{
    u32 b[4] = { 6, id, 0, 0 };
    uya_gs_dlist(b, 1);
}
void native_regs(const u32 *addr, const u64 *val, int n)
{
    u32 *b = mroom((size_t)n);
    for (int i = 0; i < n; i++) { b[i * 4] = 1; b[i * 4 + 1] = addr[i]; b[i * 4 + 2] = (u32)val[i]; b[i * 4 + 3] = (u32)(val[i] >> 32); }
    uya_gs_dlist(b, (u32)n);
}
void native_mesh_draw(u32 id, u32 first, u32 count, const float xf[32])
{
    u32 b[36] = { 7, id, first, count };
    memcpy(b + 4, xf, 32 * 4);
    uya_gs_dlist(b, 9);
}
