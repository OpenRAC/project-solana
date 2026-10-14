/* 0x0012F4A8 wad: reads level arg0's header (0x2434 bytes) from the sector in
 * the table of contents (D_0013A548, 8 bytes per level) into D_0013A5E0.
 * Same as the game's (src/core/wad.c), plus a testing switch:
 * UYA_LEVEL=n (or n@v, see host/src/rac1/boot.c) makes the first level the
 * game loads (level 0) level n instead, and sets the current level
 * (D_0015EE84) to n to match. Without UYA_LEVEL nothing changes. */
#include "common.h"
#include <stdlib.h>
#include <stdint.h>
extern int func_00121750(int, int, void *, void *);
extern int func_00120F30(int);
extern void func_00122598(int);
extern int func_00121930(void);

int func_0012F4A8(int arg0)
{
    static int used;
    if (!used) {
        const char *e = getenv("UYA_LEVEL");
        used = 1;
        if (e && arg0 == 0) {
            arg0 = atoi(e);
            *(volatile s32 *)0x0015EE84 = arg0;
            uya_log("UYA_LEVEL: loading level %d instead of level 0 (testing)", arg0);
        }
    }
    char buf[0x2800];
    char hdr[4];
    const int *entry = (const int *)(uintptr_t)(0x0013A548 + arg0 * 8);
    hdr[0] = 0x20;
    hdr[1] = *(volatile unsigned char *)0x0015EE58;
    hdr[2] = 0;
    hdr[3] = 0;
    do {
        func_00121750(*entry, 5, buf, hdr);
        while (func_00120F30(1) != 0) func_00122598(0);
    } while (func_00121930() != 0);
    for (unsigned i = 0; i < 0x2434; i++) ((char *)(uintptr_t)0x0013A5E0)[i] = buf[i];
    return 1;
}
