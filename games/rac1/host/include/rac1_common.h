/* rac1_common.h: replaces OpenRAC's include/common.h when the game's C is
 * built for a PC (gen_host.py copies it to gen/include/common.h).
 *
 * The host is a 32-bit x86 program with PS2 RAM mapped at its real addresses,
 * so pointers are 4 bytes and structs keep their PS2 layout. `long` is
 * rewritten to `long long` by gen_host.py (the EE's long is 64-bit); build
 * with -malign-double so 64-bit fields keep 8-byte alignment. */
#ifndef COMMON_H
#define COMMON_H

#include "uya_host.h"
#include "names.h"

/* Matching-only placement attributes: no meaning on the host. */
#define NOT_SDA
#define MACRO_ADDR
/* SDATA(D_x) names a scalar by another C name: an asm label to the address symbol
 * (expanded first: a level program's names carry its suffix, gen/lNN/lNN_names.h) */
#define UYA_SYMSTR_(x) #x
#define UYA_SYMSTR(x) UYA_SYMSTR_(x)
#ifdef _WIN32
#define SDATA(sym) __asm__("_" UYA_SYMSTR(sym))
#else
#define SDATA(sym) __asm__(UYA_SYMSTR(sym))
#endif

/* The decomp's quadword copy and clear (lq/sq inline asm on the EE). */
static __inline__ void qcopy(void *dst, void *src) { __builtin_memcpy(dst, src, 16); }
static __inline__ void qcopy_nc(void *dst, void *src) { __builtin_memcpy(dst, src, 16); }
static __inline__ void qzero(void *p) { __builtin_memset(p, 0, 16); }

/* EE FPU division for C patched with it (tools/source_patches.py): a division
 * by zero gives +-MAX (the sign of a XOR b), never inf or NaN. */
static __inline__ float ee_fdiv(float a, float b)
{
    if (b == 0.0f) {
        unsigned ua, ub; __builtin_memcpy(&ua, &a, 4); __builtin_memcpy(&ub, &b, 4);
        unsigned r = ((ua ^ ub) & 0x80000000u) | 0x7F7FFFFFu; float f; __builtin_memcpy(&f, &r, 4); return f;
    }
    return a / b;
}

#endif /* COMMON_H */
