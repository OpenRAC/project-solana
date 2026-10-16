/* Shrubs drawn natively (host/src/native/native_shrub.c) */
#ifndef NATIVE_SHRUB_H
#define NATIVE_SHRUB_H
#include "uya_host.h"
/* one run of the shrub program at pc: 1 if handled; the GS packets it sends are passed to
 * emit(), in order */
int native_shrub_run(u8 *vumem, u32 pc, u32 top, void (*emit)(const u32 *pk, u32 qwc));
#endif
