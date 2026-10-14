/* RAC1 start-up: what crt0 and main (src/core/boot.c, 0x0012DB18) do on the
 * PS2, for the host.
 *
 * main runs the executable's game program (func_001E99D8: logos, intro movie,
 * loading screen, sound bank, then the level load), then, forever, ParseBin
 * (0x0012DA38, host/src/rac1/levels.c) puts the next level's program over the
 * executable's main segment and returns its entry point, and main calls it.
 * The level programs are compiled separately (src/overlays, docs/LEVELS.md). */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdlib.h>
#include <string.h>

void uya_gs_init(void);
extern void func_001E99D8(void);
void *func_0012DA38(void);

void rac1_boot(void)
{
    uya_gs_init();
    uya_log("boot: GS ready");
}

/* Testing: UYA_LEVEL="N@V" asks for level N at vsync V, as the title menu's
 * "go to level" (func_0022F4A0 in the executable) does, so a run can reach
 * ParseBin without the menu (only while the executable's program runs).
 * UYA_VSYNCS=N ends the run at vsync N (with the report of missing functions). */
const void *rac1_current_level(void);
void rac1_test_hooks(void)
{
    static int done;
    static long stop = -2;
    if (stop == -2) { const char *s = getenv("UYA_VSYNCS"); stop = s ? atol(s) : -1; }
    if (stop > 0 && uya_frame_count() >= (uint32_t)stop) { uya_log("UYA_VSYNCS=%ld reached", stop); exit(0); }
    const char *e = getenv("UYA_LEVEL");
    if (!e || done || rac1_current_level()) return;
    int lvl = atoi(e);
    const char *at = strchr(e, '@');
    if (uya_frame_count() < (at ? (uint32_t)atoi(at + 1) : 0u)) return;
    done = 1;
    uya_log("UYA_LEVEL: asking for level %d at vsync %u", lvl, uya_frame_count());
    ((void (*)(int))(uintptr_t)0x0022F4A0)(lvl);
}

void rac1_run(void)
{
    {   /* The EE's FPU and the VUs round toward zero (truncate), not to nearest. The game
         * depends on it: level 06 counts a timer down by 0.1 while it is > 0, and only a
         * truncating subtraction ends at or below zero on the step the game expects (to
         * nearest leaves a tiny positive rest, one step more makes a moby's scale negative,
         * its bounding cells cross, the grid frees cells twice and a pointer runs off RAM).
         * MXCSR RC = 11 on this thread (the game's); the GS threads exist already and keep
         * round to nearest. Denormals are left alone (VU MAX/MINI compare integer patterns). */
        unsigned m = __builtin_ia32_stmxcsr();
        __builtin_ia32_ldmxcsr((m & ~0x6000u) | 0x6000u);
    }
    uya_log("calling the executable's game program (func_001E99D8)");
    func_001E99D8();
    for (;;) {
        void (*entry)(void) = (void (*)(void))func_0012DA38();
        uya_log("main: calling the program's entry point 0x%08X", (u32)(uintptr_t)entry);
        entry();
    }
}
