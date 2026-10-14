/* Level programs (docs/LEVELS.md).
 *
 * ParseBin (0x0012DA38, src/core/boot.c) is the PS2's loader for the next
 * program: by the time main calls it, the game has read the level's code
 * overlay from the disc into the buffer D_0015EF4C points at (its first word
 * is the overlay's offset). ParseBin copies the overlay's records (lit, bss,
 * data, the three vtables, text: (address, size, type, entry) and the bytes)
 * over the executable's main segment while their entry point agrees, and
 * returns it. The host does the same copy, so every data record lands where
 * the PS2 puts it, and then switches the code: the level's text becomes int3
 * (like all PS2 code space), the level's trampolines go in at its function
 * addresses (its C from src/overlays, its translated functions), and the
 * recompiled code's dispatch table becomes the level's. The entry point it
 * returns is then a trampoline into the level's own code. */
#include "uya_host.h"
#include "uya_runtime.h"
#include "rac1_levels.h"
#include <stdlib.h>
#include <string.h>

#define D_0015EF4C (*(u32 *)0x0015EF4C)     /* core_lit: the level buffer */
#define EXE_TEXT_LO 0x001E9080u               /* the executable's game program */
#define EXE_TEXT_HI 0x0023E740u

struct chunk { u32 dst, size, pad8, id; };

static const rac1_level_t *current;
const rac1_level_t *rac1_current_level(void) { return current; }

/* int3 over [lo, hi) except where a record of the program just copied lies */
static void trap_fill_outside(u32 lo, u32 hi, const struct chunk *const *recs, int n)
{
    while (lo < hi) {
        u32 end = hi;
        int inside = 0;
        for (int i = 0; i < n; i++) {
            u32 a = recs[i]->dst, b = recs[i]->dst + recs[i]->size;
            if (a <= lo && lo < b) { lo = b; inside = 1; break; }
            if (lo < a && a < end) end = a;
        }
        if (inside) continue;
        uya_fill_code_traps(lo, end);
        lo = end;
    }
}

/* ParseBin (0x0012DA38) */
void *func_0012DA38(void)
{
    u8 *p = (u8 *)(uintptr_t)(D_0015EF4C + *(u32 *)(uintptr_t)D_0015EF4C);
    const struct chunk *recs[16];
    int n = 0;
    u32 id = 0, text_addr = 0, text_bytes = 0;
    for (;;) {
        const struct chunk *c = (const struct chunk *)p;
        if (id == 0) id = c->id;
        else if (c->id != id) break;
        if (c->dst < PS2_RAM_LO || c->dst + c->size > PS2_RAM_HI || n == 16) {
            uya_log("ParseBin: record %d at 0x%08X (+0x%X) is outside RAM; stopping", n, c->dst, c->size);
            exit(1);
        }
        memcpy((void *)(uintptr_t)c->dst, c + 1, c->size);
        uya_log("ParseBin: record %d: 0x%08X-0x%08X (type %u)", n, c->dst, c->dst + c->size, c->pad8);
        if (c->dst <= id && id < c->dst + c->size) { text_addr = c->dst; text_bytes = c->size; }
        recs[n++] = c;
        p += sizeof *c + c->size;
    }
    const rac1_level_t *lv = 0;
    for (int i = 0; i < rac1_level_count; i++)
        if (rac1_levels[i].entry == id && rac1_levels[i].text_addr == text_addr && rac1_levels[i].text_bytes == text_bytes)
            lv = &rac1_levels[i];
    if (!lv) {
        uya_log("ParseBin: a program with entry 0x%08X and text 0x%08X (+0x%X) was loaded; no level program built into the "
                "wrapper matches it (build.py --levels)", id, text_addr, text_bytes);
        exit(0);
    }
    /* code: the executable's game program and the level's text are PS2 code
     * space no more and not yet (int3), then the level's trampolines */
    trap_fill_outside(EXE_TEXT_LO, EXE_TEXT_HI, recs, n);
    uya_fill_code_traps(text_addr, text_addr + text_bytes);
    uya_install_tramp_table(lv->tramps, *lv->tramp_count);
    rc_set_table(lv->rc_table, *lv->rc_count);
    current = lv;
    uya_log("ParseBin: level %02d's program is in (%d records), entry 0x%08X (vsync %u)", lv->id, n, id, uya_frame_count());
    return (void *)(uintptr_t)id;
}
