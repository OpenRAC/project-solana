/* Engine core (boot_elf) C library functions the menu calls, implemented on the host.
 * Each is named by its PS2 address so the menu's calls and function pointers reach it. */
#include "uya_host.h"

/* 0x0011A264: memset(dst, c, n) (newlib) */
void *uyafn_11A264(void *dst, s32 c, u32 n)
{
    u8 *p = dst;
    while (n--) *p++ = (u8)c;
    return dst;
}

#include <stdarg.h>
int vprintf(const char *, va_list);

/* 0x00121760: printf(fmt, ...) */
int uyafn_121760(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* 0x0011F0A0: FlushCache(mode). Nothing to do on x86. */
s32 uyafn_11F0A0(s32 mode) { (void)mode; return 0; }

void uya_log(const char *fmt, ...);
/* 0x00116F98: __assert(file, line, expression) (newlib). Logged; the game carries on. */
void uyafn_116F98(const char *file, s32 line, const char *expr)
{
    uya_log("ASSERT %s:%d: %s", file ? file : "?", line, expr ? expr : "?");
}

int vsprintf(char *, const char *, va_list);
/* 0x0011B2E8: sprintf(buf, fmt, ...) */
int uyafn_11B2E8(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

/* 0x00116FD0: atexit(fn). The menu registers cleanup it never needs here. */
s32 uyafn_116FD0(void (*fn)(void)) { (void)fn; return 0; }

/* 0x0011BB58: strncpy (newlib, an MMI-optimised version on the EE). */
char *uyafn_11BB58(char *dst, const char *src, u32 n)
{
    u32 i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

/* 0x0013D3E0: the game's rand(): seed (0x001D4D80) = seed * 0x343FD + 0x269EC3,
 * returns bits 16-30. */
s32 uyafn_13D3E0(void)
{
    u32 *seed = (u32 *)0x001D4D80;
    *seed = *seed * 0x343FDu + 0x269EC3u;
    return (s32)((*seed >> 16) & 0x7FFF);
}

/* 0x00124ED8: LoadExecPS2(path, argc, argv): the menu starts another
 * executable (Multiplayer starts cdrom0:\I5BOOTN.ELF, the online game). The
 * wrapper has only the menu, so it says so and stops. */
void uya_log(const char *fmt, ...);
void exit(int);
void uyafn_124ED8(const char *path, s32 argc, const char **argv)
{
    (void)argc; (void)argv;
    uya_log("the menu now starts %s (another program on the disc); the wrapper only has the menu, so it stops here.", path ? path : "?");
    exit(0);
}
/* 0x0012EF10, 0x00121B38: shut down the IOP link before LoadExecPS2; nothing to do. */
void uyafn_12EF10(void) {}
void uyafn_121B38(void) {}
/* 0x0013B0C8: hand the boot options string to the next program; nothing to do. */
void uyafn_13B0C8(void *p) { (void)p; }

/* 0x0011A0B0: memcpy(dst, src, n) */
void *uyafn_11A0B0(void *dst, const void *src, u32 n)
{
    u8 *d = dst; const u8 *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/* 0x0011B868: strlen(s) */
u32 uyafn_11B868(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* 0x0011B754: strcpy(dst, src) */
char *uyafn_11B754(char *dst, const char *src) { char *d = dst; while ((*d++ = *src++)); return dst; }

/* 0x0011B610: strcmp(a, b) */
s32 uyafn_11B610(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}

/* 0x0011B9A0: strncmp(a, b, n) */
s32 uyafn_11B9A0(const char *a, const char *b, u32 n)
{
    for (; n; n--, a++, b++) { if (*a != *b) return (u8)*a - (u8)*b; if (!*a) return 0; }
    return 0;
}

/* 0x0011AF48: printf(fmt, ...) through newlib's stdout (another entry to the same output) */
int uyafn_11AF48(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
