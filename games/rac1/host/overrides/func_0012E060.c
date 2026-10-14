/* 0x0012E060 snd_BankLoadByLoc(lsn, offset): the IOP loads a sound bank from the disc. Returns a bank handle (0 on failure). */
#include "common.h"
u32 rac1_snd_bank_load_lsn(s32 lsn, s32 off);
u32 func_0012E060(s32 lsn, s32 off)
{
    u32 h;
    *(volatile s32 *)0x0015ED88 = 0;
    h = rac1_snd_bank_load_lsn(lsn, off);
    uya_log("989snd: bank at LSN %d (+%d): handle %08X", lsn, off, h);
    return h;
}
