#include <stdio.h>
/* libdma and the EE DMA controller, as far as the menu needs them.
 *
 * sceDmaSend walks the source chain (the DMAtag list) the menu built and
 * hands each block of data to uya_dma_sink(); the GS renderer will take its
 * packets from there. Tags with the IRQ bit, and the end of the chain, raise
 * the channel's DMAC interrupt: the CHCR TAG field holds the last tag's
 * upper 16 bits, the way the menu's handlers expect. */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdlib.h>
#include <string.h>

/* EE DMA channel register blocks: CHCR at +0x00, MADR +0x10, QWC +0x20, TADR +0x30. */
static const u32 chan_base[10] = {
    0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
    0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400,
};

/* ---- DMAC interrupt handlers (kernel AddDmacHandler / EnableDmac) ---- */
typedef s32 (*dmac_handler_t)(s32 ch);
static dmac_handler_t handlers[16];
static u32 enabled;

/* 0x0011EB30: AddDmacHandler(channel, handler, next): returns a handler id. */
s32 uyafn_11EB30(s32 ch, dmac_handler_t h, s32 next)
{
    (void)next;
    if (ch >= 0 && ch < 16) handlers[ch] = h;
    return ch + 1;
}

/* 0x0011F9A8: EnableDmac(channel) */
s32 uyafn_11F9A8(s32 ch) { if (ch >= 0 && ch < 16) enabled |= 1u << ch; return 0; }

static int walking[10];   /* a chain walk is running on the channel (its IRQ handler may set STR) */
static int irq_handled(s32 ch) { return ch >= 0 && ch < 16 && (enabled & (1u << ch)) && handlers[ch]; }

/* RemoveDmacHandler / DisableDmac */
s32 uya_dmac_remove(s32 ch) { if (ch >= 0 && ch < 16) handlers[ch] = 0; return 0; }
s32 uya_dmac_disable(s32 ch) { if (ch >= 0 && ch < 16) enabled &= ~(1u << ch); return 0; }

static void raise_irq(s32 ch)
{
    if (ch >= 0 && ch < 16 && (enabled & (1u << ch)) && handlers[ch]) handlers[ch](ch);
}

/* 0x0012D650: sceDmaGetChan(id) */
void *uyafn_12D650(s32 id)
{
    if (id < 0 || id > 9) return 0;
    return (void *)(uintptr_t)chan_base[id];
}

static u32 zero_qw[4 * 4096];
static void *phys(u32 a)
{
    /* host memory (the recompiled code's stack, host buffers): as is */
    if (a >= 0x40000000u && (a >> 28) != 7 && (a >> 31) == 0) return (void *)(uintptr_t)a;
    if (a & 0x80000000u) return (void *)(uintptr_t)(PS2_SPR + (a & 0x3FF0));  /* scratchpad */
    a &= 0x01FFFFF0u;
    if (a < PS2_RAM_LO) {   /* kernel area: never valid for the game; read as zeros */
        static int warned;
        if (!warned++) uya_log("DMA: chain points into the kernel area (0x%08X); reading zeros", a);
        return zero_qw;
    }
    return (void *)(uintptr_t)a;
}

unsigned long long nsends, nqw;

/* Where transferred data goes: VIF1 data to the VIF decoder (host/src/gfx/vif.c). */
void uya_vif1_data(const u32 *data, u32 qwc);
void uya_vif1_words(const u32 *data, u32 nwords);
void uya_gs_gif(const u32 *d, u32 qwc);
void uya_vif0_data(const u32 *data, u32 qwc);
void uya_vif0_words(const u32 *data, u32 nwords);
static void uya_dma_sink(s32 ch, const void *data, u32 qwc)
{
    nqw += qwc;
    if (ch == 0) { uya_vif0_data(data, qwc); return; }
    if (ch == 1) uya_vif1_data(data, qwc);
    if (ch == 2) { extern const char *uya_gs_src; uya_gs_src = "PATH3 (DMA ch2)"; uya_gs_gif(data, qwc); }   /* GIF channel: PATH3 */
}

/* a VIF1 chain tag the terrain hook kept, sent on as the walker would have */
#include "../native/native_terrain.h"
void uya_dma_vif1_replay(const u32 *q, int tte)
{
    u32 qwc = q[0] & 0xFFFF, id = (q[0] >> 28) & 7, addr = q[1] & 0x7FFFFFFF;
    if (tte) uya_vif1_words(q + 2, 2);
    if (id == 3 || id == 4) uya_dma_sink(1, phys(addr), qwc);
    else uya_dma_sink(1, q + 4, qwc);
}

static s32 chan_index(void *chan)
{
    for (s32 i = 0; i < 10; i++) if ((u32)(uintptr_t)chan == chan_base[i]) return i;
    return -1;
}

/* 0x0012D938: sceDmaSend(chan, tag): start a source-chain transfer from `tag`. */
s32 uyafn_12D938(void *chan, void *tag)
{
    s32 ch = chan_index(chan);
    volatile u32 *regs = (volatile u32 *)chan;
    u32 t = (u32)(uintptr_t)tag, stack[2], sp = 0;
    u32 tte = regs[0] & 0x40;                    /* CHCR TTE as the game set it */
    nsends++;
    if (nsends <= 4 || (nsends & (nsends - 1)) == 0)
        uya_log("DMA send #%llu on channel %d, chain at 0x%08X (%llu qwords so far)", nsends, ch, t, nqw);
    if (ch == 1) native_terrain_chain();
    for (int guard = 0; guard < 1000000; guard++) {
        u32 *q = phys(t);
        u32 lo = q[0], hi = q[1];
        u32 qwc = lo & 0xFFFF, id = (lo >> 28) & 7, irq = lo >> 31, addr = hi & 0x7FFFFFFF;
        u32 next = 0;
        int end = 0;
        {
            static long long dlog = -2;
            if (dlog == -2) { const char *e = getenv("UYA_DMALOG"); dlog = e ? atoll(e) : -1; }
            extern int uya_vif_log;
            uya_vif_log = (long long)nsends == dlog;
            if (uya_vif_log) uya_log("  DMA tag at 0x%08X: id %u qwc %u addr 0x%08X upper %08X %08X", t, id, qwc, addr, q[2], q[3]);
        }
        /* terrain draws in the chain become native mesh draws (host/src/native/native_terrain.c) */
        if (ch == 1) {
            if (!irq && (id == 1 || id == 3)) {
                if (native_terrain_tag(q, tte != 0)) { t = id == 3 ? t + 16 : t + 16 + qwc * 16; continue; }
            } else native_terrain_flush();
        }
        /* Tag transfer (CHCR TTE): the tag's upper 64 bits go to the VIF first
         * (two VIF codes). The menu's VIF1 chains always carry VIF codes there
         * (NOP, DIRECT), so it is on for channel 1. */
        if (ch == 1 && tte) uya_vif1_words(q + 2, 2);
        switch (id) {
        case 0: uya_dma_sink(ch, phys(addr), qwc); end = 1; break;                   /* refe */
        case 1: uya_dma_sink(ch, q + 4, qwc); next = t + 16 + qwc * 16; break;       /* cnt  */
        case 2: uya_dma_sink(ch, q + 4, qwc); next = addr; break;                    /* next */
        case 3: case 4: uya_dma_sink(ch, phys(addr), qwc); next = t + 16; break;     /* ref, refs */
        case 5: uya_dma_sink(ch, q + 4, qwc);                                        /* call */
            if (sp < 2) stack[sp++] = t + 16 + qwc * 16;
            next = addr; break;
        case 6: uya_dma_sink(ch, q + 4, qwc);                                        /* ret  */
            if (sp) next = stack[--sp]; else end = 1;
            break;
        case 7: uya_dma_sink(ch, q + 4, qwc); end = 1; break;                        /* end  */
        }
        if (irq || end) {
            u32 tie = regs[0] & 0x80;
            regs[0] = (lo & 0xFFFF0000u) | (regs[0] & 0xC1) | 0x4 | 0x100;  /* CHCR: TAG, chain mode, STR */
            regs[12] = next ? next : t;                  /* TADR: the handlers ignore a zero tag address */
            if (irq && !end && tie && irq_handled(ch)) {
                /* CHCR TIE: the DMAC stops at a tag with the IRQ bit; the
                 * channel's handler (RAC1's VU1 chain handler) restarts it
                 * by setting STR again. Without that the chain stays
                 * suspended until the game sets STR. */
                regs[0] &= ~0x100u;
                walking[ch] = 1;
                raise_irq(ch);
                walking[ch] = 0;
                if (!(regs[0] & 0x100)) { regs[8] = 0; return 0; }
                t = regs[12];
                continue;
            }
            raise_irq(ch);
        }
        if (end) break;
        t = next;
    }
    if (ch == 1) native_terrain_flush();
    if (ch == 1) {
        void uya_gfx_frame_stats(void); void uya_gif_stats(void);
        static long long stop_after = -2;
        if (stop_after == -2) { const char *e = getenv("UYA_FRAMES"); stop_after = e ? atoll(e) : -1; }
        uya_gfx_frame_stats();
        if (nsends == 600) uya_gif_stats();
        if (stop_after > 0 && (long long)nsends >= stop_after) {
            void uya_gs_stats(void);
            const char *rd = getenv("UYA_RAMDUMP");   /* testing: save PS2 RAM (0x100000-0x2000000) */
            void vu_stats(void);
            uya_log("UYA_FRAMES=%lld reached", stop_after); uya_gif_stats(); uya_gs_stats(); vu_stats();
            if (rd) { FILE *f = fopen(rd, "wb"); if (f) { fwrite((void *)0x100000, 1, 0x1F00000, f); fclose(f); } }
            exit(0);
        }
    }
    regs[0] &= ~0x100u;
    regs[8] = 0;
    return 0;
}

/* RAC1 starts DMA by writing a channel's registers itself (CHCR with STR,
 * bit 8), not only through sceDmaSend. Those registers are plain memory on
 * the host, so uya_dma_poll() runs any channel whose STR bit is set: chain
 * mode from TADR through the same walker as sceDmaSend, normal mode as one
 * block of QWC quadwords at MADR. Channel 0 (VIF0) carries VU0 microcode
 * uploads and data to the VIF0 decoder (host/src/gfx/vif.c). Called from the game's busy-wait
 * helper, at every vsync and from the GS sync calls. */
void uya_dma_kick(s32 ch);
void uya_dma_poll(void)
{
    static int busy;
    if (busy) return;
    busy = 1;
    for (s32 ch = 0; ch < 10; ch++) uya_dma_kick(ch);
    busy = 0;
}

/* Scratchpad channels: 8 (fromSPR: SPR -> RAM) and 9 (toSPR: RAM -> SPR), normal mode;
 * SADR at +0x80 is the scratchpad offset. */
static void spr_transfer(s32 ch)
{
    volatile u32 *r = (volatile u32 *)(uintptr_t)chan_base[ch];
    u32 qwc = r[8] & 0xFFFF, madr = r[4], sadr = r[32] & 0x3FF0;
    u8 *spr = (u8 *)(uintptr_t)PS2_SPR, *ram = phys(madr);
    for (u32 i = 0; i < qwc; i++) {
        u8 *s = spr + ((sadr + 16 * i) & 0x3FF0), *m = ram + 16 * i;
        if (ch == 9) memcpy(s, m, 16); else memcpy(m, s, 16);
    }
    r[4] = madr + qwc * 16; r[32] = (sadr + qwc * 16) & 0x3FF0; r[8] = 0;
}

/* run channel ch now if its STR bit is set */
void uya_dma_kick(s32 ch)
{
    volatile u32 *r = (volatile u32 *)(uintptr_t)chan_base[ch];
    u32 chcr = r[0];
    if (!(chcr & 0x100)) return;
    if (ch < 10 && walking[ch]) return;   /* restarted from its own IRQ handler: the walk goes on */
    u32 mode = (chcr >> 2) & 3;
    if (ch == 1 || ch == 2) {
        if (mode == 1) { uyafn_12D938((void *)(uintptr_t)chan_base[ch], (void *)(uintptr_t)r[12]); return; }
        if (r[8]) uya_dma_sink(ch, phys(r[4]), r[8] & 0xFFFF);
    } else if (ch == 0) {   /* VIF0: VU0 microcode (MPG) and data */
        if (mode == 1) {
            u32 t = r[12], stack[2], sp = 0;
            for (int g = 0; g < 65536; g++) {
                u32 *q = phys(t); u32 qwc = q[0] & 0xFFFF, id = (q[0] >> 28) & 7, addr = q[1] & 0x7FFFFFFF, next = 0;
                int end = 0;
                if (chcr & 0x40) uya_vif0_words(q + 2, 2);
                switch (id) {
                case 0: uya_dma_sink(0, phys(addr), qwc); end = 1; break;
                case 1: uya_dma_sink(0, q + 4, qwc); next = t + 16 + qwc * 16; break;
                case 2: uya_dma_sink(0, q + 4, qwc); next = addr; break;
                case 3: case 4: uya_dma_sink(0, phys(addr), qwc); next = t + 16; break;
                case 5: uya_dma_sink(0, q + 4, qwc); if (sp < 2) stack[sp++] = t + 16 + qwc * 16; next = addr; break;
                case 6: uya_dma_sink(0, q + 4, qwc); if (sp) next = stack[--sp]; else end = 1; break;
                default: uya_dma_sink(0, q + 4, qwc); end = 1; break;
                }
                if (end) break;
                t = next;
            }
        } else if (r[8]) uya_dma_sink(0, phys(r[4]), r[8] & 0xFFFF);
    } else if (ch == 8 || ch == 9) {
        if (mode == 0) spr_transfer(ch);
        else if (mode == 1) {   /* chain mode toSPR: walk tags from TADR into the scratchpad */
            u32 t = r[12], sadr = r[32] & 0x3FF0;
            for (int g = 0; g < 4096; g++) {
                u32 *q = phys(t); u32 qwc = q[0] & 0xFFFF, id = (q[0] >> 28) & 7, addr = q[1] & 0x7FFFFFFF;
                const u8 *src = (id == 1 || id == 7) ? (u8 *)(q + 4) : (u8 *)phys(addr);
                for (u32 i = 0; i < qwc; i++) memcpy((u8 *)(uintptr_t)PS2_SPR + ((sadr + 16 * i) & 0x3FF0), src + 16 * i, 16);
                sadr = (sadr + 16 * qwc) & 0x3FF0;
                if (id == 7 || id == 0) break;
                t = id == 1 ? t + 16 + qwc * 16 : (id == 2 ? addr : t + 16);
            }
            r[32] = sadr;
        }
    }
    r[0] &= ~0x100u;
    r[8] = 0;
}

void rc_io_write32(u32 a, u32 v)
{
    *(volatile u32 *)(uintptr_t)a = v;
    for (s32 ch = 0; ch < 10; ch++)
        if (a == chan_base[ch] && (v & 0x100)) { uya_dma_kick(ch); return; }
}

typedef union { u64 d[2]; u32 w[4]; } R128h;
void rc_io_write128(u32 a, const void *v)
{
    if (a == 0x10005000u) { uya_vif1_data((const u32 *)v, 1); return; }   /* VIF1 FIFO */
    if (a == 0x10006000u) { extern const char *uya_gs_src; uya_gs_src = "PATH3 (FIFO)"; uya_gs_gif((const u32 *)v, 1); return; }      /* GIF FIFO (PATH3) */
    if (a == 0x10004000u) { uya_vif0_data((const u32 *)v, 1); return; }   /* VIF0 FIFO */
    const u32 *w = v;
    for (int i = 0; i < 4; i++) rc_io_write32(a + 4 * i, w[i]);
}
