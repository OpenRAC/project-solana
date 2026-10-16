/* Ties drawn natively (host/src/native/native_tie.c) */
#ifndef NATIVE_TIE_H
#define NATIVE_TIE_H
#include "uya_host.h"
void native_tie_setup(const u8 *vumem);
/* one instance: the GS packet into out (at most 0x400 qwords); returns its length in qwords
 * (0: no class set up) */
u32 native_tie_draw(const u8 *vumem, u32 top, u32 *out);
u32 native_tie_size(void);
/* the alpha test switch the program sends before an instance, when its mode changes (0 or 2 qwords) */
u32 native_tie_state(u8 *mem, u32 top, u32 *out);   /* the current class's packet length in qwords */
#endif
