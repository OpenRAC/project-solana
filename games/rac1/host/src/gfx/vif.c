/* VIF0 and VIF1: the command streams DMA channels 0 and 1 (and the VIF FIFOs)
 * carry to the vector units.
 *
 * UNPACK writes VU data memory (every vn/vl format, signed/unsigned, the
 * STMASK/STROW/STCOL masking, the STCYCL write/skip cycles, the STMOD
 * offset/difference modes and VIF1's double buffering with BASE/OFFSET/TOPS
 * and the +TOPS flag). MPG uploads microcode (host/src/gfx/vu.c); MSCAL,
 * MSCALF and MSCNT run the translated microprogram there (tools/vu_recomp.py)
 * to completion: the VU programs end with XGKICKs that hand their GIF packets
 * to the GS. DIRECT/DIRECTHL carry GIF packets for the GS (PATH2). The flushes
 * have nothing to wait for, since everything before them has already run. */
#include "uya_host.h"
#include "uya_runtime.h"
#include "vu.h"
#include <stdlib.h>
#include <string.h>

void uya_gif_packet(const u32 *data, u32 qwc);
void vu_mpg(int vu, u32 addr, const u32 *w, u32 ninstr);
void vu_run(int vu, u32 pc);

typedef struct {
    int id;
    u32 cl, wl, mode, mask, row[4], col[4];
    u32 base, ofst, tops, top, itops, itop, dbf, mark;
    u32 code, need, got;         /* the command whose data is still coming */
    u32 *buf, cap;
} Vif;

static u32 buf0[0x1000], buf1[0x10000 * 4 + 16];
static Vif vif[2] = { { .id = 0, .cl = 1, .wl = 1, .buf = buf0, .cap = sizeof buf0 / 4 },
                      { .id = 1, .cl = 1, .wl = 1, .buf = buf1, .cap = sizeof buf1 / 4 } };
static unsigned long long cmd_count[128], frames;

static const char *cmd_name(u32 c)
{
    switch (c) {
    case 0x00: return "NOP"; case 0x01: return "STCYCL"; case 0x02: return "OFFSET"; case 0x03: return "BASE";
    case 0x04: return "ITOP"; case 0x05: return "STMOD"; case 0x06: return "MSKPATH3"; case 0x07: return "MARK";
    case 0x10: return "FLUSHE"; case 0x11: return "FLUSH"; case 0x13: return "FLUSHA"; case 0x14: return "MSCAL";
    case 0x15: return "MSCALF"; case 0x17: return "MSCNT"; case 0x20: return "STMASK"; case 0x30: return "STROW";
    case 0x31: return "STCOL"; case 0x4A: return "MPG"; case 0x50: return "DIRECT"; case 0x51: return "DIRECTHL";
    }
    return (c & 0x60) == 0x60 ? "UNPACK" : "?";
}

u32 vu_vif_top(int vu) { return vif[vu & 1].top; }
u32 vu_vif_itop(int vu) { return vif[vu & 1].itop; }

/* vectors of an UNPACK of `num` writes that take data (the rest are filled) */
static u32 data_vectors(const Vif *v, u32 num)
{
    u32 cl = v->cl, wl = v->wl ? v->wl : 1;
    if (wl <= cl) return num;
    return (num / wl) * cl + (num % wl < cl ? num % wl : cl);
}

static u32 unpack_words(const Vif *v, u32 cmd, u32 num)
{
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
    u32 bits = vl == 3 ? 16 : (32 >> vl) * (vn + 1);
    return (bits * data_vectors(v, num) + 31) / 32;
}

/* element k (of the format's element size) of the data */
static inline u32 elem(const u32 *d, u32 nwords, u32 vl, u32 k, int usn)
{
    switch (vl) {
    case 0: return k < nwords ? d[k] : 0;
    case 1: { if (k / 2 >= nwords) return 0; u16 h = ((const u16 *)d)[k]; return usn ? h : (u32)(s32)(s16)h; }
    case 2: { if (k / 4 >= nwords) return 0; u8 b = ((const u8 *)d)[k]; return usn ? b : (u32)(s32)(s8)b; }
    }
    return 0;
}


/* the usual UNPACK: no mask, every write takes data (WL <= CL); the format is
 * a constant here (see unpack()), so elem() comes down to a load */
static inline __attribute__((always_inline)) void unpack_plain(Vif *v, u8 *mem, u32 qmask, u32 addr, u32 cl, u32 wl,
                                                                const u32 *d, u32 nwords, u32 num, const u32 vn, const u32 vl, u32 usn)
{
    for (u32 k = 0; k < num; k++) {
        u32 qa = wl == cl ? addr + k : addr + (k / wl) * cl + k % wl, val[4];
        u32 e = k * (vn + 1);
        switch (vn) {
        case 0: val[0] = val[1] = val[2] = val[3] = elem(d, nwords, vl, e, usn); break;
        case 1: val[0] = val[2] = elem(d, nwords, vl, e, usn); val[1] = val[3] = elem(d, nwords, vl, e + 1, usn); break;
        default: for (int c = 0; c < 4; c++) val[c] = elem(d, nwords, vl, e + c, usn); break;
        }
        u32 *dst = (u32 *)(mem + ((qa & qmask) << 4));
        if (v->mode == 0) { dst[0] = val[0]; dst[1] = val[1]; dst[2] = val[2]; dst[3] = val[3]; }
        else if (v->mode == 1) for (int c = 0; c < 4; c++) dst[c] = val[c] + v->row[c];
        else if (v->mode == 2) for (int c = 0; c < 4; c++) { v->row[c] += val[c]; dst[c] = v->row[c]; }
        else for (int c = 0; c < 4; c++) dst[c] = val[c];
    }
}

static void unpack(Vif *v, u32 code, const u32 *d, u32 nwords)
{
    u32 cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    if (!num) num = 256;
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3, m = (cmd >> 4) & 1, usn = (imm >> 14) & 1;
    u32 qmask = v->id ? 0x3FF : 0xFF;
    u32 addr = imm & 0x3FF;
    if (v->id && (imm & 0x8000)) addr += v->tops;
    u8 *mem = (u8 *)(uintptr_t)(v->id ? 0x1100C000u : 0x11004000u);
    u32 cl = v->cl, wl = v->wl ? v->wl : 1, di = 0;
    if (!m && wl <= cl && vl != 3) {
        switch (vn * 4 + vl) {
#define U(n, l) case n * 4 + l: unpack_plain(v, mem, qmask, addr, cl, wl, d, nwords, num, n, l, usn); return;
        U(0, 0) U(0, 1) U(0, 2) U(1, 0) U(1, 1) U(1, 2) U(2, 0) U(2, 1) U(2, 2) U(3, 0) U(3, 1) U(3, 2)
#undef U
        }
    }
    for (u32 k = 0; k < num; k++) {
        u32 pos = k % wl, qa, has;
        if (wl <= cl) { qa = addr + (k / wl) * cl + pos; has = 1; }
        else { qa = addr + k; has = pos < cl; }
        u32 val[4] = { 0, 0, 0, 0 };
        if (has) {
            if (vl == 3) {                              /* V4-5: RGBA 5:5:5:1 */
                u32 h = di / 2 < nwords ? ((const u16 *)d)[di] : 0;
                val[0] = (h & 0x1F) << 3; val[1] = ((h >> 5) & 0x1F) << 3; val[2] = ((h >> 10) & 0x1F) << 3; val[3] = ((h >> 15) & 1) << 7;
            } else {
                u32 e = di * (vn + 1);
                switch (vn) {
                case 0: val[0] = val[1] = val[2] = val[3] = elem(d, nwords, vl, e, usn); break;
                case 1: val[0] = val[2] = elem(d, nwords, vl, e, usn); val[1] = val[3] = elem(d, nwords, vl, e + 1, usn); break;
                case 2: for (int c = 0; c < 4; c++) val[c] = elem(d, nwords, vl, e + c, usn); break;   /* w: the next element */
                case 3: for (int c = 0; c < 4; c++) val[c] = elem(d, nwords, vl, e + c, usn); break;
                }
            }
            di++;
        }
        u32 *dst = (u32 *)(mem + ((qa & qmask) << 4));
        u32 r = pos < 3 ? pos : 3;
        for (int c = 0; c < 4; c++) {
            u32 mk = m ? (v->mask >> ((r * 4 + c) * 2)) & 3 : (has ? 0 : 1);
            switch (mk) {
            case 0:
                if (v->mode == 1) dst[c] = val[c] + v->row[c];
                else if (v->mode == 2) { v->row[c] += val[c]; dst[c] = v->row[c]; }
                else dst[c] = val[c];
                break;
            case 1: dst[c] = v->row[c]; break;
            case 2: dst[c] = v->col[r]; break;
            default: break;
            }
        }
    }
}

static void start(Vif *v, u32 pc)
{
    if (v->id) {
        v->top = v->tops;
        if (v->dbf) { v->tops = v->base; v->dbf = 0; }
        else { v->tops = (v->base + v->ofst) & 0x3FF; v->dbf = 1; }
    }
    v->itop = v->itops;
    vu_run(v->id, pc);
}

static void bad_cmd(Vif *v, u32 code)
{
    static u32 seen[2][4];
    u32 c = (code >> 24) & 0x7F;
    if (seen[v->id][c / 32] & (1u << (c % 32))) return;
    seen[v->id][c / 32] |= 1u << (c % 32);
    uya_log("VIF%d: command 0x%02X (%08X) not handled", v->id, c, code);
}

/* run a command once all its data (d, n words) is there */
int uya_vif_log;   /* testing: UYA_DMALOG=n also lists send n's VIF commands */

static u32 hist[64][3], hist_n;   /* testing: the last VIF1 commands (code, TOPS, first data word) */
void uya_vif1_dump_hist(void)
{
    uya_log("VIF1: BASE %03X OFFSET %03X TOPS %03X TOP %03X DBF %u ITOP %03X; last commands:", vif[1].base, vif[1].ofst, vif[1].tops, vif[1].top, vif[1].dbf, vif[1].itop);
    for (u32 i = 0; i < 64; i++) { u32 k = (hist_n + i) & 63; if (hist[k][0] || hist[k][1]) uya_log("   %-8s %08X  tops %03X  data0 %08X", cmd_name((hist[k][0] >> 24) & 0x7F), hist[k][0], hist[k][1], hist[k][2]); }
}

static void exec(Vif *v, u32 code, const u32 *d, u32 n)
{
    u32 cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    if (v->id) { cmd_count[cmd]++; hist[hist_n & 63][0] = code; hist[hist_n & 63][1] = v->tops; hist[hist_n & 63][2] = n ? d[0] : 0; hist_n++; }
    if (uya_vif_log) uya_log("    VIF%d %-8s %08X (%u data words)", v->id, cmd_name(cmd), code, n);
    switch (cmd) {
    case 0x00: case 0x06: case 0x10: case 0x11: case 0x13: break;
    case 0x01: v->cl = imm & 0xFF; v->wl = (imm >> 8) & 0xFF; break;
    case 0x02: v->ofst = imm & 0x3FF; v->dbf = 0; v->tops = v->base; break;
    case 0x03: v->base = imm & 0x3FF; break;
    case 0x04: v->itops = imm & 0x3FF; break;
    case 0x05: v->mode = imm & 3; break;
    case 0x07: v->mark = imm; break;
    case 0x14: case 0x15: start(v, imm * 8); break;
    case 0x17: start(v, v->id ? rc_vu1.tpc : rc_vu0.tpc); break;
    case 0x20: v->mask = d[0]; break;
    case 0x30: memcpy(v->row, d, 16); break;
    case 0x31: memcpy(v->col, d, 16); break;
    case 0x4A: vu_mpg(v->id, imm * 8, d, num ? num : 256); break;
    case 0x50: case 0x51:
        if (v->id) {
            extern const char *uya_gs_src; uya_gs_src = "PATH2 (VIF1 DIRECT)";
            static int left = -1;      /* testing: UYA_BADTEX=n logs DIRECT packets with a TEX0 whose top half looks like a pointer */
            if (left < 0) { const char *e = getenv("UYA_BADTEXV"); left = e ? atoi(e) : 0; }
            if (left > 0)
                for (u32 k = 0; k + 3 < n; k += 4)
                    if ((d[k + 2] & 0xFF) == 6 && (d[k + 1] >> 16) >= 0xFF00) {
                        left--;
                        uya_log("DIRECT packet at %08X (%u qwords) has TEX0 %08X%08X at qword %u:", (u32)(uintptr_t)d, n / 4, d[k + 1], d[k], k / 4);
                        for (u32 j = 0; j < n && j < 64; j += 4) uya_log("   %08X: %08X %08X %08X %08X", (u32)(uintptr_t)(d + j), d[j], d[j + 1], d[j + 2], d[j + 3]);
                        break;
                    }
            uya_gif_packet(d, n / 4);
        }
        else bad_cmd(v, code);
        break;
    default:
        if ((cmd & 0x60) == 0x60) unpack(v, code, d, n);
        else bad_cmd(v, code);
        break;
    }
}

/* words of data that follow a command */
static u32 data_words(Vif *v, u32 code)
{
    u32 cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF, num = (code >> 16) & 0xFF;
    switch (cmd) {
    case 0x20: return 1;
    case 0x30: case 0x31: return 4;
    case 0x4A: return (num ? num : 256) * 2;
    case 0x50: case 0x51: return (imm ? imm : 0x10000) * 4;
    }
    if ((cmd & 0x60) == 0x60) return unpack_words(v, cmd, num ? num : 256);
    return 0;
}

static void feed(Vif *v, const u32 *w, u32 n)
{
    u32 i = 0;
    while (i < n) {
        if (v->need) {
            u32 take = v->need < n - i ? v->need : n - i;
            if (!v->got && take == v->need) {            /* all of it here: no copy */
                v->need = 0;
                exec(v, v->code, w + i, take);
                i += take;
                continue;
            }
            for (u32 k = 0; k < take; k++) if (v->got + k < v->cap) v->buf[v->got + k] = w[i + k];
            v->got += take; v->need -= take; i += take;
            if (!v->need) { u32 g = v->got < v->cap ? v->got : v->cap; v->got = 0; exec(v, v->code, v->buf, g); }
            continue;
        }
        u32 code = w[i++];
        u32 nd = data_words(v, code);
        if (!nd) { exec(v, code, 0, 0); continue; }
        v->code = code; v->need = nd; v->got = 0;
    }
}

void uya_vif0_words(const u32 *w, u32 n) { feed(&vif[0], w, n); }
void uya_vif0_data(const u32 *w, u32 qwc) { feed(&vif[0], w, qwc * 4); }
void uya_vif1_words(const u32 *w, u32 n) { feed(&vif[1], w, n); }
void uya_vif1_data(const u32 *w, u32 qwc) { feed(&vif[1], w, qwc * 4); }

void uya_gfx_frame_stats(void)
{
    frames++;
    if (frames == 3 || frames == 600 || frames == 3000) {
        void vu_stats(void);
        uya_log("VIF1 command counts after %llu sends:", frames);
        for (int c = 0; c < 128; c++)
            if (cmd_count[c]) uya_log("  %-8s %llu", cmd_name(c), cmd_count[c]);
        vu_stats();
    }
}
