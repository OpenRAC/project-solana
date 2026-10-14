/* libgraph (Sony's GS library) functions the menu calls, plus boot_elf's
 * game-state word. Ported from the decomp's boot_elf sources and assembly
 * (src/boot_elf/core/12A948.c, asm/boot_elf/nonmatchings/core). */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdlib.h>

void uya_gs_gif(const u32 *d, u32 qwc);

typedef struct { s16 inter, omode, ffmode, version; } GParam;

/* 0x0012C078: sceGsGetGParam(); the table is boot_elf data (interlace, NTSC, frame mode). */
/* RAC1: libgraph's GParam is D_00132E40 (func_00121D08). */
void *uyafn_12C078(void) { return (void *)0x00132E40; }

/* 0x0012C138: sceGsSetDefDispEnv(disp, psm, w, h, dx, dy) for NTSC and PAL */
void uyafn_12C138(u64 *disp, s32 psm, s32 w, s32 h, s32 dx, s32 dy)
{
    GParam *gp = uyafn_12C078();
    s16 sw = (s16)w, sh = (s16)h;
    disp[0] = 0x66;                                            /* PMODE: circuit 2, ALP 0 */
    disp[1] = gp->inter == 0 ? 2 : (gp->ffmode ? 3 : 1);        /* SMODE2 */
    disp[2] = ((u64)(psm & 0xF) << 15) | ((u64)(((sw + 63) >> 6) & 0x3F) << 9);   /* DISPFB */
    if ((gp->omode == 2 || gp->omode == 3) && sw > 0) {
        s32 ntsc = gp->omode == 2;
        s32 magh = (sw + 0x9FF) / sw;
        u64 x = (u64)((dx * magh + (ntsc ? 0x27C : 0x290)) & 0xFFF);
        u64 y = (u64)(((s16)dy + (ntsc ? 0x32 : 0x48)) & 0xFFF);
        u64 dh = (u64)((gp->inter == 1 && gp->ffmode) ? sh * 2 - 1 : sh - 1);
        if (gp->inter != 1) y = (u64)(((s16)dy + (ntsc ? 0x19 : 0x24)) & 0xFFF), dh = (u64)(sh - 1);
        disp[3] = x | (y << 12) | ((u64)(magh - 1) << 23) | ((u64)(magh * sw - 1) << 32) | (dh << 44);
    } else if (gp->omode == 0x50 && sw > 0) {                 /* DTV 480p (progressive scan) */
        u64 x = (u64)((((0x2D0 - sw) / 2) * 2 + dx * 2 + 0xE8) & 0xFFF);
        u64 y = (u64)(((s16)dy + 0x23) & 0xFFF);
        disp[3] = x | (y << 12) | (1ull << 23) | ((u64)(sw * 2 - 1) << 32) | ((u64)(sh - 1) << 44);
    } else {
        disp[3] = 0;
    }
    disp[4] = 0;                                               /* BGCOLOR */
}

/* 0x0012C4B0: sceGsPutDispEnv(disp) */
void uyafn_12C4B0(u64 *disp)
{
    GParam *gp = uyafn_12C078();
    if (!disp) return;
    if (gp->version == 1) {
        *(volatile u64 *)0x12000000 = disp[0];
        *(volatile u64 *)0x12000070 = disp[2];
        *(volatile u64 *)0x12000080 = disp[3];
        *(volatile u64 *)0x120000C0 = disp[4];
    } else {
        *(volatile u64 *)0x12000000 = disp[0];
        *(volatile u64 *)0x12000020 = disp[1];
        *(volatile u64 *)0x12000090 = disp[2];
        *(volatile u64 *)0x120000A0 = disp[3];
        *(volatile u64 *)0x120000E0 = disp[4];
    }
}

/* 0x0012C570: size of a frame buffer in pages */
s16 uyafn_12C570(s16 psm, s16 w, s16 h)
{
    GParam *gp = uyafn_12C078();
    s32 bw = (w + 63) / 64, bh = (psm & 2) ? (h + 63) / 64 : (h + 31) / 32;
    if (gp->inter == 1 && gp->ffmode == 0) return (s16)(bw * bh);
    return (s16)(bw * bh * 2);
}

/* 0x0012C638: sceGsSetDefDrawEnv(draw, psm, w, h, ztest, zpsm): 8 A+D pairs */
s32 uyafn_12C638(u64 *d, s16 psm, s16 w, s16 h, s16 ztest, s16 zpsm)
{
    d[1] = 0x4C; d[0] = ((u64)(((w + 63) >> 6) & 0x3F) << 16) | ((u64)(psm & 0xF) << 24);
    d[3] = 0x4E; d[2] = (u64)(u16)uyafn_12C570(psm, w, h) | ((u64)(zpsm & 0xF) << 24) | (ztest == 0 ? 1ull << 32 : 0);
    d[5] = 0x18; d[4] = ((u64)(0x800 - (s16)(w >> 1)) << 4) | ((u64)(0x800 - (s16)(h >> 1)) << 36);
    d[7] = 0x40; d[6] = ((u64)(w - 1) << 16) | ((u64)(h - 1) << 48);
    d[9] = 0x1A; d[8] = 1;
    d[11] = 0x46; d[10] = 1;
    d[13] = 0x45; d[12] = (psm & 2) ? 1 : 0;
    d[15] = 0x47; d[14] = ztest ? (((u64)(ztest & 3) << 17) | 0x10000) : 0x30000;
    return 8;
}

/* 0x0012C9A0: sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h): a 6-qword
 * GIF packet (BITBLTBUF, TRXPOS, TRXREG, TRXDIR, then an IMAGE tag). */
s32 uyafn_12C9A0(u64 *lp, s32 dbp, s32 dbw, s32 dpsm, s32 x, s32 y, s32 w, s32 h)
{
    s16 p = (s16)dpsm, W = (s16)w, H = (s16)h;
    s32 qwc;
    /* libgraph's table (RAC1 0x001531E0): PSMT8H and PSMT4HL/HH are 8- and
     * 4-bit transfers too (the HUD's PSMT8H textures, re-sent when the pause
     * menu closes); a format it does not know sends no image */
    switch (p) {
    case 0x00: case 0x30: qwc = W * H / 4; break;
    case 0x01: case 0x31: qwc = W * H * 3 / 16; break;
    case 0x02: case 0x0A: case 0x32: case 0x3A: qwc = W * H / 8; break;
    case 0x13: case 0x1B: qwc = W * H / 16; break;
    case 0x14: case 0x24: case 0x2C: qwc = W * H / 32; break;
    default: qwc = 0; break;
    }
    if (qwc > 0x7FFF) { uya_log("sceGsSetDefLoadImage: image too large"); return 0; }
    lp[0] = 4 | 0x8000 | (1ull << 60); lp[1] = 0xE;
    lp[2] = ((u64)(u16)(s16)dbp << 32) | ((u64)((s16)dbw & 0x3F) << 48) | ((u64)(p & 0x3F) << 56); lp[3] = 0x50;
    lp[4] = ((u64)((s16)x & 0x7FF) << 32) | ((u64)((s16)y & 0x7FF) << 48); lp[5] = 0x51;
    lp[6] = (u64)(W & 0xFFF) | ((u64)(H & 0xFFF) << 32); lp[7] = 0x52;
    lp[8] = 0; lp[9] = 0x53;
    lp[10] = (u64)(qwc & 0x7FFF) | 0x8000 | (2ull << 58); lp[11] = 0;
    return 6;
}

static const u32 *ps2_ptr(const void *p)
{
    u32 a = (u32)(uintptr_t)p;
    /* PS2 uncached / accelerated mirrors of main RAM; anything else (main RAM,
     * the scratchpad, or a host pointer such as a stack buffer) is used as is */
    if (a >= 0x20000000 && a < 0x40000000) return (const u32 *)(uintptr_t)(a & 0x0FFFFFFF);
    return (const u32 *)p;
}

/* 0x0012CCC8: sceGsExecLoadImage(lp, src): sends the packet then the pixels over GIF channel 2 */
s32 uyafn_12CCC8(u64 *lp, void *src)
{
    { extern const char *uya_gs_src; uya_gs_src = "PATH3 (GS library)"; }
    uya_gs_gif(ps2_ptr(lp), 6);
    uya_gs_gif(ps2_ptr(src), (u32)(lp[10] & 0x7FFF));
    return 0;
}

/* 0x0013D3C0 / 0x0013D3C8: set / get a game-state word in boot_elf (gp-relative).
 * The menu's front loop uses it for its own states (0 = title, 5 = attract demo...). */
static s32 game_state;
void uyafn_13D3C0(s32 v) { game_state = v; }
s32 uyafn_13D3C8(void) { return game_state; }

/* 0x0012BEE8: sceGsResetGraph(mode, inter, omode, ffmode). Mode 0 resets the
 * GS and sets the video mode, mode 5 only sets the mode, mode 1 only resets.
 * The host keeps the mode in the GParam table (sceGsGetGParam) and in SMODE2,
 * which is what the display code reads; there is no CRT to program. */
void uyafn_12BEE8(s32 mode, s32 inter, s32 omode, s32 ffmode)
{
    GParam *gp = uyafn_12C078();
    if (mode != 0 && mode != 5) return;
    gp->inter = (s16)inter;
    gp->omode = (s16)omode;
    gp->ffmode = ffmode != 0;
    *(volatile u64 *)0x12000020 = (u64)((inter & 1) | ((ffmode & 1) << 1));
    { void uya_set_video_pal(int pal); uya_set_video_pal((omode & 0xFF) == 3); }   /* vsync pacing (graph.c) */
    uya_log("video mode: %s, %s%s", (omode & 0xFF) == 0x50 ? "480p" : (omode & 0xFF) == 3 ? "PAL" : (omode & 0xFF) == 2 ? "NTSC" : "other",
            inter & 1 ? "interlaced" : "non-interlaced", ffmode & 1 ? " frame mode" : "");
}

/* 0x0012A950: sceGsResetPath(): resets VIF1, VU1 and the GIF. Nothing to reset here. */
void uyafn_12A950(void) {}

/* 0x0012D4D8: sceGsSyncVCallback(func): installs a vblank handler and returns
 * the previous one. The menu passes boot_elf's handler at 0x0013D208, which
 * only feeds boot_elf's own timing; the host keeps time itself, so the handler
 * is recorded and not called. */
/* RAC1: the game's handler (vsync_callback, 0x0012F308) counts frames and
 * reads timer 0, so it is called at every vsync (host/src/sdk/graph.c). */
static s32 vsync_cb;
s32 uyafn_12D4D8(s32 fn) { s32 old = vsync_cb; vsync_cb = fn; return old; }
void uya_call_vsync_callback(void)
{
    if (vsync_cb) { typedef s32 (*cb_t)(s32); ((cb_t)(uintptr_t)(u32)vsync_cb)(0); }
}

/* 0x0012A9F0: sceGsSyncPath(mode, timeout): waits for DMA, VIF1 and GIF to go
 * idle and returns 0. Host transfers finish when they are sent, so the paths
 * are always idle. */
s32 uyafn_12A9F0(s32 mode, s32 timeout) { (void)mode; (void)timeout; return 0; }
