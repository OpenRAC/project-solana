/* GIF data from VIF1 DIRECT (PATH2): drawn by the software GS (host/src/gs) and counted here. */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdlib.h>

static unsigned long long prims[8], tags, regs_written[256], ad_addr[128], image_qw;

void uya_gs_gif(const u32 *d, u32 qwc);

void uya_gif_packet(const u32 *d, u32 qwc)
{
    u32 q = 0;
    static int want = -1;
    uya_gs_gif(d, qwc);
    if (want < 0) want = getenv("UYA_GIFSTATS") != 0;   /* testing: count tags and registers */
    if (!want) return;
    while (q < qwc) {
        const u32 *t = d + q * 4;
        u64 lo = ((u64)t[1] << 32) | t[0], hi = ((u64)t[3] << 32) | t[2];
        u32 nloop = lo & 0x7FFF, eop = (lo >> 15) & 1, pre = (lo >> 46) & 1, prim = (lo >> 47) & 0x7FF;
        u32 flg = (lo >> 58) & 3, nreg = (lo >> 60) & 0xF;
        if (!nreg) nreg = 16;
        tags++;
        if (pre) prims[prim & 7]++;
        q++;
        if (flg == 0) {        /* PACKED: one qword per register per loop */
            for (u32 l = 0; l < nloop; l++)
                for (u32 r = 0; r < nreg; r++) {
                    u32 rg = (hi >> (r * 4)) & 0xF;
                    regs_written[rg]++;
                    if (q < qwc) {
                        const u32 *v = d + q * 4;
                        if (rg == 0xE) { ad_addr[v[2] & 0x7F]++; if ((v[2] & 0x7F) == 0) prims[v[0] & 7]++; }
                        if (rg == 0x0) prims[v[0] & 7]++;
                    }
                    q++;
                }
        } else if (flg == 1) { /* REGLIST: 64 bits per register */
            q += (nloop * nreg + 1) / 2;
        } else {               /* IMAGE */
            q += nloop; image_qw += nloop;
        }
        if (eop) break;
    }
}

void uya_gif_stats(void)
{
    uya_log("GIF tags %llu; prims: point %llu line %llu linestrip %llu tri %llu tristrip %llu fan %llu sprite %llu",
            tags, prims[0], prims[1], prims[2], prims[3], prims[4], prims[5], prims[6]);
    uya_log("PACKED regs: %llu A+D, %llu RGBAQ, %llu ST, %llu UV, %llu XYZF2, %llu XYZ2; image qwords %llu",
            regs_written[0xE], regs_written[1], regs_written[2], regs_written[3], regs_written[4], regs_written[5], image_qw);
    for (int a = 0; a < 128; a++) if (ad_addr[a]) uya_log("  A+D GS reg 0x%02X: %llu", a, ad_addr[a]);
}
