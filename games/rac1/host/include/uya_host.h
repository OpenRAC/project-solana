/* uya_host.h: replaces the decomp's common.h when frontbin's C is built for a PC.
 *
 * Memory model: the host is a 32-bit x86 program, so pointers are 4 bytes like
 * on the EE and every struct keeps its PS2 layout. PS2 main RAM is mapped at its
 * real addresses (0x00100000-0x01FFFFFF), so the decomp's globals (declared
 * extern, placed by address through abs_syms.s) and its absolute constants
 * point at real memory. long is rewritten to long long by gen_host.py (the
 * EE's long is 64-bit); build with -malign-double so 64-bit fields keep 8-byte
 * alignment.
 */
#ifndef UYA_HOST_H
#define UYA_HOST_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;
typedef float f32;
typedef double f64;

/* The decomp's assembly-inclusion macros have no meaning on the host;
 * gen_host.py removes their uses, these are a fallback. */
#define INCLUDE_ASM(FOLDER, NAME)
#define INCLUDE_RODATA(FOLDER, NAME)
#define ASM_FUNC(FOLDER, NAME)
#define LINKER_REMNANT(FOLDER, NAME)
#define TEXT_PADDING(N)

/* The EE's MMI plzcw on one word: leading bits equal to the sign bit, minus one. */
static inline s32 uya_plzcw(u32 x)
{
    u32 s = x >> 31;
    s32 n = 0;
    for (int b = 31; b >= 0 && ((x >> b) & 1) == s; b--) n++;
    return n - 1;
}

/* A function the host does not implement yet: logs once per name. */
void uya_unimpl(const char *name);
void uya_log(const char *fmt, ...);

#endif
