/* gs.c: a software Graphics Synthesizer, enough for the menu.
 *
 * Input is GIF data (PATH2 from VIF1 DIRECT, PATH3 from GIF DMA). It keeps
 * the 4 MB local memory with the real swizzled layouts, so a texture written
 * in one format and read in another (fonts, CLUTs) works like on the PS2.
 *
 * Done: every primitive type, flat and Gouraud colour, ST/Q and UV texturing
 * (32/24/16-bit, 8/4-bit with CLUT, 8H/4HL/4HH), nearest and bilinear
 * filtering, mipmapping, wrap modes, texture functions, fog, alpha test,
 * destination alpha test, depth test, alpha blending, colour clamp,
 * FBMSK/ZMSK, scissor, host-to-local and local-to-local transfers, CLUT
 * loading (CSM1), and the display circuits (PMODE/DISPFB/DISPLAY) for showing
 * a frame.
 * Not done: antialiasing, dithering, CSM2, local-to-host transfers. */
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "uya_host.h"
#include "uya_runtime.h"
#include "gs_swizzle.h"
#include "gs_types.h"
#include "gs_gl.h"
static inline unsigned long long rdtsc_(void) { unsigned lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((unsigned long long)hi << 32) | lo; }
unsigned long long uya_t_gs, uya_tex_px;
int uya_timing = -1;   /* UYA_FPSLOG: time the GS and the VUs (rdtsc around each primitive, packet and VU run) */
static int timing(void) { if (uya_timing < 0) uya_timing = getenv("UYA_FPSLOG") != 0; return uya_timing; }

#define BITS(v, lo, n) ((u32)(((v) >> (lo)) & ((1ull << (n)) - 1)))

static u32 *vram;                     /* 1M words = 4 MB */

/* ---- addressing ---- */
static inline u32 a32(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 b = bp + ((y >> 5) * bw + (x >> 6)) * 32 + gs_block32[(y >> 3) & 3][(x >> 3) & 7];
    return ((b & 0x3FFF) << 6) | gs_col32[y & 7][x & 7];
}
static inline u32 a32z(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 b = bp + ((y >> 5) * bw + (x >> 6)) * 32 + (gs_block32[(y >> 3) & 3][(x >> 3) & 7] ^ 0x18);
    return ((b & 0x3FFF) << 6) | gs_col32[y & 7][x & 7];
}
static inline u32 a16(u32 bp, u32 bw, u32 x, u32 y, const uint8_t (*bt)[4], u32 z)
{
    u32 b = bp + ((y >> 6) * bw + (x >> 6)) * 32 + (bt[(y >> 3) & 7][(x >> 4) & 3] ^ z);
    return ((b & 0x3FFF) << 7) | gs_col16[y & 7][x & 15];       /* halfword index */
}
static inline u32 a8(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 b = bp + ((y >> 6) * (bw >> 1) + (x >> 7)) * 32 + gs_block8[(y >> 4) & 3][(x >> 4) & 7];
    return ((b & 0x3FFF) << 8) | gs_col8[y & 15][x & 15];       /* byte index */
}
static inline u32 a4(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 b = bp + ((y >> 7) * (bw >> 1) + (x >> 7)) * 32 + gs_block4[(y >> 4) & 7][(x >> 5) & 3];
    return ((b & 0x3FFF) << 9) | gs_col4[y & 15][x & 31];       /* nibble index */
}

static inline u16 *vram16(void) { return (u16 *)vram; }
static inline u8 *vram8(void) { return (u8 *)vram; }

/* raw pixel read/write in any storage format (colour formats return the stored bits) */
static u32 px_read(u32 psm, u32 bp, u32 bw, u32 x, u32 y)
{
    switch (psm) {
    case 0x00: case 0x01: return vram[a32(bp, bw, x, y)];
    case 0x02: return vram16()[a16(bp, bw, x, y, gs_block16, 0)];
    case 0x0A: return vram16()[a16(bp, bw, x, y, gs_block16s, 0)];
    case 0x13: return vram8()[a8(bp, bw, x, y)];
    case 0x14: { u32 n = a4(bp, bw, x, y); return (vram8()[n >> 1] >> ((n & 1) * 4)) & 15; }
    case 0x1B: return vram[a32(bp, bw, x, y)] >> 24;
    case 0x24: return (vram[a32(bp, bw, x, y)] >> 24) & 15;
    case 0x2C: return vram[a32(bp, bw, x, y)] >> 28;
    case 0x30: case 0x31: return vram[a32z(bp, bw, x, y)];
    case 0x32: return vram16()[a16(bp, bw, x, y, gs_block16, 0x18)];
    case 0x3A: return vram16()[a16(bp, bw, x, y, gs_block16s, 0x18)];
    }
    return 0;
}
static void px_write(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v)
{
    u32 i;
    switch (psm) {
    case 0x00: vram[a32(bp, bw, x, y)] = v; break;
    case 0x01: i = a32(bp, bw, x, y); vram[i] = (vram[i] & 0xFF000000) | (v & 0xFFFFFF); break;
    case 0x02: vram16()[a16(bp, bw, x, y, gs_block16, 0)] = (u16)v; break;
    case 0x0A: vram16()[a16(bp, bw, x, y, gs_block16s, 0)] = (u16)v; break;
    case 0x13: vram8()[a8(bp, bw, x, y)] = (u8)v; break;
    case 0x14: i = a4(bp, bw, x, y); vram8()[i >> 1] = (u8)((vram8()[i >> 1] & ((i & 1) ? 0x0F : 0xF0)) | ((v & 15) << ((i & 1) * 4))); break;
    case 0x1B: i = a32(bp, bw, x, y); vram[i] = (vram[i] & 0x00FFFFFF) | (v << 24); break;
    case 0x24: i = a32(bp, bw, x, y); vram[i] = (vram[i] & 0xF0FFFFFF) | ((v & 15) << 24); break;
    case 0x2C: i = a32(bp, bw, x, y); vram[i] = (vram[i] & 0x0FFFFFFF) | ((v & 15) << 28); break;
    case 0x30: vram[a32z(bp, bw, x, y)] = v; break;
    case 0x31: i = a32z(bp, bw, x, y); vram[i] = (vram[i] & 0xFF000000) | (v & 0xFFFFFF); break;
    case 0x32: vram16()[a16(bp, bw, x, y, gs_block16, 0x18)] = (u16)v; break;
    case 0x3A: vram16()[a16(bp, bw, x, y, gs_block16s, 0x18)] = (u16)v; break;
    }
}
static u32 psm_bits(u32 psm)
{
    switch (psm) {
    case 0x01: case 0x31: return 24;
    case 0x02: case 0x0A: case 0x32: case 0x3A: return 16;
    case 0x13: return 8;
    case 0x14: return 4;
    default: return 32;    /* 32-bit, and the H formats take whole words of GS memory */
    }
}

/* bits per pixel of transfer data (host <-> local): the H formats send 8 or
 * 4 bits a pixel like PSMT8/PSMT4, though they sit in 32-bit words in GS
 * memory (psm_bits above, for the memory they take) */
static u32 trx_bits(u32 psm)
{
    switch (psm) {
    case 0x1B: return 8;
    case 0x24: case 0x2C: return 4;
    default: return psm_bits(psm);
    }
}

/* ---- registers ---- */

static gs_ctx_t ctx[2];
static u64 prim, prmode, prmodecont = 1, rgbaq, st, uv, fog, texa, fogcol, colclamp, pabe, dthe;
static u64 bitbltbuf, trxpos, trxreg, texclut;
static float qreg = 1.0f;
static u32 clut[256];                  /* loaded CLUT entries (raw 32-bit or 16-bit values) */
static u32 clut_hash;
static u32 cbp0, cbp1;

static vtx_t vq[3];
static int vcount;                     /* vertices queued for the current primitive */

static unsigned long long stat_prims, stat_pixels;
static int tc_nopin = -1;
static unsigned long long gs_thread_busy, n_tc_reuse, n_tc_stale, n_mark_trx, n_mark_draw, n_drain_wait, n_tc_build, n_trx_px, t_tcb, t_trx, t_draw;
extern const char *uya_gs_src;
static const char *gs_src_int = "?";   /* the source of the data the GS is working on */
static int mtgs;                         /* the GS runs on its own thread */
static const u32 *gif_cur_qw;            /* testing: the GIF qword being processed */

static int trxlog_on(void);
/* ---- CLUT load (CSM1 layout) ---- */
static void clut_read(u64 t0);
/* GL: exchange the loaded palette with a saved one (gs_gl.c keeps a batch's) */
void gs_clut_swap(u32 *pal, u32 *hash)
{
    u32 t[256]; memcpy(t, clut, sizeof t); memcpy(clut, pal, sizeof t); memcpy(pal, t, sizeof t);
    u32 h = clut_hash; clut_hash = *hash; *hash = h;
}
static u64 clut_pend; static int clut_pending;   /* GL: a load not done yet */
static void clut_flush(void)
{
    if (!clut_pending) return;
    clut_pending = 0;
    gsgl_cpu_read(BITS(clut_pend, 37, 14), 4);
    clut_read(clut_pend);
}
static void clut_load(u64 t0)
{
    u32 psm = BITS(t0, 20, 6), cbp = BITS(t0, 37, 14), cpsm = BITS(t0, 51, 4), csa = BITS(t0, 56, 5);
    u32 cld = BITS(t0, 61, 3);
    int load = 0;
    switch (cld) {
    case 1: load = 1; break;
    case 2: load = 1; cbp0 = cbp; break;
    case 3: load = 1; cbp1 = cbp; break;
    case 4: if (cbp != cbp0) { load = 1; cbp0 = cbp; } break;
    case 5: if (cbp != cbp1) { load = 1; cbp1 = cbp; } break;
    }
    if (trxlog_on()) uya_log("TEX0 %016llX: CLD %u CBP %04X CSA %u PSM %X CPSM %X -> %s", (unsigned long long)t0, cld, cbp, csa, psm, cpsm, load ? "load" : "keep");
    if (!load) return;
    if (gsgl_on) {
        int pal = psm == 0x13 || psm == 0x1B || psm == 0x14 || psm == 0x24 || psm == 0x2C;
        gsgl_clut_changed();                 /* (a batch decoding later uses the old palette) */
        /* GL: a load from memory a GPU target owns waits until a primitive
         * uses the palette: games point CBP at the frame buffer to force the
         * next load (CLD 4/5), and a full 8-bit load replaces it anyway */
        /* (a pending load from a GPU target that another load replaces is
         * dropped, not read back: the force-load trick's frame-buffer data is
         * never used; reading it cost a GPU readback a frame in the menu) */
        clut_pending = 0;
        if (!pal) return;
        if (gsgl_tex_gpu(0x00, cbp, 1, 16, 16)) { clut_pend = t0; clut_pending = 1; return; }
        gsgl_cpu_read(cbp, 4);
    }
    clut_read(t0);
}
/* the palette into clut[] (CSM1) */
static void clut_read(u64 t0)
{
    u32 psm = BITS(t0, 20, 6), cbp = BITS(t0, 37, 14), cpsm = BITS(t0, 51, 4), csa = BITS(t0, 56, 5);
    u32 cp = (cpsm == 0) ? 0x00 : (cpsm == 0xA ? 0x0A : 0x02);
    if ((psm == 0x13 || psm == 0x1B) && cp == 0x00) {   /* the same as below, the 32-bit reads inline */
        for (u32 i = 0; i < 256; i++) {
            u32 x = (i & 7) | ((i & 0x10) >> 1), y = ((i >> 3) & 1) | ((i >> 5) << 1);
            clut[i] = vram[a32(cbp, 1, x, y)];
        }
    } else if (psm == 0x13 || psm == 0x1B) {          /* 8-bit: 256 entries, 16x16 with index bits 3 and 4 swapped */
        for (u32 i = 0; i < 256; i++) {
            u32 x = (i & 7) | ((i & 0x10) >> 1), y = ((i >> 3) & 1) | ((i >> 5) << 1);
            clut[i] = px_read(cp, cbp, 1, x, y);
        }
    } else if (psm == 0x14 || psm == 0x24 || psm == 0x2C) {   /* 4-bit: 16 entries, 8x2, at CSA */
        u32 base = (cp == 0x00 ? (csa & 15) : csa) * 16;
        for (u32 i = 0; i < 16; i++) clut[(base + i) & 255] = px_read(cp, cbp, 1, i & 7, i >> 3);
    }
    {   /* a hash of the palette, for the texture cache */
        u32 h = 2166136261u;
        for (int i = 0; i < 256; i++) h = (h ^ clut[i]) * 16777619u;
        clut_hash = h;
        if (trxlog_on()) uya_log("   clut hash %08X", h);
    }
}

static void prim_reset(void) { vcount = 0; }
/* testing: UYA_TRXLOG=from,to logs transfers, CLUT loads and texture builds in that vsync range */
static int trxlog_on(void)
{
    static int st = -1; static unsigned a, b;
    if (st < 0) { const char *e = getenv("UYA_TRXLOG"); st = e && sscanf(e, "%u,%u", &a, &b) == 2; }
    if (!st) return 0;
    unsigned f = uya_frame_count();
    static unsigned lastf;
    if (f >= a && f <= b && f != lastf) { lastf = f; uya_log("---- vsync %u", f); }
    return f >= a && f <= b;
}

/* ---- texture sampling ---- */
typedef struct { u8 r, g, b, a; } rgba_t;

static inline rgba_t c32(u32 v) { rgba_t c = {(u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24)}; return c; }
static inline rgba_t c16(u32 v, int aem, u8 ta0, u8 ta1)
{
    rgba_t c;
    c.r = (u8)((v & 31) << 3); c.g = (u8)(((v >> 5) & 31) << 3); c.b = (u8)(((v >> 10) & 31) << 3);
    c.a = (v & 0x8000) ? ta1 : ((aem && !(v & 0x7FFF)) ? 0 : ta0);
    return c;
}

/* the texture at mip level `lvl` (0: TEX0's own) */
/* where mip level `lvl` of the context's texture is (0: TEX0's own) */
static void tex_level(const gs_ctx_t *c, int lvl, u32 *tbp_, u32 *tbw_, u32 *tw_, u32 *th_)
{
    u64 t0 = c->tex0;
    u32 tbp = BITS(t0, 0, 14), tbw = BITS(t0, 14, 6), psm = BITS(t0, 20, 6);
    u32 tw = 1u << BITS(t0, 26, 4), th = 1u << BITS(t0, 30, 4);
    if (lvl > 0) {
        if (BITS(c->tex1, 9, 1)) {            /* MTBA: levels 1-3 follow level 0 */
            u32 b = tbp, w = tw, h = th, bw = tbw;
            for (int k = 0; k < lvl && k < 3; k++) {
                u32 bits = psm_bits(psm); b += (w * h * bits / 8 + 255) / 256; w >>= 1; h >>= 1; bw = bw > 1 ? bw >> 1 : 1;
            }
            tbp = b; tbw = bw;
        } else {
            u64 m = lvl <= 3 ? c->miptbp1 : c->miptbp2;
            int k = (lvl - 1) % 3;
            tbp = BITS(m, k * 20, 14); tbw = BITS(m, k * 20 + 14, 6);
        }
        tw >>= lvl; th >>= lvl;
        if (!tw) tw = 1;
        if (!th) th = 1;
    }
    if (tw > 1024) tw = 1024;
    if (th > 1024) th = 1024;
    *tbp_ = tbp; *tbw_ = tbw; *tw_ = tw; *th_ = th;
}

/* one texel, decoded to RGBA (CLUT and TEXA applied) */
static inline u32 rgba_u32(rgba_t r) { return (u32)r.r | ((u32)r.g << 8) | ((u32)r.b << 16) | ((u32)r.a << 24); }
static inline rgba_t u32_rgba(u32 v) { rgba_t r = { (u8)v, (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) }; return r; }
static rgba_t texel_raw(u64 t0, u32 tbp, u32 tbw, u32 x, u32 y)
{
    u32 psm = BITS(t0, 20, 6);
    u8 ta0 = (u8)BITS(texa, 0, 8), ta1 = (u8)BITS(texa, 32, 8);
    int aem = (int)BITS(texa, 15, 1);
    u32 cpsm = BITS(t0, 51, 4), csa = BITS(t0, 56, 5);
    u32 v = px_read(psm, tbp, tbw, x & 2047, y & 2047);
    rgba_t r;
    switch (psm) {
    case 0x00: return c32(v);
    case 0x01: r = c32(v); r.a = (aem && !(v & 0xFFFFFF)) ? 0 : ta0; return r;
    case 0x02: case 0x0A: return c16(v, aem, ta0, ta1);
    case 0x13: case 0x1B: v = clut[v & 255]; break;
    case 0x14: case 0x24: case 0x2C: v = clut[((cpsm == 0 ? (csa & 15) : csa) * 16 + v) & 255]; break;
    default: return c32(v);
    }
    return cpsm == 0 ? c32(v) : c16(v, aem, ta0, ta1);
}

/* ---- texture cache ----
 * Textures are decoded (swizzle, CLUT, TEXA) into plain RGBA arrays once and
 * reused until GS memory under them changes: transfers and draws mark the
 * 8 KB pages they write (page_gen), and an entry is checked against its pages
 * once per primitive. */
static u32 page_gen[512], dirty_epoch = 1;
static void mark_blocks(u32 bp, u32 nblocks)
{
    u32 p0 = bp / 32, p1 = (bp + (nblocks ? nblocks : 1) - 1) / 32;
    if (p1 > 511) p1 = 511;
    for (u32 p = p0; p <= p1 && p < 512; p++) page_gen[p]++;
    dirty_epoch++;
}
/* Content of GS memory by page, for the texture cache: games stream textures
 * through the same memory every frame (Kerwan: ~370 rebuilds a vsync), so an
 * entry whose pages were written again is still good if the bytes are the
 * same. A page is hashed once per change (page_gen). */
static u32 page_hg[512] = { [0 ... 511] = ~0u };
static u64 page_h[512];
static u64 pages_content(u32 p0, u32 p1)
{
    u64 h = 0x9E3779B97F4A7C15ull ^ ((u64)p0 << 32) ^ p1;
    for (u32 p = p0; p <= p1; p++) {
        if (page_hg[p] != page_gen[p]) {
            const u64 *w = (const u64 *)(vram + p * 2048);
            u64 a = 0, b = 0, c = 0, d = 0;
            for (u32 i = 0; i < 1024; i += 4) {
                a = (a ^ w[i]) * 0x100000001B3ull; b = (b ^ w[i + 1]) * 0x9E3779B97F4A7C15ull;
                c = (c ^ w[i + 2]) * 0xC2B2AE3D27D4EB4Full; d = (d ^ w[i + 3]) * 0x165667B19E3779F9ull;
            }
            page_h[p] = a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ (a >> 29) ^ (c >> 31);
            page_hg[p] = page_gen[p];
        }
        h = (h ^ page_h[p]) * 0x100000001B3ull + p;
    }
    return h;
}
/* blocks a w x h area of a buffer (base bp, width bw in 64-pixel units) can touch */
static u32 area_blocks(u32 psm, u32 bw, u32 h)
{
    u32 bits = psm_bits(psm);
    if (bits == 24) bits = 32;
    u32 w = (bw ? bw : 1) * 64;
    return (w * h * bits / 8 + 255) / 256 + 64;
}
typedef struct { u64 k1, k2, ch; u32 epoch, sum, p0, p1, w, h, used, ser; u32 *px; } tcache_t;
#define TC_BUDGET ((size_t)256 << 20)   /* bytes of decoded textures kept */
static size_t tc_bytes;
static tcache_t tc[TC_N];
static u32 tc_clock, prim_serial;
static struct { u32 serial; const tcache_t *e; } tc_memo[8];
/* a whole texture level to RGBA, a swizzle block at a time */
static void decode_tex(u32 *out, u64 t0, u32 tbp, u32 tbw, u32 tw, u32 th)
{
    u32 psm = BITS(t0, 20, 6), cpsm = BITS(t0, 51, 4), csa = BITS(t0, 56, 5);
    u8 ta0 = (u8)BITS(texa, 0, 8), ta1 = (u8)BITS(texa, 32, 8);
    int aem = (int)BITS(texa, 15, 1);
    u32 pal[256];
    if (psm == 0x13 || psm == 0x1B || psm == 0x14 || psm == 0x24 || psm == 0x2C) {
        u32 off = (psm == 0x13 || psm == 0x1B) ? 0 : (cpsm == 0 ? (csa & 15) : csa) * 16;
        u32 n = (psm == 0x13 || psm == 0x1B) ? 256 : 16;
        for (u32 i = 0; i < n; i++) { u32 v = clut[(off + i) & 255]; pal[i] = rgba_u32(cpsm == 0 ? c32(v) : c16(v, aem, ta0, ta1)); }
    }
    for (u32 y = 0; y < th; y++) {
        u32 *row = out + y * tw;
        u32 yy = y & 2047;
        switch (psm) {
        case 0x13: {
            const u8 *col = gs_col8[yy & 15];
            for (u32 x0 = 0; x0 < tw; x0 += 16) {
                u32 base = a8(tbp, tbw, x0, yy) - col[0], n = tw - x0 < 16 ? tw - x0 : 16;
                for (u32 i = 0; i < n; i++) row[x0 + i] = pal[vram8()[base + col[i]]];
            }
        } break;
        case 0x14: {
            const u16 *col = gs_col4[yy & 15];
            for (u32 x0 = 0; x0 < tw; x0 += 32) {
                u32 base = a4(tbp, tbw, x0, yy) - col[0], n = tw - x0 < 32 ? tw - x0 : 32;
                for (u32 i = 0; i < n; i++) { u32 nb = base + col[i]; row[x0 + i] = pal[(vram8()[nb >> 1] >> ((nb & 1) * 4)) & 15]; }
            }
        } break;
        case 0x00: case 0x01: case 0x1B: case 0x24: case 0x2C: {
            const u8 *col = gs_col32[yy & 7];
            for (u32 x0 = 0; x0 < tw; x0 += 8) {
                u32 base = a32(tbp, tbw, x0, yy) - col[0], n = tw - x0 < 8 ? tw - x0 : 8;
                for (u32 i = 0; i < n; i++) {
                    u32 v = vram[base + col[i]];
                    switch (psm) {
                    case 0x00: row[x0 + i] = v; break;
                    case 0x01: { rgba_t r = c32(v); r.a = (aem && !(v & 0xFFFFFF)) ? 0 : ta0; row[x0 + i] = rgba_u32(r); } break;
                    case 0x1B: row[x0 + i] = pal[v >> 24]; break;
                    case 0x24: row[x0 + i] = pal[(v >> 24) & 15]; break;
                    default: row[x0 + i] = pal[v >> 28]; break;
                    }
                }
            }
        } break;
        case 0x02: case 0x0A: {
            const uint8_t (*bt)[4] = psm == 0x02 ? gs_block16 : gs_block16s;
            const u8 *col = gs_col16[yy & 7];
            for (u32 x0 = 0; x0 < tw; x0 += 16) {
                u32 base = a16(tbp, tbw, x0, yy, bt, 0) - col[0], n = tw - x0 < 16 ? tw - x0 : 16;
                for (u32 i = 0; i < n; i++) row[x0 + i] = rgba_u32(c16(vram16()[base + col[i]], aem, ta0, ta1));
            }
        } break;
        default:
            for (u32 x = 0; x < tw; x++) row[x] = rgba_u32(texel_raw(t0, tbp, tbw, x, yy));
            break;
        }
    }
}

static void gs_drain(void);
static int dbg_race(u32 d0, u32 d1, const char *what, u32 bp);
static int dbg_drain_all = -1;
static u32 n_layout_drain;   /* testing: UYA_DRAINALL=1 (texture builds), 2 (transfers), 3 (both) */
static u8 page_drawn[512];            /* pages draws (maybe still queued) write */
static int range_drawn(u32 bp, u32 n)
{
    u32 p0 = bp / 32, p1 = (bp + (n ? n : 1) - 1) / 32;
    if (p1 > 511) p1 = 511;
    for (u32 p = p0; p <= p1; p++) if (page_drawn[p]) return 1;
    return 0;
}
static u32 cur_target[6];            /* the current target's pages (frame, then Z), still being drawn; [4], [5]: their layouts */
/* A page's layout (buffer width and format) as queued draws use it: workers
 * own rows, and the same memory seen with another width or format puts a
 * row in other rows, so a draw with another layout over queued draws' pages
 * must wait for them (else a worker can read what another has not written) */
static u16 page_layout[512];
static int layout_conflict(u32 bp, u32 n, u16 lay)
{
    u32 p0 = bp / 32, p1 = (bp + (n ? n : 1) - 1) / 32;
    if (p1 > 511) p1 = 511;
    for (u32 p = p0; p <= p1; p++) if (page_drawn[p] && page_layout[p] != lay) return 1;
    return 0;
}
static void set_drawn(u32 bp, u32 n, u16 lay)
{
    u32 p0 = bp / 32, p1 = (bp + (n ? n : 1) - 1) / 32;
    if (p1 > 511) p1 = 511;
    for (u32 p = p0; p <= p1; p++) { page_drawn[p] = 1; page_layout[p] = lay; }
}
static void set_drawn_target(void) { set_drawn(cur_target[0], cur_target[1], (u16)cur_target[4]); if (cur_target[3]) set_drawn(cur_target[2], cur_target[3], (u16)cur_target[5]); }
static void st_dirty_force(void);
static void retire(void *px);
static size_t retire_n;
static void retire_free(int all);
/* Decoded textures come and go all the time (about 200 a vsync in a level, as
 * the game streams textures through GS memory). Big blocks from malloc are
 * fresh mappings that fault in page by page, so freed buffers are kept for
 * reuse by size (a few dozen, at most 64 MB). */
static struct { void *p; size_t n; } tpool[64];
static int tpool_n; static size_t tpool_bytes;
static void *tex_alloc(size_t n)
{
    for (int i = tpool_n - 1; i >= 0; i--)
        if (tpool[i].n == n) { void *p = tpool[i].p; tpool_bytes -= n; tpool[i] = tpool[--tpool_n]; return p; }
    return malloc(n);
}
static void tex_free(void *p, size_t n)
{
    if (!p) return;
    if (n >= 4096 && tpool_bytes + n <= ((size_t)64 << 20)) {
        if (tpool_n == 64) { free(tpool[0].p); tpool_bytes -= tpool[0].n; tpool[0] = tpool[--tpool_n]; }
        tpool[tpool_n].p = p; tpool[tpool_n].n = n; tpool_n++; tpool_bytes += n;
        return;
    }
    free(p);
}
static void tex_pool_flush(void) { while (tpool_n) { tpool_n--; free(tpool[tpool_n].p); } tpool_bytes = 0; }
static const tcache_t *tc_get(const gs_ctx_t *c, int lvl)
{
    if (tc_memo[lvl].serial == prim_serial) return tc_memo[lvl].e;
    u64 t0 = c->tex0;
    u32 tbp, tbw, tw, th, psm = BITS(t0, 20, 6);
    tex_level(c, lvl, &tbp, &tbw, &tw, &th);
    if (lvl == 0) {   /* the REGION modes can address past the texture's size: decode that far too */
        u32 wms = BITS(c->clamp, 0, 2), wmt = BITS(c->clamp, 2, 2);
        u32 mu = BITS(c->clamp, 14, 10), mv = BITS(c->clamp, 34, 10);
        if (wms == 2 && mu + 1 > tw) tw = mu + 1;
        if (wms == 3 && (mu | BITS(c->clamp, 4, 10)) + 1 > tw) tw = (mu | BITS(c->clamp, 4, 10)) + 1;
        if (wmt == 2 && mv + 1 > th) th = mv + 1;
        if (wmt == 3 && (mv | BITS(c->clamp, 24, 10)) + 1 > th) th = (mv | BITS(c->clamp, 24, 10)) + 1;
        if (tw > 1024) tw = 1024;
        if (th > 1024) th = 1024;
    }
    u64 k1 = (u64)tbp | ((u64)tbw << 14) | ((u64)psm << 20) | ((u64)tw << 26) | ((u64)th << 40);
    u32 pal = (psm == 0x13 || psm == 0x1B || psm == 0x14 || psm == 0x24 || psm == 0x2C);
    u64 k2 = pal ? ((u64)clut_hash | ((u64)BITS(t0, 51, 4) << 32) | ((u64)BITS(t0, 56, 5) << 36)) : 0;
    k2 ^= (u64)(BITS(texa, 0, 8) | (BITS(texa, 15, 1) << 8) | (BITS(texa, 32, 8) << 9)) << 44;
    static tcache_t *last[8];
    tcache_t *e = 0;
    static int16_t hidx[8192];                          /* hash of the keys -> entry + 1 */
    u32 hh = (u32)((k1 * 0x9E3779B97F4A7C15ull ^ k2 * 0xC2B2AE3D27D4EB4Full) >> 51);
    u32 p0 = tbp / 32, p1 = (tbp + area_blocks(psm, tbw, th)) / 32;
    if (p1 > 511) p1 = 511;
    u32 sum = 0; int have_sum = 0;
    u64 ch = 0; int have_ch = 0;
    if (last[lvl] && last[lvl]->px && last[lvl]->used && last[lvl]->k1 == k1 && last[lvl]->k2 == k2 && last[lvl]->epoch == dirty_epoch) e = last[lvl];
    else {
        /* the entries of these keys (several, one per content the memory held): the
         * one whose pages did not change, else one with the same bytes there now */
        int stale_seen = 0;
        for (u32 k = 0; k < 16 && !e; k++) {
            int j = hidx[(hh + k) & 8191] - 1;
            if (j < 0 || !tc[j].px || !tc[j].used || tc[j].k1 != k1 || tc[j].k2 != k2) continue;
            tcache_t *x = &tc[j];
            if (x->epoch == dirty_epoch) { e = x; break; }
            if (!have_sum) { for (u32 p = p0; p <= p1; p++) sum += page_gen[p]; have_sum = 1; }
            if (x->sum == sum) { x->epoch = dirty_epoch; e = x; break; }
            /* the bytes decide, unless a GPU target owns some of those pages (GL: GS memory is not current there) */
            if ((dbg_drain_all & 128) || (gsgl_on && gsgl_tex_owned(psm, tbp, tbw, tw, th))) { x->used = 0; x->k1 = ~0ull; stale_seen = 1; continue; }
            if (range_drawn(p0 * 32, (p1 - p0 + 1) * 32)) gs_drain();   /* queued draws may still write those pages */
            if (!have_ch) { ch = pages_content(p0, p1); have_ch = 1; }
            if (x->ch == ch) { x->sum = sum; x->epoch = dirty_epoch; e = x; n_tc_reuse++; break; }
            stale_seen = 1;
        }
        if (!e && stale_seen) n_tc_stale++;
    }
    if (dbg_drain_all & 64) e = 0;
    if (!e) {
        /* the texture's memory must hold what the queued draws put there; the
         * entry's old pixels may still be in use by queued draws, so they are
         * retired (freed once the workers are past them), not overwritten */
        if (dbg_drain_all & 1 || range_drawn(p0 * 32, (p1 - p0 + 1) * 32)) gs_drain();
        else if (dbg_drain_all & 8) dbg_race(p0, p1, "texture", tbp);
        n_tc_build++;
        if (trxlog_on()) uya_log("tex build lvl %d TBP %04X TBW %u PSM %X %ux%u k2 %llX pages %u-%u", lvl, tbp, tbw, psm, tw, th, (unsigned long long)k2, p0, p1);
        int best = 0;                                    /* the least recently used slot */
        extern u32 gsgl_tc_pin[TC_N], gsgl_batch_id;
        if (tc_nopin < 0) tc_nopin = getenv("UYA_TC_NOPIN") != 0;   /* testing: the old behaviour */
#define TC_PINNED(i) (gsgl_on && gsgl_tc_pin[i] == gsgl_batch_id && !tc_nopin)   /* the GL batch being built uses it */
        while (TC_PINNED(best) && best < TC_N - 1) best++;
        for (int i = 0; i < TC_N; i++) { if (TC_PINNED(i)) continue; if (!tc[i].px || !tc[i].used) { best = i; break; } if (tc[i].used < tc[best].used) best = i; }
        e = &tc[best];
        {   /* put it in the hash (a free or stale slot of its probe run) */
            u32 k;
            for (k = 0; k < 16; k++) { int j = hidx[(hh + k) & 8191] - 1; if (j < 0 || j == best || !tc[j].px || !tc[j].used) break; }
            hidx[(hh + (k < 16 ? k : 0)) & 8191] = (int16_t)(best + 1);
        }
        if (e->px) { retire_n = (size_t)e->w * e->h * 4; retire(e->px); tc_bytes -= (size_t)e->w * e->h * 4; e->px = 0; }
        {   /* a memory budget for the decoded textures: the least recently used go first */
            size_t need = (size_t)tw * th * 4;
            static size_t budget;   /* testing: UYA_TC_BUDGET_MB=n keeps less (evictions often) */
            if (!budget) { const char *x = getenv("UYA_TC_BUDGET_MB"); budget = x && atoi(x) > 0 ? (size_t)atoi(x) << 20 : TC_BUDGET; }
            while (tc_bytes + need > budget) {
                int lru = -1;
                for (int i = 0; i < TC_N; i++)
                    if (tc[i].px && &tc[i] != e && !TC_PINNED(i) && tc[i].used + 8 < tc_clock && (lru < 0 || tc[i].used < tc[lru].used)) lru = i;
                if (lru < 0) break;
                retire_n = (size_t)tc[lru].w * tc[lru].h * 4; retire(tc[lru].px); tc_bytes -= (size_t)tc[lru].w * tc[lru].h * 4;
                tc[lru].px = 0; tc[lru].used = 0; tc[lru].k1 = ~0ull;
            }
            e->px = tex_alloc(need);
            if (!e->px) { gs_drain(); retire_free(1); tex_pool_flush(); e->px = malloc(need); }
            if (!e->px) { uya_log("GS: out of memory for a %ux%u texture", tw, th); exit(1); }
            tc_bytes += need;
        }
        e->k1 = k1; e->k2 = k2; e->w = tw; e->h = th; e->p0 = p0; e->p1 = p1;
        { static u32 ser_n; e->ser = ++ser_n; }
        if (gsgl_on) gsgl_tex_read(psm, tbp, tbw, tw, th);   /* GS memory a GPU target drew */
        unsigned long long t_ = rdtsc_();
        decode_tex(e->px, t0, tbp, tbw, tw, th);
        {   /* testing: UYA_TEXDUMP=tbp saves that texture (RGBA, alpha as a second image) when decoded */
            static int want = -2; static int nd;
            if (want == -2) { const char *x = getenv("UYA_TEXDUMP"); want = x ? (int)strtol(x, 0, 16) : -1; }
            if ((int)tbp == want && nd < 40 && (!getenv("UYA_TRXLOG") || trxlog_on())) {
                char path[64]; snprintf(path, sizeof path, "tex_%04X_%02d_v%u_l%d.ppm", tbp, nd++, uya_frame_count(), lvl);
                FILE *f = fopen(path, "wb");
                if (f) { fprintf(f, "P6 %u %u 255\n", tw, th * 2);
                    for (u32 i = 0; i < tw * th; i++) { fputc(e->px[i] & 255, f); fputc((e->px[i] >> 8) & 255, f); fputc((e->px[i] >> 16) & 255, f); }
                    for (u32 i = 0; i < tw * th; i++) { u32 a = e->px[i] >> 24; fputc(a > 255 ? 255 : a, f); fputc(a, f); fputc(a, f); }
                    fclose(f); }
                uya_log("texture %04X (%ux%u, PSM %X, CBP %X) saved to %s", tbp, tw, th, (u32)BITS(t0, 20, 6), (u32)BITS(t0, 37, 14), path);
            }
        }
        t_tcb += rdtsc_() - t_;
        sum = 0;
        for (u32 p = p0; p <= p1; p++) sum += page_gen[p];
        e->sum = sum; e->epoch = dirty_epoch;
        e->ch = (gsgl_on && gsgl_tex_owned(psm, tbp, tbw, tw, th)) ? 0 : pages_content(p0, p1);   /* (never a hash of pages a target owns) */
    }
    e->used = ++tc_clock;
    last[lvl] = e;
    tc_memo[lvl].serial = prim_serial; tc_memo[lvl].e = e;
    return e;
}

/* ---- drawing ----
 * A primitive is turned into a command (pcmd_t: the pixel pipeline's state,
 * the texture's decoded levels from the cache, the vertices) that the
 * rasterizer runs. With UYA_GS_THREADS (default: the number of cores - 1, at
 * most 8) worker threads run the commands, each one drawing its own rows
 * (4-row bands), so every pixel still sees the primitives in order. Anything
 * that reads or writes GS memory outside the drawing (transfers, the texture
 * cache, the display) waits for the workers first (gs_drain). */
/* (tlevel_t, pcmd_t, pprim_t: gs_types.h) */

#define AINL static inline __attribute__((always_inline))
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline float fast_log2(float x)   /* log2|x| to about 0.0002 */
{
    u32 b; memcpy(&b, &x, 4);
    b &= 0x7FFFFFFFu;
    if (!b) return -126.0f;
    float m = (float)(b & 0x7FFFFF) * (1.0f / 8388608.0f);
    return (float)((int)(b >> 23) - 127) + m * (1.4385482f + m * (-0.6780911f + m * (0.3236495f - 0.0842966f * m)));
}

static u32 texel_outside(const pcmd_t *c, int lvl, int x, int y)
{
    gs_ctx_t t; memset(&t, 0, sizeof t);
    t.tex0 = c->tex0; t.tex1 = c->tex1; t.miptbp1 = c->miptbp1; t.miptbp2 = c->miptbp2;
    u32 tbp, tbw, w, h;
    tex_level(&t, lvl, &tbp, &tbw, &w, &h);
    return rgba_u32(texel_raw(c->tex0, tbp, tbw, (u32)x, (u32)y));
}

AINL int wrap1(int x, int n, int wm, int mn, int mx)
{
    switch (wm) {
    case 0: return x & (n - 1);
    case 1: return x < 0 ? 0 : (x >= n ? n - 1 : x);
    case 2: return x < mn ? mn : (x > mx ? mx : x);
    default: return (x & mn) | mx;
    }
}
AINL u32 tfetch(const pcmd_t *c, int lvl, int x, int y)
{
    const tlevel_t *L = &c->lv[lvl];
    int tw = L->w, th = L->h;
    x = wrap1(x, L->lw, c->wms, c->minu, c->maxu);
    y = wrap1(y, L->lh, c->wmt, c->minv, c->maxv);
    if ((u32)x < (u32)tw && (u32)y < (u32)th) return L->px[y * tw + x];
    return texel_outside(c, lvl, x, y);
}

AINL u32 lerp4(u32 p00, u32 p10, u32 p01, u32 p11, u32 fu, u32 fv)
{
    /* two channels at a time: red/blue and green/alpha, 8.8 fixed point */
    u32 iu = 256 - fu, iv = 256 - fv;
    u32 rb0 = ((p00 & 0x00FF00FF) * iu + (p10 & 0x00FF00FF) * fu) >> 8 & 0x00FF00FF;
    u32 rb1 = ((p01 & 0x00FF00FF) * iu + (p11 & 0x00FF00FF) * fu) >> 8 & 0x00FF00FF;
    u32 ga0 = (((p00 >> 8) & 0x00FF00FF) * iu + ((p10 >> 8) & 0x00FF00FF) * fu) >> 8 & 0x00FF00FF;
    u32 ga1 = (((p01 >> 8) & 0x00FF00FF) * iu + ((p11 >> 8) & 0x00FF00FF) * fu) >> 8 & 0x00FF00FF;
    u32 rb = (rb0 * iv + rb1 * fv) >> 8 & 0x00FF00FF;
    u32 ga = (ga0 * iv + ga1 * fv) >> 8 & 0x00FF00FF;
    return rb | (ga << 8);
}

AINL u32 tsample_lvl(const pcmd_t *c, int lvl, float u, float v, int linear)
{
    if (!linear) return tfetch(c, lvl, (int)floorf(u), (int)floorf(v));
    u -= 0.5f; v -= 0.5f;
    float fx = floorf(u), fy = floorf(v);
    int x0 = (int)fx, y0 = (int)fy;
    /* u - floor(u) is in [0, 1): a signed conversion gives the same (and a NaN the same 0x80000000 as clang's unsigned one) */
    u32 fu = (u32)(s32)((u - fx) * 256.0f), fv = (u32)(s32)((v - fy) * 256.0f);
    const tlevel_t *L = &c->lv[lvl];
    int tw = L->w, th = L->h;
    int xa = wrap1(x0, L->lw, c->wms, c->minu, c->maxu), xb = wrap1(x0 + 1, L->lw, c->wms, c->minu, c->maxu);
    int ya = wrap1(y0, L->lh, c->wmt, c->minv, c->maxv), yb = wrap1(y0 + 1, L->lh, c->wmt, c->minv, c->maxv);
    u32 p00, p10, p01, p11;
    if ((u32)xa < (u32)tw && (u32)xb < (u32)tw && (u32)ya < (u32)th && (u32)yb < (u32)th) {
        const u32 *ra = L->px + ya * tw, *rb = L->px + yb * tw;
        p00 = ra[xa]; p10 = ra[xb]; p01 = rb[xa]; p11 = rb[xb];
    } else {
        p00 = tfetch(c, lvl, x0, y0); p10 = tfetch(c, lvl, x0 + 1, y0); p01 = tfetch(c, lvl, x0, y0 + 1); p11 = tfetch(c, lvl, x0 + 1, y0 + 1);
    }
    return lerp4(p00, p10, p01, p11, fu, fv);
}

/* filtering and mipmapping (TEX1: LCM, MXL, MMAG, MMIN, L, K); q is the pixel's Q */
AINL u32 tsample(const pcmd_t *c, float u, float v, float q)
{
    int lin = (c->mmin == 1 || c->mmin == 4 || c->mmin == 5);   /* minification filter within a level */
    if ((c->mxl == 0 || c->mmin < 2) && lin == c->mmag) return tsample_lvl(c, 0, u, v, lin);
    float lod = c->lcm ? c->k : -fast_log2(q) * (float)(1 << c->l) + c->k;
    /* The GS works the LOD out to 4 fraction bits, rounded (the precision of K):
     * a level boundary has that much slack. Games rely on it: RAC1 uploads only
     * the levels its distance test says a moby needs (the others' TBP is stale or
     * 0, the frame buffer), and its test is about as tight. */
    lod = floorf(lod * 16.0f + 0.5f) * (1.0f / 16.0f);
    if (lod <= 0.0f || c->mxl == 0 || c->mmin < 2) return tsample_lvl(c, 0, u, v, lod <= 0.0f ? c->mmag : lin);
    if (c->mmin == 3 || c->mmin == 5) {               /* between two levels */
        int l0 = (int)floorf(lod); float fr = lod - (float)l0;
        /* (x * 2^-n is x / 2^n exactly) */
        if (l0 >= (int)c->mxl) { float s = 1.0f / (float)(1 << c->mxl); return tsample_lvl(c, c->mxl, u * s, v * s, lin); }
        float s0 = 1.0f / (float)(1 << l0), s1 = 1.0f / (float)(2 << l0);
        u32 a = tsample_lvl(c, l0, u * s0, v * s0, lin);
        u32 b = tsample_lvl(c, l0 + 1, u * s1, v * s1, lin);
        u32 f = (u32)(fr * 256), o = 0;
        for (int sh = 0; sh < 32; sh += 8) o |= ((((a >> sh) & 255) * (256 - f) + ((b >> sh) & 255) * f) >> 8) << sh;
        return o;
    }
    int lvl = (int)floorf(lod + 0.5f);
    if (lvl > (int)c->mxl) lvl = (int)c->mxl;
    if (!lvl) return tsample_lvl(c, 0, u, v, lin);
    float s = 1.0f / (float)(1 << lvl);
    return tsample_lvl(c, lvl, u * s, v * s, lin);
}

/* one pixel through the pipeline: u, v are level-0 texel coordinates */
static int ptrace_on, ptrace_x, ptrace_y; static u32 ptrace_fbp; static const void *ptrace_prim;   /* testing: UYA_PIXTRACE=x,y,fbp */
static unsigned ptrace_from;   /* optional 4th field of UYA_PIXTRACE: first vsync to log */
static void ptrace_log(const pcmd_t *p, u32 z, int r, int g, int b, int a, float u, float v, float q)
{
    static int left = 400;
    if (uya_frame_count() < ptrace_from) return;
    if (left-- <= 0) return;
    if (left == 399) uya_log("pixtrace: from vsync %u", uya_frame_count());
    uya_log("pixtrace [%s]: z %08X rgba %d,%d,%d,%d uv %.2f,%.2f q %g | TME %u FST %u TFX %u TCC %u tex0 %016llX tw %d th %d ABE %u (%u%u%u%u fix %u) ATE %u/%u/%u ZTE %u/%u ZPSM %X FBMSK %08X IIP %u FGE %u",
            gs_src_int, z, r, g, b, a, u, v, q, p->tme, p->fst, p->tfx, p->tcc, (unsigned long long)p->tex0, p->lv[0].w, p->lv[0].h, p->abe, p->ba, p->bb, p->bc, p->bd, p->fix,
            p->ate, p->atst, p->aref, p->zte, p->ztst, p->zpsm, p->fbmsk, p->iip, p->fge);
    uya_log("   afail %u, fba %u, colclamp %u, prim at %p", p->afail, p->fba, p->clampc, ptrace_prim);
    if (p->tme) uya_log("   tex1 %016llX miptbp1 %016llX mmin %u mmag %u mxl %u l %u k %g lcm %u: lod %g", (unsigned long long)p->tex1, (unsigned long long)p->miptbp1,
                        p->mmin, p->mmag, p->mxl, p->l, p->k, p->lcm, p->lcm ? p->k : -fast_log2(q) * (float)(1 << p->l) + p->k);
    if (ptrace_prim) {
        const pprim_t *pr = ptrace_prim;
        uya_log("   prim type %u: (%.1f,%.1f rgba %u,%u,%u,%u st %g,%g q %g) (%.1f,%.1f rgba %u,%u,%u,%u) (%.1f,%.1f rgba %u,%u,%u,%u)", pr->type,
                pr->v[0].x, pr->v[0].y, pr->v[0].r, pr->v[0].g, pr->v[0].b, pr->v[0].a, pr->v[0].s, pr->v[0].t, pr->v[0].q,
                pr->v[1].x, pr->v[1].y, pr->v[1].r, pr->v[1].g, pr->v[1].b, pr->v[1].a, pr->v[2].x, pr->v[2].y, pr->v[2].r, pr->v[2].g, pr->v[2].b, pr->v[2].a);
    }
}
static inline void pixel(const pcmd_t *p, int x, int y, u32 z, int r, int g, int b, int a, float u, float v, float q, int f)
{
    if (__builtin_expect(ptrace_on, 0) && x == ptrace_x && y == ptrace_y && p->fbp == ptrace_fbp) ptrace_log(p, z, r, g, b, a, u, v, q);
    if (p->date) {                         /* destination alpha test (RAC1 darkens the pixels its shadows marked with it) */
        u32 m;
        if (p->fpsm == 0x00) m = vram[a32(p->fbp, p->fbw, (u32)x, (u32)y)] >> 31;
        else if (p->fpsm == 0x02 || p->fpsm == 0x0A) m = (px_read(p->fpsm, p->fbp, p->fbw, (u32)x, (u32)y) >> 15) & 1;
        else m = p->datm;                  /* PSMCT24 has no alpha to test */
        if (m != p->datm) return;
    }
    u32 zaddr = 0;                         /* depth test first: a failing pixel is dropped whatever else happens */
    if (p->zpsm == 0x31 || p->zpsm == 0x30) zaddr = a32z(p->zbp, p->fbw, (u32)x, (u32)y);
    if (p->ztst != 1) {
        if (p->ztst == 0) return;
        u32 zd = (p->zpsm == 0x31 || p->zpsm == 0x30) ? vram[zaddr] : px_read(p->zpsm, p->zbp, p->fbw, (u32)x, (u32)y);
        u32 zs = z;
        if (p->zpsm == 0x31) { zs &= 0xFFFFFF; zd &= 0xFFFFFF; }
        if (p->zpsm == 0x32 || p->zpsm == 0x3A) { zs = zs > 0xFFFF ? 0xFFFF : zs; }
        if (p->ztst == 2 ? zs < zd : zs <= zd) return;
    }
    if (p->tme) {
        u32 t = tsample(p, u, v, q);
        int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
        switch (p->tfx) {
        case 0: r = (tr * r) >> 7; g = (tg * g) >> 7; b = (tb * b) >> 7; a = p->tcc ? (ta * a) >> 7 : a; break;
        case 1: r = tr; g = tg; b = tb; a = p->tcc ? ta : a; break;
        case 2: r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a; a = p->tcc ? ta + a : a; break;
        default: r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a; a = p->tcc ? ta : a; break;
        }
        r = clamp255(r); g = clamp255(g); b = clamp255(b); a = clamp255(a);
    }
    if (p->fge) {
        r = (r * f + p->fogr * (255 - f)) >> 8; g = (g * f + p->fogg * (255 - f)) >> 8; b = (b * f + p->fogb * (255 - f)) >> 8;
    }
    int write_fb = 1, write_z = !p->zmsk, write_a = 1;
    if (p->ate) {
        int pass;
        switch (p->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < (int)p->aref; break;
        case 3: pass = a <= (int)p->aref; break;
        case 4: pass = a == (int)p->aref; break;
        case 5: pass = a >= (int)p->aref; break;
        case 6: pass = a > (int)p->aref; break;
        default: pass = a != (int)p->aref; break;
        }
        if (!pass) {
            switch (p->afail) {
            case 0: return;
            case 1: write_z = 0; break;
            case 2: write_fb = 0; break;
            case 3: write_z = 0; write_a = 0; break;
            }
        }
    }
    if (write_fb) {
        u32 old = 0, out;
        int dr = 0, dg = 0, db = 0, da = 0;
        int need_old = p->abe || p->fbmsk || !write_a || p->fpsm == 0x01;
        u32 faddr = p->fpsm <= 1 ? a32(p->fbp, p->fbw, (u32)x, (u32)y) : 0;
        if (need_old) {
            old = p->fpsm <= 1 ? vram[faddr] : px_read(p->fpsm, p->fbp, p->fbw, (u32)x, (u32)y);
            if (p->fpsm == 0x02 || p->fpsm == 0x0A) {
                dr = (old & 31) << 3; dg = ((old >> 5) & 31) << 3; db = ((old >> 10) & 31) << 3; da = (old & 0x8000) ? 0x80 : 0;
            } else {
                dr = old & 255; dg = (old >> 8) & 255; db = (old >> 16) & 255; da = p->fpsm == 0x01 ? 0x80 : (int)(old >> 24);
            }
        }
        if (p->abe) {
            int cs[3] = {r, g, b}, cd[3] = {dr, dg, db}, o[3];
            int cc = p->bc == 0 ? a : (p->bc == 1 ? da : (int)p->fix);
            for (int k = 0; k < 3; k++) {
                int A = p->ba == 0 ? cs[k] : (p->ba == 1 ? cd[k] : 0);
                int B = p->bb == 0 ? cs[k] : (p->bb == 1 ? cd[k] : 0);
                int D = p->bd == 0 ? cs[k] : (p->bd == 1 ? cd[k] : 0);
                o[k] = (((A - B) * cc) >> 7) + D;
            }
            r = o[0]; g = o[1]; b = o[2];
        }
        if (p->clampc) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
        else { r &= 255; g &= 255; b &= 255; }
        if (p->fba) a |= 0x80;
        if (!write_a) a = da;
        if (p->fpsm == 0x02 || p->fpsm == 0x0A) {
            out = (u32)(r >> 3) | ((u32)(g >> 3) << 5) | ((u32)(b >> 3) << 10) | ((a & 0x80) ? 0x8000u : 0);
            out = (out & ~(p->fbmsk & 0xFFFF)) | (old & p->fbmsk & 0xFFFF);
        } else {
            out = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 255) << 24);
            out = (out & ~p->fbmsk) | (old & p->fbmsk);
        }
        if (p->fpsm == 0x00) vram[faddr] = out;
        else if (p->fpsm == 0x01) vram[faddr] = (vram[faddr] & 0xFF000000u) | (out & 0xFFFFFF);
        else px_write(p->fpsm, p->fbp, p->fbw, (u32)x, (u32)y, out);
    }
    if (write_z && p->zte) {
        if (p->zpsm == 0x30) vram[zaddr] = z;
        else if (p->zpsm == 0x31) vram[zaddr] = (vram[zaddr] & 0xFF000000u) | (z & 0xFFFFFF);
        else px_write(p->zpsm, p->zbp, p->fbw, (u32)x, (u32)y, z);
    }
    if (__builtin_expect(ptrace_on, 0) && x == ptrace_x && y == ptrace_y && p->fbp == ptrace_fbp && uya_frame_count() >= ptrace_from)
        uya_log("   -> %08X (fb %d, z %d, a %d)", px_read(p->fpsm, p->fbp, p->fbw, (u32)x, (u32)y), write_fb, write_z, write_a);
}

/* The same pipeline for the targets almost every pixel goes to: 32-bit colour
 * (PSMCT32, no FBMSK) and 32/24-bit Z. The frame and Z addresses share the
 * swizzle (Z's block is the colour block ^ 0x18), and the row's part of it is
 * worked out once per row (rowaddr_t). Same results as pixel(). */
typedef struct { u32 rowoff; const u8 *brow, *crow; } rowaddr_t;
static inline int fast_ok(const pcmd_t *p) { return p->fpsm == 0 && !p->date && (p->zpsm == 0x30 || p->zpsm == 0x31) && !p->fbmsk && !ptrace_on; }
AINL void rowaddr(const pcmd_t *p, int y, rowaddr_t *ra)
{
    ra->rowoff = ((u32)y >> 5) * p->fbw * 32; ra->brow = gs_block32[((u32)y >> 3) & 3]; ra->crow = gs_col32[y & 7];
}
/* the conditions under which pixel() would use z (else it can be left at 0) */
static inline int z_used(const pcmd_t *p) { return p->ztst != 1 || (p->zte && !p->zmsk) || ptrace_on || !fast_ok(p); }
AINL u32 z_conv(double zz) { return zz <= 0 ? 0 : (zz >= 4294967295.0 ? 0xFFFFFFFFu : (u32)zz); }
/* Specialised copies: a spec S (SP_ON and the flags) makes the pipeline's main
 * switches constants, so each copy has only its own steps; S = 0 reads them
 * all from the command. */
#define SP_ON 1
#define SP_TME 2
#define SP_BIL 4      /* bilinear on level 0 only (else tsample decides) */
#define SP_ABE 8
#define SP_ZT 16      /* a depth test (ZTST 2 or 3) */
#define SP_ZW 32      /* Z can be written (ZTE, no ZMSK) */
#define SPF(S, bit, rt) (((S) & SP_ON) ? (((S) & (bit)) != 0) : (rt))
static inline int spec_key(const pcmd_t *p)
{
    int S = SP_ON;
    if (p->tme) {
        int lin = (p->mmin == 1 || p->mmin == 4 || p->mmin == 5);
        S |= SP_TME;
        if ((p->mxl == 0 || p->mmin < 2) && lin == p->mmag && lin) S |= SP_BIL;
    }
    if (p->abe) S |= SP_ABE;
    if (p->ztst != 1) S |= SP_ZT;
    if (p->zte && !p->zmsk) S |= SP_ZW;
    return S;
}
#define SPEC_LIST(M) SL_T(M, SP_ON) SL_T(M, SP_ON | SP_TME) SL_T(M, SP_ON | SP_TME | SP_BIL)
#define SL_T(M, b) SL_A(M, b) SL_A(M, b | SP_ABE)
#define SL_A(M, b) SL_Z(M, b) SL_Z(M, b | SP_ZT)
#define SL_Z(M, b) M(b) M(b | SP_ZW)
AINL void pixel_fast(u32 *__restrict vr, const pcmd_t *p, const rowaddr_t *ra, int x, u32 z, int r, int g, int b, int a, float u, float v, float q, int f, const int S)
{
    u32 off = ra->rowoff + ((u32)x >> 6) * 32, blk = ra->brow[((u32)x >> 3) & 7], col = ra->crow[x & 7];
    u32 zaddr = (((p->zbp + off + (blk ^ 0x18)) & 0x3FFF) << 6) | col;
    if (SPF(S, SP_ZT, p->ztst != 1)) {
        if (p->ztst == 0) return;
        u32 zd = vr[zaddr], zs = z;
        if (p->zpsm == 0x31) { zs &= 0xFFFFFF; zd &= 0xFFFFFF; }
        if (p->ztst == 2 ? zs < zd : zs <= zd) return;
    }
    if (SPF(S, SP_TME, p->tme)) {
        u32 t = (S & SP_BIL) ? tsample_lvl(p, 0, u, v, 1) : tsample(p, u, v, q);
        int tr = t & 255, tg = (t >> 8) & 255, tb = (t >> 16) & 255, ta = t >> 24;
        switch (p->tfx) {
        case 0: r = (tr * r) >> 7; g = (tg * g) >> 7; b = (tb * b) >> 7; a = p->tcc ? (ta * a) >> 7 : a; break;
        case 1: r = tr; g = tg; b = tb; a = p->tcc ? ta : a; break;
        case 2: r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a; a = p->tcc ? ta + a : a; break;
        default: r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a; a = p->tcc ? ta : a; break;
        }
        r = clamp255(r); g = clamp255(g); b = clamp255(b); a = clamp255(a);
    }
    if (p->fge) {
        r = (r * f + p->fogr * (255 - f)) >> 8; g = (g * f + p->fogg * (255 - f)) >> 8; b = (b * f + p->fogb * (255 - f)) >> 8;
    }
    int write_fb = 1, write_z = SPF(S, SP_ZW, p->zte && !p->zmsk), write_a = 1;
    if (p->ate) {
        int pass;
        switch (p->atst) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = a < (int)p->aref; break;
        case 3: pass = a <= (int)p->aref; break;
        case 4: pass = a == (int)p->aref; break;
        case 5: pass = a >= (int)p->aref; break;
        case 6: pass = a > (int)p->aref; break;
        default: pass = a != (int)p->aref; break;
        }
        if (!pass) {
            switch (p->afail) {
            case 0: return;
            case 1: write_z = 0; break;
            case 2: write_fb = 0; break;
            case 3: write_z = 0; write_a = 0; break;
            }
        }
    }
    if (write_fb) {
        u32 faddr = (((p->fbp + off + blk) & 0x3FFF) << 6) | col;
        int da = 0;
        if (SPF(S, SP_ABE, p->abe)) {
            u32 old = vr[faddr];
            int cs[3] = {r, g, b}, cd[3] = {(int)(old & 255), (int)((old >> 8) & 255), (int)((old >> 16) & 255)}, o[3];
            da = (int)(old >> 24);
            int cc = p->bc == 0 ? a : (p->bc == 1 ? da : (int)p->fix);
            for (int k = 0; k < 3; k++) {
                int A = p->ba == 0 ? cs[k] : (p->ba == 1 ? cd[k] : 0);
                int B = p->bb == 0 ? cs[k] : (p->bb == 1 ? cd[k] : 0);
                int D = p->bd == 0 ? cs[k] : (p->bd == 1 ? cd[k] : 0);
                o[k] = (((A - B) * cc) >> 7) + D;
            }
            r = o[0]; g = o[1]; b = o[2];
        } else if (!write_a) da = (int)(vr[faddr] >> 24);
        if (p->clampc) { r = clamp255(r); g = clamp255(g); b = clamp255(b); }
        else { r &= 255; g &= 255; b &= 255; }
        if (p->fba) a |= 0x80;
        if (!write_a) a = da;
        vr[faddr] = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)(a & 255) << 24);
    }
    if (write_z) {
        if (p->zpsm == 0x30) vr[zaddr] = z;
        else vr[zaddr] = (vr[zaddr] & 0xFF000000u) | (z & 0xFFFFFF);
    }
}

/* a triangle's attributes as planes (value = dx * x + dy * y + c) */
typedef struct { float rdx, rdy, rc, gdx, gdy, gc, bdx, bdy, bc, adx, ady, ac, fdx, fdy, fc, udx, udy, uc, vdx, vdy, vc, qdx, qdy, qc; double zA, zB, zC; int fr, fg, fb, fa; } tplanes_t;
/* a row of a triangle (x from xs to xe) and of a sprite, through pixel_fast; a
 * row starts each attribute from its plane (only those the pixels use) */
AINL void tri_row(const pcmd_t *p, int y, int xs, int xe, const tplanes_t *P, const int S)
{
    rowaddr_t ra; rowaddr(p, y, &ra);
    u32 *__restrict vr = vram;   /* GS memory is not the command's memory */
    const int need_z = (S & SP_ON) ? (S & (SP_ZT | SP_ZW)) != 0 : z_used(p), tme = SPF(S, SP_TME, p->tme), persp = tme && !p->fst, iip = p->iip;
    const float fx = (float)xs, py = (float)y;
    const float rdx = P->rdx, gdx = P->gdx, bdx = P->bdx, adx = P->adx, fdx = P->fdx, udx = P->udx, vdx = P->vdx, qdx = P->qdx;
    const double zA = P->zA;
    const int fr = P->fr, fg = P->fg, fb = P->fb, fa = P->fa;
    float r = 0, g = 0, b = 0, a = 0, f = 0, u = 0, v = 0, q = 0;
    double zz = 0;
    if (iip) { r = rdx * fx + P->rdy * py + P->rc; g = gdx * fx + P->gdy * py + P->gc; b = bdx * fx + P->bdy * py + P->bc; a = adx * fx + P->ady * py + P->ac; }
    if (p->fge) f = fdx * fx + P->fdy * py + P->fc;
    if (tme) { u = udx * fx + P->udy * py + P->uc; v = vdx * fx + P->vdy * py + P->vc; q = qdx * fx + P->qdy * py + P->qc; }
    if (need_z) zz = zA * xs + P->zB * y + P->zC;
    for (int x = xs; x <= xe; x++) {
        u32 z = need_z ? z_conv(zz) : 0;
        float tu = u, tv = v;
        if (persp && q != 0) { float iq = 1.0f / q; tu = u * iq; tv = v * iq; }
        if (iip) pixel_fast(vr, p, &ra, x, z, (int)r, (int)g, (int)b, (int)a, tu, tv, q, (int)f, S);
        else pixel_fast(vr, p, &ra, x, z, fr, fg, fb, fa, tu, tv, q, (int)f, S);
        r += rdx; g += gdx; b += bdx; a += adx; f += fdx; u += udx; v += vdx; q += qdx; zz += zA;
    }
}
AINL void sprite_row(const pcmd_t *p, int y, int ix0, int ix1, u32 z, int cr, int cg, int cb, int ca, int cf, float u0, float x0, float du, float vv, float q, const int S)
{
    rowaddr_t ra; rowaddr(p, y, &ra);
    u32 *__restrict vr = vram;   /* GS memory is not the command's memory */
    for (int x = ix0; x < ix1; x++)
        pixel_fast(vr, p, &ra, x, z, cr, cg, cb, ca, u0 + (x - x0) * du, vv, q, cf, S);
}

/* rows a worker draws */
#define ROW_MINE(y) (nth == 1 || (((u32)(y) >> 2) % (u32)nth) == (u32)tid)

static void vtx_uvq(const pcmd_t *p, const vtx_t *v, float *u, float *w, float *q)
{
    if (p->fst) { *u = v->u; *w = v->v; *q = 1.0f; }
    else { *u = v->s * p->tw; *w = v->t * p->th; *q = v->q; }
}

static void r_sprite(const pcmd_t *p, const vtx_t *V_, int tid, int nth)
{
    const vtx_t *a = &V_[0], *b = &V_[1];
    float x0 = a->x, x1 = b->x, y0 = a->y, y1 = b->y;
    float u0, v0, q0, u1, v1, q1;
    vtx_uvq(p, a, &u0, &v0, &q0);
    vtx_uvq(p, b, &u1, &v1, &q1);
    if (!p->fst) { u0 /= q1; v0 /= q1; u1 /= q1; v1 /= q1; }     /* sprites use the last Q */
    float q = p->fst ? 1.0f : q1;
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = u0; u0 = u1; u1 = t; }
    if (y0 > y1) { float t = y0; y0 = y1; y1 = t; t = v0; v0 = v1; v1 = t; }
    int ix0 = (int)ceilf(x0), ix1 = (int)ceilf(x1), iy0 = (int)ceilf(y0), iy1 = (int)ceilf(y1);
    if (ix0 < p->sx0) ix0 = p->sx0;
    if (iy0 < p->sy0) iy0 = p->sy0;
    if (ix1 > p->sx1 + 1) ix1 = p->sx1 + 1;
    if (iy1 > p->sy1 + 1) iy1 = p->sy1 + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > 2048) ix1 = 2048;
    if (iy1 > 2048) iy1 = 2048;
    float du = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0, dv = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0;
    if (fast_ok(p)) {
        if (p->ztst == 0) return;
        u32 z = b->z; int cr = b->r, cg = b->g, cb = b->b, ca = b->a, cf = b->f, S = spec_key(p);
        for (int y = iy0; y < iy1; y++) {
            if (!ROW_MINE(y)) continue;
            float vv = v0 + (y - y0) * dv;
            switch (S) {
#define SR(k) case k: sprite_row(p, y, ix0, ix1, z, cr, cg, cb, ca, cf, u0, x0, du, vv, q, k); break;
            SPEC_LIST(SR)
#undef SR
            default: sprite_row(p, y, ix0, ix1, z, cr, cg, cb, ca, cf, u0, x0, du, vv, q, 0); break;
            }
        }
        return;
    }
    for (int y = iy0; y < iy1; y++) {
        if (!ROW_MINE(y)) continue;
        float vv = v0 + (y - y0) * dv;
        for (int x = ix0; x < ix1; x++)
            pixel(p, x, y, b->z, b->r, b->g, b->b, b->a, u0 + (x - x0) * du, vv, q, b->f);
    }
}

static void r_tri(const pcmd_t *p, const vtx_t *V_, int tid, int nth)
{
    const vtx_t *v0 = &V_[0], *v1 = &V_[1], *v2 = &V_[2];
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
    if (fabsf(area) < 1e-6f) return;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int ix0 = (int)ceilf(minx), ix1 = (int)ceilf(maxx), iy0 = (int)ceilf(miny), iy1 = (int)ceilf(maxy);
    if (ix0 < p->sx0) ix0 = p->sx0;
    if (iy0 < p->sy0) iy0 = p->sy0;
    if (ix1 > p->sx1 + 1) ix1 = p->sx1 + 1;
    if (iy1 > p->sy1 + 1) iy1 = p->sy1 + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > 2048) ix1 = 2048;
    if (iy1 > 2048) iy1 = 2048;
    if (ix0 >= ix1 || iy0 >= iy1) return;
    /* barycentric weights as planes: w_k = A_k x + B_k y + C_k */
    float inv = 1.0f / area;
    const vtx_t *V[3] = {v0, v1, v2};
    float A[3], B[3], C[3];
    for (int k = 0; k < 3; k++) {
        const vtx_t *a = V[(k + 1) % 3], *b = V[(k + 2) % 3];
        /* w_k(px,py) = ((a.x - px)(b.y - py) - (b.x - px)(a.y - py)) / area */
        A[k] = (a->y - b->y) * inv;
        B[k] = (b->x - a->x) * inv;
        C[k] = (a->x * b->y - b->x * a->y) * inv;
    }
    /* attribute planes: value = dx * px + dy * py + c */
#define PLANE(get, dx, dy, c0) do { float a0_ = (get(v0)), a1_ = (get(v1)), a2_ = (get(v2)); \
        dx = a0_ * A[0] + a1_ * A[1] + a2_ * A[2]; dy = a0_ * B[0] + a1_ * B[1] + a2_ * B[2]; c0 = a0_ * C[0] + a1_ * C[1] + a2_ * C[2]; } while (0)
    float rdx = 0, rdy = 0, rc = 0, gdx = 0, gdy = 0, gc = 0, bdx = 0, bdy = 0, bc_ = 0, adx = 0, ady = 0, ac = 0, fdx, fdy, fc;
    if (p->iip) {
#define GR(v) (float)(v)->r
#define GG(v) (float)(v)->g
#define GB(v) (float)(v)->b
#define GA(v) (float)(v)->a
        PLANE(GR, rdx, rdy, rc); PLANE(GG, gdx, gdy, gc); PLANE(GB, bdx, bdy, bc_); PLANE(GA, adx, ady, ac);
    }
#define GF(v) (float)(v)->f
    PLANE(GF, fdx, fdy, fc);
    /* z in double: 32-bit depths need more than a float's precision */
    double zA = 0, zB = 0, zC = 0;
    if (!fast_ok(p) || z_used(p)) {
        double d[3] = {v0->z, v1->z, v2->z};
        double Ad[3], Bd[3], Cd[3], invd = 1.0 / ((double)(v1->x - v0->x) * (v2->y - v0->y) - (double)(v2->x - v0->x) * (v1->y - v0->y));
        for (int k = 0; k < 3; k++) {
            const vtx_t *a = V[(k + 1) % 3], *b = V[(k + 2) % 3];
            Ad[k] = ((double)a->y - b->y) * invd; Bd[k] = ((double)b->x - a->x) * invd; Cd[k] = ((double)a->x * b->y - (double)b->x * a->y) * invd;
        }
        for (int k = 0; k < 3; k++) { zA += d[k] * Ad[k]; zB += d[k] * Bd[k]; zC += d[k] * Cd[k]; }
    }
    float udx = 0, udy = 0, uc = 0, vdx = 0, vdy = 0, vc = 0, qdx = 0, qdy = 0, qc = 1;
    if (p->tme) {
        float uu[3], vv[3], qq[3];
        for (int k = 0; k < 3; k++) vtx_uvq(p, V[k], &uu[k], &vv[k], &qq[k]);
        float a0_, a1_, a2_;
        a0_ = uu[0]; a1_ = uu[1]; a2_ = uu[2];
        udx = a0_ * A[0] + a1_ * A[1] + a2_ * A[2]; udy = a0_ * B[0] + a1_ * B[1] + a2_ * B[2]; uc = a0_ * C[0] + a1_ * C[1] + a2_ * C[2];
        a0_ = vv[0]; a1_ = vv[1]; a2_ = vv[2];
        vdx = a0_ * A[0] + a1_ * A[1] + a2_ * A[2]; vdy = a0_ * B[0] + a1_ * B[1] + a2_ * B[2]; vc = a0_ * C[0] + a1_ * C[1] + a2_ * C[2];
        if (!p->fst) {
            a0_ = qq[0]; a1_ = qq[1]; a2_ = qq[2];
            qdx = a0_ * A[0] + a1_ * A[1] + a2_ * A[2]; qdy = a0_ * B[0] + a1_ * B[1] + a2_ * B[2]; qc = a0_ * C[0] + a1_ * C[1] + a2_ * C[2];
        }
    }
    /* Pixel centres on an edge belong to one triangle only (the GS's top-left
     * rule): to the triangle that edge is a left edge of (w grows with x), or a
     * top edge (horizontal, w grows with y). Drawing them twice put a darker or
     * brighter seam on every shared edge of blended geometry (the diagonal of
     * a quad). eps[k] is the weight a pixel must reach for edge k. */
    float eps[3];
    for (int k = 0; k < 3; k++) eps[k] = (A[k] > 0 || (A[k] == 0 && B[k] > 0)) ? -1e-5f : 1e-5f;
    const int fast = fast_ok(p), S = fast ? spec_key(p) : 0;
    if (fast && p->ztst == 0) return;          /* nothing passes */
    tplanes_t P = { rdx, rdy, rc, gdx, gdy, gc, bdx, bdy, bc_, adx, ady, ac, fdx, fdy, fc, udx, udy, uc, vdx, vdy, vc, qdx, qdy, qc, zA, zB, zC,
                    V_[2].r, V_[2].g, V_[2].b, V_[2].a };
    float iA[3];
    for (int k = 0; k < 3; k++) iA[k] = A[k] != 0 ? 1.0f / A[k] : 0;
    for (int y = iy0; y < iy1; y++) {
        if (!ROW_MINE(y)) continue;
        float py = (float)y;
        /* the row's span: every w_k >= eps[k] */
        float lo = (float)ix0, hi = (float)(ix1 - 1);
        int empty = 0;
        for (int k = 0; k < 3; k++) {
            float rest = B[k] * py + C[k];
            if (A[k] > 0) { float bnd = (eps[k] - rest) * iA[k]; if (bnd > lo) lo = bnd; }
            else if (A[k] < 0) { float bnd = (eps[k] - rest) * iA[k]; if (bnd < hi) hi = bnd; }
            else if (rest < eps[k]) { empty = 1; break; }
        }
        if (empty) continue;
        int xs = (int)ceilf(lo), xe = (int)floorf(hi);
        if (xs < ix0) xs = ix0;
        if (xe > ix1 - 1) xe = ix1 - 1;
        /* the bounds are exact to rounding: check the end pixels with the weights */
        while (xs <= xe) {
            float px = (float)xs;
            if (A[0] * px + B[0] * py + C[0] >= eps[0] && A[1] * px + B[1] * py + C[1] >= eps[1] && A[2] * px + B[2] * py + C[2] >= eps[2]) break;
            xs++;
        }
        while (xe >= xs) {
            float px = (float)xe;
            if (A[0] * px + B[0] * py + C[0] >= eps[0] && A[1] * px + B[1] * py + C[1] >= eps[1] && A[2] * px + B[2] * py + C[2] >= eps[2]) break;
            xe--;
        }
        if (xs > xe) continue;
        if (fast) {
            switch (S) {
#define TR(k) case k: tri_row(p, y, xs, xe, &P, k); break;
            SPEC_LIST(TR)
#undef TR
            default: tri_row(p, y, xs, xe, &P, 0); break;
            }
            continue;
        }
        float fx = (float)xs;
        float r = rdx * fx + rdy * py + rc, g = gdx * fx + gdy * py + gc, b = bdx * fx + bdy * py + bc_, a = adx * fx + ady * py + ac;
        float f = fdx * fx + fdy * py + fc;
        float u = udx * fx + udy * py + uc, v = vdx * fx + vdy * py + vc, q = qdx * fx + qdy * py + qc;
        double zz = zA * xs + zB * y + zC;
        int fr = V_[2].r, fg = V_[2].g, fb = V_[2].b, fa = V_[2].a;
        for (int x = xs; x <= xe; x++) {
            u32 z = zz <= 0 ? 0 : (zz >= 4294967295.0 ? 0xFFFFFFFFu : (u32)zz);
            float tu = u, tv = v;
            if (p->tme && !p->fst && q != 0) { float iq = 1.0f / q; tu = u * iq; tv = v * iq; }
            if (p->iip) pixel(p, x, y, z, (int)r, (int)g, (int)b, (int)a, tu, tv, q, (int)f);
            else pixel(p, x, y, z, fr, fg, fb, fa, tu, tv, q, (int)f);
            r += rdx; g += gdx; b += bdx; a += adx; f += fdx; u += udx; v += vdx; q += qdx; zz += zA;
        }
    }
#undef PLANE
}

static void r_line(const pcmd_t *p, const vtx_t *V_, int tid, int nth)
{
    const vtx_t *a = &V_[0], *b = &V_[1];
    float dx = b->x - a->x, dy = b->y - a->y;
    int n = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (n <= 0) n = 1;
    float ua, va, qa, ub, vb, qb;
    vtx_uvq(p, a, &ua, &va, &qa);
    vtx_uvq(p, b, &ub, &vb, &qb);
    if (!p->fst) { ua /= qa; va /= qa; ub /= qb; vb /= qb; }
    float q = p->fst ? 1.0f : qb;
    for (int i = 0; i < n; i++) {
        float t = (float)i / n;
        int x = (int)floorf(a->x + dx * t + 0.5f), y = (int)floorf(a->y + dy * t + 0.5f);
        if (x < p->sx0 || x > p->sx1 || y < p->sy0 || y > p->sy1 || x < 0 || y < 0 || x > 2047 || y > 2047 || !ROW_MINE(y)) continue;
        pixel(p, x, y, b->z, (int)(a->r + (b->r - a->r) * t), (int)(a->g + (b->g - a->g) * t), (int)(a->b + (b->b - a->b) * t),
              (int)(a->a + (b->a - a->a) * t), ua + (ub - ua) * t, va + (vb - va) * t, q, b->f);
    }
}

static void r_point(const pcmd_t *p, const vtx_t *V_, int tid, int nth)
{
    const vtx_t *v = &V_[0];
    int x = (int)floorf(v->x), y = (int)floorf(v->y);
    if (x < p->sx0 || x > p->sx1 || y < p->sy0 || y > p->sy1 || x < 0 || y < 0 || x > 2047 || y > 2047 || !ROW_MINE(y)) return;
    float u, w, q;
    vtx_uvq(p, v, &u, &w, &q);
    if (!p->fst && q != 0) { u /= q; w /= q; }
    pixel(p, x, y, v->z, v->r, v->g, v->b, v->a, u, w, q, v->f);
}

static void run_cmd(const pcmd_t *p, const pprim_t *pr, int tid, int nth)
{
    ptrace_prim = pr;
    if (nth > 1 && !(dbg_drain_all & 16)) {   /* skip a primitive none of whose rows (4-row bands, dealt round-robin) are this thread's */
        const vtx_t *v = pr->v;
        int nv = pr->type == 3 ? 3 : (pr->type == 0 ? 1 : 2);
        float y0 = v[0].y, y1 = v[0].y;
        for (int k = 1; k < nv; k++) { if (v[k].y < y0) y0 = v[k].y; if (v[k].y > y1) y1 = v[k].y; }
        int a = (int)floorf(y0) - 1, b = (int)ceilf(y1) + 1;
        if (a < p->sy0) a = p->sy0;
        if (b > p->sy1) b = p->sy1;
        if (a < 0) a = 0;
        if (b > 2047) b = 2047;
        if (a > b) return;
        int b0 = a >> 2, b1 = b >> 2;
        if (b1 - b0 + 1 < nth) {
            int mine = 0;
            for (int k = b0; k <= b1; k++) if (k % nth == tid) { mine = 1; break; }
            if (!mine) return;
        }
    }
    switch (pr->type) {
    case 0: r_point(p, pr->v, tid, nth); break;
    case 1: r_line(p, pr->v, tid, nth); break;
    case 3: r_tri(p, pr->v, tid, nth); break;
    case 6: r_sprite(p, pr->v, tid, nth); break;
    }
}

/* ---- worker threads ---- */
#define QN 8192
#define SN (QN * 2)
static pprim_t *cq;                      /* ring of primitives */
static pcmd_t *sring;                    /* ring of states the primitives use (a new one only when the state changes) */
static u32 s_head;
/* the counters on cache lines of their own: the producer writes q_head and
 * each worker its q_tail all the time, and sharing a line makes every write
 * take the line from the other cores */
typedef struct { volatile u32 v; char pad_[60]; } __attribute__((aligned(64))) line_u32;
static line_u32 q_head_;                 /* written by the producer */
static line_u32 q_tail_[16];             /* each worker's progress */
#define q_head (q_head_.v)
static int nworkers;
static volatile int gs_quit;
static int dbg_race(u32 d0, u32 d1, const char *what, u32 bp)
{
    if (!nworkers || !sring || !cq) return 0;
    u32 lo = q_head; for (int i = 0; i < nworkers; i++) { u32 t = q_tail_[i].v; if (q_head - t > q_head - lo) lo = t; }
    for (u32 q = lo; q != q_head; q++) {
        const pcmd_t *S = &sring[cq[q % QN].st % SN];
        u32 f0 = S->fbp / 32, f1 = (S->fbp + area_blocks(S->fpsm, S->fbw, S->sy1 + 1) - 1) / 32;
        if (f0 <= d1 && d0 <= f1) { uya_log("RACE: %s at %04X (pages %u-%u) while prim %u draws FBP %04X (pages %u-%u), %u queued", what, bp, d0, d1, q, S->fbp, f0, f1, q_head - lo); gs_drain(); return 1; }
    }
    return 0;
}

#ifdef _WIN32
#include <windows.h>
static void cpu_relax(int n) { if (n < 64) __asm__ volatile("pause"); else Sleep(0); }
#else
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
static void cpu_relax(int n) { if (n < 64) __asm__ volatile("pause"); else sched_yield(); }
#include <semaphore.h>
#include <time.h>
#endif

/* parking an idle thread (so idle drawing threads leave the cores to the
 * game and the GS thread) and waking it */
typedef struct __attribute__((aligned(64))) {
    volatile int sleeping;
#ifdef _WIN32
    HANDLE ev;
#else
    sem_t sem;
#endif
} park_t;
static void park_init(park_t *k)
{
    k->sleeping = 0;
#ifdef _WIN32
    k->ev = CreateEventA(0, FALSE, FALSE, 0);
#else
    sem_init(&k->sem, 0, 0);
#endif
}
static void park_wait(park_t *k)             /* returns on a wake-up or after 2 ms */
{
#ifdef _WIN32
    WaitForSingleObject(k->ev, 2);
#else
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ts.tv_sec++; }
    while (sem_timedwait(&k->sem, &ts) == -1 && errno == EINTR) ;
#endif
}
static void park_wake(park_t *k)
{
    if (!__atomic_load_n(&k->sleeping, __ATOMIC_ACQUIRE)) return;
#ifdef _WIN32
    SetEvent(k->ev);
#else
    sem_post(&k->sem);
#endif
}
static park_t wpark[16];

static void worker_loop(int tid)
{
    u32 t = 0;
    int spin = 0;
    for (;;) {
        u32 h = __atomic_load_n(&q_head, __ATOMIC_ACQUIRE);
        if (t == h) {
            if (spin++ < 2000) { __asm__ volatile("pause"); continue; }
            __atomic_store_n(&wpark[tid].sleeping, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&q_head, __ATOMIC_SEQ_CST) == t) park_wait(&wpark[tid]);
            __atomic_store_n(&wpark[tid].sleeping, 0, __ATOMIC_RELEASE);
            continue;
        }
        spin = 0;
        while (t != h) { const pprim_t *pr = &cq[t % QN]; run_cmd(&sring[pr->st % SN], pr, tid, nworkers); t++; if ((t & 255) == 0) __atomic_store_n(&q_tail_[tid].v, t, __ATOMIC_RELEASE); }
        __atomic_store_n(&q_tail_[tid].v, t, __ATOMIC_RELEASE);
    }
}
#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID a) { worker_loop((int)(intptr_t)a); return 0; }
#else
static void *worker_main(void *a) { worker_loop((int)(intptr_t)a); return 0; }
#endif

static void mtgs_init(int cores);
static void gs_threads_init(void)
{
    { const char *d = getenv("UYA_DRAINALL"); dbg_drain_all = d ? atoi(d) : 0; }
    { const char *t = getenv("UYA_PIXTRACE"); if (t && sscanf(t, "%d,%d,%x,%u", &ptrace_x, &ptrace_y, &ptrace_fbp, &ptrace_from) >= 3) ptrace_on = 1; }
    const char *e = getenv("UYA_GS_THREADS");
    int n;
    if (e) n = atoi(e);
    else {
#ifdef _WIN32
        SYSTEM_INFO si; GetSystemInfo(&si); n = (int)si.dwNumberOfProcessors - 1;
#else
        n = (int)sysconf(_SC_NPROCESSORS_ONLN) - 1;
#endif
        if (n > 8) n = 8;
    }
    {
        int cores;
#ifdef _WIN32
        SYSTEM_INFO si2; GetSystemInfo(&si2); cores = (int)si2.dwNumberOfProcessors;
#else
        cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
        mtgs_init(cores);
        if (mtgs && !e && n > 1) n--;     /* the GS thread takes a core */
    }
    if (n < 1) n = 0;
    if (n > 16) n = 16;
    if (n == 1) n = 0;                    /* one worker gains nothing over drawing here */
    if (gsgl_on) n = 0;                   /* the GPU draws */
    nworkers = n;
    if (!n) return;
    cq = malloc(sizeof(pprim_t) * QN);
    for (int i = 0; i < n; i++) park_init(&wpark[i]);
    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        CreateThread(0, 0, worker_main, (LPVOID)(intptr_t)i, 0, 0);
#else
        pthread_t th; pthread_create(&th, 0, worker_main, (void *)(intptr_t)i); pthread_detach(th);
#endif
    }
    uya_log("GS: drawing on %d threads", n);
}

static inline int gs_busy(void)
{
    for (int i = 0; i < nworkers; i++) if (__atomic_load_n(&q_tail_[i].v, __ATOMIC_ACQUIRE) != q_head) return 1;
    return 0;
}
/* wait until the workers have drawn everything queued */
static void retire_free(int all);
static void gs_drain(void)
{
    if (!nworkers) return;
    if (gs_busy()) n_drain_wait++;
    int spin = 0;
    while (gs_busy()) cpu_relax(spin++);
    retire_free(1);
    memset(page_drawn, 0, sizeof page_drawn);
    set_drawn_target();
}
static struct { void *px; u32 head; size_t n; } retired[4096];
static u32 n_retired;
static void retire_free(int all)
{
    u32 lo = q_head;
    if (!all) for (int i = 0; i < nworkers; i++) { u32 t = __atomic_load_n(&q_tail_[i].v, __ATOMIC_ACQUIRE); if (q_head - t > q_head - lo) lo = t; }
    u32 k = 0;
    for (u32 i = 0; i < n_retired; i++) {
        if ((s32)(lo - retired[i].head) >= 0) { if (dbg_drain_all & 4) { u32 *q = retired[i].px; for (size_t j = 0; j < retired[i].n / 4; j++) q[j] = 0xFFFF00FFu; static void *pz[64]; static u32 npz; if (pz[npz & 63]) free(pz[npz & 63]); pz[npz++ & 63] = q; } else tex_free(retired[i].px, retired[i].n); }
        else retired[k++] = retired[i];
    }
    n_retired = k;
}
static void retire(void *px)
{
    st_dirty_force();
    if (!nworkers) { tex_free(px, retire_n); return; }
    if (n_retired == 4096) retire_free(0);
    if (n_retired == 4096) { gs_drain(); }
    retired[n_retired].px = px; retired[n_retired].head = q_head; retired[n_retired].n = retire_n; n_retired++;
}

static void submit(const pprim_t *p)
{
    if (!nworkers) {
        if (!uya_timing) { run_cmd(&sring[p->st % SN], p, 0, 1); return; }
        unsigned long long t_ = rdtsc_(); run_cmd(&sring[p->st % SN], p, 0, 1); t_draw += rdtsc_() - t_; return;
    }
    u32 h = q_head;
    int spin = 0;
    for (;;) {                            /* wait for room: no worker may be QN behind */
        int full = 0;
        for (int i = 0; i < nworkers; i++) if (h - __atomic_load_n(&q_tail_[i].v, __ATOMIC_ACQUIRE) >= QN - 1) full = 1;
        if (!full) break;
        cpu_relax(spin++);
    }
    cq[h % QN] = *p;
    __atomic_store_n(&q_head, h + 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < nworkers; i++) park_wake(&wpark[i]);
}

/* the command for the current primitive: pipeline state and texture levels */
static int cmd_setup(pcmd_t *p)
{
    u64 pr = prmodecont ? prim : ((prim & 7) | (prmode & ~7ull));
    const gs_ctx_t *c = &ctx[BITS(pr, 9, 1)];
    memset(p, 0, sizeof *p);
    p->tme = BITS(pr, 4, 1); p->fge = BITS(pr, 5, 1); p->abe = BITS(pr, 6, 1); p->fst = BITS(pr, 8, 1); p->iip = BITS(pr, 3, 1);
    p->fbp = BITS(c->frame, 0, 9) * 32; p->fbw = BITS(c->frame, 16, 6); p->fpsm = BITS(c->frame, 24, 6); p->fbmsk = (u32)(c->frame >> 32);
    p->zbp = BITS(c->zbuf, 0, 9) * 32; p->zpsm = BITS(c->zbuf, 24, 4) | 0x30; p->zmsk = BITS(c->zbuf, 32, 1);
    p->ate = BITS(c->test, 0, 1); p->atst = BITS(c->test, 1, 3); p->aref = BITS(c->test, 4, 8); p->afail = BITS(c->test, 12, 2);
    p->zte = BITS(c->test, 16, 1); p->ztst = BITS(c->test, 17, 2);
    p->date = BITS(c->test, 14, 1); p->datm = BITS(c->test, 15, 1);
    p->ba = BITS(c->alpha, 0, 2); p->bb = BITS(c->alpha, 2, 2); p->bc = BITS(c->alpha, 4, 2); p->bd = BITS(c->alpha, 6, 2); p->fix = BITS(c->alpha, 32, 8);
    p->tfx = BITS(c->tex0, 35, 2); p->tcc = BITS(c->tex0, 34, 1); p->fba = BITS(c->fba, 0, 1); p->clampc = BITS(colclamp, 0, 1);
    p->sx0 = (int)BITS(c->scissor, 0, 11); p->sx1 = (int)BITS(c->scissor, 16, 11);
    p->sy0 = (int)BITS(c->scissor, 32, 11); p->sy1 = (int)BITS(c->scissor, 48, 11);
    p->tw = (float)(1u << BITS(c->tex0, 26, 4)); p->th = (float)(1u << BITS(c->tex0, 30, 4));
    p->fogr = (u8)BITS(fogcol, 0, 8); p->fogg = (u8)BITS(fogcol, 8, 8); p->fogb = (u8)BITS(fogcol, 16, 8);
    if (!p->zte) { p->ztst = 1; }          /* depth test off: always pass */
    prim_serial++;
    {   /* the pages this target writes count as changed (texture cache) */
        static u64 lf, lz, ls; static int lzte;
        if (c->frame != lf || c->zbuf != lz || c->scissor != ls || (int)p->zte != lzte) {
            u32 h = (u32)p->sy1 + 1;
            if (lf) mark_blocks((u32)BITS(lf, 0, 9) * 32, area_blocks(BITS(lf, 24, 6), BITS(lf, 16, 6), (u32)BITS(ls, 48, 11) + 1));
            mark_blocks(p->fbp, area_blocks(p->fpsm, p->fbw, h)); n_mark_draw++;
            if (p->zte) mark_blocks(p->zbp, area_blocks(p->zpsm, p->fbw, h));
            cur_target[0] = p->fbp; cur_target[1] = area_blocks(p->fpsm, p->fbw, h);
            cur_target[2] = p->zbp; cur_target[3] = (p->zte || !p->zmsk) ? area_blocks(p->zpsm, p->fbw, h) : 0;
            cur_target[4] = p->fbw | (p->fpsm << 8); cur_target[5] = p->fbw | (p->zpsm << 8);
            if (layout_conflict(cur_target[0], cur_target[1], (u16)cur_target[4]) ||
                (cur_target[3] && layout_conflict(cur_target[2], cur_target[3], (u16)cur_target[5]))) { n_layout_drain++; gs_drain(); }
            set_drawn_target();
            lf = c->frame; lz = c->zbuf; ls = c->scissor; lzte = (int)p->zte;
        }
    }
    if (p->tme) {
        u64 t1 = c->tex1;
        p->lcm = BITS(t1, 0, 1); p->mxl = BITS(t1, 2, 3); p->mmag = BITS(t1, 5, 1); p->mmin = BITS(t1, 6, 3); p->l = BITS(t1, 19, 2);
        if (p->mxl > 6) p->mxl = 6;
        p->k = (float)((s32)(BITS(t1, 32, 12) << 20) >> 20) / 16.0f;
        p->wms = BITS(c->clamp, 0, 2); p->wmt = BITS(c->clamp, 2, 2);
        p->minu = (int)BITS(c->clamp, 4, 10); p->maxu = (int)BITS(c->clamp, 14, 10);
        p->minv = (int)BITS(c->clamp, 24, 10); p->maxv = (int)BITS(c->clamp, 34, 10);
        p->tex0 = c->tex0; p->tex1 = c->tex1; p->miptbp1 = c->miptbp1; p->miptbp2 = c->miptbp2;
        int nl = (p->mmin >= 2) ? p->mxl : 0;
        if (gsgl_on) {
            u32 tpsm_ = BITS(c->tex0, 20, 6);
            if (tpsm_ == 0x13 || tpsm_ == 0x1B || tpsm_ == 0x14 || tpsm_ == 0x24 || tpsm_ == 0x2C) clut_flush();   /* (only a paletted texture uses it) */
            p->clamp = c->clamp; p->texa = texa;
            if (gsgl_tex_setup(p)) { nl = -1; p->mxl = 0; }   /* read from a GPU target */
        }
        for (int lv = 0; lv <= nl; lv++) {
            u32 b_, bw_, lw_, lh_; tex_level(c, lv, &b_, &bw_, &lw_, &lh_);
            p->lv[lv].lw = (int)lw_; p->lv[lv].lh = (int)lh_;
            /* GL: a level in memory a GPU target owns is decoded only if a batch needs it */
            if (gsgl_on && gsgl_tex_gpu(BITS(c->tex0, 20, 6), b_, bw_, lw_, lh_)) { p->defer |= (u8)(1 << lv); continue; }
            const tcache_t *e = tc_get(c, lv);
            p->lv[lv].px = e->px; p->lv[lv].w = (int)e->w; p->lv[lv].h = (int)e->h;
            p->lv[lv].tci = (int)(e - tc); p->lv[lv].ser = e->ser;
            if (gsgl_on) gsgl_tex_touch(&p->lv[lv]);
        }
        if (nl >= 0 && nl < (int)p->mxl) p->mxl = (u8)nl;
    }
    return 1;
}

/* memcmp of two commands (cmd_setup zeroes the padding), a word at a time */
static inline int cmd_same(const pcmd_t *a, const pcmd_t *b)
{
    const u32 *x = (const u32 *)a, *y = (const u32 *)b;
    u32 d = 0;
    for (u32 i = 0; i < sizeof *a / 4; i++) d |= x[i] ^ y[i];
    return d == 0;
}
static int st_dirty = 1, st_tme;
static u32 st_epoch;
static void st_dirty_force(void) { st_dirty = 1; }
static void vertex_kick(u32 X, u32 Y, u32 Z, u8 F, int draw)
{
    u64 pr = prmodecont ? prim : ((prim & 7) | (prmode & ~7ull));
    const gs_ctx_t *c = &ctx[BITS(pr, 9, 1)];
    vtx_t v;
    v.x = ((float)X - (float)BITS(c->xyoffset, 0, 16)) / 16.0f;
    v.y = ((float)Y - (float)BITS(c->xyoffset, 32, 16)) / 16.0f;
    v.z = Z;
    v.r = (u8)BITS(rgbaq, 0, 8); v.g = (u8)BITS(rgbaq, 8, 8); v.b = (u8)BITS(rgbaq, 16, 8); v.a = (u8)BITS(rgbaq, 24, 8);
    { u32 s = (u32)st, t = (u32)(st >> 32); memcpy(&v.s, &s, 4); memcpy(&v.t, &t, 4); }
    v.q = qreg;
    v.u = (float)BITS(uv, 0, 14) / 16.0f;
    v.v = (float)BITS(uv, 16, 14) / 16.0f;
    v.f = F;
    u32 type = (u32)(prim & 7);
    static const int need[8] = {1, 2, 2, 3, 3, 3, 2, 1};
    vq[vcount++] = v;
    if (vcount < need[type]) return;
    if (draw && type != 7) {
        pprim_t pc;
        static const u8 kind[8] = {0, 1, 1, 3, 3, 3, 6, 0};
        stat_prims++;
        if (!sring) sring = calloc(SN, sizeof(pcmd_t));
        /* a new state only when a register changed, or GS memory under the texture */
        if (st_dirty || (st_tme && st_epoch != dirty_epoch)) {
            pcmd_t ns;
            cmd_setup(&ns);
            if (!s_head || !cmd_same(&ns, &sring[(s_head - 1) % SN])) { sring[s_head % SN] = ns; s_head++; }
            st_dirty = 0; st_tme = ns.tme; st_epoch = dirty_epoch;
        }
        pc.st = s_head - 1;
        {
            static int left = -1;
            if (left < 0) { const char *e = getenv("UYA_SPRLOG"); left = e ? atoi(e) : 0; }
            if (left > 0 && kind[type] == 6) {
                const pcmd_t *S = &sring[pc.st % SN];
                left--;
                uya_log("sprite (%s) (%.1f,%.1f)-(%.1f,%.1f) z %X rgba %u,%u,%u,%u uv %.1f,%.1f-%.1f,%.1f st %g,%g q %g TME %u FST %u TEX0 %016llX ABE %u ATE %u/%u ZTE %u/%u FRAME %llX", gs_src_int,
                        vq[0].x, vq[0].y, vq[1].x, vq[1].y, vq[1].z, vq[1].r, vq[1].g, vq[1].b, vq[1].a, vq[0].u, vq[0].v, vq[1].u, vq[1].v, vq[0].s, vq[0].t, vq[1].q,
                        S->tme, S->fst, (unsigned long long)ctx[BITS(pr, 9, 1)].tex0, S->abe, S->ate, S->atst, S->zte, S->ztst, (unsigned long long)ctx[BITS(pr, 9, 1)].frame);
            }
        }
        {   /* testing: UYA_PRIMLOG=v,n[,1] logs n primitives from vsync v (,1: only FST ones, the 2D overlays) */
            static int left = -1, fst_only; static unsigned from;
            if (left < 0) { const char *e = getenv("UYA_PRIMLOG"); left = 0; if (e) sscanf(e, "%u,%d,%d", &from, &left, &fst_only); }
            if (left > 0 && uya_frame_count() >= from && (!fst_only || BITS(pr, 8, 1))) {
                const pcmd_t *S = &sring[pc.st % SN];
                left--;
                uya_log("prim %u (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f) rgba %u,%u,%u,%u uv %.1f,%.1f TME %u FST %u TEX0 %016llX ABE %u ATE %u/%u/%u ZTE %u/%u FRAME %llX ALPHA %llX TEXA %llX", kind[type],
                        vq[0].x, vq[0].y, vq[1].x, vq[1].y, vq[2].x, vq[2].y, vq[0].r, vq[0].g, vq[0].b, vq[0].a, vq[0].u, vq[0].v,
                        S->tme, S->fst, (unsigned long long)ctx[BITS(pr, 9, 1)].tex0, S->abe, S->ate, S->atst, S->aref, S->zte, S->ztst, (unsigned long long)ctx[BITS(pr, 9, 1)].frame,
                        (unsigned long long)ctx[BITS(pr, 9, 1)].alpha, (unsigned long long)texa);
            }
        }
        pc.type = kind[type];
        pc.v[0] = vq[0]; pc.v[1] = vq[1]; pc.v[2] = vq[2];
        if (kind[type] == 3) {
            const vtx_t *v0 = &vq[0], *v1 = &vq[1], *v2 = &vq[2];
            float area = (v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y);
            static int left = -1;
            if (left < 0) { const char *e = getenv("UYA_BIGTRI"); left = e ? atoi(e) : 0; }
            if (left > 0 && fabsf(area) > 60000.0f) {
                left--;
                uya_log("big tri (%s): (%.1f,%.1f,%08X q %g) (%.1f,%.1f,%08X q %g) (%.1f,%.1f,%08X q %g) prim %llX", gs_src_int,
                        v0->x, v0->y, v0->z, v0->q, v1->x, v1->y, v1->z, v1->q, v2->x, v2->y, v2->z, v2->q, (unsigned long long)pr);
            }
        }
#ifdef GS_PRIM_HOOK
        GS_PRIM_HOOK(&sring[pc.st % SN], &pc, &ctx[BITS(pr, 9, 1)]);
#endif
        if (gsgl_on) gsgl_prim(&sring[pc.st % SN], pc.st, &pc);
        else submit(&pc);
    }
    switch (type) {
    case 2: vq[0] = vq[1]; vcount = 1; break;                 /* line strip */
    case 4: vq[0] = vq[1]; vq[1] = vq[2]; vcount = 2; break;  /* triangle strip */
    case 5: vq[1] = vq[2]; vcount = 2; break;                 /* fan keeps vertex 0 */
    default: vcount = 0; break;
    }
}

/* ---- transfers ---- */
static u32 trx_x, trx_y, trx_active;
static u8 trx_acc[8];
static u32 trx_nacc;

/* Uploads: games re-send the same texture to the same place every frame. An
 * upload whose data (hash) and place match the last one there, with nothing
 * else written to those pages since, leaves GS memory as it was, so it does
 * not count as a change for the texture cache. */
static u32 trx_pend, trx_rbp, trx_rn, trx_hash;
static struct { u64 key; u32 hash, sum, used; } up_rec[1024];
static u32 pages_sum(u32 bp, u32 n)
{
    u32 p0 = bp / 32, p1 = (bp + (n ? n : 1) - 1) / 32, sum = 0;
    if (p1 > 511) p1 = 511;
    for (u32 p = p0; p <= p1; p++) sum = sum * 31 + page_gen[p];
    return sum;
}
static u32 si_hash[0x4000];   /* testing: the VRAM StoreImage saved, by block (UYA_SICHECK) */
static u32 vhash(u32 bp, u32 nb) { u32 h = 2166136261u; for (u32 i = bp * 64; i < (bp + nb) * 64 && i < (1u << 20); i++) h = (h ^ vram[i]) * 16777619u; return h; }
static void trx_finish(void)
{
    {   static int on = -1; if (on < 0) on = getenv("UYA_SICHECK") != 0;
        u32 dbp = BITS(bitbltbuf, 32, 14);
        if (on && si_hash[dbp] && BITS(bitbltbuf, 48, 6) == 1) { uya_log("restore %04X: VRAM %s the saved data", dbp, (vhash(dbp, 0x40) | 1) == si_hash[dbp] ? "matches" : "DIFFERS from"); si_hash[dbp] = 0; }
    }
    trx_pend = 0;
    u64 key = (bitbltbuf >> 32) ^ ((u64)(trxpos >> 32) << 20) ^ (trxreg * 0x9E3779B97F4A7C15ull);
    u32 i = (u32)(key ^ (key >> 29)) & 1023, k;
    for (k = 0; k < 8; k++) { u32 j = (i + k) & 1023; if (up_rec[j].used && up_rec[j].key == key) { i = j; break; } }
    if (k < 8 && !(dbg_drain_all & 32) && up_rec[i].hash == trx_hash && up_rec[i].sum == pages_sum(trx_rbp, trx_rn)) { if (trxlog_on()) uya_log("   upload done (same data, %08X)", trx_hash); return; }   /* same data again */
    if (trxlog_on()) uya_log("   upload done (%08X), marks blocks %04X+%X", trx_hash, trx_rbp, trx_rn);
    if (k == 8) {   /* a free slot near i, else i */
        for (k = 0; k < 8; k++) if (!up_rec[(i + k) & 1023].used) break;
        i = (i + (k < 8 ? k : 0)) & 1023;
    }
    mark_blocks(trx_rbp, trx_rn); n_mark_trx++;
    up_rec[i].key = key; up_rec[i].hash = trx_hash; up_rec[i].sum = pages_sum(trx_rbp, trx_rn); up_rec[i].used = 1;
}

static void trx_start(u32 dir)
{
    {   /* wait for queued draws only if they write what this reads or writes */
        u32 dbp = BITS(bitbltbuf, 32, 14), dbw = BITS(bitbltbuf, 48, 6), dpsm = BITS(bitbltbuf, 56, 6);
        u32 sbp = BITS(bitbltbuf, 0, 14), sbw = BITS(bitbltbuf, 16, 6), spsm = BITS(bitbltbuf, 24, 6);
        u32 h = BITS(trxreg, 32, 12);
        if (range_drawn(dbp, area_blocks(dpsm, dbw, BITS(trxpos, 48, 11) + h)) ||
            (dir == 2 && range_drawn(sbp, area_blocks(spsm, sbw, BITS(trxpos, 16, 11) + h))) || (dbg_drain_all & 2)) gs_drain();
        else if ((dbg_drain_all & 8) && nworkers && sring) {   /* testing: a transfer over pages queued draws still write */
            u32 lo = q_head; for (int i = 0; i < nworkers; i++) { u32 t = q_tail_[i].v; if (q_head - t > q_head - lo) lo = t; }
            u32 d0 = dbp / 32, d1 = (dbp + area_blocks(dpsm, dbw, BITS(trxpos, 48, 11) + h) - 1) / 32;
            for (u32 q = lo; q != q_head; q++) {
                const pcmd_t *S = &sring[cq[q % QN].st % SN];
                u32 f0 = S->fbp / 32, f1 = (S->fbp + area_blocks(S->fpsm, S->fbw, S->sy1 + 1) - 1) / 32;
                if (f0 <= d1 && d0 <= f1) { uya_log("RACE: transfer dir %u to %04X (pages %u-%u) while prim %u draws FBP %04X (pages %u-%u), %u queued", dir, dbp, d0, d1, q, S->fbp, f0, f1, q_head - lo); gs_drain(); break; }
            }
        }
    }
    if (trxlog_on()) uya_log("TRX dir %u [%s]: %04X/%u/%X (%u,%u) -> %04X/%u/%X (%u,%u) %ux%u%s", dir, gs_src_int, (u32)BITS(bitbltbuf, 0, 14), (u32)BITS(bitbltbuf, 16, 6), (u32)BITS(bitbltbuf, 24, 6),
                             (u32)BITS(trxpos, 0, 11), (u32)BITS(trxpos, 16, 11), (u32)BITS(bitbltbuf, 32, 14), (u32)BITS(bitbltbuf, 48, 6), (u32)BITS(bitbltbuf, 56, 6),
                             (u32)BITS(trxpos, 32, 11), (u32)BITS(trxpos, 48, 11), (u32)BITS(trxreg, 0, 12), (u32)BITS(trxreg, 32, 12), trx_pend ? " (last one unfinished)" : "");
    if (gsgl_on && (dir == 0 || dir == 2)) {   /* GL: what GPU targets drew there, and their copies of it */
        u32 w = BITS(trxreg, 0, 12), h = BITS(trxreg, 32, 12);
        if (dir == 2) gsgl_cpu_read_rect(BITS(bitbltbuf, 24, 6), BITS(bitbltbuf, 0, 14), BITS(bitbltbuf, 16, 6), BITS(trxpos, 0, 11), BITS(trxpos, 16, 11), w, h);
        gsgl_cpu_write_rect(BITS(bitbltbuf, 56, 6), BITS(bitbltbuf, 32, 14), BITS(bitbltbuf, 48, 6), BITS(trxpos, 32, 11), BITS(trxpos, 48, 11), w, h);
    }
    trx_active = 0;
    if (trx_pend) { mark_blocks(trx_rbp, trx_rn); trx_pend = 0; }   /* an unfinished upload */
    if (dir == 0 || dir == 2) {
        u32 dbp = BITS(bitbltbuf, 32, 14), dbw = BITS(bitbltbuf, 48, 6), dpsm = BITS(bitbltbuf, 56, 6);
        u32 dy = BITS(trxpos, 48, 11), h = BITS(trxreg, 32, 12);
        trx_rbp = dbp; trx_rn = area_blocks(dpsm, dbw, dy + h);
        if (dir == 2) mark_blocks(trx_rbp, trx_rn);
        else { trx_pend = 1; trx_hash = 2166136261u; }
    }
    if (dir == 0) { trx_x = 0; trx_y = 0; trx_nacc = 0; trx_active = 1; }
    else if (dir == 2) {                    /* local to local */
        u32 sbp = BITS(bitbltbuf, 0, 14), sbw = BITS(bitbltbuf, 16, 6), spsm = BITS(bitbltbuf, 24, 6);
        u32 dbp = BITS(bitbltbuf, 32, 14), dbw = BITS(bitbltbuf, 48, 6), dpsm = BITS(bitbltbuf, 56, 6);
        u32 sx = BITS(trxpos, 0, 11), sy = BITS(trxpos, 16, 11), dx = BITS(trxpos, 32, 11), dy = BITS(trxpos, 48, 11);
        u32 w = BITS(trxreg, 0, 12), h = BITS(trxreg, 32, 12);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                px_write(dpsm, dbp, dbw, (dx + x) & 2047, (dy + y) & 2047, px_read(spsm, sbp, sbw, (sx + x) & 2047, (sy + y) & 2047));
    }
}

static void trx_pixel(u32 v)
{
    n_trx_px++;
    u32 dbp = BITS(bitbltbuf, 32, 14), dbw = BITS(bitbltbuf, 48, 6), dpsm = BITS(bitbltbuf, 56, 6);
    u32 dx = BITS(trxpos, 32, 11), dy = BITS(trxpos, 48, 11);
    u32 w = BITS(trxreg, 0, 12), h = BITS(trxreg, 32, 12);
    if (!w || trx_y >= h) { trx_active = 0; return; }
    px_write(dpsm, dbp, dbw, (dx + trx_x) & 2047, (dy + trx_y) & 2047, v);
    trx_hash = (trx_hash ^ v) * 16777619u;
    if (++trx_x >= w) { trx_x = 0; if (++trx_y >= h) { trx_active = 0; if (trx_pend) trx_finish(); } }
}

static void trx_data_(const u8 *d, u32 bytes);
static void trx_data(const u8 *d, u32 bytes)
{
    unsigned long long t_ = rdtsc_();
    trx_data_(d, bytes);
    t_trx += rdtsc_() - t_;
}
static void trx_data_(const u8 *d, u32 bytes)
{
    u32 dpsm = BITS(bitbltbuf, 56, 6), bits = trx_bits(dpsm);
    u32 i = 0;
    /* whole runs of a row at a time for the common formats */
    if ((dpsm == 0x13 || dpsm == 0x00 || dpsm == 0x14) && !trx_nacc) {
        u32 dbp = BITS(bitbltbuf, 32, 14), dbw = BITS(bitbltbuf, 48, 6);
        u32 dx = BITS(trxpos, 32, 11), dy = BITS(trxpos, 48, 11);
        u32 w = BITS(trxreg, 0, 12), h = BITS(trxreg, 32, 12);
        while (trx_active && i < bytes) {
            if (!w || trx_y >= h) { trx_active = 0; break; }
            u32 avail = dpsm == 0x13 ? bytes - i : (dpsm == 0x00 ? (bytes - i) / 4 : (bytes - i) * 2);
            u32 run = w - trx_x;
            if (run > avail) run = avail;
            if (dpsm == 0x14) run &= ~1u;
            if (!run) break;
            u32 y = (dy + trx_y) & 2047, hsh = trx_hash;
            /* a swizzle column group (16, 8 or 32 pixels: the same block) at a time */
            if (dpsm == 0x13) {
                const u8 *col = gs_col8[y & 15];
                for (u32 k = 0; k < run; ) {
                    u32 X = (dx + trx_x + k) & 2047, n = 16 - (X & 15);
                    if (n > run - k) n = run - k;
                    u32 base = a8(dbp, dbw, X & ~15u, y) - col[0];
                    for (u32 j = 0; j < n; j++) { u32 v = d[i + k + j]; vram8()[base + col[(X & 15) + j]] = (u8)v; hsh = (hsh ^ v) * 16777619u; }
                    k += n;
                }
                i += run;
            } else if (dpsm == 0x00) {
                const u8 *col = gs_col32[y & 7];
                for (u32 k = 0; k < run; ) {
                    u32 X = (dx + trx_x + k) & 2047, n = 8 - (X & 7);
                    if (n > run - k) n = run - k;
                    u32 base = a32(dbp, dbw, X & ~7u, y) - col[0];
                    for (u32 j = 0; j < n; j++) { u32 v; memcpy(&v, d + i + 4 * (k + j), 4); vram[base + col[(X & 7) + j]] = v; hsh = (hsh ^ v) * 16777619u; }
                    k += n;
                }
                i += run * 4;
            } else {
                const u16 *col = gs_col4[y & 15];
                for (u32 k = 0; k < run; ) {
                    u32 X = (dx + trx_x + k) & 2047, n = 32 - (X & 31);
                    if (n > run - k) n = run - k;
                    u32 base = a4(dbp, dbw, X & ~31u, y) - col[0];
                    for (u32 j = 0; j < n; j++) {
                        u32 kk = k + j, v = (d[i + kk / 2] >> ((kk & 1) * 4)) & 15, nb = base + col[(X & 31) + j];
                        vram8()[nb >> 1] = (u8)((vram8()[nb >> 1] & ((nb & 1) ? 0x0F : 0xF0)) | (v << ((nb & 1) * 4)));
                        hsh = (hsh ^ v) * 16777619u;
                    }
                    k += n;
                }
                i += run / 2;
            }
            trx_hash = hsh; n_trx_px += run;
            trx_x += run;
            if (trx_x >= w) { trx_x = 0; if (++trx_y >= h) { trx_active = 0; if (trx_pend) trx_finish(); } }
        }
    }
    for (; i < bytes && trx_active; i++) {
        if (bits == 4) { trx_pixel(d[i] & 15); if (trx_active) trx_pixel(d[i] >> 4); continue; }
        if (bits == 8) { trx_pixel(d[i]); continue; }
        trx_acc[trx_nacc++] = d[i];
        u32 need = bits / 8;
        if (trx_nacc == need) {
            u32 v = trx_acc[0] | (trx_acc[1] << 8) | (need > 2 ? (u32)trx_acc[2] << 16 : 0) | (need > 3 ? (u32)trx_acc[3] << 24 : 0);
            trx_pixel(v);
            trx_nacc = 0;
        }
    }
}

static void gs_bad(const char *what, u64 v);
/* ---- register writes ---- */
void uya_gs_write(u32 addr, u64 v)
{
#ifdef GS_WRITE_HOOK
    GS_WRITE_HOOK(addr, v);
#endif
    {   /* testing: UYA_FINDREG=reg,value (hex): where the GIF data that writes it lies (with UYA_MTGS=0), once per vsync */
        static int on = -1; static unsigned reg, lastv; static u64 want;
        if (on < 0) { const char *e = getenv("UYA_FINDREG"); on = e && sscanf(e, "%x,%llx", &reg, (unsigned long long *)&want) == 2; }
        if (on && addr == reg && v == want && lastv != uya_frame_count()) {
            lastv = uya_frame_count();
            uya_log("FINDREG %02X = %016llX at vsync %u from %s, data at %08X", reg, (unsigned long long)v, lastv, gs_src_int, (u32)(uintptr_t)gif_cur_qw);
            if (!mtgs && gif_cur_qw) for (int j = -24; j < 8; j++) { const u32 *w = gif_cur_qw + j * 4; uya_log("   %08X: %08X %08X %08X %08X", (u32)(uintptr_t)w, w[0], w[1], w[2], w[3]); }
        }
    }
    if (!(addr <= 0x05 && addr) && addr != 0x0A && addr != 0x0C && addr != 0x0D && (addr < 0x50 || addr > 0x54)) st_dirty = 1;
    switch (addr) {
    case 0x00: prim = v; prim_reset(); break;
    case 0x01: rgbaq = v; { u32 q = (u32)(v >> 32); memcpy(&qreg, &q, 4); } break;
    case 0x02: st = v; break;
    case 0x03: uv = v; break;
    case 0x04: vertex_kick(BITS(v, 0, 16), BITS(v, 16, 16), BITS(v, 32, 24), (u8)BITS(v, 56, 8), 1); break;
    case 0x05: vertex_kick(BITS(v, 0, 16), BITS(v, 16, 16), (u32)(v >> 32), 0, 1); break;
    case 0x0C: vertex_kick(BITS(v, 0, 16), BITS(v, 16, 16), BITS(v, 32, 24), (u8)BITS(v, 56, 8), 0); break;
    case 0x0D: vertex_kick(BITS(v, 0, 16), BITS(v, 16, 16), (u32)(v >> 32), 0, 0); break;
    case 0x06: case 0x07:
        {
            static int left = -1;      /* testing: UYA_BADTEX=n logs TEX0 writes whose top half looks like a pointer, with where the data came from */
            if (left < 0) { const char *e = getenv("UYA_BADTEX"); left = e ? atoi(e) : 0; }
            if (left > 0 && (v >> 48) >= 0xFF00) {
                left--;
                uya_log("odd TEX0 %016llX from %s, data at %08X", (unsigned long long)v, gs_src_int, (u32)(uintptr_t)gif_cur_qw);
                for (int j = -12; j < 8; j++) { const u32 *w = gif_cur_qw + j * 4; uya_log("   %08X: %08X %08X %08X %08X", (u32)(uintptr_t)w, w[0], w[1], w[2], w[3]); }
            }
        }
        ctx[addr - 6].tex0 = v; clut_load(v); break;
    case 0x08: case 0x09: ctx[addr - 8].clamp = v; break;
    case 0x0A: fog = v; break;
    case 0x14: case 0x15: ctx[addr - 0x14].tex1 = v; break;
    case 0x16: case 0x17: {
        gs_ctx_t *c = &ctx[addr - 0x16];
        u64 m = (0x3Full << 20) | (~0ull << 37);           /* PSM and the CLUT fields come from TEX2 */
        c->tex0 = (c->tex0 & ~m) | (v & m);
        clut_load(c->tex0);
    } break;
    case 0x18: case 0x19: ctx[addr - 0x18].xyoffset = v; break;
    case 0x34: case 0x35: ctx[addr - 0x34].miptbp1 = v; break;
    case 0x36: case 0x37: ctx[addr - 0x36].miptbp2 = v; break;
    case 0x1A: prmodecont = v & 1; break;
    case 0x1B: prmode = v; break;
    case 0x1C: texclut = v; break;
    case 0x3B: texa = v; break;
    case 0x3D: fogcol = v; break;
    case 0x40: case 0x41: ctx[addr - 0x40].scissor = v; break;
    case 0x42: case 0x43: ctx[addr - 0x42].alpha = v; break;
    case 0x45: dthe = v; break;
    case 0x46: colclamp = v; break;
    case 0x47: case 0x48: ctx[addr - 0x47].test = v; break;
    case 0x49: pabe = v; break;
    case 0x4A: case 0x4B: ctx[addr - 0x4A].fba = v; break;
    case 0x4C: case 0x4D: {
        static int nlog = -1;
        if (nlog < 0) { const char *e = getenv("UYA_FRAMELOG"); nlog = e ? atoi(e) : 0; }
        if (nlog > 0 && ctx[addr - 0x4C].frame != v) { nlog--; uya_log("GS FRAME_%u = FBP 0x%X FBW %u PSM 0x%X MSK %08X", addr - 0x4C + 1, BITS(v, 0, 9) * 32, BITS(v, 16, 6) * 64, BITS(v, 24, 6), (u32)(v >> 32)); }
        { u32 ps = BITS(v, 24, 6); if (!BITS(v, 16, 6) || !(ps == 0 || ps == 1 || ps == 2 || ps == 0xA)) gs_bad("FRAME", v); }
        ctx[addr - 0x4C].frame = v;
    } break;
    case 0x4E: case 0x4F: ctx[addr - 0x4E].zbuf = v; break;
    case 0x50: bitbltbuf = v; break;
    case 0x51: trxpos = v; break;
    case 0x52: trxreg = v; break;
    case 0x53: trx_start(BITS(v, 0, 2)); break;
    case 0x54: { u8 b[8]; memcpy(b, &v, 8); if (trx_active) trx_data(b, 8); } break;
    default: break;
    }
}

/* ---- GIF ---- */
/* Each GIF path (PATH1 XGKICK, PATH2 VIF1 DIRECT, PATH3 GIF DMA/FIFO) has its
 * own tag in progress: the GIF only switches paths between packets. */
typedef struct { u32 nloop, nreg, flg, eop, r; u64 regs; int active; } giftag_t;
static giftag_t gif_tags[3];
/* testing: who sent the GIF data (set by the callers), and the last tags */
const char *uya_gs_src = "?";
static struct { const char *src; u64 lo, hi; } tag_hist[16];
static u32 tag_hist_n;
static void gs_bad(const char *what, u64 v)
{
    static int left = 3;
    if (left <= 0) return;
    left--;
    uya_log("GS: implausible %s %016llX (from %s) at vsync %u; last GIF tags:", what, (unsigned long long)v, gs_src_int, uya_frame_count());
    for (u32 i = 0; i < 16; i++) {
        u32 k = (tag_hist_n + i) & 15;
        if (tag_hist[k].src) uya_log("   %-28s %016llX %016llX", tag_hist[k].src, (unsigned long long)tag_hist[k].lo, (unsigned long long)tag_hist[k].hi);
    }
    if (gs_src_int[4] == '1' && !mtgs) { void uya_vu_dump_kick(void); uya_vu_dump_kick(); }
}


/* ---- testing: recording the GS input ----
 * UYA_GSREC=file,from,n (or F9 in the game window) saves the GS state at vsync `from`
 * (registers and memory) and then every GIF packet, movie frame and vsync of
 * the next n vsyncs, so tools/gs_replay.c can run the same input through
 * another build of this file and compare the results (VRAM after each vsync). */
#define GS_SNAP_FIELDS(X) X(ctx) X(prim) X(prmode) X(prmodecont) X(rgbaq) X(st) X(uv) X(fog) X(texa) X(fogcol) X(colclamp) \
    X(pabe) X(dthe) X(bitbltbuf) X(trxpos) X(trxreg) X(texclut) X(qreg) X(clut) X(clut_hash) X(cbp0) X(cbp1) X(vq) X(vcount) \
    X(gif_tags) X(trx_x) X(trx_y) X(trx_active) X(trx_acc) X(trx_nacc)
static FILE *rec_f;
static long rec_vs, rec_from = -1, rec_to;
static volatile int rec_hotkey;
/* F9 (platform_win32.c): record the next UYA_GSREC_N vsyncs (default 150) to gsrec_NN.bin next to the exe */
void uya_gs_rec_hotkey(void) { rec_hotkey = 1; }
static void rec_put(const void *p, size_t n) { if (rec_f) fwrite(p, 1, n, rec_f); }
static void rec_vsync(const u64 *r)
{
    if (rec_from == -1) {
        const char *e = getenv("UYA_GSREC");
        rec_from = -2;
        if (e) { char path[256]; long n = 0; if (sscanf(e, "%255[^,],%ld,%ld", path, &rec_from, &n) == 3) { rec_to = rec_from + n; rec_f = fopen(path, "wb"); if (!rec_f) rec_from = -2; } else rec_from = -2; }
    }
    if (rec_hotkey && !rec_f) {
        static int nrec;
        char path[600]; const char *ne = getenv("UYA_GSREC_N"); long n = ne ? atol(ne) : 150;
        rec_hotkey = 0;
        if (n < 1) n = 150;
        snprintf(path, sizeof path, "%s/gsrec_%02d.bin", plat_exe_dir(), nrec++);
        rec_f = fopen(path, "wb");
        if (rec_f) { rec_from = rec_vs; rec_to = rec_vs + n; uya_log("GS input recording to %s (%ld vsyncs)", path, n); }
    }
    rec_hotkey = 0;
    if (!rec_f) { rec_vs++; return; }
    if (rec_vs == rec_from) {
        u32 m = 0x52534755u; rec_put(&m, 4);
        if (gsgl_on) gsgl_cpu_read(0, 0x4000);
#define X(f) { u32 n_ = sizeof f; rec_put(&n_, 4); rec_put(&f, sizeof f); }
        GS_SNAP_FIELDS(X)
#undef X
        rec_put(vram, 4 << 20);
    } else if (rec_vs > rec_from) {
        u32 k = 2; rec_put(&k, 4); rec_put(r, 48);
        if (rec_vs >= rec_to) { fclose(rec_f); rec_f = 0; uya_log("GS input recorded"); }
    }
    rec_vs++;
}
static void rec_gif(const u32 *d, u32 qwc, u32 path)
{
    if (!rec_f || rec_vs <= rec_from) return;
    u32 h[3] = { 1, path, qwc }; rec_put(h, 12); rec_put(d, qwc * 16);
}
static void uya_gs_gif_(const u32 *d, u32 qwc);
/* ---- the GS thread ----
 * With UYA_MTGS (on by default with 3 or more cores) GIF data is copied into
 * a FIFO and the GS (register writes, primitives, transfers, the texture
 * cache) runs on its own thread, like the real GS beside the EE. The EE side
 * waits for it at each vsync before the frame is shown (gs_sync). */
#define GF_WORDS (1u << 22)
static u32 *gfifo;
static line_u32 gf_head_, gf_tail_;     /* written by the game's thread and the GS thread */
#define gf_head (gf_head_.v)
#define gf_tail (gf_tail_.v)
static const char *path_name[3] = { "PATH1 (XGKICK)", "PATH2 (VIF1 DIRECT)", "PATH3" };

static park_t gspark;
static void gs_vsync_marker(const u32 *e);
static void gs_movie_marker(const u32 *e);
static void gl_mem_do(const u32 *e);
static void gs_thread_loop(void)
{
    int spin = 0;
    if (gsgl_on && !gsgl_threaded()) plat_gl_current(1);       /* the GL context lives on this thread (unless on the GL thread) */
    for (;;) {
        u32 h = __atomic_load_n(&gf_head, __ATOMIC_ACQUIRE), t = gf_tail;
        if (t == h) {
            if (spin++ < 2000) { __asm__ volatile("pause"); continue; }
            __atomic_store_n(&gspark.sleeping, 1, __ATOMIC_SEQ_CST);
            if (__atomic_load_n(&gf_head, __ATOMIC_SEQ_CST) == t) park_wait(&gspark);
            __atomic_store_n(&gspark.sleeping, 0, __ATOMIC_RELEASE);
            continue;
        }
        spin = 0;
        unsigned long long b0_ = rdtsc_();
        while (t != h) {
            const u32 *e = gfifo + (t % GF_WORDS);
            u32 qwc = e[0], path = e[1];
            if (path == 0xFFFFFFFFu) { t += GF_WORDS - (t % GF_WORDS); continue; }   /* wrap to the start */
            if (path == 0xFFFFFFFEu) { gs_vsync_marker(e + 4); t += 4 + qwc * 4; continue; }   /* a vsync: capture the frame */
            if (path == 0xFFFFFFFCu) { gs_movie_marker(e + 4); t += 4 + qwc * 4; continue; }   /* an FMV frame to show */
            if (path == 0xFFFFFFFDu) { gl_mem_do(e + 4); t += 4 + qwc * 4; continue; }         /* GL: the game's thread touches GS memory */
            gs_src_int = path_name[path];
            if (__builtin_expect(rec_f != 0, 0)) rec_gif(e + 4, qwc, path);
            uya_gs_gif_(e + 4, qwc);
            t += 4 + qwc * 4;
        }
        gs_thread_busy += rdtsc_() - b0_;
        __atomic_store_n(&gf_tail, t, __ATOMIC_RELEASE);
    }
}
#ifdef _WIN32
static DWORD WINAPI gs_thread_main(LPVOID a) { (void)a; gs_thread_loop(); return 0; }
#else
static void *gs_thread_main(void *a) { (void)a; gs_thread_loop(); return 0; }
#endif
static void mtgs_init(int cores)
{
    const char *e = getenv("UYA_MTGS");
    mtgs = e ? atoi(e) : cores >= 3;
    if (!mtgs) return;
    gfifo = malloc(GF_WORDS * 4);
    park_init(&gspark);
#ifdef _WIN32
    CreateThread(0, 0, gs_thread_main, 0, 0, 0);
#else
    pthread_t th; pthread_create(&th, 0, gs_thread_main, 0); pthread_detach(th);
#endif
    uya_log("GS: on its own thread");
}
/* wait until the GS thread has done everything sent to it */
static void gs_sync(void)
{
    if (!mtgs) return;
    int spin = 0;
    while (__atomic_load_n(&gf_tail, __ATOMIC_ACQUIRE) != gf_head) cpu_relax(spin++);
}

static void gs_fifo_put(const void *data, u32 qwc, u32 path)
{
    const u32 *d = data;
    while (qwc) {
        u32 n = qwc > 0x10000 ? 0x10000 : qwc, need = 4 + n * 4, h = gf_head, spin = 0;
        if ((h % GF_WORDS) + need > GF_WORDS) {    /* no room before the end: a wrap marker */
            while (h + (GF_WORDS - h % GF_WORDS) - __atomic_load_n(&gf_tail, __ATOMIC_ACQUIRE) > GF_WORDS - 4) cpu_relax(spin++);
            gfifo[h % GF_WORDS] = 0; gfifo[h % GF_WORDS + 1] = 0xFFFFFFFFu;
            h += GF_WORDS - h % GF_WORDS;
            __atomic_store_n(&gf_head, h, __ATOMIC_RELEASE);
        }
        while (h + need - __atomic_load_n(&gf_tail, __ATOMIC_ACQUIRE) > GF_WORDS - 4) cpu_relax(spin++);
        u32 *w = gfifo + h % GF_WORDS;
        w[0] = n; w[1] = path; w[2] = w[3] = 0;
        memcpy(w + 4, d, n * 16);
        __atomic_store_n(&gf_head, h + need, __ATOMIC_SEQ_CST);
        park_wake(&gspark);
        d += n * 4; qwc -= n;
    }
}

void uya_gs_gif(const u32 *d, u32 qwc)
{
    unsigned long long t = timing() ? rdtsc_() : 0;
    if (__builtin_expect(rec_f != 0, 0) && !mtgs) rec_gif(d, qwc, (uya_gs_src[0] == 'P' && uya_gs_src[4] >= '1' && uya_gs_src[4] <= '3') ? (u32)(uya_gs_src[4] - '1') : 2);
    if (!mtgs) { gs_src_int = uya_gs_src; uya_gs_gif_(d, qwc); }
    else gs_fifo_put(d, qwc, (uya_gs_src[0] == 'P' && uya_gs_src[4] >= '1' && uya_gs_src[4] <= '3') ? (u32)(uya_gs_src[4] - '1') : 2);
    if (uya_timing) uya_t_gs += rdtsc_() - t;
}
static void uya_gs_gif_(const u32 *d, u32 qwc)
{
    u32 q = 0;
    if (!vram) return;
    int path = (gs_src_int[0] == 'P' && gs_src_int[4] >= '1' && gs_src_int[4] <= '3') ? gs_src_int[4] - '1' : 2;
    giftag_t *tp = &gif_tags[path];
#define tag (*tp)
    if (path == 0) tag.active = 0;          /* XGKICK starts a packet at its address */
    while (q < qwc) {
        const u32 *t = d + q * 4; gif_cur_qw = t;
        if (!tag.active) {
            u64 lo = ((u64)t[1] << 32) | t[0], hi = ((u64)t[3] << 32) | t[2];
            tag.nloop = (u32)(lo & 0x7FFF); tag.eop = BITS(lo, 15, 1); tag.flg = BITS(lo, 58, 2);
            tag.nreg = BITS(lo, 60, 4); if (!tag.nreg) tag.nreg = 16;
            tag.regs = hi; tag.r = 0;
            tag_hist[tag_hist_n & 15].src = gs_src_int; tag_hist[tag_hist_n & 15].lo = lo; tag_hist[tag_hist_n & 15].hi = hi; tag_hist_n++;
            {
                static int nlog = -1;
                static unsigned nfrom;
                if (nlog < 0) { const char *e = getenv("UYA_GSLOG"); nlog = e ? atoi(e) : 0; e = getenv("UYA_GSLOG_FROM"); nfrom = e ? (unsigned)atoi(e) : 0; }   /* testing: UYA_GSLOG=n tags from vsync UYA_GSLOG_FROM */
                if (nlog > 0 && uya_frame_count() >= nfrom) { nlog--; uya_log("GIF tag [%s, %u qw]: nloop %u eop %u pre %u prim 0x%X flg %u nreg %u regs %016llX", gs_src_int, qwc - q, tag.nloop, tag.eop, BITS(lo, 46, 1), BITS(lo, 47, 11), tag.flg, tag.nreg, (unsigned long long)hi); }
            }
            if (path == 1) {   /* testing: UYA_P2LOG=1 logs PATH2 tags that look like data, with the PATH2 tags before */
                static int p2on = -1, p2n = 30; static u32 hn; static struct { u32 v, q, n; u64 lo, hi; } h[12];
                if (p2on < 0) p2on = getenv("UYA_P2LOG") != 0;
                if (p2on) {
                    if (((lo >> 16) & 0x3FFFFFFFull) && p2n > 0) {
                        p2n--;
                        uya_log("P2 bad tag v%u q %u/%u: %016llX %016llX; earlier PATH2 tags:", uya_frame_count(), q, qwc, (unsigned long long)lo, (unsigned long long)hi);
                        for (u32 k = 0; k < 12; k++) { u32 x = (hn + k) % 12; if (h[x].n) uya_log("   v%u q %u/%u: %016llX %016llX", h[x].v, h[x].q, h[x].n, (unsigned long long)h[x].lo, (unsigned long long)h[x].hi); }
                        if (p2n >= 28) { void uya_vif1_dump_hist(void); uya_vif1_dump_hist(); }
                    }
                    h[hn % 12].v = uya_frame_count(); h[hn % 12].q = q; h[hn % 12].n = qwc; h[hn % 12].lo = lo; h[hn % 12].hi = hi; hn++;
                }
            }
            if (BITS(lo, 46, 1)) uya_gs_write(0x00, BITS(lo, 47, 11));
            qreg = 1.0f;
            q++;
            tag.active = tag.nloop > 0;
            continue;
        }
        if (tag.flg == 0) {                      /* PACKED */
            u32 rg = (u32)(tag.regs >> (tag.r * 4)) & 15;
            u64 lo = ((u64)t[1] << 32) | t[0], hi = ((u64)t[3] << 32) | t[2];
            switch (rg) {
            case 0x0: uya_gs_write(0x00, lo); break;
            case 0x1: rgbaq = (t[0] & 255) | ((t[1] & 255) << 8) | ((t[2] & 255) << 16) | ((u64)(t[3] & 255) << 24) | ((u64)0 << 32); break;
            case 0x2: st = lo; memcpy(&qreg, &t[2], 4); break;
            case 0x3: uv = (t[0] & 0x3FFF) | ((u64)(t[1] & 0x3FFF) << 16); break;
            case 0x4: vertex_kick(t[0] & 0xFFFF, t[1] & 0xFFFF, (t[2] >> 4) & 0xFFFFFF, (u8)(t[3] >> 4), !((t[3] >> 15) & 1)); break;
            case 0x5: vertex_kick(t[0] & 0xFFFF, t[1] & 0xFFFF, t[2], 0, !((t[3] >> 15) & 1)); break;
            case 0xA: fog = (u64)((t[3] >> 4) & 255) << 56; break;
            case 0xC: vertex_kick(t[0] & 0xFFFF, t[1] & 0xFFFF, (t[2] >> 4) & 0xFFFFFF, (u8)(t[3] >> 4), 0); break;
            case 0xD: vertex_kick(t[0] & 0xFFFF, t[1] & 0xFFFF, t[2], 0, 0); break;
            case 0xE: uya_gs_write((u32)(hi & 0xFF), lo); break;
            case 0xF: break;
            default: uya_gs_write(rg, lo); break;    /* TEX0_x, CLAMP_x: same as A+D at that address */
            }
            q++;
            if (++tag.r >= tag.nreg) { tag.r = 0; if (--tag.nloop == 0) tag.active = 0; }
        } else if (tag.flg == 1) {               /* REGLIST: two 64-bit registers per qword */
            for (int h = 0; h < 2 && tag.active; h++) {
                u64 val = ((u64)t[h * 2 + 1] << 32) | t[h * 2];
                u32 rg = (u32)(tag.regs >> (tag.r * 4)) & 15;
                if (rg != 0xE && rg != 0xF) uya_gs_write(rg == 0x1 ? 0x01 : rg, val);
                if (++tag.r >= tag.nreg) { tag.r = 0; if (--tag.nloop == 0) tag.active = 0; }
            }
            q++;
        } else {                                  /* IMAGE: all of the packet's data here at once */
            u32 n = qwc - q < tag.nloop ? qwc - q : tag.nloop;
            if (trx_active) trx_data((const u8 *)t, n * 16);
            q += n;
            if ((tag.nloop -= n) == 0) tag.active = 0;
        }
    }
#undef tag
}

/* ---- init and display ---- */
/* UYA_RENDERER=gl|soft (or renderer= in rac1_wrapper.cfg): the GPU draws
 * (default where there is GL: Windows) or the software rasterizer; GL falls
 * back to software if it cannot start */
const char *uya_renderer_cfg;
void uya_gs_init(void)
{
    if (vram) return;
    gs_swizzle_init();
    timing();
    vram = calloc(1, 4 << 20);
    {
        const char *r = getenv("UYA_RENDERER");
        if (!r) r = uya_renderer_cfg;
#ifdef _WIN32
        if (!r) r = "gl";
#else
        if (!r) r = "soft";
#endif
        if (!strcmp(r, "gl")) {
            if (plat_gl_init() && gsgl_init(vram)) { plat_gl_active = 1; uya_log("GS: drawing with OpenGL"); }
            else uya_log("GS: OpenGL not available, drawing in software");
        }
    }
    if (gsgl_on && !gsgl_threaded()) plat_gl_current(0);     /* free for the GS thread (MTGS) to take */
    gs_threads_init();
    if (gsgl_on && !mtgs && !gsgl_threaded()) plat_gl_current(1);
}

/* GL with MTGS: the game's thread asks the GS thread to sync GS memory with
 * the GPU targets before it reads (StoreImage) or writes (movie frames) it */
static void gl_mem_do(const u32 *e)
{
    if (e[0]) gsgl_cpu_write_rect(e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    else gsgl_cpu_read_rect(e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
}
static void gs_sync(void);
static void gl_mem_req(int write, u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h)
{
    if (!gsgl_on) return;
    u32 e[8] = { (u32)write, psm, bp, bw, x, y, w, h };
    if (!mtgs) { gl_mem_do(e); return; }
    gs_fifo_put(e, 2, 0xFFFFFFFDu);
    gs_sync();
}

static u32 frame_px[1024 * 1024];

/* what the display circuits show (PMODE, SMODE2, DISPFB, DISPLAY); 0 if nothing */
static int gs_disp(u64 pmode, u64 smode2, u64 dispfb_1, u64 display_1, u64 dispfb_2, u64 display_2, gs_disp_t *d)
{
    static int logged;
    int circ = (pmode & 1) ? 1 : ((pmode & 2) ? 2 : 0);
    if (!circ) return 0;
    u64 dispfb = circ == 1 ? dispfb_1 : dispfb_2;
    u64 display = circ == 1 ? display_1 : display_2;
    u32 fbp = BITS(dispfb, 0, 9) * 32, fbw = BITS(dispfb, 9, 6), psm = BITS(dispfb, 15, 5);
    u32 dbx = BITS(dispfb, 32, 11), dby = BITS(dispfb, 43, 11);
    u32 magh = BITS(display, 23, 4) + 1, magv = BITS(display, 27, 2) + 1;
    u32 dw = BITS(display, 32, 12) + 1, dh = BITS(display, 44, 11) + 1;
    u32 w = dw / magh, h = dh / magv;
    if ((smode2 & 3) == 3) h /= 2;              /* interlaced, frame mode: each field shows half the lines */
    if (w > 1024) w = 1024;
    if (h > 1024) h = 1024;
    static u64 last_disp, last_fb;
    if (display != last_disp || dispfb != last_fb) { logged = 0; last_disp = display; last_fb = dispfb; }
    if (!logged) {
        uya_log("GS display: circuit %d, FBP 0x%X FBW %u PSM 0x%X, %ux%u (DW %u DH %u MAGH %u MAGV %u, SMODE2 %llu)",
                circ, fbp, fbw * 64, psm, w, h, dw, dh, magh, magv, (unsigned long long)smode2);
        logged = 1;
    }
    if (!w || !h || !fbw) return 0;
    /* Adjust Screen moves the picture by changing DISPLAY's DX/DY. The first
     * position seen in each video mode is the centre; later changes shift the
     * picture in the window by the same amount (DX counts in MAGH units). */
    static u64 base_key; static s32 base_dx, base_dy;
    s32 cdx = (s32)BITS(display, 0, 12), cdy = (s32)BITS(display, 12, 11);
    u64 key = (display >> 23) | (smode2 << 60);
    if (key != base_key) { base_key = key; base_dx = cdx; base_dy = cdy; }
    s32 shx = (cdx - base_dx) / (s32)magh, shy = cdy - base_dy;
    if ((smode2 & 1) && !((smode2 >> 1) & 1)) shy *= 2;   /* interlaced field mode: DY counts field lines */
    d->fbp = fbp; d->fbw = fbw; d->psm = psm; d->dbx = dbx; d->dby = dby; d->w = w; d->h = h; d->shx = shx; d->shy = shy;
    return 1;
}
static void fps_log(void);
/* the frame the display circuits show, from GS memory into out (BGRX); 0 if nothing is shown */
static int gs_capture(u64 pmode, u64 smode2, u64 dispfb_1, u64 display_1, u64 dispfb_2, u64 display_2, u32 *out, u32 *ow, u32 *oh)
{
    gs_disp_t d;
    if (!gs_disp(pmode, smode2, dispfb_1, display_1, dispfb_2, display_2, &d)) { fps_log(); return 0; }
    u32 fbp = d.fbp, fbw = d.fbw, psm = d.psm, dbx = d.dbx, dby = d.dby, w = d.w, h = d.h;
    s32 shx = d.shx, shy = d.shy;
    const int p32 = !(psm == 0x02 || psm == 0x0A);
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            s32 fx = (s32)x - shx, fy = (s32)y - shy;
            if (fx < 0 || fy < 0 || fx >= (s32)w || fy >= (s32)h) { out[y * w + x] = 0; continue; }
            u32 v = p32 ? vram[a32(fbp, fbw, (dbx + (u32)fx) & 2047, (dby + (u32)fy) & 2047)]   /* px_read for PSMCT32/24 */
                        : px_read(psm, fbp, fbw, (dbx + (u32)fx) & 2047, (dby + (u32)fy) & 2047), c;
            if (psm == 0x02 || psm == 0x0A) c = ((v & 31) << 19) | (((v >> 5) & 31) << 11) | (((v >> 10) & 31) << 3);
            else c = ((v & 255) << 16) | (v & 0xFF00) | ((v >> 16) & 255);   /* RGBA in memory to BGRX for the window */
            out[y * w + x] = c;
        }
    fps_log();
    *ow = w; *oh = h;
    return 1;
}
/* UYA_FPSLOG: speed and GS statistics every 100 vsyncs; UYA_VRAMDUMP=n: GS memory to vram.bin at vsync n */
static void fps_log(void)
{
    {
        static long vn; static long want = -2;
        if (want == -2) { const char *e = getenv("UYA_VRAMDUMP"); want = e ? atol(e) : -1; }
        if (vn % 100 == 0) {   /* VU results the VU would have kept finite (vu_fc, vu.h): a few lines, for bug reports */
            extern u32 vu_nonfinite; static u32 nf0, nlines;
            u32 nf = vu_nonfinite;
            if (nf != nf0 && nlines < 40) { nlines++; uya_log("VU: %u results overflowed to inf/NaN by vsync %ld (made +-MAX, as on the PS2)", nf - nf0, vn); }
            nf0 = nf;
        }
        if (getenv("UYA_FPSLOG") && vn % 100 == 0) {
            static struct timespec t0; struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            extern unsigned long long uya_t_vu; static unsigned long long c0, g0, v0; unsigned long long c1 = rdtsc_();
            if (vn) uya_log("vsync %ld: %.1f vsyncs/s; GS %.0f%%, VU %.0f%% (VU includes its GS)", vn, 100.0 / ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9),
                            100.0 * (uya_t_gs - g0) / (c1 - c0), 100.0 * (uya_t_vu - v0) / (c1 - c0));
            static unsigned long long p0, x0; static unsigned long long tp0;
            extern unsigned long long uya_tex_px;
            extern unsigned long long nsends; static unsigned long long s0;
            uya_log("   %llu DMA sends (frames) in 100 vsyncs; per vsync: %llu waits for the GS threads (%u for layouts), %llu textures decoded, %llu pixels uploaded", nsends - s0, n_drain_wait / 100, n_layout_drain / 100, n_tc_build / 100, n_trx_px / 100); s0 = nsends;
            uya_log("   texture rebuilds for stale data %llu, reused by content %llu; marks: uploads %llu, targets %llu", n_tc_stale / 100, n_tc_reuse / 100, n_mark_trx / 100, n_mark_draw / 100);
            n_tc_stale = n_tc_reuse = n_mark_trx = n_mark_draw = 0;
            uya_log("   time: drawing %.0f%%, texture decode %.0f%%, uploads %.0f%%", 100.0 * t_draw / (c1 - c0), 100.0 * t_tcb / (c1 - c0), 100.0 * t_trx / (c1 - c0));
            if (mtgs || gsgl_on) {   /* where a frame's time goes, per thread (ms per vsync) */
                double ms = (double)((t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 100.0, busy = 0, wait = 0;
                if (gsgl_on) gsgl_thread_times(&busy, &wait);
                uya_log("   threads (ms per vsync, %.1f between vsyncs): GS thread busy %.1f (%.1f of it waiting for the GL thread), GL thread busy %.1f%s",
                        ms, mtgs ? ms * (double)gs_thread_busy / (double)(c1 - c0) : 0.0, wait / 100.0, busy / 100.0, gsgl_threaded() ? "" : " (no GL thread)");
                gs_thread_busy = 0;
            }
            n_drain_wait = n_tc_build = n_trx_px = 0; n_layout_drain = 0; t_draw = t_tcb = t_trx = 0;
            uya_log("   per vsync: %llu prims, %llu pixels written, %llu textured pixels", (stat_prims - p0) / 100, (stat_pixels - x0) / 100, (uya_tex_px - tp0) / 100);
            p0 = stat_prims; x0 = stat_pixels; tp0 = uya_tex_px;
            c0 = c1; g0 = uya_t_gs; v0 = uya_t_vu;
            t0 = t1;
        }
        if (++vn == want) { if (gsgl_on) gsgl_cpu_read(0, 0x4000); FILE *f = fopen("vram.bin", "wb"); if (f) { fwrite(vram, 4, 1 << 20, f); fclose(f); } uya_log("VRAM saved at vsync %ld", vn); }
    }
}
/* GL: the display drawn into the window */
static void gl_present(const u64 *r)
{
    gs_disp_t d;
    int ok = gs_disp(r[0], r[1], r[2], r[3], r[4], r[5], &d);
    fps_log();
    gsgl_present(ok ? &d : 0);
}


/* With the GS on its own thread the frame is captured there when the GS gets
 * to the vsync (a marker in its FIFO), and the game goes on at once: it shows
 * the last captured frame, so the GS works on frame N while the game prepares
 * frame N+1 (at most two vsyncs ahead). */
static u32 cap_buf[3][1024 * 1024], cap_w[3], cap_h[3], cap_ok[3];
static volatile u32 vs_sent, vs_done;
static void gs_vsync_marker(const u32 *e)
{
    u64 r[6]; memcpy(r, e, sizeof r);
    if (rec_f || rec_hotkey) gs_drain();
    rec_vsync(r);
    if (gsgl_on) { gl_present(r); __atomic_store_n(&vs_done, vs_done + 1, __ATOMIC_RELEASE); return; }
    gs_drain();
    u32 k = vs_done % 3;
    cap_ok[k] = gs_capture(r[0], r[1], r[2], r[3], r[4], r[5], cap_buf[k], &cap_w[k], &cap_h[k]);
    __atomic_store_n(&vs_done, vs_done + 1, __ATOMIC_RELEASE);
}

/* FMV frames in GL mode: the GL context belongs to the GS (thread), so a
 * decoded frame goes there to be drawn into the window. Two buffers; the
 * player waits until the older one has been shown. */
static u32 mv_buf[2][1024 * 1024];
static volatile u32 mv_sent, mv_done;
static void gs_movie_marker(const u32 *e)
{
    u32 k = e[0] & 1;
    gsgl_present_bgra(mv_buf[k], (int)e[1], (int)e[2]);
    __atomic_store_n(&mv_done, mv_done + 1, __ATOMIC_RELEASE);
}
/* 1 if the GL renderer showed the frame (else the caller presents it itself) */
int uya_gs_movie_frame(const u32 *px, int w, int h)
{
    if (!gsgl_on || !vram || w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
    if (!mtgs) { gsgl_present_bgra(px, w, h); return 1; }
    int spin = 0;
    while (mv_sent - __atomic_load_n(&mv_done, __ATOMIC_ACQUIRE) >= 2) cpu_relax(spin++);
    u32 k = mv_sent & 1;
    memcpy(mv_buf[k], px, (size_t)w * h * 4);
    u32 m[12] = { k, (u32)w, (u32)h };
    mv_sent++;
    gs_fifo_put(m, 3, 0xFFFFFFFCu);   /* its own marker (0xFFFFFFFD is the GL memory sync) */
    return 1;
}

void uya_gs_vsync(void)
{
    if (!vram) return;
    u64 r[6] = { *(volatile u64 *)0x12000000, *(volatile u64 *)0x12000020,
                 *(volatile u64 *)0x12000070, *(volatile u64 *)0x12000080, *(volatile u64 *)0x12000090, *(volatile u64 *)0x120000A0 };
    if (!mtgs) {
        u32 w, h;
        gs_drain();
        rec_vsync(r);
        if (gsgl_on) { gl_present(r); if (!plat_present(0, 0, 0)) { uya_log("window closed"); exit(0); } return; }
        if (gs_capture(r[0], r[1], r[2], r[3], r[4], r[5], frame_px, &w, &h) && !plat_present(frame_px, (int)w, (int)h)) { uya_log("window closed"); exit(0); }
        return;
    }
    gs_fifo_put(r, 3, 0xFFFFFFFEu);      /* 48 bytes: 3 qwords */
    vs_sent++;
    {   /* no more than two vsyncs ahead of the GS */
        int spin = 0;
        while (vs_sent - __atomic_load_n(&vs_done, __ATOMIC_ACQUIRE) > 2) cpu_relax(spin++);
    }
    static u32 shown;
    u32 d = __atomic_load_n(&vs_done, __ATOMIC_ACQUIRE);
    if (gsgl_on) { if (!plat_present(0, 0, 0)) { uya_log("window closed"); exit(0); } return; }   /* the GS thread showed it */
    if (d != shown) {
        u32 k = (d - 1) % 3;
        shown = d;
        if (cap_ok[k] && !plat_present(cap_buf[k], (int)cap_w[k], (int)cap_h[k])) { uya_log("window closed"); exit(0); }
    } else if (!plat_present(0, 0, 0)) { uya_log("window closed"); exit(0); }   /* nothing new: keep the window alive */
}

/* Writes a w x h image of RGBA8 pixels into GS memory as PSMCT32 at block
 * address tbp, buffer width tbw (in 64-pixel units). The movie player uses it
 * to put a decoded frame where the PS2's IPU output would be. */
void uya_gs_write_ct32(u32 tbp, u32 tbw, int w, int h, const u32 *rgba)
{
    if (!vram) return;
    gs_sync();
    gs_drain();
    if (rec_f && rec_vs > rec_from) { u32 hd[5] = { 3, tbp, tbw, (u32)w, (u32)h }; rec_put(hd, 20); rec_put(rgba, (size_t)w * h * 4); }
    gl_mem_req(1, 0, tbp, tbw, 0, 0, (u32)w, (u32)h);
    mark_blocks(tbp, area_blocks(0, tbw, (u32)h));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) vram[a32(tbp, tbw, (u32)x, (u32)y)] = rgba[y * w + x];
}

/* GS -> memory (sceGsExecStoreImage): the w x h area at (x, y) of the buffer
 * at block bp (width bw, format psm), packed row by row */
void uya_gs_store_image(u32 bp, u32 bw, u32 psm, u32 x, u32 y, u32 w, u32 h, void *dst)
{
    if (!vram) return;
    gs_sync();
    gs_drain();
    gl_mem_req(0, psm, bp, bw, x, y, w, h);
    if (bw == 1 && psm == 0 && w == 64 && h == 64 && bp < 0x4000) si_hash[bp] = vhash(bp, 0x40) | 1;
    u32 bits = trx_bits(psm), n = 0;
    u8 *o = dst;
    for (u32 j = 0; j < h; j++)
        for (u32 i = 0; i < w; i++, n++) {
            u32 v = px_read(psm, bp, bw, (x + i) & 2047, (y + j) & 2047);
            switch (bits) {
            case 4: if (n & 1) o[n >> 1] = (u8)((o[n >> 1] & 0x0F) | (v << 4)); else o[n >> 1] = (u8)((o[n >> 1] & 0xF0) | (v & 15)); break;
            case 8: o[n] = (u8)v; break;
            case 16: memcpy(o + n * 2, &v, 2); break;
            case 24: memcpy(o + n * 3, &v, 3); break;
            default: memcpy(o + n * 4, &v, 4); break;
            }
        }
}

void uya_gs_stats(void)
{
    uya_log("GS: %llu primitives drawn, %llu pixels written", stat_prims, stat_pixels);
}

/* GL: a level of a state's texture decoded now (for gs_gl.c's deferred levels) */
int gs_tex_late(const pcmd_t *p, int lvl, tlevel_t *out)
{
    gs_ctx_t c; memset(&c, 0, sizeof c);
    c.tex0 = p->tex0; c.tex1 = p->tex1; c.clamp = p->clamp; c.miptbp1 = p->miptbp1; c.miptbp2 = p->miptbp2;
    u64 sv = texa; texa = p->texa;
    prim_serial++;                        /* not the memo of the state being set up */
    const tcache_t *e = tc_get(&c, lvl);
    prim_serial++;
    texa = sv;
    u32 b_, bw_, lw_, lh_; tex_level(&c, lvl, &b_, &bw_, &lw_, &lh_);
    out->px = e->px; out->w = (int)e->w; out->h = (int)e->h; out->lw = (int)lw_; out->lh = (int)lh_;
    out->tci = (int)(e - tc); out->ser = e->ser;
    return 1;
}
