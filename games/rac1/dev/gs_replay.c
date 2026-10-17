/* gs_replay.c: runs GS input recorded with UYA_GSREC (host/src/gs/gs.c)
 * through a build of gs.c and prints a hash of GS memory and of the shown
 * frame after every vsync, and the time the GS took. Two builds of gs.c that
 * print the same hashes drew the same pixels.
 *
 * build (32-bit like the game; GS_C is the gs.c to test):
 *   zig cc -target x86-linux-musl -O2 -march=nehalem -mfpmath=sse -malign-double -fno-strict-aliasing -fwrapv \
 *       -I host/include -I host/src/gs -DGS_C='"../host/src/gs/gs.c"' tools/gs_replay.c host/src/gs/gs_gl.c -o gs_replay -static -lpthread
 *   (a gs.c elsewhere: an absolute path in GS_C and -I host/src/gs)
 * run:  gs_replay rec.bin [frames_dir]   (UYA_GS_THREADS=n: drawing threads, default 0;
 *       REPLAY_QUIET=1: no GS log; REPLAY_PROF=file: a program counter histogram, as UYA_PROF)
 *
 * The GL renderer (host/src/gs/gs_gl.c) in a 64-bit build with an EGL
 * context without a window (Mesa llvmpipe), frames read back from the GPU:
 *   gcc -O2 -DGS_GL -I host/include -I host/src/gs -DGS_C='"../host/src/gs/gs.c"' tools/gs_replay.c \
 *       host/src/gs/gs_gl.c tools/gs_gl_egl.c -o gs_replay_gl -ldl -lpthread -lm
 *   REPLAY_SOFT=1: the software GS in the same binary; REPLAY_GLNULL=1: GL calls do nothing (the
 *   CPU cost of the front end and the batching alone); REPLAY_VRAMSYNC=1: GPU targets read back into
 *   GS memory at each vsync, so the vram hash compares */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/time.h>
#include GS_C

/* REPLAY_PROF=file: a histogram of the program counter, 1 ms of CPU time apart ("eip tid count") */
static u32 prof_eip[1 << 16], prof_n[1 << 16];
static void on_prof(int sig, siginfo_t *si, void *ctxv)
{
    u32 eip = (u32)((ucontext_t *)ctxv)->uc_mcontext.gregs[
#ifdef __x86_64__
    REG_RIP
#else
    REG_EIP
#endif
    ], h = (eip * 2654435761u) >> 16;
    (void)sig; (void)si;
    for (int k = 0; k < 64; k++) { u32 j = (h + k) & 0xFFFF; if (prof_n[j] && prof_eip[j] != eip) continue; prof_eip[j] = eip; prof_n[j]++; return; }
}
static void prof_dump(void)
{
    FILE *f = fopen(getenv("REPLAY_PROF"), "w");
    if (f) { for (int j = 0; j < 1 << 16; j++) if (prof_n[j]) fprintf(f, "%08x 0 %u\n", prof_eip[j], prof_n[j]); fclose(f); }
}

#ifdef GS_GL
#include "gs_gl_api.h"
int egl_gl_init(void);
int plat_gl_active;
static int gl_null;
int plat_gl_init(void) { return gl_null ? 1 : egl_gl_init(); }
void egl_gl_current(int on);
void plat_gl_current(int on) { if (!gl_null) egl_gl_current(on); }
void plat_gl_swap(void) {}
void plat_gl_size(int *w, int *h) { *w = 640; *h = 480; }
/* REPLAY_GLNULL: every GL call returns 0 (a few return what initialisation needs) */
static long gl_nop(void) { return 0; }
static void GLAPIENTRY gl_getiv(GLuint a, GLenum b, GLint *v) { (void)a; (void)b; *v = 1; }
static char gl_mapbuf[8 << 20];
static void *GLAPIENTRY gl_map(GLenum a, GLintptr b, GLsizeiptr c, GLbitfield d) { (void)a; (void)b; (void)c; (void)d; return gl_mapbuf; }
static GLuint gl_names;
static void GLAPIENTRY gl_gen(GLsizei n, GLuint *o) { for (int i = 0; i < n; i++) o[i] = ++gl_names; }
static GLuint GLAPIENTRY gl_create(void) { return ++gl_names; }
static GLuint GLAPIENTRY gl_create1(GLenum e) { (void)e; return ++gl_names; }
void *plat_gl_proc(const char *name);
static void *null_proc(const char *n)
{
    if (!strcmp(n, "glGetShaderiv") || !strcmp(n, "glGetProgramiv")) return (void *)gl_getiv;
    if (!strcmp(n, "glMapBufferRange")) return (void *)gl_map;
    if (!strcmp(n, "glGenTextures") || !strcmp(n, "glGenBuffers") || !strcmp(n, "glGenVertexArrays") || !strcmp(n, "glGenFramebuffers")) return (void *)gl_gen;
    if (!strcmp(n, "glCreateProgram")) return (void *)gl_create;
    if (!strcmp(n, "glCreateShader")) return (void *)gl_create1;
    return (void *)gl_nop;
}
#else
int plat_gl_active;   /* (no GL in this build) */
int plat_gl_init(void) { return 0; }
void plat_gl_current(int on) { (void)on; }
void plat_gl_swap(void) {}
void plat_gl_size(int *w, int *h) { *w = 640; *h = 480; }
void *plat_gl_proc(const char *name) { (void)name; return 0; }
#endif
static uint32_t replay_vs;
uint32_t uya_frame_count(void) { return replay_vs; }
u32 vu_nonfinite;
const char *plat_exe_dir(void) { return "."; }
void uya_log(const char *fmt, ...)
{
    if (getenv("REPLAY_QUIET")) return;
    va_list ap; va_start(ap, fmt); fprintf(stderr, "[gs] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
}
int plat_present(const uint32_t *px, int w, int h) { (void)px; (void)w; (void)h; return 1; }
unsigned long long nsends, uya_t_vu;
void uya_vu_dump_kick(void) {}
void uya_vif1_dump_hist(void) {}

static uint32_t hash32(const void *p, size_t n)   /* FNV-1a over words */
{
    const uint32_t *w = p; uint32_t h = 2166136261u;
    for (size_t i = 0; i < n / 4; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu_s(void) { struct timespec t; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void rd(FILE *f, void *p, size_t n) { if (fread(p, 1, n, f) != n) { fprintf(stderr, "short read\n"); exit(1); } }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: gs_replay rec.bin [frames_dir]\n"); return 1; }
    if (!getenv("UYA_GS_THREADS")) setenv("UYA_GS_THREADS", "0", 1);
    /* the game's rounding (toward zero, host/src/rac1/boot.c), for the CPU's work on native
     * meshes (REPLAY_SOFT): REPLAY_RN=1 leaves the C default */
    if (!getenv("REPLAY_RN")) __builtin_ia32_ldmxcsr(__builtin_ia32_stmxcsr() | 0x6000u);
    if (getenv("REPLAY_PROF")) {
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO | SA_RESTART; sa.sa_sigaction = on_prof; sigaction(SIGPROF, &sa, 0);
        struct itimerval it = { { 0, 1000 }, { 0, 1000 } }; setitimer(ITIMER_PROF, &it, 0); atexit(prof_dump);
    }
    setenv("UYA_MTGS", "0", 1);
#ifdef GS_GL
    if (!getenv("REPLAY_SOFT")) setenv("UYA_RENDERER", "gl", 1);
    gl_null = getenv("REPLAY_GLNULL") != 0;
    if (gl_null) { extern void *(*gl_proc_override)(const char *); gl_proc_override = null_proc; }   /* the loader hands out the stubs */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    uya_gs_init();
    u32 m; rd(f, &m, 4);
    if (m != 0x52534755u) { fprintf(stderr, "not a GS recording\n"); return 1; }
#define X(fl) { u32 n_; rd(f, &n_, 4); if (n_ != sizeof fl) { fprintf(stderr, "field " #fl " size %u, expected %u\n", n_, (u32)sizeof fl); return 1; } rd(f, &fl, sizeof fl); }
    GS_SNAP_FIELDS(X)
#undef X
    rd(f, vram, 4 << 20);
    static u32 buf[(0x10000 + 16) * 4];
    static u32 *big; static size_t bign;
    double t = 0, t0 = now_s(), c = 0;
    u32 k;
    while (fread(&k, 4, 1, f) == 1) {
        if (k == 1) {
            u32 h[2]; rd(f, h, 8);
            u32 *d = buf;
            if (h[1] > 0x10000) { if (bign < h[1] * 16) { free(big); big = malloc(h[1] * 16); bign = h[1] * 16; } d = big; }
            rd(f, d, h[1] * 16);
            double a = now_s(), ac = cpu_s();
            if (h[0] == 3) { uya_gs_src = "native"; uya_gs_dlist(d, h[1]); }   /* a native draw list */
            else { uya_gs_src = path_name[h[0] < 3 ? h[0] : 2]; uya_gs_gif(d, h[1]); }
            t += now_s() - a; c += cpu_s() - ac;
        } else if (k == 2) {
            u64 r[6]; rd(f, r, 48);
            double a = now_s(), ac = cpu_s();
            u32 w = 0, hh = 0;
            if (getenv("REPLAY_DISPFBP")) r[2] = (r[2] & ~0x1FFull) | ((u64)strtol(getenv("REPLAY_DISPFBP"), 0, 16) / 32);   /* testing: show another buffer */
            gs_drain();
            int ok;
#ifdef GS_GL
            if (gsgl_on) {
                gs_disp_t dd;
                ok = gs_disp(r[0], r[1], r[2], r[3], r[4], r[5], &dd);
                if (ok && !gl_null) ok = gsgl_capture(&dd, frame_px);
                else gsgl_flush();
                w = dd.w; hh = dd.h;
                if (getenv("REPLAY_VRAMSYNC")) gsgl_cpu_read(0, 0x4000);
                if (ok && !gl_null && getenv("REPLAY_FULL") && argc > 2) {   /* the upscaled picture too: full_NNNNN.ppm */
                    int gsgl_capture_full(const gs_disp_t *d, u32 *out, int *ow, int *oh);
                    static u32 *big; int fw, fh;
                    if (!big) big = malloc((size_t)4096 * 4096 * 4);
                    if (gsgl_capture_full(&dd, big, &fw, &fh)) {
                        char p[512]; snprintf(p, sizeof p, "%s/full_%05u.ppm", argv[2], replay_vs + 1);
                        FILE *o = fopen(p, "wb");
                        if (o) { fprintf(o, "P6 %d %d 255\n", fw, fh);
                            for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) { u32 c = big[(size_t)y * fw + x]; fputc(c & 255, o); fputc((c >> 8) & 255, o); fputc((c >> 16) & 255, o); }
                            fclose(o); }
                    }
                }
            } else
#endif
            ok = gs_capture(r[0], r[1], r[2], r[3], r[4], r[5], frame_px, &w, &hh);
            t += now_s() - a; c += cpu_s() - ac;
            replay_vs++;
            printf("vsync %u vram %08X frame %08X\n", replay_vs, hash32(vram, 4 << 20), ok ? hash32(frame_px, (size_t)w * hh * 4) : 0);
            if (argc > 2 && ok) {
                char p[512]; snprintf(p, sizeof p, "%s/r_%05u.ppm", argv[2], replay_vs);
                FILE *o = fopen(p, "wb");
                if (o) { fprintf(o, "P6 %u %u 255\n", w, hh);
                    for (u32 i = 0; i < w * hh; i++) { u32 c = frame_px[i]; fputc((c >> 16) & 255, o); fputc((c >> 8) & 255, o); fputc(c & 255, o); }
                    fclose(o); }
            }
        } else if (k == 3) {
            u32 h[4]; rd(f, h, 16);
            u32 *px = malloc((size_t)h[2] * h[3] * 4); rd(f, px, (size_t)h[2] * h[3] * 4);
            double a = now_s();
            uya_gs_write_ct32(h[0], h[1], (int)h[2], (int)h[3], px);
            t += now_s() - a;
            free(px);
        } else { fprintf(stderr, "bad record %u\n", k); return 1; }
    }
#ifdef GS_GL
    if (gsgl_on) gsgl_stats((int)replay_vs);
#endif
    fprintf(stderr, "%u vsyncs: GS time %.3f s (%.2f ms/vsync), CPU %.2f ms/vsync, wall %.3f s\n", replay_vs, t, replay_vs ? 1000 * t / replay_vs : 0,
            replay_vs ? 1000 * c / replay_vs : 0, now_s() - t0);
    return 0;
}
