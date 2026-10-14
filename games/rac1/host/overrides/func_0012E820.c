/* 0x0012E820 snd_SendIOPCommandNoWait(cmd, size, data, callback, arg): the reply goes to the callback at the next flush */
#include "common.h"
s32 rac1_snd_command(s32 cmd, s32 size, const void *data);
void rac1_snd_owe(void *fn, s64 arg, s32 result);
void func_0012E820(s32 cmd, s32 size, void *data, void *cb, s64 arg)
{
    s32 r = rac1_snd_command(cmd, size, data);
    rac1_snd_owe(cb, arg, r);
}
