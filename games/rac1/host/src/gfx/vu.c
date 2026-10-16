/* VU0/VU1 micro mode: which translated microprogram is in micro memory, and
 * running it.
 *
 * The game uploads microcode with MPG (VIF0 for VU0, VIF1 for VU1). The bytes
 * land in micro memory at their real address (VU0 0x11000000, 4 KB; VU1
 * 0x11008000, 16 KB). tools/vu_recomp.py translated every microprogram image
 * in the executable's .vutext to C at build time and kept its bytes: after an
 * upload, every known upload block that overlaps it is compared with micro
 * memory, so a start address (vcallms / MSCAL / MSCNT) maps to the image whose
 * blocks are all in place. Microcode that matches nothing is reported once
 * with its address and a hash (UYA_VUDUMP=dir also saves it), so it can be
 * added to the build. */
#include "uya_host.h"
#include "uya_runtime.h"
#include "vu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { u32 vma, size; const u32 *words; } VuBlock;
typedef struct { const char *name; int vu; void (*fn)(RcVu0 *, u32); int nblk; const VuBlock *blk; } VuImage;
extern const VuImage vu_images[];
extern const int vu_image_count;

RcVu0 rc_vu1 = { .vf = { [0] = {0, 0, 0, 1} } };

#define MAX_IMG 64
static u8 blk_ok[MAX_IMG][16];          /* block b of image i is in micro memory */
static int cur_img[2][0x4000 / 8];      /* image last found for a start address, -1 none */
static int cache_valid[2];

static u8 *micro(int vu) { return (u8 *)(uintptr_t)(vu ? 0x11008000u : 0x11000000u); }
u32 vu_nonfinite;   /* VU FMAC results that overflowed to inf/NaN (made +-MAX: vu_fc, vu.h) */
static u32 micro_size(int vu) { return vu ? 0x4000 : 0x1000; }

void vu_bad(int vu, u32 pc, const char *what)
{
    static int n;
    if (n++ < 32) uya_log("VU%d microprogram: %s at 0x%04X", vu, what, pc);
}

static u32 fnv(const u8 *p, u32 n) { u32 h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }

/* MPG: ninstr instructions to micro memory at byte address addr */
void vu_mpg(int vu, u32 addr, const u32 *w, u32 ninstr)
{
    u8 *m = micro(vu);
    u32 msz = micro_size(vu), bytes = ninstr * 8;
    for (u32 i = 0; i < ninstr; i++) memcpy(m + ((addr + i * 8) & (msz - 1)), w + i * 2, 8);
    /* re-check every known block that overlaps the upload */
    int covered = 0;
    for (int i = 0; i < vu_image_count && i < MAX_IMG; i++) {
        const VuImage *im = &vu_images[i];
        if (im->vu != vu) continue;
        for (int b = 0; b < im->nblk && b < 16; b++) {
            const VuBlock *bl = &im->blk[b];
            if (bl->vma < addr + bytes && addr < bl->vma + bl->size) {
                blk_ok[i][b] = memcmp(m + bl->vma, bl->words, bl->size) == 0;
                if (blk_ok[i][b] && bl->vma <= addr && addr + bytes <= bl->vma + bl->size) covered = 1;
            }
        }
    }
    cache_valid[vu] = 0;
    if (!covered) {
        static u32 seen[64]; static int nseen;
        u32 h = fnv((const u8 *)w, bytes);
        int k;
        for (k = 0; k < nseen && seen[k] != h; k++) ;
        if (k == nseen) {
            if (nseen < 64) seen[nseen++] = h;
            uya_log("VU%d: microcode uploaded at 0x%04X (%u instructions, hash %08X) is not one the build translated", vu, addr, ninstr, h);
            const char *dir = getenv("UYA_VUDUMP");
            if (dir) {
                char path[512]; snprintf(path, sizeof path, "%s/vu%d_%04X_%08X.bin", dir, vu, addr, h);
                FILE *f = fopen(path, "wb"); if (f) { fwrite(w, 1, bytes, f); fclose(f); }
            }
        }
    }
}

/* The image to run from pc: the one whose upload containing pc is in micro
 * memory, preferring one that is there completely (a later upload may have
 * replaced a part of an image the program at pc never reaches, e.g. image
 * 28259's stub at 0x0000 under the 0x0000-0x0580 one). */
static int find_image(int vu, u32 pc)
{
    if (!cache_valid[vu]) { memset(cur_img[vu], 0xFF, sizeof cur_img[vu]); cache_valid[vu] = 1; }
    int *slot = &cur_img[vu][(pc & (micro_size(vu) - 1)) / 8];
    if (*slot >= 0) return *slot;
    int part = -1;
    for (int i = 0; i < vu_image_count && i < MAX_IMG; i++) {
        const VuImage *im = &vu_images[i];
        if (im->vu != vu) continue;
        int has = 0, ok = 1;
        for (int b = 0; b < im->nblk && b < 16; b++) {
            const VuBlock *bl = &im->blk[b];
            if (pc >= bl->vma && pc < bl->vma + bl->size && blk_ok[i][b]) has = 1;
            if (!blk_ok[i][b]) ok = 0;
        }
        if (has && ok) { *slot = i; return i; }
        if (has && part < 0) part = i;
    }
    return *slot = part;
}

static unsigned long long runs[2], kicks, kick_qw;
static unsigned long long img_runs[MAX_IMG];
static u32 *pc_runs[MAX_IMG];             /* runs per start address, for the statistics */
static const char *cur_name = ""; static u32 cur_pc;
const char *uya_vu_cur_image(void) { return cur_name; }   /* (the last microprogram run) */

/* Testing: UYA_VUCAPTURE=image,n saves the state (start address, registers,
 * data memory) before and after the n-th run of that image to
 * vucap_before.bin / vucap_after.bin in the working directory, to replay it
 * against a reference interpreter (tools/vu_refcheck.py). The file ends
 * with VIF TOP and ITOP. */
static void capture(const char *path, int vu, u32 pc, const RcVu0 *v)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&pc, 4, 1, f); fwrite(v, sizeof *v, 1, f);
    fwrite((void *)(uintptr_t)(vu ? 0x1100C000u : 0x11004000u), 1, vu ? 0x4000 : 0x1000, f);
    u32 tops[2] = { vu_vif_top(vu), vu_vif_itop(vu) };
    fwrite(tops, 4, 2, f);
    fclose(f);
}

static void xgkick_flush(void);
/* testing: UYA_VUCAPSEQ=image,vsync appends every VU1 run of that image during that vsync
 * to vucapseq.bin: "VCAP", start pc, the state and data memory before, the GIF packets it
 * kicked (count, then qwords + data each), the state and data memory after. For checking a
 * native replacement of a microprogram against the program itself (dev/vu_harness). */
static FILE *seq_f; static int seq_on = -1, seq_kicks; static long seq_kpos; static char seq_img[32]; static unsigned seq_vs;
static int seq_want(const char *img)
{
    if (seq_on < 0) { const char *e = getenv("UYA_VUCAPSEQ"); seq_on = e && sscanf(e, "%31[^,],%u", seq_img, &seq_vs) == 2; }
    return seq_on > 0 && uya_frame_count() == seq_vs && !strcmp(img, seq_img);
}
static void seq_state(const RcVu0 *v)
{
    fwrite(v, sizeof *v, 1, seq_f); fwrite((void *)(uintptr_t)0x1100C000u, 1, 0x4000, seq_f);
    u32 tops[2] = { vu_vif_top(1), vu_vif_itop(1) }; fwrite(tops, 4, 2, seq_f);
}
static void seq_begin(u32 pc, const RcVu0 *v)
{
    if (!seq_f) seq_f = fopen("vucapseq.bin", "wb");
    if (!seq_f) return;
    fwrite("VCAP", 1, 4, seq_f); fwrite(&pc, 4, 1, seq_f); seq_state(v);
    seq_kicks = 0; seq_kpos = ftell(seq_f); fwrite(&seq_kicks, 4, 1, seq_f);
}
static void seq_kick(const u32 *buf, u32 n)
{
    if (!seq_f || seq_kpos <= 0) return;
    fwrite(&n, 4, 1, seq_f); fwrite(buf, 16, n, seq_f); seq_kicks++;
}
static void seq_end(const RcVu0 *v)
{
    if (!seq_f) return;
    long here = ftell(seq_f); fseek(seq_f, seq_kpos, SEEK_SET); fwrite(&seq_kicks, 4, 1, seq_f); fseek(seq_f, here, SEEK_SET);
    seq_kpos = 0; seq_state(v); fflush(seq_f);
}
/* Microprograms replaced by native code (host/src/native): the program never runs; the native
 * code reads what the game unpacked into VU1 data memory and sends the GS what the program sent.
 * UYA_NATIVE=0 runs the programs instead. */
#include "../native/native_tie.h"
#include "../native/native_shrub.h"
#include "../native/native_tfrag.h"
#include "../native/native_moby.h"
void uya_gif_packet(const u32 *data, u32 qwc);
static const char *native_src = "PATH1 native";
static void native_emit(const u32 *pk, u32 qwc)
{
    extern const char *uya_gs_src; uya_gs_src = native_src;
    uya_gif_packet(pk, qwc);
}
static int native_vu1(const char *img, u32 pc)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("UYA_NATIVE"); on = !e || atoi(e) != 0; if (!on) uya_log("UYA_NATIVE=0: the VU1 programs draw everything"); }
    if (!on) return 0;
    const u8 *mem = (const u8 *)(uintptr_t)0x1100C000u;
    if (img[0] == '5' && !strcmp(img, "56467")) {           /* shrubs */
        native_src = "PATH1 native shrub";
        if (native_shrub_run((u8 *)(uintptr_t)0x1100C000u, pc, vu_vif_top(1), native_emit)) return 1;
        static int told; if (!told++) uya_log("native shrubs: no native code for the program's start 0x%04X (the program runs)", pc);
        return 0;
    }
    if (img[0] == '1' && !strcmp(img, "13859")) {           /* mobys */
        static int reset;
        static NativeMobyKicks k;
        if (!reset++) native_moby_reset();
        if (native_moby_run(pc, (u8 *)(uintptr_t)0x1100C000u, vu_vif_top(1), &k) < 0) {
            static int told; if (!told++) uya_log("native mobys: no native code for the program's start 0x%04X (the program runs)", pc);
            return 0;
        }
        extern const char *uya_gs_src;
        for (u32 i = 0; i < k.npk; i++) { uya_gs_src = "PATH1 native moby"; uya_gif_packet(k.d + k.off[i] * 4, k.len[i]); }
        return 1;
    }
    if (img[0] == '5' && !strcmp(img, "55907")) {           /* terrain (tfrag) */
        static int reset;
        static u32 out[0x4000 * 4];
        static NativeTfragPacket pk[512];
        if (!reset++) native_tfrag_reset();
        int n = native_tfrag_run(pc, (u8 *)(uintptr_t)0x1100C000u, vu_vif_top(1), out, 0x4000, pk, 512);
        if (n < 0) {
            static int told; if (!told++) uya_log("native tfrag: no native code for the program's start 0x%04X (the program runs)", pc);
            return 0;
        }
        extern const char *uya_gs_src;
        for (int i = 0; i < n; i++) { uya_gs_src = "PATH1 native tfrag"; uya_gif_packet(out + pk[i].off * 4, pk[i].n); }
        return 1;
    }
    if (img[0] == '1' && !strcmp(img, "13507")) {           /* ties */
        switch (pc) {
        case 0x30: native_tie_setup(mem); return 1;        /* a class packet */
        case 0x10: case 0x20: case 0x40: return 1;          /* the program's own bookkeeping */
        case 0x00: {                                        /* an instance */
            static u32 buf[0x400 * 4];
            u32 sz = native_tie_size();
            if (!sz) return 0;
            extern const char *uya_gs_src;
            u32 top = vu_vif_top(1), ns = native_tie_state((u8 *)(uintptr_t)0x1100C000u, top, buf);
            if (ns) { uya_gs_src = "PATH1 native tie"; uya_gif_packet(buf, ns); }
            memset(buf, 0, sz * 16);
            u32 n = native_tie_draw(mem, top, buf);
            uya_gs_src = "PATH1 native tie";
            if (n) uya_gif_packet(buf, n);
            return 1;
        }
        }
    }
    return 0;
}
unsigned long long uya_t_vu;
static void vu_run_(int vu, u32 pc);
extern int uya_timing;   /* host/src/gs/gs.c: UYA_FPSLOG */
void vu_run(int vu, u32 pc)
{
    if (uya_timing <= 0) { vu_run_(vu, pc); return; }
    unsigned lo, hi, lo2, hi2; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    vu_run_(vu, pc);
    __asm__ volatile("rdtsc" : "=a"(lo2), "=d"(hi2));
    uya_t_vu += (((unsigned long long)hi2 << 32) | lo2) - (((unsigned long long)hi << 32) | lo);
}
static void vu_run_(int vu, u32 pc)
{
    RcVu0 *v = vu ? &rc_vu1 : &rc_vu0;
    pc &= micro_size(vu) - 8;
    int i = find_image(vu, pc);
    if (i < 0) {
        static u32 seen[2][0x4000 / 8 / 32];
        u32 k = pc / 8;
        if (!(seen[vu][k / 32] & (1u << (k % 32)))) {
            seen[vu][k / 32] |= 1u << (k % 32);
            uya_log("VU%d: no translated microprogram for start address 0x%04X", vu, pc);
        }
        return;
    }
    runs[vu]++;
    img_runs[i]++;
    if (!pc_runs[i]) pc_runs[i] = calloc(0x800, sizeof(u32));
    if (pc_runs[i]) pc_runs[i][(pc / 8) & 0x7FF]++;
    cur_name = vu_images[i].name; cur_pc = pc;
    static int want = -2, seen; static char wname[32]; static unsigned wpc = 0xFFFFFFFFu;
    if (want == -2) { const char *e = getenv("UYA_VUCAPTURE"); want = -1; if (e && sscanf(e, "%31[^,],%d,%x", wname, &want, &wpc) < 2) want = -1; }
    if (want >= 0 && !strcmp(wname, vu_images[i].name) && (wpc == 0xFFFFFFFFu || wpc == pc) && seen++ == want) {
        capture("vucap_before.bin", vu, pc, v);
        vu_images[i].fn(v, pc);
        if (vu) xgkick_flush();
        capture("vucap_after.bin", vu, pc, v);
        uya_log("VU capture of image %s run %d (from 0x%04X) saved", wname, want, pc);
        return;
    }
    if (vu) {   /* replaced by native code? (UYA_SRCSTATS: its time as a "native" row, its GS work left out) */
        extern int uya_srcstats_on(void); extern void uya_srcstat_add(const char *, unsigned long long, unsigned long long, unsigned long long);
        extern unsigned long long uya_gs_time_in(void);
        int st = uya_srcstats_on();
        unsigned long long g0 = st ? uya_gs_time_in() : 0, t0 = st ? __builtin_ia32_rdtsc() : 0;
        if (native_vu1(vu_images[i].name, pc)) {
            if (st) { char key[48]; snprintf(key, sizeof key, "native %s@%04X", vu_images[i].name, pc); uya_srcstat_add(key, __builtin_ia32_rdtsc() - t0 - (uya_gs_time_in() - g0), 0, 0); }
            return;
        }
    }
    {
        extern int uya_srcstats_on(void); extern void uya_srcstat_add(const char *, unsigned long long, unsigned long long, unsigned long long);
        if (vu && uya_srcstats_on()) {   /* (UYA_SRCSTATS: the microprogram's own time, its GS work left out) */
            extern unsigned long long uya_gs_time_in(void);
            unsigned long long g0 = uya_gs_time_in(), t0 = __builtin_ia32_rdtsc();
            vu_images[i].fn(v, pc);
            xgkick_flush();
            unsigned long long dt = __builtin_ia32_rdtsc() - t0 - (uya_gs_time_in() - g0);
            char key[48]; snprintf(key, sizeof key, "VU %s@%04X", vu_images[i].name, pc);
            uya_srcstat_add(key, dt, 0, 0);
            return;
        }
    }
    if (vu && seq_want(vu_images[i].name)) {   /* UYA_VUCAPSEQ: every run of the image in one vsync */
        seq_begin(pc, v);
        vu_images[i].fn(v, pc);
        xgkick_flush();
        seq_end(v);
        return;
    }
    vu_images[i].fn(v, pc);
    if (vu) xgkick_flush();
}

/* VU1 XGKICK: the GIF packet at quadword qaddr of VU1 data memory goes to the
 * GS (PATH1). The packet is complete when the program kicks it. */
void uya_gif_packet(const u32 *data, u32 qwc);
static u32 kbuf[0x400 * 4], kbuf_n, kbuf_q;
void uya_vu_dump_kick(void)
{
    void uya_vif1_dump_hist(void);
    uya_log("current XGKICK: qw %03X, %u qwords, image %s from %04X; vi:", kbuf_q, kbuf_n, cur_name, cur_pc);
    for (int k = 0; k < 16; k += 4) uya_log("   vi%-2d %04X %04X %04X %04X", k, rc_vu1.vi[k] & 0xFFFF, rc_vu1.vi[k + 1] & 0xFFFF, rc_vu1.vi[k + 2] & 0xFFFF, rc_vu1.vi[k + 3] & 0xFFFF);
    for (u32 k = 0; k < kbuf_n && k < 48; k++) uya_log("   %03X: %08X %08X %08X %08X", (kbuf_q + k) & 0x3FF, kbuf[k * 4], kbuf[k * 4 + 1], kbuf[k * 4 + 2], kbuf[k * 4 + 3]);
    uya_vif1_dump_hist();
}
/* XGKICK starts the transfer; the GIF reads the packet while the program
 * goes on, and programs rely on that: they finish the packet in the kick's
 * own branch delay slot or end-of-program slot (image 221571 writes the last
 * GIF tag's EOP there). So a kick is sent when the next kick comes or the
 * microprogram ends. */
static u32 kick_pending = 0xFFFFFFFFu;
static void xgkick_send(u32 qaddr);
void vu_xgkick(u32 qaddr)
{
    if (kick_pending != 0xFFFFFFFFu) xgkick_send(kick_pending);
    kick_pending = qaddr & 0x3FF;
}
static void xgkick_flush(void)
{
    if (kick_pending != 0xFFFFFFFFu) { u32 q = kick_pending; kick_pending = 0xFFFFFFFFu; xgkick_send(q); }
}
static void xgkick_send(u32 qaddr)
{
    u32 *buf = kbuf;
    const u32 *mem = (const u32 *)(uintptr_t)0x1100C000u;
    u32 q = qaddr & 0x3FF, n = 0;
    while (n < 0x400) {
        const u32 *t = mem + q * 4;
        u64 lo = ((u64)t[1] << 32) | t[0];
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, flg = (lo >> 58) & 3, nreg = (lo >> 60) & 15;
        if (!nreg) nreg = 16;
        u32 len = 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
        for (u32 k = 0; k < len && n < 0x400; k++) { memcpy(buf + n * 4, mem + ((q + k) & 0x3FF) * 4, 16); n++; }
        q = (q + len) & 0x3FF;
        if (eop) break;
    }
    kicks++; kick_qw += n; kbuf_n = n; kbuf_q = qaddr & 0x3FF;
    {
        static int badleft = 2;
        const u32 *t0 = mem + (qaddr & 0x3FF) * 4;
        if (badleft > 0 && !(t0[0] | t0[1] | t0[2] | t0[3])) {
            void uya_vif1_dump_hist(void);
            badleft--;
            uya_log("XGKICK of an empty GIF tag at qw %03X (image %s from %04X); vi:", qaddr & 0x3FF, cur_name, cur_pc);
            for (int k = 0; k < 16; k += 4) uya_log("   vi%-2d %04X %04X %04X %04X", k, rc_vu1.vi[k] & 0xFFFF, rc_vu1.vi[k + 1] & 0xFFFF, rc_vu1.vi[k + 2] & 0xFFFF, rc_vu1.vi[k + 3] & 0xFFFF);
            uya_vif1_dump_hist();
        }
    }
    static int left = -1;                   /* testing: UYA_KICKLOG=n lists the first n packets */
    if (left < 0) { const char *e = getenv("UYA_KICKLOG"); left = e ? atoi(e) : 0; }
    if (left > 0) {
        left--;
        uya_log("XGKICK at qw 0x%03X, %u qwords, image %s from 0x%04X:", qaddr & 0x3FF, n, cur_name, cur_pc);
        for (u32 k = 0; k < n && k < 24; k++) uya_log("   %08X %08X %08X %08X", buf[k * 4], buf[k * 4 + 1], buf[k * 4 + 2], buf[k * 4 + 3]);
    }
    {   /* "PATH1 <image>@<pc> kick <qw>" (snprintf's format, by hand: there are thousands of kicks a frame) */
        extern const char *uya_gs_src; static char src[64];
        static const char hx[] = "0123456789ABCDEF";
        char *o = src; const char *nm = cur_name;
        memcpy(o, "PATH1 ", 6); o += 6;
        while (*nm && o < src + 40) *o++ = *nm++;
        *o++ = '@';
        for (int sh = 12; sh >= 0; sh -= 4) *o++ = hx[(cur_pc >> sh) & 15];
        memcpy(o, " kick ", 6); o += 6;
        for (int sh = 8; sh >= 0; sh -= 4) *o++ = hx[((qaddr & 0x3FF) >> sh) & 15];
        *o = 0;
        uya_gs_src = src;
    }
    seq_kick(buf, n);
    uya_gif_packet(buf, n);
}

void vu_stats(void)
{
    uya_log("VU microprograms run: VU0 %llu, VU1 %llu; XGKICK %llu (%llu qwords)", runs[0], runs[1], kicks, kick_qw);
    for (int i = 0; i < vu_image_count && i < MAX_IMG; i++) {
        if (!img_runs[i]) continue;
        char line[256]; int o = 0;
        for (int k = 0; k < 0x800 && o < 200; k++)
            if (pc_runs[i] && pc_runs[i][k]) o += snprintf(line + o, sizeof line - o, " %04X:%u", k * 8, pc_runs[i][k]);
        line[o] = 0;
        uya_log("  image %s (VU%d): %llu runs, by start address%s", vu_images[i].name, vu_images[i].vu, img_runs[i], line);
    }
}

/* the EE's vcallms / vcallmsr */
void rc_vcallms(RcCtx *c, u32 addr)
{
    (void)c;
    rc_vu0.susp = 0;              /* a new program replaces a parked one */
    vu_run(0, addr);
}

/* The EE wrote a VI register (ctc2): a VU0 program parked in a wait loop for
 * it carries on until it ends or parks again. */
void rc_vu0_resume(void)
{
    if (!rc_vu0.susp) return;
    rc_vu0.susp = 0;
    vu_run(0, rc_vu0.tpc);
}
