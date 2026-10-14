/* Linux (i386) platform layer: fixed mappings and the int3 trap for calls
 * into PS2 code the host does not implement; the window, OpenGL, gamepads and
 * sound through SDL2 (platform_linux_sdl.h), or headless for testing. */
#ifdef __linux__
#define _GNU_SOURCE
#include "uya_runtime.h"
#include <sys/mman.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int plat_map_fixed(uint32_t addr, uint32_t size, int exec)
{
    int prot = PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0);
    void *p = mmap((void *)(uintptr_t)addr, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED || p != (void *)(uintptr_t)addr) {
        uya_log("cannot map 0x%08X+0x%X (%p)", addr, size, p);
        return -1;
    }
    return 0;
}

void plat_flush_icache(void *p, size_t n) { (void)p; (void)n; /* x86 keeps I/D coherent */ }

const char *plat_exe_dir(void)
{
    static char d[1024];
    if (!d[0]) {
        ssize_t n = readlink("/proc/self/exe", d, sizeof d - 1);
        if (n <= 0) { strcpy(d, "."); return d; }
        d[n] = 0;
        char *s = strrchr(d, '/');
        if (s) *s = 0;
    }
    return d;
}

void plat_at_exit(void) {}

#include <time.h>
uint64_t plat_time_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000;
}

void plat_sleep_us(uint64_t us)
{
    struct timespec t = { (time_t)(us / 1000000), (long)(us % 1000000) * 1000 };
    nanosleep(&t, 0);
}

/* A call landed on an int3 (0xCC) byte in PS2 code space: log it, then return
 * to the caller as if the function returned 0. cdecl: the caller pops the
 * arguments, so popping the return address is all that is needed. */
static void on_trap(int sig, siginfo_t *si, void *ctxv)
{
    ucontext_t *uc = ctxv;
    greg_t *g = uc->uc_mcontext.gregs;
    uint32_t eip = (uint32_t)g[REG_EIP];
    uint32_t esp = (uint32_t)g[REG_ESP];
    uint32_t at = eip - 1;  /* int3 already advanced eip */
    (void)si;
    if (sig != SIGTRAP || at < PS2_RAM_LO || at >= PS2_RAM_HI) {
        uya_log("fatal signal %d at eip=0x%08X", sig, eip);
        uya_report();
        _exit(1);
    }
    uint32_t ret = *(uint32_t *)(uintptr_t)esp;
    uya_trap_call(at, ret, (const uint32_t *)(uintptr_t)(esp + 4));
    g[REG_EAX] = 0;
    g[REG_EDX] = 0;
    g[REG_EIP] = ret;
    g[REG_ESP] = esp + 4;
}

#include <dlfcn.h>
static void *dladdr_base(void)   /* the game executable's own load address */
{
    static void *b; Dl_info di;
    if (!b && dladdr((void *)&dladdr_base, &di)) b = di.dli_fbase;
    return b;
}
/* "name+0xoff" for an address in the game (its symbols are exported: -rdynamic) */
static const char *sym_name(uint32_t a, char *buf, int cap)
{
    Dl_info di;
    if (dladdr((void *)(uintptr_t)a, &di) && di.dli_sname) snprintf(buf, cap, "%s+0x%X", di.dli_sname, a - (uint32_t)(uintptr_t)di.dli_saddr);
    else snprintf(buf, cap, "?");
    return buf;
}
static void on_fault(int sig, siginfo_t *si, void *ctxv)
{
    static volatile int in_crash;
    if (__atomic_exchange_n(&in_crash, 1, __ATOMIC_SEQ_CST)) _exit(1);   /* (a crash while reporting one) */
    ucontext_t *uc = ctxv;
    greg_t *g = uc->uc_mcontext.gregs;
    char nm[256];
    uint32_t eip = (uint32_t)g[REG_EIP];
    uya_log("crash: signal %d, fault address %p, eip=0x%08X", sig, si->si_addr, eip);
    uya_log("crash: in %s", sym_name(eip, nm, sizeof nm));
    {   /* the return addresses on the stack, named: the call chain */
        const uint32_t *sp = (const uint32_t *)(uintptr_t)(uint32_t)g[REG_ESP];
        Dl_info di;
        for (int i = 0, shown = 0; i < 256 && shown < 12; i++) {
            uint32_t v = sp[i];
            if (v >= 0x40000000u && v < 0x80000000u && dladdr((void *)(uintptr_t)v, &di) && di.dli_sname && di.dli_fbase == dladdr_base()) {
                uya_log("crash: called from %s", sym_name(v, nm, sizeof nm)); shown++;
            }
        }
    }
    uya_report();
    _exit(1);
}

/* testing: UYA_PROF=file samples the program counter of whichever thread is
 * running every millisecond of CPU time and writes "eip tid count" lines at
 * exit (symbolize with nm; each thread's own pc, no call stacks) */
#include <sys/time.h>
#include <sys/syscall.h>
#define PROF_N (1 << 16)
static struct { uint32_t eip, tid, n; } prof_tab[PROF_N];
static const char *prof_path;
static void on_prof(int sig, siginfo_t *si, void *ctxv)
{
    ucontext_t *uc = ctxv;
    uint32_t eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP], tid = (uint32_t)syscall(SYS_gettid);
    uint32_t h = (eip * 2654435761u ^ tid * 40503u) >> 16;
    (void)sig; (void)si;
    for (int k = 0; k < 64; k++) {
        uint32_t j = (h + k) & (PROF_N - 1);
        if (prof_tab[j].n && (prof_tab[j].eip != eip || prof_tab[j].tid != tid)) continue;
        prof_tab[j].eip = eip; prof_tab[j].tid = tid; __atomic_add_fetch(&prof_tab[j].n, 1, __ATOMIC_RELAXED);
        return;
    }
}
static void prof_dump(void)
{
    FILE *f = fopen(prof_path, "w");
    if (!f) return;
    for (int j = 0; j < PROF_N; j++) if (prof_tab[j].n) fprintf(f, "%08x %u %u\n", prof_tab[j].eip, prof_tab[j].tid, prof_tab[j].n);
    fclose(f);
}
static void prof_init(void)
{
    prof_path = getenv("UYA_PROF");
    if (!prof_path) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = on_prof;
    sigaction(SIGPROF, &sa, 0);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, 0);
    atexit(prof_dump);
}

int plat_install_trap_handler(void)
{
    prof_init();
    struct sigaction sa;
    static char altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, 0);
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = on_trap;
    sigaction(SIGTRAP, &sa, 0);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = on_fault;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    return 0;
}
#endif

#ifdef __linux__
/* Headless (no display, no SDL2, or UYA_HEADLESS / UYA_DUMP set): with UYA_DUMP=dir every
 * 15th presented frame is written there as a PPM, so output can be checked. */
#include <stdlib.h>
static int hl_present(const uint32_t *px, int w, int h)
{
    static unsigned long n;
    const char *dir = getenv("UYA_DUMP");
    static long every = -1;
    if (every < 0) { const char *e = getenv("UYA_DUMP_EVERY"); every = e ? atol(e) : 15; if (every < 1) every = 1; }
    if (!px) return 1;                            /* no new frame this vsync */
    static long from = -1;
    if (from < 0) { const char *e = getenv("UYA_DUMP_FROM"); from = e ? atol(e) : 0; }   /* testing: first vsync to dump */
    if (dir && (n % every) == 0 && uya_frame_count() >= (uint32_t)from) {
        char path[512];
        snprintf(path, sizeof path, "%s/frame_%05lu.ppm", dir, n);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6 %d %d 255\n", w, h);
            for (int i = 0; i < w * h; i++) { uint32_t c = px[i]; fputc((c >> 16) & 0xFF, f); fputc((c >> 8) & 0xFF, f); fputc(c & 0xFF, f);  /* BGRX in memory */ }
            fclose(f);
        }
    }
    n++;
    return 1;
}
/* Scripted input for testing: UYA_KEYS="start@600+10,cross@900" presses a
 * button from vsync 600 for 10 vsyncs (default 6). During a movie the vsync
 * count stands still, so a press that is due skips the movie. */
static const struct { const char *name; unsigned bit; } pad_names[] = {
    {"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080},
    {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400}, {"r1", 0x0800},
    {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000},
    /* the left stick pushed all the way (testing; libpad2's scePad2Read reports it) */
    {"lup", 0x10000}, {"ldown", 0x20000}, {"lleft", 0x40000}, {"lright", 0x80000},
    {"rup", 0x100000}, {"rdown", 0x200000}, {"rleft", 0x400000}, {"rright", 0x800000},
};
static unsigned hl_buttons(void)
{
    const char *s = getenv("UYA_KEYS");
    unsigned now = uya_frame_count(), bits = 0;
    while (s && *s) {
        char name[16];
        unsigned at = 0, dur = 6;
        int k = 0;
        while (*s && *s != '@' && *s != ',' && k < 15) name[k++] = *s++;
        name[k] = 0;
        if (*s == '@') at = (unsigned)strtoul(s + 1, (char **)&s, 10);
        if (*s == '+') dur = (unsigned)strtoul(s + 1, (char **)&s, 10);
        if (now >= at && now < at + dur)
            for (unsigned i = 0; i < sizeof pad_names / sizeof pad_names[0]; i++)
                if (!strcmp(name, pad_names[i].name)) bits |= pad_names[i].bit;
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    return bits;
}
/* UYA_SKIPMOVIES=n skips the first n movies (plat_movie_started counts them) */

/* Sound on Linux: with UYA_DUMP set, each sound is saved there as a WAV file. */
static void hl_audio_play(const int16_t *lr, size_t frames, int rate, const char *tag)
{
    const char *dir = getenv("UYA_DUMP");
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.wav", dir, tag);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = (uint32_t)(frames * 4), v;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    v = (uint32_t)rate; fwrite(&v, 4, 1, f); v = (uint32_t)rate * 4; fwrite(&v, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(lr, 4, frames, f);
    fclose(f);
}
#endif

#ifdef __linux__
/* Sound effects on Linux: with UYA_DUMP set they go to sfx_48k_stereo.raw
 * (16-bit PCM). Playback is simulated with the clock so the mixer is paced. */
static uint64_t stream_written, stream_t0;
uint64_t uya_fixed_clock(void);
static uint64_t stream_now(void) { uint64_t v = uya_fixed_clock(); return v ? v : plat_time_us(); }   /* UYA_FIXEDTIME: the game's clock */
static void hl_stream_write(const int16_t *lr, int frames)
{
    static FILE *f;
    static int tried;
    if (!tried) {
        const char *dir = getenv("UYA_DUMP");
        tried = 1;
        if (dir) { char path[512]; snprintf(path, sizeof path, "%s/sfx_48k_stereo.raw", dir); f = fopen(path, "wb"); }
    }
    if (!stream_t0) stream_t0 = stream_now();
    if (f) fwrite(lr, 4, (size_t)frames, f);
    stream_written += (uint64_t)frames;
}
static int hl_stream_queued(void)
{
    if (!stream_t0) return 0;
    uint64_t played = (stream_now() - stream_t0) * 48000 / 1000000;
    return played >= stream_written ? 0 : (int)(stream_written - played);
}
#include "platform_linux_sdl.h"

/* ---- what the game calls: SDL when there is a display, else headless (testing) ---- */
int plat_present(const uint32_t *px, int w, int h) { return sdl_on() ? sdl_present(px, w, h) : hl_present(px, w, h); }
int plat_pad_sticks(uint8_t st[4]) { return sdl_on() ? sdl_sticks(st) : 0; }
unsigned plat_pad_buttons(void) { return hl_buttons() | (sdl_on() ? sdl_buttons() : 0); }   /* (UYA_KEYS: scripted presses) */
/* movies: a fresh press of Cross or Start skips (buttons already held when the movie starts do not);
 * UYA_SKIPMOVIES=n skips the first n movies (testing) */
static int movies_started;
static unsigned skip_held;
void plat_movie_started(void) { movies_started++; pump(); skip_held = plat_pad_buttons() & 0x4008; }
int plat_key_skip(void)
{
    const char *e = getenv("UYA_SKIPMOVIES");
    if (e && movies_started <= atoi(e)) return 1;
    pump();
    unsigned b = plat_pad_buttons() & 0x4008, fresh = b & ~skip_held;
    skip_held = b;
    return fresh != 0;
}
void plat_poll(void)
{
    if (!win) return;
    pump();
    if (win_closed) { uya_log("window closed"); exit(0); }
}
void plat_audio_play(const int16_t *lr, size_t frames, int rate, const char *tag) { if (sdl_on()) sdl_audio_play(lr, frames, rate); else hl_audio_play(lr, frames, rate, tag); }
void plat_audio_stop(void) { if (sdl_on()) sdl_audio_stop(); }
void plat_stream_write(const int16_t *lr, int frames) { if (sdl_on()) sdl_stream_write(lr, frames); else hl_stream_write(lr, frames); }
int plat_stream_queued(void) { return sdl_on() ? sdl_stream_queued() : hl_stream_queued(); }
/* OpenGL through SDL (none headless: the GS draws in software) */
int plat_gl_init(void) { return sdl_gl_init(); }
void plat_gl_current(int on) { sdl_gl_current(on); }
void plat_gl_swap(void) { sdl_gl_swap(); }
void plat_gl_size(int *w, int *h) { sdl_gl_size(w, h); }
void *plat_gl_proc(const char *name) { return sdl_gl_proc(name); }
#endif
