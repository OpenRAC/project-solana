/* Shrubs that cross the screen's edges, drawn natively (host/src/native/native_shrubclip.c) */
#ifndef NATIVE_SHRUBCLIP_H
#define NATIVE_SHRUBCLIP_H
#include "uya_host.h"
/* one start of the shrub clip microprogram (VU1 image 912339) at pc: 0x0000 the frame's
 * constants, 0x0088 / 0x00A8 a shrub class, 0x0318 a group of instances. vumem is VU1 data
 * memory (16 KB, read and written as the program does); the GIF packets the program kicks are
 * passed to emit() in order, each as the GIF would read it (at the next kick or the run's end).
 * Returns 0, or -1 for a start it does not know (nothing done). top is not used (the program
 * keeps its own double buffering). */
int native_shrubclip_run(u32 pc, u8 *vumem, u32 top, void (*emit)(const u32 *pk, u32 qwc));
/* forget the carried registers (as before the first 0x0000 run) */
void native_shrubclip_reset(void);
/* for a test that starts from a captured VU state: take the registers the program carries from
 * one run to the next (vf17 vf22 vf23 vf24 vf28 vf29 vf30 vf31, vi11 vi12) from a register file
 * (vf: 32 x 4 words, vi: 16 words) */
void native_shrubclip_seed(const u32 vf[32][4], const u32 vi[16]);
/* and back (the same registers into a register file) */
void native_shrubclip_state(u32 vf[32][4], u32 vi[16]);
#endif
