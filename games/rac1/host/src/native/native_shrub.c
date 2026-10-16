/* Shrubs (small plants, grass, reeds), drawn by native code from the game's shrub data
 * instead of by the PS2's VU1 shrub microprogram (image 56467).
 *
 * Runs: 0x0000 once a frame (constants: the A+D tag, fog scale/offset, fog clamp), 0x0088 or
 * 0x00A8 per shrub class (its packet: texture registers, strip tags, vertices), 0x0318 per group
 * of up to five instances (a 4x4 transform and a color table each). Each instance becomes one
 * GS packet, built here word for word as the program built it (checked against the program
 * by dev/vu_harness on every shrub run of whole frames). */
#include "native_shrub.h"
#include <string.h>

static const u32 *Q(const u8 *mem, u32 a) { return (const u32 *)(mem + (a & 0x3FF) * 16); }
static float fc(float x) { u32 b; memcpy(&b, &x, 4); if ((b & 0x7F800000u) == 0x7F800000u) { b = (b & 0x80000000u) | 0x7F7FFFFFu; memcpy(&x, &b, 4); } return x; }
static s32 ftoi4(float f) { float x = f * 16.0f; return x >= 2147483647.0f ? 0x7FFFFFFF : x <= -2147483648.0f ? (s32)0x80000000u : (s32)x; }
static float itof12(s32 i) { return (float)i * (1.0f / 4096.0f); }

static struct {
    u32 tag_ad[4];          /* qword 0 at the frame's start: the A+D tag */
    float fogk[4], clamp[4];/* qwords 1 and 2: z, w: fog = row3.w * z + w; clamp: x = max, y = min */
    int have_consts;
    u32 bufx, bufy;         /* the program's output buffers: instances go to y, then x and y swap;
                               a class's packet template goes to the third buffer (x) and y */
    int valid;
    u32 top;                /* the class's place in VU memory */
    u32 nstrip, nvert, voff, nad;
    u32 tags[16][4], ad[4][4][4];
    s32 pos[256][4], st[256][4];   /* the class's vertices as the game unpacked them (the program
                                      converts them in place; nothing else reads them) */
} sh;

int native_shrub_run(u8 *mem, u32 pc, u32 top, void (*emit)(const u32 *, u32))
{
    switch (pc) {
    case 0x0000:
        memcpy(sh.tag_ad, Q(mem, 0), 16); memcpy(sh.fogk, Q(mem, 1), 16); memcpy(sh.clamp, Q(mem, 2), 16);
        sh.bufx = 856; sh.bufy = 520;
        sh.have_consts = 1;
        return 1;
    case 0x0088: case 0x00A8: {
        /* the next instance group goes to 238 or 379 (617 minus what is kept at qword 1, y) */
        u32 *g = (u32 *)(mem + 1 * 16);
        g[1] = (g[1] & 0xFFFF0000u) | (pc == 0x0088 ? 379u : 238u);
        const u32 *h = Q(mem, top);
        sh.nad = h[0]; sh.nstrip = h[1] < 16 ? h[1] : 16; sh.nvert = h[2] < 256 ? h[2] : 256; sh.voff = h[3];
        for (u32 s = 0; s < sh.nstrip; s++) memcpy(sh.tags[s], Q(mem, top + 1 + s), 16);
        if (sh.nad > 4) sh.nad = 4;
        for (u32 b = 0; b < sh.nad; b++) for (u32 a = 0; a < 4; a++) memcpy(sh.ad[b][a], Q(mem, top + 1 + sh.nstrip + b * 4 + a), 16);
        for (u32 j = 0; j < sh.nvert; j++) { memcpy(sh.pos[j], Q(mem, top + sh.voff + j), 16); memcpy(sh.st[j], Q(mem, top + sh.voff + h[2] + j), 16); }
        sh.top = top; sh.valid = 1;
        sh.bufx = 2064 - sh.bufx - sh.bufy;        /* the buffer neither of the last two used */
        u32 bufs[2] = { sh.bufx, sh.bufy };
        for (int k = 0; k < 2; k++) {
            u8 *b = mem + bufs[k] * 16;
            for (u32 a = 0; a < sh.nad; a++) {
                u32 d = sh.ad[a][0][3] & 0x3FF;
                if (bufs[k] + d + 5 > 0x400) continue;
                memcpy(b + d * 16, sh.tag_ad, 16);
                for (u32 r = 0; r < 4; r++) memcpy(b + (d + 1 + r) * 16, sh.ad[a][r], 16);
            }
            for (u32 t = 0; t < sh.nstrip; t++) { u32 off = sh.tags[t][3] & 0x3FF; if (bufs[k] + off < 0x400) memcpy(b + off * 16, sh.tags[t], 16); }
        }
        return 1;
    }
    case 0x0318: {
        if (!sh.valid || !sh.have_consts) return 0;
        u32 *g = (u32 *)(mem + 1 * 16);
        u32 grp = (617u - (g[1] & 0xFFFF)) & 0xFFFF;
        g[1] = (g[1] & 0xFFFF0000u) | grp;
        u32 n = Q(mem, grp)[0];
        static u32 pk[0x400 * 4];
        for (u32 i = 0; i < n && i < 8; i++) {
            u32 base = grp + 1 + i * 28;
            float R[4][4]; for (int k = 0; k < 4; k++) memcpy(R[k], Q(mem, base + k), 16);
            u32 cols = base + 4;
            u32 buf = sh.bufy;
            u8 *bm = mem + buf * 16;
            u32 size = 0;
            for (u32 b = 0; b < sh.nad; b++) { u32 d = sh.ad[b][0][3] & 0x3FF; if (d + 5 > size) size = d + 5; }
            for (u32 t = 0; t < sh.nstrip; t++) { u32 end = (sh.tags[t][3] & 0x3FF) + 1 + 3 * (sh.tags[t][0] & 0x7FFF); if (end > size) size = end; }
            float fog = fc(fc(R[3][3] * sh.fogk[2]) + sh.fogk[3]);
            if (fog < sh.clamp[0]) fog = sh.clamp[0];
            if (fog > sh.clamp[1]) fog = sh.clamp[1];
            s32 fogi = ftoi4(fog);
            u32 alpha = Q(mem, cols)[3];
            /* the program's quirk: when the third vertex ends the list early, the fourth vertex's
               color is read with the instance's color base added twice (whatever lies there) */
            int quirk = sh.nvert > 3 && (sh.st[2][3] & 0x8000);
            for (u32 j = 0; j < sh.nvert; j++) {
                const s32 *p = sh.pos[j], *st = sh.st[j];
                float x = (float)p[0], y = (float)p[1], z = (float)p[2], v[4];
                for (int k = 0; k < 4; k++) { float acc = fc(R[3][k]); acc = fc(acc + fc(R[0][k] * x)); acc = fc(acc + fc(R[1][k] * y)); v[k] = fc(fc(R[2][k] * z) + acc); }
                float q = v[3] == 0.0f ? 3.4028235e38f : 1.0f / v[3];
                u32 slot = (u32)p[3] & 0x7FFF;
                if (buf + slot + 3 > 0x400) continue;
                u32 *o = (u32 *)(bm + slot * 16);
                float sq[3] = { fc(itof12(st[0]) * q), fc(itof12(st[1]) * q), fc(itof12(st[2]) * q) };
                memcpy(o, sq, 12);
                u32 ci = ((u32)st[3] & 0x7FFF) + cols;
                if (quirk && j == 3) ci += cols;
                const u32 *c = Q(mem, ci);
                o[4] = c[0]; o[5] = c[1]; o[6] = c[2]; o[7] = alpha;
                o[8] = (u32)ftoi4(fc(v[0] * q)); o[9] = (u32)ftoi4(fc(v[1] * q)); o[10] = (u32)ftoi4(fc(v[2] * q)); o[11] = (u32)fogi;
            }
            if (buf + size > 0x400) size = 0x400 - buf;
            memcpy(pk, bm, size * 16);
            { u32 t = sh.bufx; sh.bufx = sh.bufy; sh.bufy = t; }
            emit(pk, size);
        }
        return 1;
    }
    }
    return 0;
}
