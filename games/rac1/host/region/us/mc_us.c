/* NTSC-U (SCUS_971.99) only: library functions the PAL game does not have.
 * Written in US addresses; tools/region.py copies this folder into the US
 * host tree as it is (no rewriting) and the PAL build never sees it. */
#include "uya_host.h"
#include "uya_runtime.h"

s32 rac1_mc_chdir(s32 port, s32 slot, const char *dir, char *prev);

/* 0x001240C0: sceMcChdir(port, slot, newDir, curDir), RPC 0x0C (its sync
 * callback 0x00124038 copies the previous directory into curDir) */
s32 func_001240C0(s32 port, s32 slot, char *dir, char *cur) { return rac1_mc_chdir(port, slot, dir, cur); }
