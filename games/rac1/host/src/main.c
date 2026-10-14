/* RAC1 Wrapper: runs the decompiled Ratchet & Clank (PAL, SCES_509.16) natively.
 * usage: rac1_wrapper [SCES_509.16 or -] [disc.iso]   (the executable is read from the disc by default)
 * Both come from the user's own disc; nothing from them is in this project. */
#include "uya_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The disc image: the second argument, RAC1_ISO, or `iso=PATH` in
 * rac1_wrapper.cfg next to the program. */
static const char *cfg_value(const char *key)
{
    static char line[4][1024];
    static int k;
    char p[1100];
    snprintf(p, sizeof p, "%s/rac1_wrapper.cfg", plat_exe_dir());
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char *b = line[k++ & 3];
    size_t kl = strlen(key);
    while (fgets(b, sizeof line[0], f)) {
        b[strcspn(b, "\r\n")] = 0;
        if (!strncmp(b, key, kl) && b[kl] == '=') { fclose(f); return b + kl + 1; }
    }
    fclose(f);
    return 0;
}

static const char *find_iso(int argc, char **argv)
{
    if (argc > 2) return argv[2];
    if (getenv("RAC1_ISO")) return getenv("RAC1_ISO");
    const char *v = cfg_value("iso");
    if (v) return v;
    uya_log("no disc image: put `iso=C:\\path\\to\\Ratchet & Clank (Europe).iso` in %s/rac1_wrapper.cfg", plat_exe_dir());
    return 0;
}

/* SCES_509.16: read from the disc image itself (`disc:`), unless the first
 * argument or `elf=` in the cfg names a file. */
static const char *find_elf(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-")) return argv[1];
    const char *v = cfg_value("elf");
    if (v) return v;
    return "disc:SCES_509.16";
}

/* Sections of the executable: everything but code. .vutext is VU microcode
 * (data the game uploads by DMA). */
static const char *const exe_sections[] = {
    ".vutext", "core.data", "core.rdata", "core.bss", "core.lit",
    ".lit", ".bss", ".data", "lvl.vtbl", "lvl.camvtbl", "lvl.sndvtbl", 0
};

void rac1_boot(void);
void rac1_run(void);

int uya_main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    atexit(plat_at_exit);
    uya_log_open();
    {   /* renderer=gl|soft in the cfg (UYA_RENDERER overrides it) */
        extern const char *uya_renderer_cfg;
        const char *r = cfg_value("renderer");
        uya_renderer_cfg = r ? strdup(r) : 0;
    }
    const char *elf = find_elf(argc, argv);
    if (plat_map_fixed(PS2_RAM_LO, PS2_RAM_HI - PS2_RAM_LO, 1) ||
        plat_map_fixed(PS2_SPR, PS2_SPR_SIZE, 0) ||
        plat_map_fixed(PS2_EE_REGS, PS2_EE_REGS_SZ, 0) ||
        plat_map_fixed(PS2_VU_MEM, PS2_VU_MEM_SZ, 0) ||
        plat_map_fixed(PS2_GS_PRIV, PS2_GS_PRIV_SZ, 0)) {
        uya_log("the PS2 address space could not be mapped");
        return 1;
    }
    plat_install_trap_handler();
    {
        const char *iso = find_iso(argc, argv);
        if (!iso || uya_disc_open(iso) != 0) { uya_log("the disc image could not be opened"); return 1; }
        uya_log("disc image: %s", iso);
    }
    if (uya_load_elf_sections(elf, exe_sections) <= 0) return 1;
    /* code: every byte traps until a trampoline covers it */
    uya_fill_code_traps(0x00112380, 0x0012F580);   /* core.text */
    uya_fill_code_traps(0x001E9080, 0x0023E740);   /* .text (the game program) */
    uya_install_trampolines();
    atexit(uya_report);
    rac1_boot();
    rac1_run();
    return 0;
}
