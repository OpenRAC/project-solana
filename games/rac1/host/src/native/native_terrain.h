/* Terrain (tfrags) drawn from the game's own data as native meshes (native_terrain.c) */
#ifndef NATIVE_TERRAIN_H
#define NATIVE_TERRAIN_H
#include "uya_host.h"

/* a VIF1 chain is about to be walked (host/src/sdk/dma.c): the level's tfrag table is
 * checked, and its meshes built when it is new */
void native_terrain_chain(void);
/* the chain walker's next tag (a REF or CNT without IRQ, tte: its upper words go to the VIF):
 * 1 if this file took it (a tfrag's draw, kept until its MSCAL decides it) */
int native_terrain_tag(const u32 *tag, int tte);
/* any other tag: the tags kept so far go on to the VIF as they were */
void native_terrain_flush(void);
#endif
