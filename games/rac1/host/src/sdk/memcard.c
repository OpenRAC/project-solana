/* libmc (Sony's memory card library) on the host, backed by folders.
 *
 * The menu talks to the card the way every PS2 game does: it starts a command
 * (sceMcGetInfo, sceMcOpen, sceMcRead, ...) which returns 0 when accepted, then
 * polls sceMcSync each frame until it reports the command finished, and reads
 * the command's result from there. Here each command runs at once on a folder
 * and its result waits for the next sceMcSync.
 *
 * Memory card slot 1 (port 0) is the folder "memcard/slot1" next to the
 * program; slot 2 (port 1) is "memcard/slot2" and only counts as inserted when
 * that folder exists. Inside a slot folder each save is a folder named like
 * the card's directory (BASCUS-97353RATCHET), with the save's files in it.
 *
 * Result codes are libmc's: 0 success, -1 card changed, -2 unformatted,
 * -3 card full, -4 no such file or directory (or it already exists, for
 * mkdir), -5 permission denied, -6 directory not empty, -7 too many open files,
 * -10 no card. Addresses are named by the boot_elf functions they replace
 * (src/boot_elf/core/12A948.c). */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#define rmdir_one(p) _rmdir(p)
#else
#include <unistd.h>
#define mkdir_one(p) mkdir(p, 0777)
#define rmdir_one(p) rmdir(p)
#endif

#define CARD_KB 8192                 /* an 8 MB card, counted in 1 KB clusters */

static s32 pend_cmd, pend_res;       /* the finished command waiting for sceMcSync */
static int seen[2];                  /* GetInfo reported this card already (else "new card") */

/* UYA_MCLOG=1 logs card commands (a command repeating its last result, as the
 * menu's sceMcGetInfo polling does, is logged once) and the card manager trace
 * below. In PowerShell: $env:UYA_MCLOG=1; in cmd: set UYA_MCLOG=1. */
static int mclog(void) { static int v = -1; if (v < 0) v = getenv("UYA_MCLOG") != 0; return v; }

static void finish(s32 cmd, s32 res)
{
    static s32 last_cmd = -1, last_res = 0x7FFFFFFF;
    pend_cmd = cmd; pend_res = res;
    if (mclog() && (cmd != last_cmd || res != last_res))
        uya_log("memory card: command 0x%X result %d", cmd, res);
    last_cmd = cmd; last_res = res;
}

static const char *slot_dir(int port)
{
    static char d[2][1024];
    snprintf(d[port & 1], sizeof d[0], "%s/memcard/slot%d", plat_exe_dir(), (port & 1) + 1);
    return d[port & 1];
}

static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

/* File size by seeking: st_size from stat() reads as 0 in this 32-bit build. */
static long file_size(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fclose(f); }
    return n;
}

static int card_present(int port)
{
    if (port & 1) return is_dir(slot_dir(1));
    /* slot 1 always has a card: make its folder on first use */
    char base[1024];
    snprintf(base, sizeof base, "%s/memcard", plat_exe_dir());
    mkdir_one(base);
    mkdir_one(slot_dir(0));
    return is_dir(slot_dir(0));
}

/* '*' any run of characters, '?' one character */
static int glob_match(const char *p, const char *s)
{
    if (!*p) return !*s;
    if (*p == '*') {
        for (; ; s++) { if (glob_match(p + 1, s)) return 1; if (!*s) return 0; }
    }
    if (!*s) return 0;
    if (*p == '?' || *p == *s) return glob_match(p + 1, s + 1);
    return 0;
}

/* Card path ("/DIR/file" or "DIR/file") to a host path. Wildcards in the
 * directory part pick the first directory that matches. Returns 0 if a
 * wildcard directory had no match. */
static int host_path(int port, const char *name, char *out, size_t n, char *last, size_t nlast)
{
    char tmp[512], cur[1024];
    snprintf(cur, sizeof cur, "%s", slot_dir(port));
    snprintf(tmp, sizeof tmp, "%s", name ? name : "");
    char *parts[16]; int np = 0;
    for (char *t = strtok(tmp, "/"); t && np < 16; t = strtok(0, "/")) parts[np++] = t;
    for (int i = 0; i < np - 1; i++) {
        if (strpbrk(parts[i], "*?")) {
            DIR *d = opendir(cur);
            struct dirent *e; int found = 0;
            while (d && (e = readdir(d)))
                if (e->d_name[0] != '.' && glob_match(parts[i], e->d_name)) {
                    size_t l = strlen(cur);
                    snprintf(cur + l, sizeof cur - l, "/%s", e->d_name); found = 1; break;
                }
            if (d) closedir(d);
            if (!found) return 0;
        } else {
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", parts[i]);
        }
    }
    if (last) snprintf(last, nlast, "%s", np ? parts[np - 1] : "");
    if (np) { size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", parts[np - 1]); }
    snprintf(out, n, "%s", cur);
    return 1;
}

static long used_kb(const char *dir)
{
    long kb = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1100]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st)) continue;
        kb += S_ISDIR(st.st_mode) ? 1 + used_kb(p) : (file_size(p) + 1023) / 1024;
    }
    if (d) closedir(d);
    return kb;
}

/* 0x0012DA60 / 0x0012EA38: sceMcInit and friends. Nothing to set up. */
s32 uyafn_12DA60(void) { return 0; }

/* 0x0012E2E0: sceMcSync(mode, cmd*, result*): 1 = a command finished (its id
 * and result are stored), -1 = no command pending. Commands finish at once, so
 * it never reports "still running". */
s32 uyafn_12E2E0(s32 mode, s32 *cmd, s32 *result)
{
    (void)mode;
    if (!pend_cmd) return -1;
    if (cmd) *cmd = pend_cmd;
    if (result) *result = pend_res;
    pend_cmd = 0;
    return 1;
}

/* 0x0012E400: sceMcGetInfo(port, slot, type*, free*, format*) */
s32 uyafn_12E400(s32 port, s32 slot, s32 *type, s32 *free_kb, s32 *format)
{
    (void)slot;
    static int told[2];
    if (!told[port & 1]) { told[port & 1] = 1; uya_log("memory card: slot %d is %s (%s)", (port & 1) + 1, slot_dir(port), card_present(port) ? "inserted" : "empty"); }
    if (!card_present(port)) {
        seen[port & 1] = 0;
        if (type) *type = 0;
        finish(1, -10);
        return 0;
    }
    if (type) *type = 2;                         /* PS2 memory card */
    if (format) *format = 1;
    if (free_kb) { long f = CARD_KB - 1 - used_kb(slot_dir(port)); *free_kb = f < 0 ? 0 : (s32)f; }
    finish(1, seen[port & 1] ? 0 : -1);          /* first look: "a formatted card was inserted" */
    seen[port & 1] = 1;
    return 0;
}

/* sceMcGetDir table entry (sceMcTblGetDir), 0x40 bytes */
typedef struct {
    u8 create[8], modify[8];
    u32 size;
    u16 attr, r1;
    u32 r2, pda;
    char name[32];
} McDirEnt;

static void fill_ent(McDirEnt *e, const char *name, int dir, u32 size)
{
    memset(e, 0, sizeof *e);
    e->create[2] = e->modify[2] = 0;   /* sec, min, hour left zero */
    e->create[4] = e->modify[4] = 1;   /* day 1 */
    e->create[5] = e->modify[5] = 1;   /* month 1 */
    e->create[6] = e->modify[6] = 2004 & 0xFF; e->create[7] = e->modify[7] = 2004 >> 8;
    e->size = size;
    e->attr = dir ? 0x8427 : 0x8497;
    snprintf(e->name, sizeof e->name, "%s", name);
}

static struct { char dir[1024], pat[64]; int pos; } gd;

static s32 list_dir(McDirEnt *out, s32 maxent)
{
    DIR *d = opendir(gd.dir);
    if (!d) return -4;
    struct dirent *e;
    int idx = 0, n = 0, top = !strcmp(gd.dir, slot_dir(0)) || !strcmp(gd.dir, slot_dir(1));
    /* a subdirectory lists "." and ".." first, like the card does */
    if (!top) {
        const char *dots[2] = {".", ".."};
        for (int k = 0; k < 2; k++)
            if (glob_match(gd.pat, dots[k])) {
                if (idx++ >= gd.pos && n < maxent) fill_ent(&out[n++], dots[k], 1, 0);
            }
    }
    while ((e = readdir(d)) && n < maxent) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!glob_match(gd.pat, e->d_name)) continue;
        char p[1200]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", gd.dir, e->d_name);
        if (stat(p, &st)) continue;
        if (idx++ < gd.pos) continue;
        fill_ent(&out[n++], e->d_name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : (u32)file_size(p));
    }
    closedir(d);
    gd.pos += n;
    return n;
}

/* 0x0012E580: sceMcGetDir(port, slot, name, mode, maxent, table): mode 0
 * starts a listing of name (wildcards allowed in its last part), mode 1
 * continues it. The result is the number of entries written. */
s32 uyafn_12E580(s32 port, s32 slot, char *name, s32 mode, s32 maxent, void *table)
{
    (void)slot;
    if (!card_present(port)) { finish(0xD, -10); return 0; }
    if (mclog()) uya_log("memory card: list %s mode %d max %d", name, mode, maxent);
    if (mode == 0) {
        char last[64];
        if (!host_path(port, name, gd.dir, sizeof gd.dir, last, sizeof last)) { finish(0xD, 0); gd.dir[0] = 0; return 0; }
        char *sl = strrchr(gd.dir, '/');
        if (sl) *sl = 0;                    /* gd.dir = the folder, gd.pat = the pattern in it */
        snprintf(gd.pat, sizeof gd.pat, "%s", last[0] ? last : "*");
        gd.pos = 0;
    }
    if (!gd.dir[0]) { finish(0xD, 0); return 0; }
    s32 r = list_dir((McDirEnt *)table, maxent);
    finish(0xD, r == -4 ? 0 : r);
    return 0;
}

static FILE *fds[8];

/* 0x0012DCA0: sceMcOpen(port, slot, name, mode): mode 1 read, 2 write, 3 both,
 * 0x200 create, 0x40 make a directory (what sceMcMkdir sends). Result: the
 * file descriptor, or an error. */
s32 uyafn_12DCA0(s32 port, s32 slot, char *name, s32 mode)
{
    (void)slot;
    char p[1024];
    s32 cmd = (mode & 0x40) ? 0xB : 2;
    if (!card_present(port)) { finish(cmd, -10); return 0; }
    if (mclog()) uya_log("memory card: open %s mode 0x%X", name, mode);
    if (!host_path(port, name, p, sizeof p, 0, 0)) { finish(cmd, -4); return 0; }
    if (mode & 0x40) {
        if (is_dir(p)) { finish(cmd, -4); return 0; }
        finish(cmd, mkdir_one(p) == 0 ? 0 : -4);
        return 0;
    }
    int fd;
    for (fd = 0; fd < 8 && fds[fd]; fd++);
    if (fd == 8) { finish(cmd, -7); return 0; }
    struct stat st;
    int exists = stat(p, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) { finish(cmd, -5); return 0; }
    if (!exists && !(mode & 0x200)) { finish(cmd, -4); return 0; }
    FILE *f = fopen(p, exists ? ((mode & 2) ? "r+b" : "rb") : "w+b");
    if (!f) { finish(cmd, -4); return 0; }
    fds[fd] = f;
    finish(cmd, fd);
    return 0;
}

/* 0x0012DDC8: sceMcMkdir(port, slot, name) */
s32 uyafn_12DDC8(s32 port, s32 slot, char *name) { return uyafn_12DCA0(port, slot, name, 0x40); }

/* 0x0012DE00: sceMcClose(fd) */
s32 uyafn_12DE00(s32 fd)
{
    if (fd < 0 || fd >= 8 || !fds[fd]) { finish(3, -4); return 0; }
    fclose(fds[fd]); fds[fd] = 0;
    finish(3, 0);
    return 0;
}

/* 0x0012DEC0: sceMcSeek(fd, offset, whence 0/1/2): result is the new position */
s32 uyafn_12DEC0(s32 fd, s32 off, s32 whence)
{
    if (fd < 0 || fd >= 8 || !fds[fd]) { finish(4, -4); return 0; }
    fseek(fds[fd], off, whence == 1 ? SEEK_CUR : whence == 2 ? SEEK_END : SEEK_SET);
    finish(4, (s32)ftell(fds[fd]));
    return 0;
}

/* 0x0012E050: sceMcRead(fd, buf, size): result is the byte count */
s32 uyafn_12E050(s32 fd, void *buf, s32 size)
{
    if (fd < 0 || fd >= 8 || !fds[fd]) { finish(5, -4); return 0; }
    finish(5, (s32)fread(buf, 1, (size_t)size, fds[fd]));
    return 0;
}

/* 0x0012E168: sceMcWrite(fd, buf, size): result is the byte count */
s32 uyafn_12E168(s32 fd, void *buf, s32 size)
{
    if (fd < 0 || fd >= 8 || !fds[fd]) { finish(6, -4); return 0; }
    s32 n = (s32)fwrite(buf, 1, (size_t)size, fds[fd]);
    fflush(fds[fd]);
    finish(6, n);
    return 0;
}

/* 0x0012E7A8: sceMcDelete(port, slot, name): a file, or an empty folder */
s32 uyafn_12E7A8(s32 port, s32 slot, char *name)
{
    (void)slot;
    char p[1024];
    if (!card_present(port)) { finish(0xF, -10); return 0; }
    if (!host_path(port, name, p, sizeof p, 0, 0)) { finish(0xF, -4); return 0; }
    if (is_dir(p)) {
        DIR *d = opendir(p); struct dirent *e; int empty = 1;
        while (d && (e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) empty = 0;
        if (d) closedir(d);
        if (!empty) { finish(0xF, -6); return 0; }
        finish(0xF, rmdir_one(p) == 0 ? 0 : -5);
        return 0;
    }
    finish(0xF, remove(p) == 0 ? 0 : -4);
    return 0;
}

/* 0x0012E6D8: sceMcFormat(port, slot). The folder card is always formatted. */
s32 uyafn_12E6D8(s32 port, s32 slot) { (void)slot; finish(0x10, card_present(port) ? 0 : -10); return 0; }

/* 0x0012E8D8: sceMcUnformat(port, slot). Not done to a folder: reports success. */
s32 uyafn_12E8D8(s32 port, s32 slot) { (void)slot; finish(0x11, card_present(port) ? 0 : -10); return 0; }

/* Trace of the menu's card manager (uyafn_397490, state at 0x00142430) and
 * of the card dialogs (UI state 0x001D4CE8, flags 0x001D4CEC). Called every
 * vsync; logs a line whenever one of the watched values changes. The manager's
 * idle polling (states 0 to 2) is left out. */
void uya_mc_trace(void)
{
    if (!mclog()) return;
    const u8 *m = (const u8 *)0x00142430;
    s32 v[9];
    s32 st = *(const s32 *)(m + 0x15C);
    v[0] = st > 2 ? st : 0;                    /* job running (3 and up) */
    v[1] = *(const s32 *)(m + 0x164);          /* job requested, -1 none */
    v[2] = *(const s32 *)(m + 0x16C);          /* last error/event code */
    v[3] = *(const s32 *)(m + 0x10);           /* slot 1 "card changed" flag */
    v[4] = *(const s16 *)(m + 0x18);           /* slot 1 save state */
    v[5] = *(const s32 *)(m + 0x160);          /* job step */
    v[6] = *(const s32 *)0x001D4CE8;           /* card dialog state */
    v[7] = *(const s32 *)0x001D4CEC;           /* card dialog flags */
    v[8] = *(const s32 *)(m + 0xA0 + 0x10);    /* slot 2 "card changed" flag */
    static s32 prev[9]; static int init;
    if (init && !memcmp(prev, v, sizeof v)) return;
    init = 1; memcpy(prev, v, sizeof v);
    uya_log("card mgr: job %d step %d req %d err 0x%X | slot1 changed %d save %d | slot2 changed %d | dialog %d flags 0x%X",
            v[0], v[5], v[1], v[2], v[3], v[4], v[8], v[6], v[7]);
}
