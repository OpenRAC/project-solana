/* RAC1 core: the Sony libraries, newlib and kernel calls the game makes, by
 * their PAL (SCES_509.16) addresses.
 *
 * The UYA Wrapper's implementations are kept under host/src/sdk with the
 * prefix uyafn_ (their UYA addresses). Where RAC1's library code is the same
 * function (libgraph, libdma: matched instruction for instruction, see
 * docs/RAC1_NOTES.md) the RAC1 address forwards to it. The rest is written
 * here from OpenRAC's own C for these functions (src/core). */
#include "uya_host.h"
#include "uya_runtime.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

/* ---- newlib ------------------------------------------------------------- */
void *func_00115248(void *d, const void *s, u32 n) { return memmove(d, s, n); }            /* memcpy */
void *func_001153FC(void *d, s32 c, u32 n) { return memset(d, c, n); }                      /* memset */
s32 func_001151B4(const void *a, const void *b, u32 n) { return memcmp(a, b, n); }          /* memcmp */
char *func_00116428(const char *s, s32 c) { return strchr(s, c); }                          /* strchr */
char *func_001166FC(char *d, const char *s) { return strcpy(d, s); }                        /* strcpy */
u32 func_00116810(const char *s) { return (u32)strlen(s); }                                 /* strlen */
s32 func_001165B8(const char *a, const char *b) { return strcmp(a, b); }                    /* strcmp */
s32 func_00116948(const char *a, const char *b, u32 n) { return strncmp(a, b, n); }         /* strncmp */
char *func_00116B00(char *d, const char *s, u32 n) { return strncpy(d, s, n); }             /* strncpy */
char *func_00116CC0(const char *a, const char *b) { return strstr(a, b); }                  /* strstr */

/* sprintf (0x00116248) */
int func_00116248(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

/* srand / rand: newlib's LCG on the reent structure's seed, *(D_0012F86C) + 0x58 */
#define RAND_SEED ((u32 *)(uintptr_t)(*(u32 *)0x0012F86C + 0x58))
void func_001160C8(s32 seed) { *RAND_SEED = (u32)seed; }
s32 func_001160D8(void) { return (s32)((*RAND_SEED = *RAND_SEED * 1103515245u + 12345u) & 0x7FFFFFFF); }

/* scePrintf (libkernl, 0x0011A6C8): to the log */
int func_0011A6C8(const char *fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    while (n > 0 && n < (int)sizeof b && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    uya_log("game: %s", b);
    return n;
}
void func_00124B60(void *fmt, ...) { (void)fmt; }   /* libpad2's debug print, compiled out */

/* ---- kernel syscalls (0x00118A40-0x00118EB0, one 16-byte stub each) ---- */
s32 func_00118D80(s32 mode) { (void)mode; return 0; }               /* FlushCache */
s32 func_00118EA0(void) { return 0x02000000; }                       /* GetMemorySize */
/* DMAC interrupt handlers: host/src/sdk/dma.c calls them as the chains run
 * (RAC1's VU1 chain handler, func_00235118, clears the chain-stage bits the
 * draw code waits on and restarts the DMA). */
s32 uyafn_11EB30(s32 ch, s32 (*h)(s32), s32 next);
s32 uyafn_11F9A8(s32 ch);
s32 uya_dmac_remove(s32 ch);
s32 uya_dmac_disable(s32 ch);
s32 func_00118AB0(s32 ch, s32 (*h)(s32), s32 next) { return uyafn_11EB30(ch, h, next); }   /* AddDmacHandler (0x12) */
s32 func_00118AD0(s32 ch, s32 id) { (void)id; return uya_dmac_remove(ch); }   /* RemoveDmacHandler (0x13) */
s32 func_00118B00(s32 ch) { return uyafn_11F9A8(ch); }               /* _EnableDmac (0x16) */
s32 func_00118B10(s32 ch) { return uya_dmac_disable(ch); }           /* _DisableDmac (0x17) */
s32 func_00119460(s32 ch) { return uyafn_11F9A8(ch); }               /* EnableDmac (libkernl, DI/EI around 0x16) */
s32 func_001193F8(s32 ch) { return uya_dmac_disable(ch); }           /* DisableDmac */
static s32 next_sema = 1;
s32 func_00118C70(void *param) { (void)param; return next_sema++; } /* CreateSema (0x40) */
s32 func_00118C80(s32 id) { (void)id; return 0; }                    /* DeleteSema */
s32 func_00118C90(s32 id) { (void)id; return 0; }                    /* SignalSema */
s32 func_00118CB0(s32 id) { return id; }                             /* WaitSema */
s32 func_00118CC0(s32 id) { return id; }                             /* PollSema */
s32 func_00118BE0(void) { return 1; }                                /* GetThreadId (0x2F) */
s32 func_00118E10(s32 id) { (void)id; return -1; }                   /* sceSifDmaStat (0x76): done */
/* IOP memory, as far as the game's own data goes there: sceSifSetDma's EE ->
 * IOP transfers land here, and the stash server (IOPSTASH.IRX, below) hands
 * it back */
#define IOP_RAM_SIZE (2u << 20)
static u8 iop_ram[IOP_RAM_SIZE] __attribute__((aligned(16)));
s32 func_00118E20(void *dma, s32 n)                                  /* sceSifSetDma (0x77): {src, dest, size, attr} x n */
{
    const u32 *d = (const u32 *)dma;
    for (s32 i = 0; d && i < n; i++, d += 4) {
        u32 src = d[0], dst = d[1] & 0x1FFFFFu, size = d[2];
        if ((src >> 28) == 2 || (src >> 28) == 3) src &= 0x0FFFFFFFu;  /* uncached views */
        if (dst < IOP_RAM_SIZE && size <= IOP_RAM_SIZE - dst && src >= PS2_RAM_LO && src + size <= PS2_RAM_HI)
            memcpy(iop_ram + dst, (const void *)(uintptr_t)src, size);
    }
    return 1;
}
s32 func_00118E70(s32 reg) { (void)reg; return 0x00070000; }         /* sceSifGetReg (0x7A): IOP booted, SIF ready */

/* ---- SIF RPC (libkernl): no IOP. A call completes at once with a zeroed
 * reply, which the game's wrappers read as "nothing to report", except for
 * the servers the host provides:
 *  - 0x11, IOPSTASH.IRX (the game's "stash", src/game/stash.c): a buffer of
 *    0x94000 bytes in IOP memory (the module's stash_ram) where the game keeps
 *    data it fetches back later (the pause menu's weapon models and
 *    animations). RPC 2 answers {base, size}; RPC 1 gets {IOP address} and
 *    sends back the receive size's worth of IOP memory from there (the
 *    module's rpc_stash_mgr returns that address as its reply buffer). The
 *    game sends the data with sceSifSetDma (above). Without it every stash
 *    slot was refused (size 0) and the pause menu unpacked the weapon data
 *    from a scrubbed buffer (a crash reading 0xDEE6BEEF). ---------- */
#define STASH_BASE 0x00160000u                       /* an IOP address (region:keep) */
#define STASH_SIZE 0x00094000u
void func_0011AE20(s32 mode) { (void)mode; }                         /* sceSifInitRpc */
s32 func_0011B2F8(void *cd, u32 server, s32 mode)                    /* sceSifBindRpc */
{
    (void)mode;
    if (cd) { ((u32 *)cd)[9] = 0x00F00000 | (server & 0xFFFF); }      /* a nonzero server pointer */
    return 0;
}
s32 func_0011B4C8(void *cd, s32 fno, s32 mode, void *send, s32 ssize, void *recv, s32 rsize,
                  void (*end)(void *), void *endparam)                /* sceSifCallRpc */
{
    (void)mode; (void)ssize;
    u32 server = cd ? ((u32 *)cd)[9] & 0xFFFF : 0;
    if (recv && rsize > 0) memset(recv, 0, (size_t)rsize);
    if (server == 0x11 && recv) {                                     /* IOPSTASH */
        if (fno == 2 && rsize >= 8) { ((u32 *)recv)[0] = STASH_BASE; ((u32 *)recv)[1] = STASH_SIZE; }
        else if (fno == 1 && send) {
            u32 src = ((const u32 *)send)[0] & 0x1FFFFFu;
            if (rsize > 0 && src < IOP_RAM_SIZE && (u32)rsize <= IOP_RAM_SIZE - src) memcpy(recv, iop_ram + src, (size_t)rsize);
        }
    }
    if (end) end(endparam);
    return 0;
}
s32 func_0011B6B8(void *cd) { (void)cd; return 0; }                  /* sceSifCheckStatRpc: idle */

/* ---- libgraph and libdma: the UYA implementations ------------------------ */
void uyafn_12BEE8(s32 mode, s32 inter, s32 omode, s32 ffmode);
void uyafn_12C138(u64 *disp, s32 psm, s32 w, s32 h, s32 dx, s32 dy);
void uyafn_12C4B0(u64 *disp);
s16 uyafn_12C570(s16 psm, s16 w, s16 h);
s32 uyafn_12C638(u64 *d, s16 psm, s16 w, s16 h, s16 ztest, s16 zpsm);
s32 uyafn_12C908(s32 mode);
s32 uyafn_12C9A0(u64 *lp, s32 dbp, s32 dbw, s32 dpsm, s32 x, s32 y, s32 w, s32 h);
s32 uyafn_12CCC8(u64 *lp, void *src);
s32 uyafn_12D4D8(s32 fn);
void *uyafn_12D650(s32 id);
s32 uyafn_12D938(void *chan, void *tag);
void uyafn_12A950(void);
void *uyafn_12C078(void);
void uya_gs_gif(const u32 *d, u32 qwc);

void func_00121B78(s16 mode, s16 inter, s16 out, s16 ff) { uyafn_12BEE8(mode, inter, out, ff); }  /* sceGsResetGraph */
void *func_00121D08(void) { return uyafn_12C078(); }                                          /* sceGsGetGParam */
void func_00121DC8(u64 *d, s32 psm, s32 w, s32 h, s32 dx, s32 dy) { uyafn_12C138(d, psm, w, h, dx, dy); }  /* sceGsSetDefDispEnv */
void func_00122140(u64 *d) { uyafn_12C4B0(d); }                                               /* sceGsPutDispEnv */
s16 func_00122200(s16 psm, s16 w, s16 h) { return uyafn_12C570(psm, w, h); }
s32 func_001222C8(u64 *d, s16 psm, s16 w, s16 h, s16 zt, s16 zpsm) { return uyafn_12C638(d, psm, w, h, zt, zpsm); }  /* sceGsSetDefDrawEnv */
s32 func_00122598(s32 mode) { return uyafn_12C908(mode); }                                    /* sceGsSyncV */
/* testing: UYA_SICHECK=v also logs sceGsSetDefLoadImage calls from vsync v */
s32 func_00122630(u64 *lp, s32 dbp, s32 dbw, s32 dpsm, s32 x, s32 y, s32 w, s32 h)                      /* sceGsSetDefLoadImage */
{
    static int n; static long v0 = -2;
    if (v0 == -2) { const char *e = getenv("UYA_SICHECK"); v0 = e ? atol(e) : -1; }
    if (v0 >= 0 && (long)uya_frame_count() >= v0 && n++ < 400)
        uya_log("SetDefLoadImage v%u: lp %08X bp %X bw %d psm %X (%d,%d) %dx%d", uya_frame_count(), (u32)(uintptr_t)lp, dbp, dbw, dpsm, x, y, w, h);
    return uyafn_12C9A0(lp, dbp, dbw, dpsm, x, y, w, h);
}
s32 func_00122958(u64 *lp, void *src) { return uyafn_12CCC8(lp, src); }                      /* sceGsExecLoadImage */
/* sceGsSetDefStoreImage (0x00122818) / sceGsExecStoreImage (0x00122AD8): GS -> memory.
 * The pause menu saves VRAM strips with them before it draws over that memory. */
void uya_gs_store_image(u32 bp, u32 bw, u32 psm, u32 x, u32 y, u32 w, u32 h, void *dst);
s32 func_00122818(s32 *sp, s32 sbp, s32 sbw, s32 spsm, s32 x, s32 y, s32 w, s32 h)
{
    sp[0] = 0x53544F52; sp[1] = sbp; sp[2] = sbw; sp[3] = spsm; sp[4] = x; sp[5] = y; sp[6] = w; sp[7] = h;
    return 0;
}
s32 func_00122AD8(s32 *sp, void *dst)
{
    if (sp[0] != 0x53544F52) return -1;
    { static int n; if (getenv("UYA_SICHECK") && n++ < 40) uya_log("StoreImage: bp %X bw %d psm %X (%d,%d) %dx%d to %08X", sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7], (u32)(uintptr_t)dst); }
    {   /* PS2 uncached / accelerated mirrors of main RAM */
        u32 a = (u32)(uintptr_t)dst;
        if (a >= 0x20000000 && a < 0x40000000) dst = (void *)(uintptr_t)(a & 0x0FFFFFFF);
    }
    uya_gs_store_image((u32)sp[1], (u32)sp[2], (u32)sp[3], (u32)sp[4], (u32)sp[5], (u32)sp[6], (u32)sp[7], dst);
    return 0;
}
s32 func_00123168(s32 fn) { return uyafn_12D4D8(fn); }                                        /* sceGsSyncVCallback */
void *func_001232E0(s32 id) { return uyafn_12D650(id); }                                      /* sceDmaGetChan */
s32 func_001235C8(void *chan, void *tag) { return uyafn_12D938(chan, tag); }                  /* sceDmaSend */
void func_001207B8(void) { uyafn_12A950(); }                                                  /* sceGsResetPath */
s32 func_00123308(s32 mode) { (void)mode; return 0; }                                         /* sceDmaReset */

/* sceGsPutDrawEnv (0x001224B0): a GIF packet (GIFtag + A+D pairs) over channel 2 */
s32 func_001224B0(void *pkt)
{
    u32 *q = (u32 *)pkt;
    u32 nloop = q[0] & 0x7FFF;
    { extern const char *uya_gs_src; uya_gs_src = "PATH3 (GS library)"; }
    uya_gs_gif(q, nloop + 1);
    return 0;
}

/* ---- libcdvd ------------------------------------------------------------ */
/* sceCdCallback (0x00120978): the handler runs when a read completes. Reads
 * finish at once here, so it is owed and delivered at the next sceCdSync or
 * vsync, after the caller has recorded the request (RAC1's stream code sets
 * its "busy" flag right after starting the read; the callback clears it). */
static void (*cd_cb)(s32);
static int cd_cb_owed;
void *func_00120978(void (*cb)(s32)) { void *old = (void *)cd_cb; cd_cb = cb; return old; }
void rac1_cd_service(void)
{
    if (cd_cb_owed && cd_cb) { cd_cb_owed = 0; cd_cb(1); }
}
s32 func_00121750(s32 lsn, s32 n, void *buf, void *mode)            /* sceCdRead: done at once */
{
    cd_cb_owed = 1;
    (void)mode;
    static int log = -1;
    if (log < 0) log = getenv("UYA_DISCLOG") != 0;
    if (log) uya_log("disc read LSN %d x%d -> 0x%08X", lsn, n, (u32)(uintptr_t)buf);
    if (uya_disc_read((u32)lsn, (u32)n, buf)) uya_log("disc read failed at LSN %d", lsn);
    return 1;
}
s32 func_00120F30(s32 mode) { (void)mode; rac1_cd_service(); return 0; }   /* sceCdSync: idle */
s32 func_00121930(void) { return 0; }                                /* sceCdGetError: none */
s32 func_001219C8(void) { return 1; }                                /* sceCdBreak */
s32 func_00120D28(s32 x) { (void)x; return 1; }                      /* cd_check_ncmd: ready */
s32 func_00121040(s32 x) { (void)x; return 1; }                      /* cd_check_scmd: ready */
s32 func_00121A80(u8 *clk)                                           /* sceCdReadClock: BCD */
{
    time_t t = time(0); struct tm *m = localtime(&t);
#define BCD(v) (u8)((((v) / 10) << 4) | ((v) % 10))
    if (clk) { clk[0] = 0; clk[1] = BCD(m->tm_sec); clk[2] = BCD(m->tm_min); clk[3] = BCD(m->tm_hour);
               clk[4] = 0; clk[5] = BCD(m->tm_mday); clk[6] = BCD(m->tm_mon + 1); clk[7] = BCD(m->tm_year % 100); }
#undef BCD
    return 1;
}

/* ---- IOP start-up (InitOnce, func_00201E88): there is no IOP; every step succeeds ---- */
s32 func_0011D248(char *img) { uya_log("IOP reboot with %s (no IOP on the host)", img); return 1; }   /* sceSifRebootIop */
s32 func_0011D210(void) { return 1; }                                /* sceSifSyncIop */
s32 func_0011D960(void) { return 0; }                                /* sceSifInitIopHeap */
void func_0011D9A8(void) {}
s32 func_0011CB40(void) { return 0; }
s32 func_0011BF48(void) { return 0; }                                /* sceFsReset */
s32 func_001211B0(s32 mode) { (void)mode; return 1; }                /* sceCdInit */
s32 func_00121490(s32 mode) { (void)mode; return 1; }                /* sceCdMmode / sync */
s32 func_00121688(s32 mode) { (void)mode; return 2; }                /* sceCdDiskReady: ready */
s32 func_00118D60(s32 mode) { (void)mode; return 0; }                /* syscall 0x61 */

/* ---- game helpers in hand-written assembly ---- */
void uya_dma_poll(void);
/* 0x001F9988: a short busy wait (the game's DMA wait loops call it while a
 * channel's STR bit is set); here it runs the pending transfers. */
void func_001F9988(s32 n) { (void)n; uya_dma_poll(); }

/* printf (newlib, 0x00116078): to the log */
int func_00116078(const char *fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    while (n > 0 && n < (int)sizeof b && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    uya_log("game: %s", b);
    return n;
}

/* ---- libmc: the UYA Wrapper's folder-backed card (host/src/sdk/memcard.c) ---- */
s32 uyafn_12E2E0(s32 mode, s32 *cmd, s32 *result);
s32 uyafn_12E400(s32 port, s32 slot, s32 *type, s32 *free_kb, s32 *format);
s32 uyafn_12E580(s32 port, s32 slot, char *name, s32 mode, s32 maxent, void *table);
s32 uyafn_12DCA0(s32 port, s32 slot, char *name, s32 mode);
s32 uyafn_12DDC8(s32 port, s32 slot, char *name);
s32 uyafn_12DE00(s32 fd);
s32 uyafn_12DEC0(s32 fd, s32 off, s32 whence);
s32 uyafn_12E050(s32 fd, void *buf, s32 size);
s32 uyafn_12E168(s32 fd, void *buf, s32 size);
s32 uyafn_12E7A8(s32 port, s32 slot, char *name);
s32 uyafn_12E6D8(s32 port, s32 slot);
s32 uyafn_12E8D8(s32 port, s32 slot);
s32 func_001236F0(void) { return 0; }                                                          /* sceMcInit */
s32 func_001238B0(s32 port, s32 slot, char *name, s32 mode) { return uyafn_12DCA0(port, slot, name, mode); }   /* sceMcOpen */
s32 func_001239D8(s32 port, s32 slot, char *name) { return uyafn_12DDC8(port, slot, name); }   /* sceMcMkdir */
s32 func_00123A10(s32 fd) { return uyafn_12DE00(fd); }                                         /* sceMcClose */
s32 func_00123AC8(s32 fd, s32 off, s32 whence) { return uyafn_12DEC0(fd, off, whence); }       /* sceMcSeek */
s32 func_00123C30(s32 fd, void *buf, s32 n) { return uyafn_12E050(fd, buf, n); }               /* sceMcRead */
s32 func_00123D48(s32 fd, void *buf, s32 n) { return uyafn_12E168(fd, buf, n); }               /* sceMcWrite */
s32 func_00123F30(s32 mode, s32 *cmd, s32 *result) { return uyafn_12E2E0(mode, cmd, result); } /* sceMcSync */
s32 func_00124068(s32 port, s32 slot, s32 *type, s32 *free_kb, s32 *format) { return uyafn_12E400(port, slot, type, free_kb, format); }  /* sceMcGetInfo */
s32 func_001241F0(s32 port, s32 slot, char *name, s32 mode, s32 n, void *table) { return uyafn_12E580(port, slot, name, mode, n, table); }  /* sceMcGetDir */
s32 func_00124340(s32 port, s32 slot) { return uyafn_12E6D8(port, slot); }                    /* sceMcFormat (RPC 0x10) */
s32 func_00124410(s32 port, s32 slot, char *name) { return uyafn_12E7A8(port, slot, name); }   /* sceMcDelete */
s32 func_00124528(s32 port, s32 slot) { return uyafn_12E8D8(port, slot); }                    /* sceMcUnformat (RPC 0x11) */
s32 func_00124650(void) { return 0; }                                                          /* bind the mc/pad IOP servers */

/* ---- libscf ---- */
s32 func_0012D380(void) { return 1; }   /* system language: 1 = English (InitOnce maps it to the game's language 0) */
void func_0012D818(u8 *clk) { (void)clk; }   /* sceScfGetLocalTimefromRTC: sceCdReadClock (func_00121A80) already gives local time */

/* ---- libpad2 ---- */
unsigned plat_pad_buttons(void);
int plat_pad_sticks(u8 st[4]);   /* left X, left Y, right X, right Y (0x7F centre); 0: no gamepad */
s32 func_00124B88(void) { return 1; }                                   /* scePad2Init */
s32 func_00124BC8(void *param, char *buf) { (void)param; (void)buf; return 0; }   /* scePad2CreateSocket: socket 0 */
s32 func_00124EE0(s32 port) { (void)port; return 1; }                   /* scePad2GetState: 1 = ready (UpdatePad, pad.c) */
s32 func_00124DF0(s32 port, u8 *buf) { (void)port; if (buf) memset(buf, 0, 4); return 0; }   /* button profile: none */
s32 func_00125218(s32 port, u8 *buf) { (void)port; if (buf) memset(buf, 0, 4); return 0; }
/* scePad2Read(socket, buf): buf[0] = Select..Left, buf[1] = L2..Square (both inverted), then the
 * four sticks (0x7F = centre) and twelve pressures, as UpdatePad's report parser (func_002181F0) reads them. */
s32 func_00124D18(s32 id, u8 *buf)
{
    (void)id;
    unsigned b = id == 0 ? plat_pad_buttons() : 0;
    buf[0] = (u8)~(b & 0xFF);
    buf[1] = (u8)~(b >> 8);
    for (int i = 2; i < 6; i++) buf[i] = 0x7F;
    /* bits 16..19 (UYA_KEYS lup/ldown/lleft/lright): the left stick, buf[4] X and buf[5] Y */
    if (b & 0x40000) buf[4] = 0x00; else if (b & 0x80000) buf[4] = 0xFF;
    if (b & 0x10000) buf[5] = 0x00; else if (b & 0x20000) buf[5] = 0xFF;
    /* bits 20..23: the right stick, buf[2] X and buf[3] Y */
    if (b & 0x400000) buf[2] = 0x00; else if (b & 0x800000) buf[2] = 0xFF;
    if (b & 0x100000) buf[3] = 0x00; else if (b & 0x200000) buf[3] = 0xFF;
    {   /* a gamepad's sticks (Windows: XInput), where it has one and they are off centre */
        u8 st[4];
        if (id == 0 && plat_pad_sticks(st)) {
            if (st[0] != 0x7F) buf[4] = st[0];
            if (st[1] != 0x7F) buf[5] = st[1];
            if (st[2] != 0x7F) buf[2] = st[2];
            if (st[3] != 0x7F) buf[3] = st[3];
        }
    }
    /* pressures: right, left, up, down, triangle, circle, cross, square, L1, R1, L2, R2 */
    static const int bit[12] = { 5, 7, 4, 6, 12, 13, 14, 15, 10, 11, 8, 9 };
    for (int i = 0; i < 12; i++) buf[6 + i] = (b >> bit[i]) & 1 ? 0xFF : 0;
    {   /* testing: UYA_PADLOG=1 logs the report whenever it changes */
        static int on = -1; static u8 last[6];
        if (on < 0) on = getenv("UYA_PADLOG") != 0;
        static int hero = -1;
        if (hero < 0) hero = getenv("UYA_HEROLOG") != 0;
        if (hero && id == 0) {   /* testing: the hero's position, yaw, velocity and state (D_0013F450) */
            const float *m = (const float *)(uintptr_t)0x0013F4D0;
            const u8 *pd = (const u8 *)(uintptr_t)0x0013CA40;
            int ix = (*(const s16 *)(pd + 0x18E) + 29) % 30;
            uya_log("v%u hero: pos %.2f %.2f %.2f yaw %.3f vel %.3f %.3f %.3f state %d | pad axes %.2f %.2f %.2f %.2f held %X ang %.3f mag %.3f", uya_frame_count(), m[0], m[1], m[2], m[6], m[24], m[25], m[26], *(const s32 *)(uintptr_t)(0x0013F450 + 0x198),
                    ((const float *)(pd + 0x100))[0], ((const float *)(pd + 0x100))[1], ((const float *)(pd + 0x100))[2], ((const float *)(pd + 0x100))[3], *(const s32 *)(pd + 0x1A0),
                    ((const float *)(pd + 0x258))[ix], ((const float *)(pd + 0x2D0))[ix]);
            const float *g = (const float *)(uintptr_t)0x0015EE60;
            const u8 *h = (const u8 *)(uintptr_t)0x0013F450;
            uya_log("   scales %g %g %g %g %g | hstate %d f180 %.3f f190 %.4f f194 %.4f f414 %.3f f45C %.3f i208C %d", g[0], g[1], g[2], g[3], g[4],
                    *(const s32 *)(h + 0x2084), *(const float *)(h + 0x180), *(const float *)(h + 0x190), *(const float *)(h + 0x194), *(const float *)(h + 0x414), *(const float *)(h + 0x45C), *(const s32 *)(h + 0x208C));
        }
        if (on && id == 0 && memcmp(last, buf, 6)) { memcpy(last, buf, 6); uya_log("pad: buttons %02X%02X RX %02X RY %02X LX %02X LY %02X", buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]); }
    }
    return 18;
}
