/* gs_gl_egl.c: an OpenGL 3.3 core context without a window for the GL
 * renderer's test harness (tools/gs_replay.c built with GS_GL=1, 64-bit):
 * EGL's surfaceless platform (Mesa, e.g. llvmpipe), loaded with dlopen so no
 * GL or EGL headers or link libraries are needed. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

typedef void *EGLDisplay, *EGLContext, *EGLConfig, *EGLSurface;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
static void *egl;
static void *(*eglGetProcAddress_)(const char *);
void *(*gl_proc_override)(const char *);   /* the harness's stubs (REPLAY_GLNULL) */
void *plat_gl_proc(const char *name)
{
    if (gl_proc_override) return gl_proc_override(name);
    if (!egl) return 0;
    void *p = eglGetProcAddress_ ? eglGetProcAddress_(name) : 0;
    if (!p && egl) p = dlsym(egl, name);
    return p;
}

static EGLBoolean (*egl_mc)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLDisplay egl_d; static EGLContext egl_c;
int egl_gl_init(void)
{
    egl = dlopen("libEGL.so.1", RTLD_NOW);
    if (!egl) { fprintf(stderr, "no libEGL: %s\n", dlerror()); return 0; }
    eglGetProcAddress_ = (void *(*)(const char *))dlsym(egl, "eglGetProcAddress");
    EGLDisplay (*getPlatformDisplay)(EGLenum, void *, const EGLint *) = (EGLDisplay (*)(EGLenum, void *, const EGLint *))eglGetProcAddress_("eglGetPlatformDisplayEXT");
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *) = (EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *))dlsym(egl, "eglInitialize");
    EGLBoolean (*bindAPI)(EGLenum) = (EGLBoolean (*)(EGLenum))dlsym(egl, "eglBindAPI");
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = (EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *))dlsym(egl, "eglChooseConfig");
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = (EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *))dlsym(egl, "eglCreateContext");
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = (EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext))dlsym(egl, "eglMakeCurrent");
    if (!getPlatformDisplay || !initialize || !bindAPI || !chooseConfig || !createContext || !makeCurrent) { fprintf(stderr, "EGL entry points missing\n"); return 0; }
    EGLDisplay d = getPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, 0, 0);
    EGLint ma, mi;
    if (!d || !initialize(d, &ma, &mi)) { fprintf(stderr, "eglInitialize failed\n"); return 0; }
    bindAPI(0x30A2 /* EGL_OPENGL_API */);
    EGLint ca[] = { 0x3040 /* RENDERABLE_TYPE */, 0x0008 /* OPENGL_BIT */, 0x3038 };
    EGLConfig cfg; EGLint n = 0;
    if (!chooseConfig(d, ca, &cfg, 1, &n) || n < 1) cfg = 0;   /* EGL_KHR_no_config_context */
    EGLint xa[] = { 0x3098, 3, 0x30FB, 3, 0x30FD, 1 /* core */, 0x3038 };
    EGLContext c = createContext(d, cfg, 0, xa);
    if (!c) { fprintf(stderr, "eglCreateContext failed\n"); return 0; }
    if (!makeCurrent(d, 0, 0, c)) { fprintf(stderr, "eglMakeCurrent failed\n"); return 0; }
    egl_mc = makeCurrent; egl_d = d; egl_c = c;
    return 1;
}
/* the context current on the calling thread, or released (the GL thread takes it over) */
void egl_gl_current(int on) { if (egl_mc && !egl_mc(egl_d, 0, 0, on ? egl_c : 0)) fprintf(stderr, "eglMakeCurrent(%d) failed\n", on); else if (getenv("REPLAY_GLDBG")) fprintf(stderr, "eglMakeCurrent(%d) ok\n", on); }
