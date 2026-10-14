/* 0x0012E1C8 snd_BankLoadFromEE_CB(addr, callback, arg): command 0x57. The bank is loaded at once; as in the
 * library, the reply word (the bank handle) lands in D_0015EE00 and snd_FlushSoundCommands calls the callback. */
#include "common.h"
u32 rac1_snd_bank_load_ee(u32 addr);
void func_0012E1C8(s32 addr, void *cb, s64 arg)
{
    *(volatile s32 *)0x0015ED88 = 0;
    if (*(volatile s32 *)0x0015EDC8 != 0) { uya_log("989snd: bank load from EE while another is pending"); return; }
    *(volatile s32 *)0x0015EDD0 = (s32)cb;
    *(volatile s64 *)0x0015EDD8 = arg;
    *(volatile u32 *)0x0015EE00 = rac1_snd_bank_load_ee((u32)addr);
    *(volatile s32 *)0x0015EDC8 = 1;
}
