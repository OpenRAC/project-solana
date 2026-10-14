/* rac1_levels.h: the level programs compiled into the wrapper (docs/LEVELS.md).
 *
 * Each level carries its own build of the game program; ParseBin copies it
 * over the executable's main segment. build.py compiles the levels it is
 * asked for (--levels) and lists them in gen/level_table.c; ParseBin
 * (host/src/rac1/levels.c) finds the one being loaded there and switches the
 * trampolines and the recompiled code's dispatch table to it. */
#ifndef RAC1_LEVELS_H
#define RAC1_LEVELS_H
#include "uya_runtime.h"

typedef struct RcCtx RcCtx;
typedef struct { uint32_t addr; void (*fn)(RcCtx *); } rac1_rc_row_t;

typedef struct {
    int id;                              /* level index (docs/OVERLAYS.md, Levels) */
    uint32_t entry;                      /* the entry point ParseBin returns */
    uint32_t text_addr, text_bytes;      /* its text record: how a loaded program is recognised */
    const uya_tramp_t *tramps;           /* uya_tramp_table_lNN */
    const int *tramp_count;
    const rac1_rc_row_t *rc_table;       /* rcNN_table: its recompiled functions and thunks by address */
    const int *rc_count;
} rac1_level_t;

extern const rac1_level_t rac1_levels[];
extern const int rac1_level_count;

/* host/src/rac1/rc_runtime.c: the table rc_dispatch looks addresses up in */
void rc_set_table(const rac1_rc_row_t *table, int count);
/* host/src/runtime.c */
void uya_install_tramp_table(const uya_tramp_t *table, int count);

#endif
