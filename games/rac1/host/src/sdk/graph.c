/* libgraph: vsync and the EE timer the menu measures frames with. */
#include "uya_host.h"
#include "uya_runtime.h"
#include <stdlib.h>

#define T0_COUNT (*(volatile u32 *)0x10000800)   /* EE timer 0, BUSCLK/256 = 576000 Hz */
/* the length of a field: NTSC 59.94 Hz until the game sets the video mode
 * (sceGsResetGraph, host/src/sdk/gsdk.c); RAC1 (PAL) runs at 50 Hz */
static uint32_t frame_us = 16683;
void uya_set_video_pal(int pal) { frame_us = pal ? 20000 : 16683; }
#define FRAME_US frame_us
uint32_t uya_frame_us(void) { return frame_us; }

static uint64_t last, next_vsync;
static u32 field;

/* 0x0012C908: sceGsSyncV(mode): wait for the next vertical blank, return the field (0/1).
 * Paces the game at the video mode's field rate (50 Hz PAL, 59.94 Hz NTSC) and
 * advances timer 0 by the time that really passed. */
void uya_gs_vsync(void);
void uya_sound_pump(void);
void uya_mc_trace(void);
void uya_call_vsync_callback(void);
void uya_dma_poll(void);
void rac1_cd_service(void);
void rac1_test_hooks(void);
static uint32_t vsyncs;
uint32_t uya_frame_count(void) { return vsyncs; }
/* testing: UYA_FIXEDTIME=1 puts the game on a clock that moves a field per
 * vsync (timer 0 here, the Linux sound output's pacing) and never waits, so a
 * run does the same whatever the speed of the machine (frames can be compared
 * between builds) */
static int fixed_time = -1;
static uint64_t vclock;
uint64_t uya_fixed_clock(void) { return fixed_time > 0 ? vclock : 0; }

s32 uyafn_12C908(s32 mode)
{
    vsyncs++;
    if (fixed_time < 0) fixed_time = getenv("UYA_FIXEDTIME") != 0;
    uint64_t now = fixed_time ? (vclock += FRAME_US) : plat_time_us();
    (void)mode;
    if (!next_vsync) { next_vsync = now + FRAME_US; last = now; }
    if (now < next_vsync && !fixed_time) { plat_sleep_us(next_vsync - now); now = plat_time_us(); }
    next_vsync += FRAME_US;
    if (now > next_vsync + 4 * FRAME_US) next_vsync = now + FRAME_US;   /* fell far behind: resync */
    T0_COUNT += (u32)((now - last) * 576000u / 1000000u);
    last = now;
    field ^= 1;
    uya_gs_vsync();
    uya_sound_pump();
    uya_mc_trace();
    uya_dma_poll();
    rac1_cd_service();
    uya_call_vsync_callback();
    rac1_test_hooks();
    plat_poll();
    return (s32)field;
}
