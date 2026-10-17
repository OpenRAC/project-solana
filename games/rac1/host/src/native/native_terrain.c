/* Terrain (tfrags: the level's terrain chunks) drawn from the game's own data as native meshes.
 *
 * Every frame the game's TfragProc decides which tfrags are seen and at which level of detail,
 * and writes, for each, a draw into the VU1 chain: its packet (the level's data, REFs), its
 * colours, and an MSCAL whose start is the mode. On the PS2 the VIF unpacks the packet into
 * VU1 memory and the tfrag microprogram turns it into GS strips. The modes:
 *   6     the far geometry (LOD2), as it is
 *   0x14  the full geometry (LOD0), as it is
 *   8, A  the middle geometry (LOD1) morphing toward LOD2 (8: with collapses)
 *   E, 10 LOD0 morphing toward LOD1 (E: also LOD1 toward LOD2, with collapses)
 *   2     the clip pass (tfrags crossing the screen's edges, another program)
 *
 * Here the packets are read once, when a level's tfrag table appears: each tfrag's geometry at
 * each level becomes a mesh (vertices, texture coordinates, triangle lists split where the
 * texture changes) kept by the renderer. When the chain walker (host/src/sdk/dma.c) reaches a
 * tfrag's draw, the draw is taken out of the chain and the mesh is drawn instead, with the
 * game's material (its texture blocks, patched every frame by the game, read from the level
 * data), colours (relit by the game, read from the level data) and the frame's transform and
 * constants:
 *   - modes 6 and 0x14: static meshes in the level's space, transformed by the renderer;
 *   - the morphing modes: the program's vertex work (transform, morphs, collapses: native_tfrag.c)
 *     is done here on the tfrag's packet as the level data holds it, and the mesh gets the
 *     results (clip-space vertices, colours), drawn with an identity transform.
 *   - the clip pass, with the GPU renderer: the static LOD0 mesh, cut by the renderer at the
 *     planes the clip program cuts at (with the software renderer it goes on to the VU1;
 *     UYA_NTCLIP=1 draws it as a mesh there too, without the near cut: for tests).
 * UYA_NTERRAIN=0: every draw goes to the VU1.
 *
 * The packet layout and the program's arithmetic are those native_tfrag.c documents; the
 * mesh's vertex shader (host/src/gs/gs_gl.c, nvs_src) and the GS layer's CPU form of it
 * (gs_mesh_vertex) do the program's per-vertex projection. In the software renderer the
 * result is the program's, bit for bit. */
#include "native_terrain.h"
#include "uya_runtime.h"
#include "native_dlist.h"
#include "native_tfrag.h"
#include "rac1_levels.h"
const rac1_level_t *rac1_current_level(void);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma STDC FP_CONTRACT OFF

void uya_dma_vif1_replay(const u32 *tag, int tte);

#define EE(a) ((const u8 *)(uintptr_t)(a))
static u32 rd32(u32 a) { u32 v; memcpy(&v, EE(a), 4); return v; }
static u16 rd16(u32 a) { u16 v; memcpy(&v, EE(a), 2); return v; }
static u8 rd8(u32 a) { return *EE(a); }
#define VU1MEM ((const u8 *)(uintptr_t)0x1100C000u)

typedef struct { u32 cl, wl, mode, row[4], maxq; } VifSt;
/* a tfrag's meshes: LOD2 (mode 6), LOD1 (modes 8, A), LOD0 (0x14; E, 10) */
enum { L2, L1, L0, NL };
typedef struct { u32 first, count, blk; } Range;
typedef struct {
    int ok, dirty;
    u32 nv; u16 *cidx;          /* GL vertices; the colour (vertex number) of each, for the static meshes */
    u16 *ent;                   /* the index entry (packet qword) of each, for the morphing modes */
    Range *r; int nr;
    u32 *snap; u32 nq;          /* the packet as the VIF leaves it in VU memory (qwords 0..nq-1), for the morphing modes */
    VifSt vs;                   /* and the VIF's state after it (the colours' unpack depends on it) */
} Lod;
typedef struct { u32 hdr, data; Lod l[NL]; u8 *col; u32 ncol; } Tf;
/* mesh ids: 4 a tfrag: LOD2, LOD1 (morphing), LOD0, LOD0 morphing */
static u32 mesh_id(u32 i, int L, int dyn) { return i * 4 + (L == L2 ? 0 : L == L1 ? 1 : dyn ? 3 : 2); }

static int on = -1;
static unsigned long long n_mesh[32], n_vu[32], n_chains;   /* (statistics by mode: logged now and then) */
static Tf *tf; static u32 ntf;
static u32 sig_tbl, sig_cnt, sig_sum; static const void *sig_lvl;
static u32 t_addr;                          /* the level's tag templates t4..t7 (their VIF codes) */

/* packet start address -> tfrag * 2 + (0: the LOD2 packet, 1: the common packet) */
#define HN 8192
static u32 hkey[HN], hval[HN];
static void hput(u32 k, u32 v) { u32 h = (k * 2654435761u) >> 19; while (hkey[h]) h = (h + 1) & (HN - 1); hkey[h] = k; hval[h] = v; }
static int hget(u32 k, u32 *v) { u32 h = (k * 2654435761u) >> 19; while (hkey[h]) { if (hkey[h] == k) { *v = hval[h]; return 1; } h = (h + 1) & (HN - 1); } return 0; }

/* ---- the level data as the VIF would leave it in VU memory (at TOP 0) ----
 * (the packets' own VIF codes: positions, texture coordinates and colours in the VU's layout) */
static int vif_decode(VifSt *s, const u8 *buf, u32 nbytes, u32 *mem)
{
    static const u8 SZ[16] = { 4, 2, 1, 0, 8, 4, 2, 0, 12, 6, 3, 0, 16, 8, 4, 2 };
    u32 o = 0;
    while (o + 4 <= nbytes) {
        u32 c; memcpy(&c, buf + o, 4); o += 4;
        u32 cmd = (c >> 24) & 0x7F, imm = c & 0xFFFF, num = (c >> 16) & 0xFF;
        if (cmd >= 0x60) {
            u32 k = cmd & 0xF, n = num ? num : 256, usn = (imm >> 14) & 1, addr = imm & 0x3FF;
            if (cmd & 0x10) return -1;               /* (masked writes: not in tfrag packets) */
            if (!SZ[k] || k == 0xF || s->cl < s->wl || !s->wl) return -1;
            u32 vn = (k >> 2) & 3, vl = k & 3, nc = vn + 1;
            if (o + SZ[k] * n > nbytes) return -1;
            for (u32 i = 0; i < n; i++) {
                u32 qa = (addr + (i / s->wl) * s->cl + i % s->wl) & 0x3FF;
                const u8 *p = buf + o + i * SZ[k];
                u32 v[4];
                for (u32 j = 0; j < nc; j++) {
                    if (vl == 0) memcpy(&v[j], p + 4 * j, 4);
                    else if (vl == 1) { u16 h; memcpy(&h, p + 2 * j, 2); v[j] = usn ? h : (u32)(s32)(s16)h; }
                    else v[j] = usn ? p[j] : (u32)(s32)(s8)p[j];
                }
                if (nc == 1) v[1] = v[2] = v[3] = v[0];
                u32 m = nc == 1 ? 4 : nc;
                for (u32 j = 0; j < m; j++) mem[qa * 4 + j] = s->mode == 1 ? v[j] + s->row[j] : v[j];
                if (qa + 1 > s->maxq) s->maxq = qa + 1;
            }
            o += (SZ[k] * n + 3) & ~3u;
        } else switch (cmd) {
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: break;
        case 0x01: s->cl = imm & 0xFF; s->wl = imm >> 8; break;
        case 0x05:
            s->mode = imm & 3;
            if (s->mode > 1) return -1;
            break;
        case 0x30: if (o + 16 > nbytes) return -1; memcpy(s->row, buf + o, 16); o += 16; break;
        case 0x31: o += 16; break;
        case 0x20: o += 4; break;
        default: return -1;                          /* (MSCAL, MPG, DIRECT...: not in tfrag packets) */
        }
    }
    return 0;
}
/* a REF of the chain as TfragProc writes it: template k's VIF codes (num and address patched as it
 * does, when given), then the data */
static int vif_ref(VifSt *s, int k, int num, int addr, u32 data, u32 qwc, u32 *mem)
{
    static u8 buf[8 + 256 * 16];                     /* (the tag's codes may start an UNPACK of the data) */
    if (qwc > 256) return -1;
    memcpy(buf, EE(t_addr + 16 * (k - 4) + 8), 8);
    if (num >= 0) buf[6] = (u8)num;
    if (addr >= 0) buf[4] = (u8)addr;
    memcpy(buf + 8, EE(data), qwc * 16);
    return vif_decode(s, buf, 8 + qwc * 16, mem);
}
/* tfrag t's packet for level L, then (colours: 1) its colours */
static int unpack(const Tf *t, int L, int colours, u32 *mem, VifSt *s)
{
    u32 h = t->hdr, d = t->data;
    memset(s, 0, sizeof *s); s->cl = s->wl = 4;
    if (L == L2) {
        if (vif_ref(s, 4, -1, -1, d + rd16(h + 0x14), rd8(h + 0x21), mem)) return -1;
        if (colours && vif_ref(s, 5, (rd8(h + 0x24) >> 2) << 2, rd8(h + 0x2A), d + rd16(h + 0x1E), rd8(h + 0x24) >> 2, mem)) return -1;
    } else if (L == L1) {
        if (vif_ref(s, 4, -1, -1, d + rd16(h + 0x16), rd8(h + 0x22), mem)) return -1;
        if (colours && vif_ref(s, 6, (rd8(h + 0x25) >> 2) << 2, rd8(h + 0x2A), d + rd16(h + 0x1E), rd8(h + 0x25) >> 2, mem)) return -1;
    } else {
        if (vif_ref(s, 4, -1, -1, d + rd16(h + 0x16), rd8(h + 0x20), mem)) return -1;
        if (vif_ref(s, 4, -1, -1, d + rd16(h + 0x1A), rd8(h + 0x23), mem)) return -1;
        if (colours && vif_ref(s, 5, (rd8(h + 0x26) >> 2) << 2, rd8(h + 0x2A), d + rd16(h + 0x1E), rd8(h + 0x26) >> 2, mem)) return -1;
    }
    return 0;
}

/* ---- a tfrag's geometry at one level of detail, as the program would draw it ---- */
static u32 W16(const u32 *m, u32 q, int k) { return m[(q & 0x3FF) * 4 + k] & 0xFFFF; }

static int build_lod(Tf *t, int L, u32 i)
{
    static u32 mem[1024 * 4];
    static float v6[1024 * 6]; static u8 rgba[1024 * 4]; static u16 cidx[1024], ent[1024]; static s16 map[1024];
    static u32 idx[8192]; static Range rg[256];
    u32 h = t->hdr, d = t->data;
    VifSt vs;
    memset(mem, 0, sizeof mem);
    if (unpack(t, L, 0, mem, &vs)) return 0;
    u32 nq = vs.maxq;
    u32 tbl = W16(mem, 4, 2), blocks = W16(mem, 4, 3), il = W16(mem, 3, 1), arr = W16(mem, 1, 2);
    u32 ntex = rd8(h + 0x28), adgif = d + rd16(h + 0x1C);
    memset(map, 0xFF, sizeof map);
    u32 nv = 0, ni = 0, iw = 0; int nr = 0;
    s16 code = (s16)W16(mem, tbl, 0);
    u32 count = (u16)(code + 128), blk = W16(mem, tbl, 2);
    int newtex = 1;
    for (int guard = 0; guard < 1024; guard++) {
        if (newtex) {
            if (blk % 5 || blk / 5 >= ntex) return 0;
            /* the block the program reads is the level data's (patched there every frame) */
            if (memcmp(&mem[((blocks + blk) & 0x3FF) * 4], EE(adgif + blk * 16), 5 * 16)) return 0;
            if (!nr || rg[nr - 1].blk != blk) {
                if (nr == 256) return 0;
                rg[nr].first = ni; rg[nr].count = 0; rg[nr].blk = blk; nr++;
            }
        }
        u32 strip[3] = { 0, 0, 0 };
        if (count > 1024) return 0;
        for (u32 n = 0; n < count; n++, iw++) {
            u32 e = W16(mem, il + iw / 4, iw % 4) & 0x3FF;
            if (map[e] < 0) {
                const u32 *E = &mem[e * 4];
                u32 va = E[3] & 0x3FF;
                if (va < arr || (va - arr) & 1 || nv == 1024) return 0;
                float s_, t_; memcpy(&s_, &E[0], 4); memcpy(&t_, &E[1], 4);
                v6[nv * 6 + 0] = (float)(s32)mem[va * 4 + 0];
                v6[nv * 6 + 1] = (float)(s32)mem[va * 4 + 1];
                v6[nv * 6 + 2] = (float)(s32)mem[va * 4 + 2];
                v6[nv * 6 + 3] = 1.0f;
                v6[nv * 6 + 4] = s_ - 2048.0f;        /* (the bias, 657.z: checked when drawing) */
                v6[nv * 6 + 5] = t_ - 2048.0f;
                cidx[nv] = (u16)((va - arr) / 2); ent[nv] = (u16)e;
                map[e] = (s16)nv++;
            }
            strip[0] = strip[1]; strip[1] = strip[2]; strip[2] = (u32)map[e];
            if (n >= 2) {
                if (ni + 3 > 8192) return 0;
                idx[ni++] = strip[0]; idx[ni++] = strip[1]; idx[ni++] = strip[2];
                rg[nr - 1].count += 3;
            }
        }
        code = (s16)W16(mem, ++tbl, 0);
        newtex = 0;
        if (code > 0) { count = (u16)code; continue; }
        s16 y = (s16)W16(mem, tbl, 1), z = (s16)W16(mem, tbl, 2);
        if (code == 0) break;
        if (y < 0) { if (z >= 0) { newtex = 1; blk = (u16)z; } }
        else { newtex = 1; blk = (u16)z; }
        count = (u16)(code + 128);
    }
    if (code != 0 || !ni) return 0;
    Lod *l = &t->l[L];
    if (L != L1) {                                   /* (LOD1 is drawn morphing only) */
        u32 ncol = 0;
        for (u32 k = 0; k < nv; k++) if (cidx[k] + 1u > ncol) ncol = cidx[k] + 1u;
        if (ncol > t->ncol) t->ncol = ncol;
        const u8 *col = EE(d + rd16(h + 0x1E));
        for (u32 k = 0; k < nv; k++) memcpy(rgba + k * 4, col + cidx[k] * 4, 4);
        native_mesh_create(mesh_id(i, L, 0), v6, rgba, nv, idx, ni);
    }
    if (L != L2) {                                   /* the morphing modes' mesh: its vertices come with each draw */
        memset(rgba, 0, nv * 4);
        native_mesh_create(mesh_id(i, L, 1), v6, rgba, nv, idx, ni);
        l->snap = malloc(nq * 16); memcpy(l->snap, mem, nq * 16); l->nq = nq; l->vs = vs;
    }
    l->cidx = malloc(nv * 2); memcpy(l->cidx, cidx, nv * 2);
    l->ent = malloc(nv * 2); memcpy(l->ent, ent, nv * 2);
    l->r = malloc(nr * sizeof *l->r); memcpy(l->r, rg, nr * sizeof *rg);
    l->nr = nr; l->nv = nv; l->ok = 1; l->dirty = 0;
    return 1;
}

static void drop_all(void)
{
    for (u32 i = 0; i < ntf; i++) {
        for (int L = 0; L < NL; L++) {
            Lod *l = &tf[i].l[L];
            if (l->ok) { if (L != L1) native_mesh_free(mesh_id(i, L, 0)); if (L != L2) native_mesh_free(mesh_id(i, L, 1)); }
            free(l->cidx); free(l->ent); free(l->r); free(l->snap);
        }
        free(tf[i].col);
    }
    free(tf); tf = 0; ntf = 0;
    memset(hkey, 0, sizeof hkey);
}

/* the level's table: where it is (the small-data block of levels 0, 5, 14 and 16 is 0x40
 * higher), and a signature of it, to see a new level or a relocation */
void native_terrain_chain(void)
{
    if (on < 0) { const char *e = getenv("UYA_NTERRAIN"), *n = getenv("UYA_NATIVE"); on = !(e && !atoi(e)) && !(n && !atoi(n)); }
    if (!on) return;
    if (++n_chains % 500 == 0) {
        char line[512]; int o = 0;
        for (int m = 0; m < 32 && o < 440; m++)
            if (n_mesh[m] || n_vu[m]) o += snprintf(line + o, sizeof line - o, " %X:%llu/%llu", m, n_mesh[m], n_mesh[m] + n_vu[m]);
        line[o] = 0;
        uya_log("native terrain: draws as meshes by mode:%s", line);
    }
    const rac1_level_t *lv = rac1_current_level();
    if (!lv) { if (tf) drop_all(); return; }
    int hi = lv->id == 0 || lv->id == 5 || lv->id == 14 || lv->id == 16;
    u32 tb = hi ? 0x16100Cu : 0x160FCCu;
    u32 tbl = rd32(tb), cnt = rd32(tb + 4), sum = 0;
    int valid = cnt && cnt <= 4096 && tbl >= 0x100000u && tbl + cnt * 64 <= 0x2000000u && !(tbl & 15);
    if (valid)
        for (u32 i = 0; i < cnt; i++) {
            u32 d = rd32(tbl + 64 * i + 0x10);
            if (d < 0x100000u || d >= 0x2000000u) { valid = 0; break; }
            sum = sum * 31 + d;
        }
    if (!valid) { if (tf) drop_all(); sig_lvl = 0; return; }
    if (lv == sig_lvl && tbl == sig_tbl && cnt == sig_cnt && sum == sig_sum) return;
    drop_all();
    sig_lvl = lv; sig_tbl = tbl; sig_cnt = cnt; sig_sum = sum;
    t_addr = hi ? 0x160B00u : 0x160AC0u;
    tf = calloc(cnt, sizeof *tf); ntf = cnt;
    u32 nok = 0, nverts = 0;
    for (u32 i = 0; i < cnt; i++) {
        Tf *t = &tf[i];
        t->hdr = tbl + 64 * i; t->data = rd32(t->hdr + 0x10);
        for (int L = 0; L < NL; L++) if (build_lod(t, L, i)) { nok++; nverts += t->l[L].nv; }
        if (t->ncol) { t->col = malloc(t->ncol * 4); memcpy(t->col, EE(t->data + rd16(t->hdr + 0x1E)), t->ncol * 4); }
        hput(t->data + rd16(t->hdr + 0x14), i * 2);
        hput(t->data + rd16(t->hdr + 0x16), i * 2 + 1);
    }
    uya_log("native terrain: level %d, %u tfrags, %u of %u levels of detail made meshes (%u vertices)", lv->id, cnt, nok, cnt * 3, nverts);
}

/* ---- a tfrag's draw in the chain ---- */
static struct { int open, tte, n; u32 tf, kind; const u32 *tag[4]; } g;

void native_terrain_flush(void)
{
    if (!g.open) return;
    g.open = 0;
    for (int i = 0; i < g.n; i++) uya_dma_vif1_replay(g.tag[i], g.tte);
}

static void fl4(u32 q, float f[4]) { memcpy(f, VU1MEM + (q & 0x3FF) * 16, 16); }

/* VU arithmetic for the colours (native_tfrag.c's) */
static float fc(float x) { u32 b; memcpy(&b, &x, 4); if ((b & 0x7F800000u) == 0x7F800000u) { b = (b & 0x80000000u) | 0x7F7FFFFFu; memcpy(&x, &b, 4); } return x; }
static s32 ftoi(float x)
{
    if (x >= 2147483647.0f) return 0x7FFFFFFF;
    if (x <= -2147483648.0f) return (s32)0x80000000u;
    if (x != x) return 0;
    return (s32)x;
}

/* the morphing modes: the program's vertex work on the packet, its results to mesh id */
static int morph_vertices(Tf *t, int L, u32 mode, u32 id)
{
    static u32 mem[1024 * 4];
    static float v6[1024 * 6]; static u8 rgba[1024 * 4];
    Lod *l = &t->l[L];
    memcpy(mem, l->snap, l->nq * 16);
    memset(mem + l->nq * 4, 0, (1024 - l->nq) * 16);
    VifSt vs = l->vs;
    u32 h = t->hdr, col = t->data + rd16(h + 0x1E);
    int k = L == L1 ? 6 : 5; u32 n = rd8(h + (L == L1 ? 0x25 : 0x26)) >> 2;
    if (vif_ref(&vs, k, n << 2, rd8(h + 0x2A), col, n, mem)) {
        static int told; if (!told++) { u32 c[2]; memcpy(c, EE(t_addr + 16 * (k - 4) + 8), 8); uya_log("native terrain: the colours' unpack failed (template %d: %08X %08X, cl %u wl %u)", k, c[0], c[1], vs.cl, vs.wl); }
        return 0;
    }
    memcpy(mem + 5 * 4, VU1MEM + 5 * 16, 4 * 16);           /* the transform (DrawTfrag's) */
    memcpy(mem + 656 * 4, VU1MEM + 656 * 16, 14 * 16);      /* the constants (the chain's template) */
    if (native_tfrag_lod_vertices(mode * 8, (u8 *)mem, 0)) { static int told; if (!told++) uya_log("native terrain: no LOD start 0x%X", mode * 8); return 0; }
    float cs[4]; native_tfrag_cscale(cs);
    for (u32 v = 0; v < l->nv; v++) {
        const u32 *E = &mem[l->ent[v] * 4];
        u32 va = E[3] & 0x3FF;
        float p[4], c[4], s_, t_;
        memcpy(p, &mem[va * 4], 16); memcpy(c, &mem[((va + 1) & 0x3FF) * 4], 16);
        memcpy(&s_, &E[0], 4); memcpy(&t_, &E[1], 4);
        float *o = v6 + v * 6;
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; o[4] = s_ - 2048.0f; o[5] = t_ - 2048.0f;
        for (int j = 0; j < 4; j++) rgba[v * 4 + j] = (u8)ftoi(fc(c[j] * cs[j]));
    }
    native_mesh_update(id, v6, rgba, l->nv);
    return 1;
}

/* the draw of tfrag i at level L in mode: 1 if done */
static int draw(u32 i, int L, u32 mode)
{
    Tf *t = &tf[i]; Lod *l = &t->l[L];
    if (!l->ok) return 0;
    int dyn = mode != 6 && mode != 0x14 && mode != 2, clip = mode == 2;
    /* the frame's constants and transform, as DrawTfrag and the chain's start left them for the program */
    float g656[4], g657[4], g661[4], g662[4], g664[4], M[4][4], tag[4];
    fl4(656, g656); fl4(657, g657); fl4(661, g661); fl4(662, g662); fl4(664, g664); fl4(658, tag);
    for (int r = 0; r < 4; r++) fl4(5 + r, M[r]);
    u32 adgif = t->data + rd16(t->hdr + 0x1C);
    if (g657[2] != 2048.0f) { static int told; if (!told++) uya_log("native terrain: s/t bias %g (not 2048): the VU1 draws", g657[2]); return 0; }
    if (clip && rd32(adgif + 16 + 12) != 0x45000000u) { static int told; if (!told++) uya_log("native terrain: the clip pass's s/t bias is not 2048: the VU1 draws it"); return 0; }
    u32 tl[4]; memcpy(tl, tag, 16);
    u64 lo = ((u64)tl[1] << 32) | tl[0];
    u64 prim = ((lo >> 46) & 1) ? (lo >> 47) & 0x7FF : 0x7C;
    u32 id = mesh_id(i, L, dyn);
    extern const char *uya_gs_src; uya_gs_src = "native terrain";
    float xf[32];
    if (dyn) {
        if (!morph_vertices(t, L, mode, id)) return 0;
        static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        memcpy(xf, I, sizeof I);
    } else {
        /* colours: the level data's (the game relights them there) */
        if (t->ncol) {
            const u8 *c = EE(t->data + rd16(t->hdr + 0x1E));
            if (memcmp(c, t->col, t->ncol * 4)) { memcpy(t->col, c, t->ncol * 4); t->l[L2].dirty = t->l[L0].dirty = 1; }
        }
        if (l->dirty) {
            static u8 rgba[1024 * 4];
            for (u32 k = 0; k < l->nv; k++) memcpy(rgba + k * 4, t->col + l->cidx[k] * 4, 4);
            native_mesh_colors(id, rgba, l->nv);
            l->dirty = 0;
        }
        for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) xf[r * 4 + k] = M[r][k];
    }
    if (!clip) native_tfrag_set_cscale(g664);      /* (every start of the program ends reloading its colour scale) */
    xf[16] = g656[0]; xf[17] = g656[1]; xf[18] = g656[2]; xf[19] = g661[3];
    xf[20] = g661[0]; xf[21] = g661[1]; xf[22] = g661[2]; xf[23] = 1e-3f;
    xf[24] = xf[25] = xf[26] = xf[27] = 0;      /* (XYOFFSET, texture size: the GS layer's) */
    /* the clip pass: cut at the frustum's planes (the guard band: 662) by the renderer, as the
     * clip program cuts the triangles there; the other modes are not cut */
    for (int k = 0; k < 4; k++) xf[28 + k] = clip ? g662[k] : 0.0f;
    for (int k = 0; k < l->nr; k++) {
        const Range *r = &l->r[k];
        u32 a[6]; u64 v[6];
        for (int j = 0; j < 5; j++) {
            u32 q = adgif + (r->blk + j) * 16;
            v[j] = ((u64)rd32(q + 4) << 32) | rd32(q); a[j] = rd32(q + 8) & 0xFF;
        }
        a[5] = 0x00; v[5] = prim;
        native_regs(a, v, 6);
        if (r->count) native_mesh_draw(id, r->first, r->count, xf);
    }
    return 1;
}

int native_terrain_tag(const u32 *q, int tte)
{
    if (on <= 0 || !tf || !tte) { native_terrain_flush(); return 0; }
    u32 id = (q[0] >> 28) & 7, qwc = q[0] & 0xFFFF, addr = q[1] & 0x7FFFFFFF, v;
    if (!g.open) {
        if (id != 3 || !hget(addr, &v)) return 0;
        g.open = 1; g.tte = tte; g.n = 1; g.tag[0] = q; g.tf = v >> 1; g.kind = v & 1;
        return 1;
    }
    if (id == 3 && g.n < 3) { g.tag[g.n++] = q; return 1; }
    if (id == 1 && qwc == 0 && q[2] >> 24 == 0x13 && ((q[3] >> 24) & 0x7F) == 0x14) {
        u32 mode = q[3] & 0xFFFF;
        const Tf *t = &tf[g.tf];
        u32 col = t->data + rd16(t->hdr + 0x1E), q0 = g.tag[0][0] & 0xFFFF;
        u32 a1 = g.n > 1 ? g.tag[1][1] & 0x7FFFFFFF : 0, a2 = g.n > 2 ? g.tag[2][1] & 0x7FFFFFFF : 0;
        int L = -1;
        if (mode == 6 && g.kind == 0 && g.n == 2 && q0 == rd8(t->hdr + 0x21) && a1 == col) L = L2;
        if ((mode == 8 || mode == 0xA) && g.kind == 1 && g.n == 2 && q0 == rd8(t->hdr + 0x22) && a1 == col) L = L1;
        if ((mode == 0xE || mode == 0x10 || mode == 0x14 || (mode == 2 && (uya_gs_renderer_gl() || getenv("UYA_NTCLIP")))) && g.kind == 1 && g.n == 3 && q0 == rd8(t->hdr + 0x20) &&
            a1 == t->data + rd16(t->hdr + 0x1A) && a2 == col) L = L0;
        if (L >= 0 && draw(g.tf, L, mode)) { g.open = 0; n_mesh[mode & 31]++; return 1; }
        n_vu[mode & 31]++;
    }
    native_terrain_flush();
    return native_terrain_tag(q, tte);
}
