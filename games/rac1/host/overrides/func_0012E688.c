/* 0x0012E688 snd_SendIOPCommandAndWait(cmd, size, data): to the host's sound driver (host/src/rac1/snd989.c) */
#include "common.h"
s32 rac1_snd_command(s32 cmd, s32 size, const void *data);
s32 func_0012E688(s32 cmd, s32 size, void *data) { return rac1_snd_command(cmd, size, data); }
