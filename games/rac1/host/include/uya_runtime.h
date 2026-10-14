/* uya_runtime.h: the host runtime's own interface (not seen by frontbin code). */
#ifndef UYA_RUNTIME_H
#define UYA_RUNTIME_H
#include <stdint.h>
#include <stddef.h>

/* PS2 address ranges the host maps at their real addresses. */
#define PS2_RAM_LO      0x00100000u  /* the game never touches the EE kernel area below 1 MB */
#define PS2_RAM_HI      0x02000000u  /* 32 MB of EE main RAM */
#define PS2_SPR         0x70000000u  /* scratchpad */
#define PS2_SPR_SIZE    0x4000u
#define PS2_EE_REGS     0x10000000u  /* timers, IPU, GIF, VIF0/1, DMA, INTC... */
#define PS2_EE_REGS_SZ  0x00010000u
#define PS2_VU_MEM      0x11000000u  /* VU0/VU1 code and data memory */
#define PS2_VU_MEM_SZ   0x00010000u
#define PS2_GS_PRIV     0x12000000u  /* GS privileged registers */
#define PS2_GS_PRIV_SZ  0x00002000u

typedef struct { uint32_t ps2_addr; void *host_fn; } uya_tramp_t;
extern const uya_tramp_t uya_tramp_table[];
extern const int uya_tramp_count;

/* platform layer */
int  plat_map_fixed(uint32_t addr, uint32_t size, int exec);  /* 0 on success */
int  plat_install_trap_handler(void);                         /* calls into unmapped PS2 code */
void plat_log_system(void);                                   /* the system, for bug reports (once, at start) */
void plat_flush_icache(void *p, size_t n);
const char *plat_exe_dir(void);                                /* folder of the running program */
void plat_at_exit(void);
uint64_t plat_time_us(void);                                  /* monotonic clock */
void plat_sleep_us(uint64_t us);
/* video output: show a w x h 32-bit image (bytes B, G, R, X); returns 0 once the window was closed */
int  plat_present(const uint32_t *rgbx, int w, int h);
void plat_movie_started(void);                                 /* testing hook (Linux UYA_SKIPMOVIES) */
int  plat_key_skip(void);                                      /* a skip key (Enter/Space/Esc) was pressed since the last call */
void plat_poll(void);                                          /* keep the window responsive; exits when it was closed */
/* pad buttons held on the keyboard, PS2 bit order, 1 = pressed (see host/src/sdk/pad.c) */
unsigned plat_pad_buttons(void);
/* sound: plays `frames` interleaved stereo 16-bit samples once; plat_audio_stop ends it early.
 * `tag` names the dump file on Linux (UYA_DUMP). */
void plat_audio_play(const int16_t *lr, size_t frames, int rate, const char *tag);
void plat_audio_stop(void);
/* continuous 48 kHz stereo output for the sound effects mixer (host/src/sdk/sound.c) */
void plat_stream_write(const int16_t *lr, int frames);
int  plat_stream_queued(void);                                 /* frames written but not played yet */
int  uya_disc_read(uint32_t lsn, uint32_t count, void *buf);   /* 0 on success */
uint32_t uya_frame_count(void);                                /* vsyncs so far (host/src/sdk/graph.c) */

/* runtime */
int  uya_load_elf_sections(const char *path, const char *const *sections);
void uya_install_trampolines(void);
void uya_fill_code_traps(uint32_t lo, uint32_t hi);
void uya_log(const char *fmt, ...);
void uya_log_open(void);                                       /* also log to rac1_log.txt next to the program */
void uya_trap_call(uint32_t ps2_addr, uint32_t ret_addr, const uint32_t *args);     /* from the trap handler */
void uya_report(void);
int  uya_main(int argc, char **argv);
int  uya_disc_open(const char *path);
void uya_disc_boot(void);
void uya_boot(void);                                           /* boot_elf's set-up before the menu (host/src/boot.c) */

#endif
