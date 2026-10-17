/* Shrubs drawn natively (host/src/native/native_shrub.c) */
#ifndef NATIVE_SHRUB_H
#define NATIVE_SHRUB_H
#include "uya_host.h"
/* one run of the shrub program at pc: 1 if handled; the GS packets it sends are passed to
 * emit(), in order */
int native_shrub_run(u8 *vumem, u32 pc, u32 top, void (*emit)(const u32 *pk, u32 qwc));
/* the shrub clip program (912339), drawn by the GPU renderer as meshes cut at the near plane: a
 * class packet at base (before the program's own start), a group of instances at grp (1: drawn) */
void native_shrub_clip_class(const u8 *vumem, u32 base);
int native_shrub_clip_group(const u8 *vumem, u32 grp, const float c22[4], const float c23[4], const float c29[4], const float c30[4]);
#endif
