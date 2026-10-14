/* Disc access: the engine core's CD read path, served from your own disc image.
 *
 * The game reads 2048-byte sectors by LSN. boot_elf reads the table of
 * contents (16 sectors at LSN 1001) into 0x160C40 before it starts the menu
 * (uyafn_13D2E0); uya_disc_boot() does the same. Reads finish immediately. */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *disc;
static void (*cd_callback)(s32);   /* sceCdCallback-style completion handler */
static int cd_pending;             /* a read finished; its callback is still owed */
static unsigned long long nreads, nsectors;

#ifdef _WIN32
int _fseeki64(FILE *, long long, int);
#define SEEK64(f, o) _fseeki64(f, (long long)(o), SEEK_SET)
#else
#define SEEK64(f, o) fseeko(f, (off_t)(o), SEEK_SET)
#endif

int uya_disc_open(const char *path)
{
    disc = fopen(path, "rb");
    if (!disc) { uya_log("cannot open disc image %s", path); return -1; }
    uya_log("disc image: %s", path);
    return 0;
}

static int read_sectors_q(u32 lsn, u32 count, void *buf, int quiet);
static int read_sectors(u32 lsn, u32 count, void *buf) { return read_sectors_q(lsn, count, buf, 0); }
static int read_sectors_q(u32 lsn, u32 count, void *buf, int quiet)
{
    if (!disc) { uya_log("disc read LSN %u x%u: no disc image", lsn, count); return -1; }
    if (!quiet && ((u32)(uintptr_t)buf < PS2_RAM_LO || (u32)(uintptr_t)buf + count * 2048 > PS2_RAM_HI))
        uya_log("disc read LSN %u x%u into 0x%08X (outside PS2 RAM)", lsn, count, (u32)(uintptr_t)buf);
    /* Testing on a partial copy of the disc: UYA_DISC_MAP names a file with one
     * byte per sector (1 = present). Reads of absent sectors are logged and
     * appended to <map>.missing as "lsn count" so they can be fetched. */
    {
        static FILE *map; static int tried;
        if (!tried++) { const char *m = getenv("UYA_DISC_MAP"); if (m) map = fopen(m, "rb"); }
        if (map) {
            u32 miss = 0, first = 0;
            for (u32 i = 0; i < count; i++) {
                int c = 0;
                if (fseek(map, (long)(lsn + i), SEEK_SET) == 0) c = fgetc(map);
                if (c != 1) { if (!miss) first = lsn + i; miss++; }
            }
            if (miss) {
                char p[1100]; snprintf(p, sizeof p, "%s.missing", getenv("UYA_DISC_MAP"));
                FILE *f = fopen(p, "a"); if (f) { fprintf(f, "%u %u\n", lsn, count); fclose(f); }
                uya_log("disc read LSN %u x%u: %u sectors not in the partial disc copy (first %u)", lsn, count, miss, first);
            }
        }
    }
    if (SEEK64(disc, (unsigned long long)lsn * 2048) != 0) return -1;
    size_t got = fread(buf, 2048, count, disc);
    nreads++; nsectors += count;
    if ((nreads <= 64 && !quiet) || getenv("UYA_DISCLOG")) uya_log("disc read LSN %u x%u -> 0x%08X", lsn, count, (u32)(uintptr_t)buf);
    return got == count ? 0 : -1;
}

int uya_disc_read(uint32_t lsn, uint32_t count, void *buf) { return read_sectors_q(lsn, count, buf, 1); }

/* 0x0013CD28: start a read of `count` sectors at `lsn` into `buf` (mode in the 4th argument). 1 = started. */
s32 uyafn_13CD28(s32 lsn, s32 count, void *buf, void *mode)
{
    (void)mode;
    if (read_sectors((u32)lsn, (u32)count, buf) != 0) return 0;
    cd_pending = 1;   /* on the PS2 the completion interrupt comes later, after the caller marks the read busy */
    return 1;
}

/* Deliver a finished read's callback. Called from the functions the game polls. */
void uya_cd_service(void)
{
    if (cd_pending) {
        cd_pending = 0;
        if (cd_callback) cd_callback(1);   /* 1 = read finished */
    }
}

/* 0x0013CDF0: read status; 0 = idle (with 1: poll, with 0: wait). */
s32 uyafn_13CDF0(s32 block) { (void)block; uya_cd_service(); return 0; }

/* 0x0013CF40: set the read-completion callback, returns the previous one. */
void *uyafn_13CF40(void (*cb)(s32)) { void *old = (void *)cd_callback; cd_callback = cb; return old; }

/* 0x0013CEF8: last read error; 0 = none. */
s32 uyafn_13CEF8(void) { return 0; }

/* 0x0013D290: synchronous read. */
s32 uyafn_13D290(s32 lsn, s32 count, void *buf)
{
    read_sectors((u32)lsn, (u32)count, buf);
    return 0;
}

/* 0x0013D2E0: read the table of contents into 0x160C40. */
s32 uyafn_13D2E0(void)
{
    return uyafn_13D290(0x3E9, 0x10, (void *)0x160C40);
}

void uya_disc_boot(void)
{
    uyafn_13D2E0();
    uya_log("table of contents loaded at 0x00160C40");
}

/* 0x0013D310: load the next level's part of the table of contents (boot_elf).
 * Per level there are three disc locations (TOC + 0x7400/0x7408/0x7410 +
 * level * 0x18); each is read into `buf` and copied into place: 0x60 bytes to
 * TOC + 0x79A0, 0x1818 bytes to TOC + 0x7A00 and 0x26F0 bytes to 0x00169E58. */
s32 uyafn_13D310(s32 level, u8 *buf, s32 x)
{
    u8 *toc = (u8 *)0x00160C40;
    u8 *e = toc + level * 0x18;
    (void)x;
    if (uya_disc_read(*(u32 *)(e + 0x7408), 1, buf) == 0) memcpy(toc + 0x79A0, buf, 0x60);
    if (uya_disc_read(*(u32 *)(e + 0x7400), 4, buf) == 0) memcpy(toc + 0x7A00, buf, 0x1818);
    if (uya_disc_read(*(u32 *)(e + 0x7410), 5, buf) == 0) memcpy((u8 *)0x00169E58, buf, 0x26F0);
    uya_log("level %d: table of contents loaded", level);
    return 0;
}
