/* Host runtime: loads data sections from the user's own ELFs into PS2 RAM,
 * writes x86 jumps at the PS2 address of every host-implemented function,
 * and keeps the log of unimplemented calls. */
#include "uya_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- ELF32 little-endian section loader ---- */
typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version, entry, phoff, shoff, flags;
                 uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Ehdr;
typedef struct { uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize; } Shdr;

/* A file in the disc image's root directory (ISO 9660), read whole: the
 * static recompilation runs off the disc, with nothing else from the game. */
int uya_disc_read(uint32_t lsn, uint32_t count, void *buf);
uint8_t *uya_disc_file(const char *name, uint32_t *size)
{
    uint8_t pvd[2048];
    if (uya_disc_read(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) return 0;
    uint32_t dlsn = *(uint32_t *)(pvd + 156 + 2), dlen = *(uint32_t *)(pvd + 156 + 10);
    uint32_t nsec = (dlen + 2047) / 2048;
    uint8_t *dir = malloc(nsec * 2048);
    if (!dir || uya_disc_read(dlsn, nsec, dir)) { free(dir); return 0; }
    size_t nl = strlen(name);
    for (uint32_t o = 0; o < nsec * 2048;) {
        uint8_t len = dir[o];
        if (!len) { o = (o / 2048 + 1) * 2048; continue; }
        uint8_t fl = dir[o + 32];
        const char *fn = (const char *)dir + o + 33;
        if (fl >= nl && !memcmp(fn, name, nl) && (fl == nl || fn[nl] == ';')) {
            uint32_t flsn = *(uint32_t *)(dir + o + 2), fsz = *(uint32_t *)(dir + o + 10);
            free(dir);
            uint8_t *d = malloc(((fsz + 2047) / 2048) * 2048);
            if (!d || uya_disc_read(flsn, (fsz + 2047) / 2048, d)) { free(d); return 0; }
            *size = fsz;
            return d;
        }
        o += len;
    }
    free(dir);
    return 0;
}

int uya_load_elf_sections(const char *path, const char *const *sections)
{
    uint8_t *d;
    long n;
    if (!strncmp(path, "disc:", 5)) {   /* from the disc image */
        uint32_t sz;
        d = uya_disc_file(path + 5, &sz);
        if (!d) { uya_log("%s is not on the disc image", path + 5); return -1; }
        n = (long)sz;
    } else {
        FILE *f = fopen(path, "rb");
        if (!f) { uya_log("cannot open %s", path); return -1; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        d = malloc(n);
        if (!d || fread(d, 1, n, f) != (size_t)n) { fclose(f); return -1; }
        fclose(f);
    }
    Ehdr *e = (Ehdr *)d;
    if (memcmp(e->ident, "\x7f" "ELF", 4) || e->ident[4] != 1 || e->machine != 8) {
        uya_log("%s is not a 32-bit MIPS ELF", path); free(d); return -1;
    }
    Shdr *sh = (Shdr *)(d + e->shoff);
    const char *strtab = (const char *)d + sh[e->shstrndx].offset;
    int loaded = 0;
    for (int i = 0; i < e->shnum; i++) {
        const char *nm = strtab + sh[i].name;
        for (const char *const *s = sections; *s; s++) {
            if (strcmp(nm, *s)) continue;
            uint32_t a = sh[i].addr, sz = sh[i].size;
            if (a < PS2_RAM_LO || a + sz > PS2_RAM_HI) { uya_log("%s %s out of RAM", path, nm); continue; }
            if (sh[i].type == 8 /* NOBITS */) memset((void *)(uintptr_t)a, 0, sz);
            else memcpy((void *)(uintptr_t)a, d + sh[i].offset, sz);
            uya_log("loaded %-12s 0x%08X-0x%08X from %s", nm, a, a + sz, path);
            loaded++;
        }
    }
    free(d);
    return loaded;
}

/* ---- trampolines ---- */
void uya_fill_code_traps(uint32_t lo, uint32_t hi)
{
    memset((void *)(uintptr_t)lo, 0xCC, hi - lo);  /* int3: unknown PS2 code */
}

void uya_install_tramp_table(const uya_tramp_t *table, int count)
{
    for (int i = 0; i < count; i++) {
        uint8_t *p = (uint8_t *)(uintptr_t)table[i].ps2_addr;
        int32_t rel = (int32_t)((uintptr_t)table[i].host_fn - ((uintptr_t)p + 5));
        p[0] = 0xE9;                       /* jmp rel32 */
        memcpy(p + 1, &rel, 4);
    }
    plat_flush_icache((void *)(uintptr_t)PS2_RAM_LO, PS2_RAM_HI - PS2_RAM_LO);
    uya_log("installed %d trampolines", count);
}

void uya_install_trampolines(void) { uya_install_tramp_table(uya_tramp_table, uya_tramp_count); }

/* ---- logging ---- */
static FILE *logfile;
void uya_log_open(void)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/rac1_log.txt", plat_exe_dir());
    {   /* the previous run's log is kept as rac1_log.prev.txt (a retry after a crash keeps the crash) */
        char q[1100];
        snprintf(q, sizeof q, "%s/rac1_log.prev.txt", plat_exe_dir());
        remove(q); rename(p, q);
    }
    logfile = fopen(p, "w");
    if (logfile) fprintf(stderr, "[uya] log file: %s\n", p);
}

void uya_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); fprintf(stderr, "[uya] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
    fflush(stderr);
    if (logfile) {
        va_start(ap, fmt); fprintf(logfile, "[uya] "); vfprintf(logfile, fmt, ap); fputc('\n', logfile); va_end(ap);
        fflush(logfile);
    }
}

#define MAXU 4096
static struct { const char *name; uint32_t addr; unsigned count; } unimpl[MAXU];
static int nunimpl;
static unsigned long long total_calls;

static int bump(const char *name, uint32_t addr)
{
    total_calls++;
    for (int i = 0; i < nunimpl; i++)
        if ((name && unimpl[i].name == name) || (!name && unimpl[i].addr == addr)) return ++unimpl[i].count;
    if (nunimpl < MAXU) { unimpl[nunimpl].name = name; unimpl[nunimpl].addr = addr; unimpl[nunimpl].count = 1; nunimpl++; }
    return 1;
}

void uya_unimpl(const char *name)
{
    if (bump(name, 0) == 1) uya_log("unimplemented game function %s", name);
}

static unsigned long long trap_limit = 2000000;
void uya_trap_call(uint32_t ps2_addr, uint32_t ret_addr, const uint32_t *args)
{
    if (bump(0, ps2_addr) == 1)
        uya_log("unimplemented call to PS2 0x%08X (from host 0x%08X) args %08X %08X %08X %08X",
                ps2_addr, ret_addr, args[0], args[1], args[2], args[3]);
    (void)trap_limit;
}

void uya_report(void)
{
    static int done;
    if (done++) return;
    uya_log("---- %d distinct unimplemented functions, %llu calls ----", nunimpl, total_calls);
    for (int i = 0; i < nunimpl; i++) {
        if (unimpl[i].name) uya_log("  %8u  %s", unimpl[i].count, unimpl[i].name);
        else uya_log("  %8u  PS2 0x%08X", unimpl[i].count, unimpl[i].addr);
    }
}
