/* the native renderers' draws to the GS as a native draw list (native_dlist.c) */
#ifndef NATIVE_DLIST_H
#define NATIVE_DLIST_H
#include "uya_host.h"
/* a draw the native code built in the PS2's GIF layout (a PATH1 packet, to its EOP) */
void native_kick(const u32 *pk, u32 qwc);

/* native meshes: geometry kept by the renderer (on the GPU), drawn with a transform each time.
 * id: the caller's (0..32767: terrain 0.., ties 16384.., shrubs 20480.., mobys 24576..). v6: x y z w s t per vertex (w: the weight of the transform's row 3,
 * 1 for a point in the mesh's space); rgba per vertex; triangle indices. */
void native_mesh_create(u32 id, const float *v6, const u8 *rgba, u32 nv, const u32 *idx, u32 ni);
void native_mesh_colors(u32 id, const u8 *rgba, u32 nv);
void native_mesh_update(u32 id, const float *v6, const u8 *rgba, u32 nv);   /* new vertices and colours */
/* new vertices, colours and the first ni triangle indices (no more than the mesh was made with) */
void native_mesh_update_idx(u32 id, const float *v6, const u8 *rgba, u32 nv, const u32 *idx, u32 ni);
void native_mesh_free(u32 id);
/* GS register writes (the material: TEX0, TEX1, CLAMP, ALPHA, TEST... and PRIM), as {addr, value} */
void native_regs(const u32 *addr, const u64 *val, int n);
/* draw triangles [first, first + count) of mesh id; xf as gsgl_mesh_draw (host/src/gs/gs_gl.h) */
void native_mesh_draw(u32 id, u32 first, u32 count, const float xf[32]);
/* 1 if the GPU renderer draws (it clips native meshes at the screen's edges and the near plane) */
int uya_gs_renderer_gl(void);
#endif
