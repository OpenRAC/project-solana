/* gs_gl.h: the GPU (OpenGL 3.3 core) renderer beside the software GS.
 *
 * gs.c keeps the GIF front end, the registers, GS local memory, transfers,
 * CLUTs and the texture cache; with gsgl_on its primitives go to gs_gl.c,
 * which draws them on the GPU into GL textures standing for GS frame and Z
 * buffers (targets), and keeps GS memory and the targets coherent: whatever
 * the CPU side reads or writes in memory a target covers is synced first
 * (gsgl_cpu_read / gsgl_cpu_write). All calls are made on the thread that
 * owns the GL context (the GS thread with MTGS). */
#ifndef GS_GL_H
#define GS_GL_H
#include "gs_types.h"

extern int gsgl_on;                       /* the GL renderer draws */

/* the display circuits' picture (from PMODE, SMODE2, DISPFB, DISPLAY) */
typedef struct { u32 fbp, fbw, psm, dbx, dby, w, h; s32 shx, shy; } gs_disp_t;

int  gsgl_init(u32 *vram);                /* with a current GL 3.3 context: 1 if the renderer can run */
void gsgl_prim(const pcmd_t *st, u32 st_index, const pprim_t *pr);
void gsgl_flush(void);                    /* draw the batched primitives */

/* textures (cmd_setup): 1 if the texture is read from a GPU target directly
 * (p->rt set); else whether a level's memory is newer on the GPU (it is then
 * decoded only if a batch needs it: gs_tex_late); a decoded level in use */
int  gsgl_tex_setup(pcmd_t *p);
int  gsgl_tex_gpu(u32 psm, u32 tbp, u32 tbw, u32 tw, u32 th);
void gsgl_tex_touch(const tlevel_t *l);
void gsgl_clut_changed(void);

/* GS memory the CPU side is about to read or write (a rectangle of a buffer,
 * or a range of 256-byte blocks) */
void gsgl_cpu_read_rect(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h);
void gsgl_cpu_write_rect(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h);
void gsgl_cpu_read(u32 bp, u32 nblocks);
int gsgl_threaded(void);
void gsgl_thread_times(double *busy, double *wait);   /* GL calls go to the GL thread (it owns the context) */
int gsgl_tex_owned(u32 psm, u32 tbp, u32 tbw, u32 tw, u32 th);
void gsgl_tex_read(u32 psm, u32 tbp, u32 tbw, u32 tw, u32 th);

/* the display: to the window (and swap), or read back as BGRX (testing) */
void gsgl_present(const gs_disp_t *d);
void gsgl_present_bgra(const u32 *px, int w, int h);   /* an FMV frame */
int  gsgl_capture(const gs_disp_t *d, u32 *out);
void gsgl_stats(int nvsyncs);         /* counts per vsync since the last call */

/* gs.c: decode a level of a state's texture now */
int  gs_tex_late(const pcmd_t *p, int lvl, tlevel_t *out);

/* the platform: a GL 3.3 core context for the window */
int  plat_gl_init(void);                  /* creates it, current on the calling thread */
void plat_gl_current(int on);             /* make it current on / release it from this thread */
void plat_gl_swap(void);
void plat_gl_size(int *w, int *h);        /* the window's drawable size */
extern int plat_gl_active;                /* presenting with GL (no GDI drawing) */
#endif
