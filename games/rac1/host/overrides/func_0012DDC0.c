/* 0x0012DDC0 snd_FlushSoundCommands: delivers owed replies, finished bank loads and stream-safe CD reads; nothing stays queued (host/src/rac1/snd989.c) */
#include "common.h"
s32 rac1_snd_flush(void);
s32 func_0012DDC0(void) { return rac1_snd_flush(); }
