/* Runtime for the recompiled assembly-only functions (tools/recomp.py,
 * host/include/rc.h): VU0 state, the stack the translated code runs on,
 * calls by address and the few instructions that reach the host. */
#include "rc.h"
#include <stdlib.h>
#include <stdio.h>
void uya_log(const char *fmt, ...);
void uya_unimpl(const char *name);
uint32_t uya_frame_count(void);

RcVu0 rc_vu0 = { .vf = { [0] = {0, 0, 0, 1} } };

/* the translated code's stack: 1 MB of host memory (DMA from it works, see sdk/dma.c) */
static u8 rc_stack[1 << 20] __attribute__((aligned(64)));
u32 rc_sp = 0;

/* the x86 stack (see RC_QA in rc.h): from a little above the first call into
 * translated code down 8 MB, never overlapping PS2 memory (if the stack lies
 * below 32 MB, as it can on Windows, the exception is off) */
u32 rc_cstack_lo, rc_cstack_hi;

#include <stdlib.h>
/* testing: UYA_NANWATCH=addr (hex) stops the run (SIGILL, for a debugger) at the
 * first entry into or dispatch from translated code that finds the float at addr
 * NaN or infinite, and logs where the check before and this one were made */
static void nan_check(const char *where, u32 a)
{
    static u32 w = 1; static void *prev; static u32 preva;
    if (w == 1) { const char *e = getenv("UYA_NANWATCH"); w = e ? (u32)strtoul(e, 0, 16) : 0; }
    if (!w) return;
    u32 v = *(u32 *)(uintptr_t)w;
    if ((v & 0x7F800000u) == 0x7F800000u) { uya_log("NANWATCH: %08X became %08X; previous check %p/%08X, now %s %p/%08X", w, v, prev, preva, where, __builtin_return_address(1), a); __builtin_trap(); }
    prev = __builtin_return_address(1); preva = a;
}
void rc_enter(RcCtx *c)
{
    nan_check("enter", 0);
    if (!rc_cstack_hi) {
        u32 here = (u32)(uintptr_t)__builtin_frame_address(0);
        /* the x86 stack: above PS2 RAM (0-0x01FFFFFF), which Windows' main
         * thread stack often is just above (0x03xxxxxx), Linux's far above */
        if (here > 0x02100000u) {
            rc_cstack_hi = here < 0xFFF00000u ? here + (1u << 20) : 0xFFFFFFFFu;
            rc_cstack_lo = here - (8u << 20);
            if (rc_cstack_lo < 0x02000000u) rc_cstack_lo = 0x02000000u;
        } else rc_cstack_hi = 1;    /* checked: empty range */
        uya_log("x86 stack for translated code: %08X-%08X", rc_cstack_lo, rc_cstack_hi);
    }
    memset(c, 0, sizeof *c);
    if (!rc_sp) rc_sp = (u32)(uintptr_t)(rc_stack + sizeof rc_stack - 64);
    c->r[29].d[0] = (rc_sp - 64) & ~15u;
    c->r[28].d[0] = RC_GP;
}
void rc_leave(RcCtx *c) { (void)c; }

typedef void (*rc_fn)(RcCtx *);
typedef struct { u32 addr; rc_fn fn; } rc_row;
extern const rc_row rc_table[];
extern const int rc_table_count;
void uya_trap_call(uint32_t ps2_addr, uint32_t ret_addr, const uint32_t *args);

/* The table in use: the executable's (rc_table) until ParseBin loads a level
 * (rcNN_table, host/src/rac1/levels.c). A level's table has its own
 * functions and thunks for the core functions it calls. */
static const rc_row *cur_table = rc_table;
static int cur_count = -1;
void rc_set_table(const rc_row *table, int count) { cur_table = table; cur_count = count; }

void rc_dispatch(RcCtx *c, u32 addr)
{
    nan_check("dispatch", addr);
    if (cur_count < 0) cur_count = rc_table_count;
    int lo = 0, hi = cur_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (cur_table[m].addr == addr) { cur_table[m].fn(c); return; }
        if (cur_table[m].addr < addr) lo = m + 1; else hi = m - 1;
    }
    u32 args[4] = { (u32)c->r[4].d[0], (u32)c->r[5].d[0], (u32)c->r[6].d[0], (u32)c->r[7].d[0] };
    uya_trap_call(addr, (u32)c->r[31].d[0], args);
    c->r[2].d[0] = 0;
}

/* rc_vcallms: host/src/gfx/vu.c (translated VU0 microprograms) */

void rc_syscall(RcCtx *c) { uya_log("recompiled code: syscall %d", (int)(s32)c->r[3].d[0]); }
void rc_break(u32 pc) { uya_log("recompiled code: break/trap at 0x%08X", pc); }
void rc_bad(u32 pc, const char *what)
{
    static int n;
    if (n++ < 64) uya_log("recompiled code: instruction %s at 0x%08X is not supported", what, pc);
}
u32 rc_cop0(int r)
{
    uint32_t uya_frame_us(void);
    if (r == 9) return (u32)((unsigned long long)uya_frame_count() * uya_frame_us() * 294912u / 1000u);   /* Count: 294.912 MHz, one field per vsync (20 ms PAL) */
    return 0;
}

/* testing: UYA_QUADCHECK=1 logs drawquad (func_001F7EF8) calls whose quad's
 * GS words (+112: TEX0 at +120, TEX1 +128, ALPHA +136) are not what its
 * callers put there, with the caller's return address */
void uya_dbg_quad(RcCtx *c)
{
    static int on = -1, left = 30;
    if (on < 0) on = getenv("UYA_QUADCHECK") != 0;
    if (!on || left <= 0) return;
    u32 a = (u32)c->r[4].d[0];
    const u32 *q = (const u32 *)RC_QA(a + 112);
    if (q[4] == 0x00000260 && q[5] == 0xFF90 && q[6] == 0x44 && q[7] == 0x80) return;
    left--;
    uya_log("quadcheck: drawquad(%08X) from ra %08X: GS words %08X%08X %08X%08X %08X%08X %08X%08X", a, (u32)c->r[31].d[0],
            q[1], q[0], q[3], q[2], q[5], q[4], q[7], q[6]);
}

/* testing: functions named in UYA_RC_TRACE at build time call this on entry */
void rc_trace(RcCtx *c, const char *name)
{
    /* UYA_RC_TRACE_MEM=addr,len[;addr,len...] (hex): memory shown with each call */
    static char mem[256] = "?";
    if (mem[0] == '?') { const char *e = getenv("UYA_RC_TRACE_MEM"); snprintf(mem, sizeof mem, "%s", e ? e : ""); }
    char t[512]; int n = 0;
    for (const char *p = mem; *p && n < 400;) {
        unsigned a = 0, l = 0;
        if (sscanf(p, "%x,%x", &a, &l) != 2) break;
        n += snprintf(t + n, sizeof t - n, " [%X]", a);
        for (unsigned i = 0; i < l && n < 480; i++) n += snprintf(t + n, sizeof t - n, "%02X", *(const u8 *)(uintptr_t)(a + i));
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    t[n] = 0;
    uya_log("v%u %s(%08X, %08X, %08X, %08X) ra %08X%s", uya_frame_count(), name, (u32)c->r[4].d[0], (u32)c->r[5].d[0],
            (u32)c->r[6].d[0], (u32)c->r[7].d[0], (u32)c->r[31].d[0], t);
}

/* testing: a heuristic backtrace of translated code: the words on its stack
 * (from rc_sp, which the thunks into host code set) that look like code
 * addresses (return addresses the translated functions saved) */
const char *rc_stack_scan(void)
{
    static char t[512];
    int n = 0, k = 0;
    const u32 *sp = (const u32 *)(uintptr_t)rc_sp;
    t[0] = 0;
    for (int i = 0; sp && i < 1024 && k < 12; i += 2) {
        u32 v = sp[i];
        if (v >= 0x00112380u && v < 0x00300000u && !(v & 3) && (v & 0xFFFF) && v != 0x00160000u && v != 0x00170000u) {
            n += snprintf(t + n, sizeof t - n, " %08X@%d", v, i * 4); k++;
        }
    }
    return t;
}
