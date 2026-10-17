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
#include <math.h>
#include <stdlib.h>
#include "native_dlist.h"
static void sm_class(void);
static int sm_draw(const float R[4][4], s32 fogi, const u8 *rgba);
static void sm_class(void);

static const u32 *Q(const u8 *mem, u32 a) { return (const u32 *)(mem + (a & 0x3FF) * 16); }
static float fc(float x) { u32 b; memcpy(&b, &x, 4); if ((b & 0x7F800000u) == 0x7F800000u) { b = (b & 0x80000000u) | 0x7F7FFFFFu; memcpy(&x, &b, 4); } return x; }
static s32 ftoi4(float f) { float x = f * 16.0f; return x >= 2147483647.0f ? 0x7FFFFFFF : x <= -2147483648.0f ? (s32)0x80000000u : (s32)x; }
static float itof12(s32 i) { return (float)i * (1.0f / 4096.0f); }

typedef struct ShState {
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
} ShState;
static ShState sh;


/* a class packet at VU memory top into sh */
static void load_class(const u8 *mem, u32 top)
{
    const u32 *h = Q(mem, top);
    sh.nad = h[0]; sh.nstrip = h[1] < 16 ? h[1] : 16; sh.nvert = h[2] < 256 ? h[2] : 256; sh.voff = h[3];
    for (u32 s = 0; s < sh.nstrip; s++) memcpy(sh.tags[s], Q(mem, top + 1 + s), 16);
    if (sh.nad > 4) sh.nad = 4;
    for (u32 b = 0; b < sh.nad; b++) for (u32 a = 0; a < 4; a++) memcpy(sh.ad[b][a], Q(mem, top + 1 + sh.nstrip + b * 4 + a), 16);
    for (u32 j = 0; j < sh.nvert; j++) { memcpy(sh.pos[j], Q(mem, top + sh.voff + j), 16); memcpy(sh.st[j], Q(mem, top + sh.voff + h[2] + j), 16); }
    sh.top = top; sh.valid = 1;
}

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
        load_class(mem, top);
        sm_class();
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
            static u8 rgba[256 * 4];
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
                rgba[j * 4] = (u8)c[0]; rgba[j * 4 + 1] = (u8)c[1]; rgba[j * 4 + 2] = (u8)c[2]; rgba[j * 4 + 3] = (u8)alpha;
                o[8] = (u32)ftoi4(fc(v[0] * q)); o[9] = (u32)ftoi4(fc(v[1] * q)); o[10] = (u32)ftoi4(fc(v[2] * q)); o[11] = (u32)fogi;
            }
            if (buf + size > 0x400) size = 0x400 - buf;
            memcpy(pk, bm, size * 16);
            { u32 t = sh.bufx; sh.bufx = sh.bufy; sh.bufy = t; }
            if (!sm_draw((const float (*)[4])R, fogi, rgba)) emit(pk, size);
        }
        return 1;
    }
    }
    return 0;
}

/* ---- shrubs as native meshes ----
 * As ties (native_tie.c): a class's vertices, texture coordinates and strips become a mesh the
 * first time its packet is seen (found again by its contents, the texture registers' values
 * aside); an instance is that mesh with its transform, colours (its table, its alpha) and fog,
 * in place of its GS packet. The program's output buffers in VU memory are still written as
 * the program wrote them. The packet's shape is checked when the mesh is made (items one after
 * another from qword 0 to the end-of-packet tag, every strip vertex a vertex's place, q 1, the
 * packet within its buffer); else the class goes as packets. UYA_NSHRUBMESH=0: the packets. */
#define SM_ID0 20480u
#define SM_N 4096u
typedef struct { int kind; u32 blk, first, count; u64 prim; int pre; } SmOp;   /* kind 0: a texture block; 1: strips */
typedef struct { u32 *key; u32 nkey; int ok; u32 nv; u8 *rgba; SmOp *op; int nop; } ShrubMesh;
static ShrubMesh *smt; static u32 nsm;
static int sm_on = -1, sm_cur = -1;
static unsigned long long sm_n_mesh, sm_n_pk;
#define SH_H 8192
static u64 sh_k[SH_H]; static int sh_v[SH_H];
static u32 skey[8 + 16 * 4 + 4 * 2 + 256 * 8];

static u32 sm_key(void)
{
    u32 n = 0;
    skey[n++] = sh.nad; skey[n++] = sh.nstrip; skey[n++] = sh.nvert;
    memcpy(skey + n, sh.tag_ad, 16); n += 4;
    for (u32 t = 0; t < sh.nstrip; t++) { memcpy(skey + n, sh.tags[t], 16); n += 4; }
    for (u32 b = 0; b < sh.nad; b++) for (u32 a = 0; a < 4; a++) skey[n++] = (sh.ad[b][a][2] & 0xFF) | (a ? 0 : (sh.ad[b][0][3] & 0x3FF) << 8);
    for (u32 j = 0; j < sh.nvert; j++) { memcpy(skey + n, sh.pos[j], 16); n += 4; memcpy(skey + n, sh.st[j], 16); n += 4; }
    return n;
}
static void sm_drop(void)
{
    for (u32 i = 0; i < nsm; i++) { if (smt[i].ok) native_mesh_free(SM_ID0 + i); free(smt[i].key); free(smt[i].rgba); free(smt[i].op); }
    nsm = 0; memset(sh_k, 0, sizeof sh_k);
}
static int sm_fail(int line) { static int n; if (n++ < 8) uya_log("native shrub meshes: a class goes as packets (native_shrub.c:%d)", line); return 0; }
static int sm_build(ShrubMesh *m, u32 id)
{
    u64 ta = ((u64)sh.tag_ad[1] << 32) | sh.tag_ad[0];
    int ad1 = (ta & 0x7FFF) == 4 && ((ta >> 60) & 15) == 1 && (sh.tag_ad[2] & 15) == 0xE, ad4 = (ta & 0x7FFF) == 1 && ((ta >> 60) & 15) == 4 && (sh.tag_ad[2] & 0xFFFF) == 0xEEEE;   /* (four A+D qwords, either way) */
    if (!(ad1 || ad4) || ((ta >> 58) & 3) || (ta & 0x8000)) { static int t; if (!t++) uya_log("native shrub meshes: A+D tag %08X %08X %08X %08X", sh.tag_ad[0], sh.tag_ad[1], sh.tag_ad[2], sh.tag_ad[3]); return sm_fail(__LINE__); }
    static s8 item[0x400]; static s16 itemk[0x400], start[0x400], vtx[0x400];
    memset(item, 0, sizeof item); memset(start, 0xFF, sizeof start); memset(vtx, 0xFF, sizeof vtx);
    const u32 lim = 1024 - 856;                    /* (the last buffer's room: a packet within it is within every buffer) */
    for (u32 b = 0; b < sh.nad; b++) {
        u32 d = sh.ad[b][0][3] & 0x3FF;
        if (d + 5 > lim) return sm_fail(__LINE__);
        for (u32 q = d; q < d + 5; q++) if (item[q]) return sm_fail(__LINE__);
        item[d] = 1; itemk[d] = (s16)b; for (u32 q = d + 1; q < d + 5; q++) item[q] = 3;
    }
    for (u32 t = 0; t < sh.nstrip; t++) {
        u64 ts = ((u64)sh.tags[t][1] << 32) | sh.tags[t][0];
        if (((ts >> 58) & 3) || ((ts >> 60) & 15) != 3 || (sh.tags[t][2] & 0xFFF) != 0x412) return sm_fail(__LINE__);
        u32 d = sh.tags[t][3] & 0x3FF;
        if (d + 1 + 3 * (sh.tags[t][0] & 0x7FFF) > lim || item[d]) return sm_fail(__LINE__);
        item[d] = 2; itemk[d] = (s16)t;
    }
    for (u32 j = 0; j < sh.nvert; j++) {
        if (sh.st[j][2] != 4096) return sm_fail(__LINE__);
        u32 sl = (u32)sh.pos[j][3] & 0x7FFF;
        if (sl + 3 > lim) return sm_fail(__LINE__);
        for (u32 q = sl; q < sl + 3; q++) {
            if (item[q]) return sm_fail(__LINE__);
            if (start[q] >= 0 && (u32)start[q] != sl) return sm_fail(__LINE__);
            start[q] = (s16)sl;
        }
        vtx[sl] = (s16)j;
    }
    static float v6[256 * 6]; static u32 idx[0x400 * 3]; static SmOp op[64];
    u32 ni = 0; int nop = 0, eop = 0, blk = 0;
    for (u32 q = 0; q < lim && !eop;) {
        if (item[q] == 1) {
            if (nop == 64) return sm_fail(__LINE__);
            op[nop].kind = 0; op[nop].blk = (u32)itemk[q]; nop++; blk = 1;
            q += 5;
        } else if (item[q] == 2) {
            const u32 *tg = sh.tags[itemk[q]];
            u64 ts = ((u64)tg[1] << 32) | tg[0];
            u32 n = tg[0] & 0x7FFF; int pre = (int)((ts >> 46) & 1); u64 prim = (ts >> 47) & 0x7FF;
            if (!blk) return sm_fail(__LINE__);
            if (!(nop && op[nop - 1].kind == 1 && op[nop - 1].pre == pre && op[nop - 1].prim == prim)) {
                if (nop == 64) return sm_fail(__LINE__);
                op[nop].kind = 1; op[nop].first = ni; op[nop].count = 0; op[nop].pre = pre; op[nop].prim = prim; nop++;
            }
            u32 w[3] = { 0, 0, 0 };
            for (u32 k = 0; k < n; k++) {
                u32 sl = q + 1 + 3 * k;
                if (vtx[sl] < 0) return sm_fail(__LINE__);
                w[0] = w[1]; w[1] = w[2]; w[2] = (u32)vtx[sl];
                if (k >= 2) { idx[ni++] = w[0]; idx[ni++] = w[1]; idx[ni++] = w[2]; op[nop - 1].count += 3; }
            }
            if (tg[0] & 0x8000) eop = 1;
            q += 1 + 3 * n;
        } else return sm_fail(__LINE__);
    }
    if (!eop || !ni) return sm_fail(__LINE__);
    for (u32 j = 0; j < sh.nvert; j++) {
        float *o = v6 + j * 6;
        o[0] = (float)sh.pos[j][0]; o[1] = (float)sh.pos[j][1]; o[2] = (float)sh.pos[j][2]; o[3] = 1.0f;
        o[4] = itof12(sh.st[j][0]); o[5] = itof12(sh.st[j][1]);
    }
    m->nv = sh.nvert; m->rgba = calloc(sh.nvert, 4);
    m->op = malloc(nop * sizeof *op); memcpy(m->op, op, nop * sizeof *op); m->nop = nop;
    native_mesh_create(SM_ID0 + id, v6, m->rgba, m->nv, idx, ni);
    return 1;
}
static void sm_class(void)
{
    sm_cur = -1;
    if (sm_on < 0) { const char *e = getenv("UYA_NSHRUBMESH"), *n = getenv("UYA_NATIVE"); sm_on = !(e && !atoi(e)) && !(n && !atoi(n)); }
    if (!sm_on) return;
    u32 nk = sm_key();
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < nk; i++) h = (h ^ skey[i]) * 1099511628211ull;
    if (!h) h = 1;
    u32 s = (u32)(h >> 51) & (SH_H - 1);
    while (sh_k[s]) {
        if (sh_k[s] == h) { ShrubMesh *m = &smt[sh_v[s]]; if (m->nkey == nk && !memcmp(m->key, skey, nk * 4)) { sm_cur = sh_v[s]; return; } }
        s = (s + 1) & (SH_H - 1);
    }
    if (!smt) smt = calloc(SM_N, sizeof *smt);
    if (nsm == SM_N) { sm_drop(); sm_class(); return; }
    ShrubMesh *m = &smt[nsm];
    memset(m, 0, sizeof *m);
    m->key = malloc(nk * 4); memcpy(m->key, skey, nk * 4); m->nkey = nk;
    m->ok = sm_build(m, nsm);
    sh_k[s] = h; sh_v[s] = (int)nsm;
    sm_cur = (int)nsm++;
}
/* an instance as mesh mi: texture blocks ad (the class's), transform R, then screen scale sc and
 * offset of (clip: the GPU cuts at the near plane) */
static int sm_draw_x(int sm_cur, const u32 (*ad)[4][4], const float R[4][4], s32 fogi, const u8 *rgba, const float sc[3], const float of[3], int clip)
{
    if (sm_cur < 0 || !smt[sm_cur].ok || (fogi & 0x8000)) { if (++sm_n_pk % 100000 == 1) uya_log("native shrub meshes: %llu instances as packets (class %d, fog %08X)", sm_n_pk, sm_cur, (u32)fogi); return 0; }
    ShrubMesh *m = &smt[sm_cur];
    u32 id = SM_ID0 + (u32)sm_cur;
    if (memcmp(rgba, m->rgba, m->nv * 4)) { memcpy(m->rgba, rgba, m->nv * 4); native_mesh_colors(id, m->rgba, m->nv); }
    float xf[32];
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) xf[r * 4 + k] = k < 3 ? R[r][k] * sc[k] : R[r][k];
    float F = (float)((fogi >> 4) & 0xFF);
    xf[16] = 1.0f; xf[17] = F; xf[18] = F; xf[19] = 0.0f;
    xf[20] = of[0]; xf[21] = of[1]; xf[22] = of[2]; xf[23] = -3.4028235e38f;   /* (no near limit: these shrubs are inside the view) */
    xf[24] = xf[25] = xf[26] = xf[27] = 0.0f;
    xf[28] = 0.0f; xf[29] = 0.0f; xf[30] = 1e-30f; xf[31] = 1.0f;   /* (the GPU: no near or far cut but w > 0) */
    if (clip) { if (sc[2] == 0.0f) return 0; xf[28] = 1.0f; xf[30] = 1.0f / sc[2]; }   /* (the clip program's: z >= -w; x y: the screen's edges) */
    extern const char *uya_gs_src; uya_gs_src = clip ? "native shrub clip" : "native shrub";
    for (int k = 0; k < m->nop; k++) {
        const SmOp *o = &m->op[k];
        if (o->kind == 0) {
            u32 a[4]; u64 v[4];
            for (int j = 0; j < 4; j++) { const u32 *q = ad[o->blk][j]; a[j] = q[2] & 0xFF; v[j] = ((u64)q[1] << 32) | q[0]; }
            native_regs(a, v, 4);
        } else {
            if (o->pre) { u32 a = 0; u64 v = o->prim; native_regs(&a, &v, 1); }
            native_mesh_draw(id, o->first, o->count, xf);
        }
    }
    if (++sm_n_mesh % 100000 == 0) { u32 bad = 0; for (u32 i = 0; i < nsm; i++) bad += !smt[i].ok; uya_log("native shrub meshes: %u classes (%u as packets), instances %llu as meshes, %llu as packets", nsm, bad, sm_n_mesh, sm_n_pk); }
    return 1;
}
static int sm_draw(const float R[4][4], s32 fogi, const u8 *rgba)
{
    static const float one[3] = { 1, 1, 1 }, zero[3] = { 0, 0, 0 };
    return sm_draw_x(sm_cur, (const u32 (*)[4][4])sh.ad, R, fogi, rgba, one, zero, 0);
}

/* the shrub clip program (912339): its class packets are the main program's (at base); decoded
 * before the program converts them in place, into a class of their own */
static ShState csh; static int sm_clip_cur = -1;
void native_shrub_clip_class(const u8 *mem, u32 base)
{
    static ShState keep; int kc = sm_cur;
    keep = sh;
    load_class(mem, base); sm_class();
    csh = sh; sm_clip_cur = sm_cur;
    sh = keep; sm_cur = kc;
}
/* a group of instances (at VU memory grp) as meshes the GPU cuts at the near plane; c22, c23: the
 * screen scale and offset, c29 the fog clamp, c30 z w the fog scale and offset. 1: all drawn */
int native_shrub_clip_group(const u8 *mem, u32 grp, const float c22[4], const float c23[4], const float c29[4], const float c30[4])
{
    if (sm_clip_cur < 0 || !smt[sm_clip_cur].ok || !csh.valid) { static int t; if (t++ < 3) uya_log("native shrub clip meshes: no class mesh (valid %d, mesh %d)", csh.valid, sm_clip_cur); return 0; }
    u32 n = Q(mem, grp)[0];
    if (n > 8) return 0;
    static u8 rgba[8][256 * 4]; float R[8][4][4]; s32 fogi[8];
    for (u32 i = 0; i < n; i++) {
        u32 base = grp + 1 + i * 28, cols = base + 4;
        for (int k = 0; k < 4; k++) memcpy(R[i][k], Q(mem, base + k), 16);
        float fog = fc(fc(R[i][3][3] * c30[2]) + c30[3]);
        fog = fmaxf(fog, c29[0]); fog = fminf(fog, c29[1]);
        fogi[i] = ftoi4(fog);
        if (fogi[i] & 0x8000) return 0;
        u32 alpha = Q(mem, cols)[3];
        int quirk = csh.nvert > 3 && (csh.st[2][3] & 0x8000);
        for (u32 j = 0; j < csh.nvert; j++) {
            u32 ci = ((u32)csh.st[j][3] & 0x7FFF) + cols;
            if (quirk && j == 3) ci += cols;
            const u32 *c = Q(mem, ci);
            u8 *o = rgba[i] + j * 4; o[0] = (u8)c[0]; o[1] = (u8)c[1]; o[2] = (u8)c[2]; o[3] = (u8)alpha;
        }
    }
    for (u32 i = 0; i < n; i++) sm_draw_x(sm_clip_cur, (const u32 (*)[4][4])csh.ad, (const float (*)[4])R[i], fogi[i], rgba[i], c22, c23, 1);
    return 1;
}
