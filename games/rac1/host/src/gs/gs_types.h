/* gs_types.h: the software GS's (gs.c) register context, vertex, primitive
 * and pipeline-state types, shared with the GL renderer (gs_gl.c). */
#ifndef GS_TYPES_H
#define GS_TYPES_H
#include "uya_host.h"

typedef struct {
    u64 tex0, tex1, clamp, xyoffset, scissor, alpha, test, fba, frame, zbuf, miptbp1, miptbp2;
} gs_ctx_t;

typedef struct { float x, y; u32 z; u8 r, g, b, a; float s, t, q, u, v; u8 f; } vtx_t;

/* a texture level as the drawing sees it: w, h: decoded; lw, lh: the level's
 * size (REPEAT/CLAMP); tci, ser: the texture cache entry and its build (GL) */
typedef struct { const u32 *px; int w, h, lw, lh; int tci; u32 ser; } tlevel_t;
typedef struct {
    u8 tme, fge, abe, fst, iip, ate, atst, afail, zte, ztst, zmsk;
    u8 ba, bb, bc, bd, tfx, tcc, fba, clampc, lcm, mxl, mmag, mmin, l, wms, wmt, dthe_;
    u8 date, datm;                      /* destination alpha test: only pixels whose alpha MSB is DATM are drawn */
    u8 aref, fix, fogr, fogg, fogb;
    u8 defer;                           /* GL: levels not decoded yet (their memory is a GPU target's) */
    u16 rt;                             /* GL: the texture is a GPU target (index + 1) */
    u32 fbp, fbw, fpsm, fbmsk, zbp, zpsm;
    int sx0, sx1, sy0, sy1;
    int minu, maxu, minv, maxv;
    float tw, th, k;
    tlevel_t lv[7];
    u64 tex0, tex1, miptbp1, miptbp2;   /* for the REGION modes' reads outside a level */
    u64 clamp, texa;                    /* GL: for decoding a deferred level */
} pcmd_t;
typedef struct { u32 st; u8 type; vtx_t v[3]; } pprim_t;   /* a primitive: its state (index) and vertices */

#define TC_N 4096                       /* texture cache entries */
#endif
