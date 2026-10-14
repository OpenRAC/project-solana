/* platform_linux_sdl.h: the window, OpenGL, gamepads and sound on Linux, through SDL2
 * (included once by platform_linux.c).
 *
 * libSDL2-2.0.so.0 is loaded at run time (dlopen), so the game builds without SDL's
 * headers and still runs headless (testing) where there is no SDL or no display. Only
 * the few SDL2 declarations used are written out here (SDL2's ABI is stable).
 *
 *   window   one resizable window, 640x480 to start; F11 or UYA_FULLSCREEN=1 for full screen
 *   OpenGL   a 3.3 core context for the GS renderer (gs_gl.c); vsync with UYA_VSYNC=1
 *   gamepads SDL's game controller API: Xbox, PlayStation, Switch and Steam Deck controls
 *            by position (south = Cross, east = Circle, west = Square, north = Triangle)
 *   sound    two queued audio devices: one-shot sounds (plat_audio_play), and the 48 kHz
 *            effects stream (plat_stream_write) */
#include <dlfcn.h>

/* ---- SDL2 declarations (SDL.h, 2.0.x) ---- */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_GameController SDL_GameController;
typedef void *SDL_GLContext;
typedef uint32_t SDL_AudioDeviceID;
typedef struct { int freq; uint16_t format; uint8_t channels, silence; uint16_t samples, padding; uint32_t size; void *callback, *userdata; } SDL_AudioSpec;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { uint32_t flags; void *format; int w, h, pitch; void *pixels; } SDL_Surface_head;   /* (the start of SDL_Surface) */
#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_INIT_GAMECONTROLLER 0x2000u
#define SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x1001u
#define SDL_WINDOW_OPENGL 0x2u
#define SDL_WINDOW_SHOWN 0x4u
#define SDL_WINDOW_RESIZABLE 0x20u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x2000u
#define SDL_QUIT_EV 0x100u
#define SDL_WINDOWEVENT_EV 0x200u
#define SDL_KEYDOWN_EV 0x300u
#define SDL_WINDOWEVENT_CLOSE 14
#define SDL_SCANCODE_F9 66
#define SDL_SCANCODE_F11 68
#define SDL_GL_DOUBLEBUFFER 5
#define SDL_GL_CONTEXT_MAJOR_VERSION 17
#define SDL_GL_CONTEXT_MINOR_VERSION 18
#define SDL_GL_CONTEXT_PROFILE_MASK 21
#define SDL_GL_CONTEXT_PROFILE_CORE 1
#define AUDIO_S16LSB 0x8010
#define SDL_PIXELFORMAT_RGB888 0x16161804u   /* (XRGB8888: the frames' BGRX bytes) */

static struct {
    int (*Init)(uint32_t);
    const char *(*GetError)(void);
    int (*SetHint)(const char *, const char *);
    SDL_Window *(*CreateWindow)(const char *, int, int, int, int, uint32_t);
    void (*SetWindowTitle)(SDL_Window *, const char *);
    int (*SetWindowFullscreen)(SDL_Window *, uint32_t);
    uint32_t (*GetWindowFlags)(SDL_Window *);
    void (*GetWindowSize)(SDL_Window *, int *, int *);
    int (*PollEvent)(void *);
    void (*PumpEvents)(void);
    int (*ShowCursor)(int);
    int (*GL_SetAttribute)(int, int);
    SDL_GLContext (*GL_CreateContext)(SDL_Window *);
    int (*GL_MakeCurrent)(SDL_Window *, SDL_GLContext);
    void (*GL_SwapWindow)(SDL_Window *);
    void *(*GL_GetProcAddress)(const char *);
    int (*GL_SetSwapInterval)(int);
    void (*GL_GetDrawableSize)(SDL_Window *, int *, int *);
    void *(*GetWindowSurface)(SDL_Window *);
    int (*UpdateWindowSurface)(SDL_Window *);
    void *(*CreateRGBSurfaceWithFormatFrom)(void *, int, int, int, int, uint32_t);
    void (*FreeSurface)(void *);
    int (*FillRect)(void *, const SDL_Rect *, uint32_t);
    int (*UpperBlitScaled)(void *, const SDL_Rect *, void *, SDL_Rect *);
    int (*NumJoysticks)(void);
    int (*IsGameController)(int);
    SDL_GameController *(*GameControllerOpen)(int);
    void (*GameControllerClose)(SDL_GameController *);
    int (*GameControllerGetAttached)(SDL_GameController *);
    const char *(*GameControllerName)(SDL_GameController *);
    uint8_t (*GameControllerGetButton)(SDL_GameController *, int);
    int16_t (*GameControllerGetAxis)(SDL_GameController *, int);
    SDL_AudioDeviceID (*OpenAudioDevice)(const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int);
    void (*CloseAudioDevice)(SDL_AudioDeviceID);
    void (*PauseAudioDevice)(SDL_AudioDeviceID, int);
    int (*QueueAudio)(SDL_AudioDeviceID, const void *, uint32_t);
    uint32_t (*GetQueuedAudioSize)(SDL_AudioDeviceID);
    void (*ClearQueuedAudio)(SDL_AudioDeviceID);
    void (*GetVersion)(uint8_t v[3]);
    const char *(*GetCurrentVideoDriver)(void);
    const char *(*GetCurrentAudioDriver)(void);
} sdl;

/* 1: SDL is there and a display is (the game draws in a window); 0: headless */
static int sdl_state = -1;
static int sdl_on(void)
{
    if (sdl_state >= 0) return sdl_state;
    sdl_state = 0;
    if (getenv("UYA_HEADLESS") || getenv("UYA_DUMP")) return 0;   /* (testing: frames and sound to files) */
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) { uya_log("no display: running without a window"); return 0; }
    void *h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("libSDL2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { uya_log("SDL2 not found (%s): install the 32-bit SDL2 library (libSDL2-2.0.so.0, i386)", dlerror()); return 0; }
    static const char *const names[] = {
        "SDL_Init", "SDL_GetError", "SDL_SetHint", "SDL_CreateWindow", "SDL_SetWindowTitle", "SDL_SetWindowFullscreen",
        "SDL_GetWindowFlags", "SDL_GetWindowSize", "SDL_PollEvent", "SDL_PumpEvents", "SDL_ShowCursor",
        "SDL_GL_SetAttribute", "SDL_GL_CreateContext", "SDL_GL_MakeCurrent", "SDL_GL_SwapWindow", "SDL_GL_GetProcAddress",
        "SDL_GL_SetSwapInterval", "SDL_GL_GetDrawableSize", "SDL_GetWindowSurface", "SDL_UpdateWindowSurface",
        "SDL_CreateRGBSurfaceWithFormatFrom", "SDL_FreeSurface", "SDL_FillRect", "SDL_UpperBlitScaled",
        "SDL_NumJoysticks", "SDL_IsGameController", "SDL_GameControllerOpen", "SDL_GameControllerClose",
        "SDL_GameControllerGetAttached", "SDL_GameControllerName", "SDL_GameControllerGetButton", "SDL_GameControllerGetAxis",
        "SDL_OpenAudioDevice", "SDL_CloseAudioDevice", "SDL_PauseAudioDevice", "SDL_QueueAudio", "SDL_GetQueuedAudioSize",
        "SDL_ClearQueuedAudio", "SDL_GetVersion", "SDL_GetCurrentVideoDriver", "SDL_GetCurrentAudioDriver",
    };
    void **fp = (void **)&sdl;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!(fp[i] = dlsym(h, names[i]))) { uya_log("SDL2: %s missing (SDL 2.0.10 or newer needed)", names[i]); return 0; }
    sdl.SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    sdl.SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    if (sdl.Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0 && sdl.Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        uya_log("SDL2: cannot start (%s)", sdl.GetError());
        return 0;
    }
    {
        uint8_t v[3] = {0, 0, 0};
        const char *vd = sdl.GetCurrentVideoDriver(), *ad = sdl.GetCurrentAudioDriver();
        sdl.GetVersion(v);
        uya_log("SDL %d.%d.%d: video %s, audio %s", v[0], v[1], v[2], vd ? vd : "none", ad ? ad : "none");
    }
    sdl_state = 1;
    return 1;
}

/* ---- the window ---- */
static SDL_Window *win;
static SDL_GLContext glc;
int plat_gl_active;
static void win_create(uint32_t extra)
{
    if (win || !sdl_on()) return;
    uint32_t fl = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | extra;
    const char *fs = getenv("UYA_FULLSCREEN");
    if (fs && atoi(fs)) fl |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    win = sdl.CreateWindow("Project Solana", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480, fl);
    if (!win) uya_log("SDL2: no window (%s)", sdl.GetError());
    else if (fl & SDL_WINDOW_FULLSCREEN_DESKTOP) sdl.ShowCursor(0);
}

static void toggle_fullscreen(void)
{
    if (!win) return;
    int fs = (sdl.GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP;
    sdl.SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    sdl.ShowCursor(fs);
}

static volatile int win_closed;
static void pump(void)
{
    if (!win) return;
    union { uint32_t type; uint8_t b[64]; struct { uint32_t type, ts, wid; uint8_t state, repeat, p2, p3; int32_t scancode, sym; } key;
            struct { uint32_t type, ts, wid; uint8_t event; } w; uint8_t pad[128]; } ev;
    while (sdl.PollEvent(&ev)) {
        if (ev.type == SDL_QUIT_EV) win_closed = 1;
        else if (ev.type == SDL_WINDOWEVENT_EV && ev.w.event == SDL_WINDOWEVENT_CLOSE) win_closed = 1;
        else if (ev.type == SDL_KEYDOWN_EV && !ev.key.repeat) {
            if (ev.key.scancode == SDL_SCANCODE_F9) { void uya_gs_rec_hotkey(void); uya_gs_rec_hotkey(); }   /* testing: record the GS input */
            if (ev.key.scancode == SDL_SCANCODE_F11) toggle_fullscreen();
        }
    }
}

/* ---- OpenGL ---- */
void *sdl_gl_proc(const char *name) { return sdl.GL_GetProcAddress ? sdl.GL_GetProcAddress(name) : 0; }
static int sdl_gl_init(void)
{
    if (!sdl_on()) return 0;
    sdl.GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    sdl.GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    sdl.GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    sdl.GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win_create(SDL_WINDOW_OPENGL);
    if (!win) return 0;
    glc = sdl.GL_CreateContext(win);
    if (!glc) { uya_log("GL: no OpenGL 3.3 core context (%s)", sdl.GetError()); return 0; }
    {   /* UYA_VSYNC=1: swaps wait for the monitor (the game paces itself anyway) */
        const char *e = getenv("UYA_VSYNC");
        sdl.GL_SetSwapInterval(e ? atoi(e) : 0);
    }
    return 1;
}
static void sdl_gl_current(int on) { if (glc) sdl.GL_MakeCurrent(win, on ? glc : 0); }
static void sdl_gl_swap(void) { if (glc) sdl.GL_SwapWindow(win); }
static void sdl_gl_size(int *w, int *h)
{
    *w = 640; *h = 480;
    if (win && glc) sdl.GL_GetDrawableSize(win, w, h);   /* pixels (HiDPI) */
    else if (win) sdl.GetWindowSize(win, w, h);
}

/* the frame rate in the title, as on Windows */
static void title_fps(void)
{
    extern unsigned long long nsends;
    static uint64_t t0; static unsigned long long s0;
    uint64_t t = plat_time_us();
    if (!t0) { t0 = t; s0 = nsends; return; }
    if (t - t0 < 1000000) return;
    char title[64];
    snprintf(title, sizeof title, "Project Solana - %.0f FPS", (double)(nsends - s0) / ((double)(t - t0) / 1e6));
    sdl.SetWindowTitle(win, title);
    t0 = t; s0 = nsends;
}

/* the software renderer's frames (when there is no OpenGL): scaled into the window, 4:3 */
static int sdl_present(const uint32_t *px, int w, int h)
{
    win_create(0);
    pump();
    if (win_closed) return 0;
    if (!win) return 1;
    title_fps();
    if (plat_gl_active || !px) return 1;   /* (GL: the GS thread draws the window) */
    void *dst = sdl.GetWindowSurface(win);
    void *src = sdl.CreateRGBSurfaceWithFormatFrom((void *)px, w, h, 32, w * 4, SDL_PIXELFORMAT_RGB888);
    if (dst && src) {
        const SDL_Surface_head *d = dst;
        int vw = d->w, vh = d->w * 3 / 4;
        if (vh > d->h) { vh = d->h; vw = d->h * 4 / 3; }
        SDL_Rect r = { (d->w - vw) / 2, (d->h - vh) / 2, vw, vh };
        sdl.FillRect(dst, 0, 0);
        sdl.UpperBlitScaled(src, 0, dst, &r);
        sdl.UpdateWindowSurface(win);
    }
    if (src) sdl.FreeSurface(src);
    return 1;
}

/* ---- gamepads ---- */
static SDL_GameController *pad;
static uint64_t pad_next;
static SDL_GameController *pad_get(void)
{
    if (pad && !sdl.GameControllerGetAttached(pad)) { uya_log("gamepad: disconnected"); sdl.GameControllerClose(pad); pad = 0; }
    if (pad) return pad;
    uint64_t t = plat_time_us();
    if (t < pad_next) return 0;
    pad_next = t + 1000000;   /* looked for every second */
    int n = sdl.NumJoysticks();
    for (int i = 0; i < n && !pad; i++)
        if (sdl.IsGameController(i) && (pad = sdl.GameControllerOpen(i))) uya_log("gamepad: %s", sdl.GameControllerName(pad));
    return pad;
}
static unsigned sdl_buttons(void)
{
    if (!sdl_on()) return 0;
    if (!win) sdl.PumpEvents();
    SDL_GameController *g = pad_get();
    if (!g) return 0;
    /* SDL_GameControllerButton: A B X Y Back Guide Start LS RS LB RB Up Down Left Right */
    static const unsigned map[15] = { 0x4000, 0x2000, 0x8000, 0x1000, 0x0001, 0, 0x0008, 0x0002, 0x0004, 0x0400, 0x0800, 0x0010, 0x0040, 0x0080, 0x0020 };
    unsigned b = 0;
    for (int i = 0; i < 15; i++) if (map[i] && sdl.GameControllerGetButton(g, i)) b |= map[i];
    if (sdl.GameControllerGetAxis(g, 4) > 8000) b |= 0x0100;   /* L2 */
    if (sdl.GameControllerGetAxis(g, 5) > 8000) b |= 0x0200;   /* R2 */
    return b;
}
static uint8_t sdl_axis(int16_t v)   /* -32768..32767 -> 0..255, 0x80 centre, with a dead zone; down is + (as the PS2) */
{
    int x = v / 256;
    if (x > -24 && x < 24) return 0x7F;
    x += 0x80;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}
static int sdl_sticks(uint8_t st[4])
{
    if (!sdl_on()) return 0;
    SDL_GameController *g = pad_get();
    if (!g) return 0;
    for (int i = 0; i < 4; i++) st[i] = sdl_axis(sdl.GameControllerGetAxis(g, i));
    return 1;
}

/* ---- sound ---- */
static SDL_AudioDeviceID one_dev, stream_dev;
static int stream_failed;
static SDL_AudioDeviceID audio_open(int rate)
{
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = rate; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
    SDL_AudioDeviceID d = sdl.OpenAudioDevice(0, 0, &want, &have, 0);   /* (SDL converts to what the device takes) */
    if (!d) { uya_log("sound: no output device (%s)", sdl.GetError()); return 0; }
    sdl.PauseAudioDevice(d, 0);
    return d;
}
static void sdl_audio_stop(void)
{
    if (!one_dev) return;
    sdl.ClearQueuedAudio(one_dev);
    sdl.CloseAudioDevice(one_dev);
    one_dev = 0;
}
static void sdl_audio_play(const int16_t *lr, size_t frames, int rate)
{
    sdl_audio_stop();
    if (!(one_dev = audio_open(rate))) return;
    sdl.QueueAudio(one_dev, lr, (uint32_t)(frames * 4));
}
#define STREAM_MAX_FRAMES (32 * 1024)   /* as the Windows ring: beyond it, writes are dropped */
static int sdl_stream_queued(void)
{
    if (!stream_dev) return stream_failed ? 1 << 20 : 0;
    return (int)(sdl.GetQueuedAudioSize(stream_dev) / 4);
}
static void sdl_stream_write(const int16_t *lr, int frames)
{
    if (!stream_dev && !stream_failed && !(stream_dev = audio_open(48000))) stream_failed = 1;
    if (!stream_dev || sdl_stream_queued() + frames > STREAM_MAX_FRAMES) return;
    sdl.QueueAudio(stream_dev, lr, (uint32_t)frames * 4);
}
