/* Windows (x86) platform layer. The menu code is in rac1_game.dll; the
 * launcher (launcher/launcher_win32.c) is a tiny exe whose own image covers
 * PS2 RAM, so that range is reserved before Windows places the stack and heaps. */
#ifdef _WIN32
#include "uya_runtime.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <string.h>
#include <ctype.h>

extern int plat_gl_active;   /* the GS presents with OpenGL (below) */

/* Something other than the game holds a PS2 address: say what, and stop (taking the memory
 * over would break whatever owns it: once it was this DLL's own code, relocated there). */
static int map_conflict(uint32_t addr, uint32_t size, uint32_t at, const MEMORY_BASIC_INFORMATION *mi)
{
    char who[MAX_PATH] = "memory of another program loaded into the game (an overlay or similar)", msg[1024];
    if (mi && mi->Type == MEM_IMAGE && GetModuleFileNameA((HMODULE)mi->AllocationBase, who, sizeof who)) {}
    uya_log("cannot map PS2 memory 0x%08X+0x%X: 0x%08X is in use by %s", addr, size, at, who);
    snprintf(msg, sizeof msg, "The game needs the memory at 0x%08X for the PS2, but it is in use by:\n\n%s\n\n"
             "If this is a program injected into games (an overlay, recorder or RGB tool), close it or exclude "
             "rac1_wrapper.exe from it, then try again.", at, who);
    MessageBoxA(0, msg, "Project Solana", MB_ICONERROR);
    return -1;
}

int plat_map_fixed(uint32_t addr, uint32_t size, int exec)
{
    DWORD prot = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    static int said;
    if (!said++) {   /* (normally 0x40000000; elsewhere when Windows relocated it) */
        MEMORY_BASIC_INFORMATION me;
        if (VirtualQuery((void *)plat_map_fixed, &me, sizeof me)) uya_log("rac1_game.dll loaded at %p", me.AllocationBase);
        if ((uintptr_t)me.AllocationBase < 0x40000000u) {   /* (rac1_wrapper.exe keeps it above: see launcher_win32.c) */
            char msg[512];
            snprintf(msg, sizeof msg, "rac1_game.dll was loaded at 0x%08X, but the game needs it at 0x40000000 or above "
                     "(lower addresses belong to the PS2). Please report this, with rac1_log.txt.", (unsigned)(uintptr_t)me.AllocationBase);
            uya_log("%s", msg);
            MessageBoxA(0, msg, "Project Solana", MB_ICONERROR);
            return -1;
        }
    }
    MEMORY_BASIC_INFORMATION mi;
    void *p = 0;
    if (VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof mi)) {
        if (mi.State == MEM_RESERVE && mi.AllocationBase == (void *)(uintptr_t)addr && mi.RegionSize >= size)
            p = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_COMMIT, prot);   /* rac1_wrapper.exe reserved it before loading this DLL */
        else if (mi.State == MEM_FREE)
            p = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, prot);
    }
    if (p == (void *)(uintptr_t)addr) return 0;
    /* already there: only PS2 RAM, which is the wrapper's own image section (.ps2ram) */
    HMODULE self = GetModuleHandleA(0);
    uint32_t a = addr;
    while (a < addr + size) {
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) return map_conflict(addr, size, a, 0);
        if (mi.State != MEM_COMMIT || mi.AllocationBase != (void *)self) return map_conflict(addr, size, a, &mi);
        DWORD old;
        VirtualProtect(mi.BaseAddress, mi.RegionSize, prot, &old);
        a = (uint32_t)(uintptr_t)mi.BaseAddress + (uint32_t)mi.RegionSize;
    }
    return 0;
}

void plat_flush_icache(void *p, size_t n) { FlushInstructionCache(GetCurrentProcess(), p, n); }

const char *plat_exe_dir(void)
{
    static char d[MAX_PATH];
    if (!d[0]) {
        DWORD n = GetModuleFileNameA(0, d, sizeof d);
        if (!n) { strcpy(d, "."); return d; }
        char *s = strrchr(d, '\\');
        if (s) *s = 0;
    }
    return d;
}

/* Started by double-click, the console belongs to this process alone and
 * would close at once; wait for a key so the log can be read. */
void plat_at_exit(void)
{
    static int done;
    DWORD pids[2];
    if (done++) return;
    if (GetConsoleProcessList(pids, 2) == 1) {
        fprintf(stderr, "\nPress Enter to close this window.\n");
        getchar();
    }
}

/* the function at rva in rac1_game.sym (written by build.py: "RVA name" per line, sorted),
 * read with Win32 calls only: the C library may be what crashed */
static int sym_lookup(uint32_t rva, char *name, int cap, uint32_t *start)
{
    char p[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(0, p, MAX_PATH);
    while (n && p[n - 1] != '\\') n--;
    lstrcpyA(p + n, "rac1_game.sym");
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(f, 0), got = 0;
    char *b = sz && sz < (64u << 20) ? VirtualAlloc(0, sz + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : 0;
    int ok = b && ReadFile(f, b, sz, &got, 0) && got == sz;
    CloseHandle(f);
    if (!ok) { if (b) VirtualFree(b, 0, MEM_RELEASE); return 0; }
    b[sz] = '\n';
    const char *best = 0; uint32_t ba = 0;
    for (char *q = b; q < b + sz;) {
        uint32_t a = 0; char *s = q;
        for (; s < q + 8; s++) a = a * 16 + (uint32_t)(*s <= '9' ? *s - '0' : (*s | 32) - 'a' + 10);
        if (a > rva) break;
        best = s + 1; ba = a;
        while (*q != '\n') q++;
        q++;
    }
    int k = 0;
    if (best) for (; best[k] != '\n' && best[k] != '\r' && k < cap - 1; k++) name[k] = best[k];
    name[k] = 0; *start = ba;
    VirtualFree(b, 0, MEM_RELEASE);
    return k > 0;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    uint32_t at = (uint32_t)(uintptr_t)er->ExceptionAddress;
    if (er->ExceptionCode == EXCEPTION_BREAKPOINT && at >= PS2_RAM_LO && at < PS2_RAM_HI) {
        uint32_t ret = *(uint32_t *)(uintptr_t)c->Esp;
        uya_trap_call(at, ret, (const uint32_t *)(uintptr_t)(c->Esp + 4));
        c->Eax = 0; c->Edx = 0;
        c->Eip = ret; c->Esp += 4;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    static volatile LONG in_crash;
    if (InterlockedExchange(&in_crash, 1)) ExitProcess(1);   /* (a crash while reporting one) */
    {   /* where: module+offset, the form crash_lookup.py and Windows' event log use */
        char mod[MAX_PATH] = "?"; MEMORY_BASIC_INFORMATION mi;
        uintptr_t base = 0;
        if (VirtualQuery(er->ExceptionAddress, &mi, sizeof mi) && mi.Type == MEM_IMAGE) {
            base = (uintptr_t)mi.AllocationBase;
            if (GetModuleFileNameA((HMODULE)base, mod, sizeof mod)) { char *s = strrchr(mod, '\\'); if (s) memmove(mod, s + 1, strlen(s)); }
        }
        uint32_t off = (uint32_t)((uintptr_t)er->ExceptionAddress - base);
        uya_log("crash: exception 0x%08lX at 0x%08X (%s+0x%X), fault address 0x%08lX", er->ExceptionCode, at, base ? mod : "no module",
                off, er->NumberParameters > 1 ? (unsigned long)er->ExceptionInformation[1] : 0ul);
        char fn[256]; uint32_t fs;
        MEMORY_BASIC_INFORMATION me;   /* (this DLL: is the crash in it?) */
        if (base && VirtualQuery((void *)sym_lookup, &me, sizeof me) && me.AllocationBase == (void *)base && sym_lookup(off, fn, sizeof fn, &fs))
            uya_log("crash: in %s+0x%X", fn, off - fs);
        /* the return addresses on the stack, named: the call chain */
        const uint32_t *sp = (const uint32_t *)(uintptr_t)c->Esp;
        for (int i = 0, shown = 0; i < 256 && shown < 12 && !IsBadReadPtr(sp + i, 4); i++) {
            uintptr_t v = sp[i];
            if (base && v > base && v < base + 0x10000000 && VirtualQuery((void *)v, &me, sizeof me) && me.AllocationBase == (void *)base &&
                (me.Protect & 0xF0) && sym_lookup((uint32_t)(v - base), fn, sizeof fn, &fs)) {
                uya_log("crash: called from %s+0x%X", fn, (uint32_t)(v - base) - fs); shown++;
            }
        }
    }
    uya_log("crash: thread %lu, eax %08lX ecx %08lX edx %08lX ebx %08lX esi %08lX edi %08lX esp %08lX ebp %08lX",
            GetCurrentThreadId(), c->Eax, c->Ecx, c->Edx, c->Ebx, c->Esi, c->Edi, c->Esp, c->Ebp);
    {   /* the stack: return addresses near its top, then the frame chain */
        const uint32_t *sp = (const uint32_t *)(uintptr_t)c->Esp;
        for (int i = 0; i < 24; i += 8)
            uya_log("crash: stack +%02X: %08X %08X %08X %08X %08X %08X %08X %08X", i * 4, sp[i], sp[i + 1], sp[i + 2], sp[i + 3], sp[i + 4], sp[i + 5], sp[i + 6], sp[i + 7]);
        uint32_t bp = (uint32_t)c->Ebp;
        for (int i = 0; i < 12 && bp > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)bp, 8); i++) {
            uya_log("crash: frame %d: return %08X", i, ((uint32_t *)(uintptr_t)bp)[1]);
            bp = ((uint32_t *)(uintptr_t)bp)[0];
        }
    }
    uya_report();
    plat_at_exit();
    ExitProcess(1);
    return EXCEPTION_CONTINUE_SEARCH;
}

int plat_install_trap_handler(void)
{
    return AddVectoredExceptionHandler(1, on_exception) ? 0 : -1;
}

uint64_t plat_time_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000u + (uint64_t)(c.QuadPart % f.QuadPart) * 1000000u / f.QuadPart;
}

void plat_sleep_us(uint64_t us)
{
    static int period;
    if (!period) { timeBeginPeriod(1); period = 1; }   /* 1 ms sleep resolution */
    if (us >= 1000) Sleep((DWORD)(us / 1000));
}

/* ---- the window ----
 * A plain Win32 window; frames are stretched into it with StretchDIBits
 * (4:3, 640x480 to start, resizable). */
static HWND win;
static volatile int win_closed;

/* Controller only: the keyboard does not play (F9 still records a glitch capture). */

/* XInput gamepads (loaded at run time: no XInput, no gamepad) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } xi_pad_t;
typedef struct { DWORD dwPacketNumber; xi_pad_t Gamepad; } xi_state_t;
typedef DWORD (WINAPI *xi_get_t)(DWORD, xi_state_t *);
static xi_get_t xi_get;
static int xi_tried;
static int xi_read(xi_state_t *st)
{
    if (!xi_tried) {
        static const char *dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        xi_tried = 1;
        for (int i = 0; i < 3 && !xi_get; i++) { HMODULE m = LoadLibraryA(dlls[i]); if (m) xi_get = (xi_get_t)(void *)GetProcAddress(m, "XInputGetState"); }
        if (xi_get) uya_log("gamepad: XInput available");
    }
    if (!xi_get) return 0;
    static uint64_t next_try;   /* a missing pad is slow to poll: try it again every second */
    uint64_t t = plat_time_us();
    if (next_try && t < next_try) return 0;
    if (xi_get(0, st) != 0) { next_try = t + 1000000; return 0; }
    next_try = 0;
    return 1;
}
static unsigned xi_buttons(const xi_pad_t *g)
{
    static const struct { WORD xi; unsigned ps2; } map[] = {
        {0x0001, 0x0010}, {0x0002, 0x0040}, {0x0004, 0x0080}, {0x0008, 0x0020},   /* d-pad up down left right */
        {0x0010, 0x0008}, {0x0020, 0x0001},                                       /* Start, Back = Select */
        {0x0040, 0x0002}, {0x0080, 0x0004},                                       /* L3, R3 */
        {0x0100, 0x0400}, {0x0200, 0x0800},                                       /* LB = L1, RB = R1 */
        {0x1000, 0x4000}, {0x2000, 0x2000}, {0x4000, 0x8000}, {0x8000, 0x1000},   /* A = Cross, B = Circle, X = Square, Y = Triangle */
    };
    unsigned b = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (g->wButtons & map[i].xi) b |= map[i].ps2;
    if (g->bLeftTrigger > 64) b |= 0x0100;   /* L2 */
    if (g->bRightTrigger > 64) b |= 0x0200;  /* R2 */
    return b;
}
static uint8_t stick(SHORT v, int invert)
{
    int x = v / 256;                       /* -128..127 */
    if (x > -24 && x < 24) return 0x7F;    /* dead zone */
    if (invert) x = -x - 1;
    x += 0x80;
    return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}
/* ---- PlayStation controllers (DualShock 4, DualSense) over USB or Bluetooth ----
 * Read as plain HID devices (hid.dll and setupapi.dll loaded at run time) on
 * a thread of their own, which keeps the latest state. Reports handled:
 *   DualShock 4: 0x01 (USB, and Bluetooth before the full mode) and 0x11
 *     (Bluetooth full mode): LX LY RX RY, hat + face buttons, shoulder and
 *     stick buttons, PS, L2 R2 analog, from offset 1 (0x01) or 3 (0x11)
 *   DualSense: 0x01 over USB (64 bytes) and 0x31 over Bluetooth: LX LY RX RY
 *     L2 R2, a counter, then the button bytes as the DualShock 4 has them;
 *     its short Bluetooth 0x01 report has the DualShock 4 layout */
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } hid_attr_t;   /* HIDD_ATTRIBUTES */
typedef struct { DWORD cbSize; GUID InterfaceClassGuid; DWORD Flags; ULONG_PTR Reserved; } sp_ifdata_t;
typedef struct { DWORD cbSize; CHAR DevicePath[1]; } sp_ifdetail_t;
static void (WINAPI *p_HidD_GetHidGuid)(GUID *);
static BOOLEAN (WINAPI *p_HidD_GetAttributes)(HANDLE, hid_attr_t *);
static HANDLE (WINAPI *p_SetupDiGetClassDevsA)(const GUID *, PCSTR, HWND, DWORD);
static BOOL (WINAPI *p_SetupDiEnumDeviceInterfaces)(HANDLE, void *, const GUID *, DWORD, sp_ifdata_t *);
static BOOL (WINAPI *p_SetupDiGetDeviceInterfaceDetailA)(HANDLE, sp_ifdata_t *, sp_ifdetail_t *, DWORD, DWORD *, void *);
static BOOL (WINAPI *p_SetupDiDestroyDeviceInfoList)(HANDLE);
static volatile LONG ds_ok;                 /* a controller is connected and reporting */
static volatile unsigned ds_buttons;        /* PS2 pad bits */
static volatile uint8_t ds_sticks[4];       /* LX LY RX RY, 0x80 centre, down = +Y as on the PS2 */

static HANDLE ds_open(int *dualsense)
{
    GUID g; p_HidD_GetHidGuid(&g);
    HANDLE set = p_SetupDiGetClassDevsA(&g, 0, 0, 0x02 | 0x10);   /* DIGCF_PRESENT | DIGCF_DEVICEINTERFACE */
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    HANDLE found = INVALID_HANDLE_VALUE;
    static int logged;   /* the first scan is logged: which HID devices there are, and why one was not used */
    int ndev = 0;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE; i++) {
        sp_ifdata_t ifd; memset(&ifd, 0, sizeof ifd); ifd.cbSize = sizeof ifd;
        if (!p_SetupDiEnumDeviceInterfaces(set, 0, &g, i, &ifd)) break;
        DWORD need = 0;
        p_SetupDiGetDeviceInterfaceDetailA(set, &ifd, 0, 0, &need, 0);
        if (!need) continue;
        sp_ifdetail_t *det = malloc(need);
        if (!det) continue;
        det->cbSize = sizeof(DWORD) + sizeof(CHAR);   /* 5 on 32-bit Windows */
        if (p_SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, need, 0, 0)) {
            ndev++;
            {   /* the path names the vendor and product: only Sony devices are opened */
                char low[512]; size_t k = 0;
                for (; det->DevicePath[k] && k < sizeof low - 1; k++) low[k] = (char)tolower((unsigned char)det->DevicePath[k]);
                low[k] = 0;
                int sony = strstr(low, "054c") != 0;
                if (!logged && sony) uya_log("gamepad: HID device %s", det->DevicePath);
                if (!sony) { free(det); continue; }
            }
            HANDLE h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (h == INVALID_HANDLE_VALUE)   /* some systems open game controllers for reading only */
                h = CreateFileA(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (h == INVALID_HANDLE_VALUE && !logged) uya_log("gamepad: cannot open it (error %lu)", GetLastError());
            if (h != INVALID_HANDLE_VALUE) {
                hid_attr_t at; memset(&at, 0, sizeof at); at.Size = sizeof at;
                int ok = 0;
                if (p_HidD_GetAttributes(h, &at) && at.VendorID == 0x054C) {
                    if (at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0) { ok = 1; *dualsense = 0; }   /* DualShock 4 (and its USB adapter) */
                    if (at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2) { ok = 1; *dualsense = 1; }                            /* DualSense (Edge) */
                }
                if (!logged) uya_log("gamepad:   054C:%04X%s", at.ProductID, ok ? " (a DualShock 4 / DualSense)" : " (not a known controller)");
                if (ok) { found = h; uya_log("gamepad: %s (054C:%04X) found", *dualsense ? "DualSense" : "DualShock 4", at.ProductID); }
                else CloseHandle(h);
            }
        }
        free(det);
    }
    p_SetupDiDestroyDeviceInfoList(set);
    if (!logged) uya_log("gamepad: %d HID devices, %s", ndev, found != INVALID_HANDLE_VALUE ? "a PlayStation controller is used" : "no PlayStation controller found (connected later, it is picked up)");
    logged = 1;
    return found;
}

/* PS2 bits from the two button bytes the DualShock 4 and DualSense share, and the PS byte */
static unsigned ds_map(uint8_t b0, uint8_t b1)
{
    static const unsigned hat[9] = { 0x10, 0x10 | 0x20, 0x20, 0x20 | 0x40, 0x40, 0x40 | 0x80, 0x80, 0x80 | 0x10, 0 };
    unsigned b = hat[(b0 & 15) > 8 ? 8 : (b0 & 15)];
    if (b0 & 0x10) b |= 0x8000;   /* Square */
    if (b0 & 0x20) b |= 0x4000;   /* Cross */
    if (b0 & 0x40) b |= 0x2000;   /* Circle */
    if (b0 & 0x80) b |= 0x1000;   /* Triangle */
    if (b1 & 0x01) b |= 0x0400;   /* L1 */
    if (b1 & 0x02) b |= 0x0800;   /* R1 */
    if (b1 & 0x04) b |= 0x0100;   /* L2 */
    if (b1 & 0x08) b |= 0x0200;   /* R2 */
    if (b1 & 0x10) b |= 0x0001;   /* Share / Create = Select */
    if (b1 & 0x20) b |= 0x0008;   /* Options = Start */
    if (b1 & 0x40) b |= 0x0002;   /* L3 */
    if (b1 & 0x80) b |= 0x0004;   /* R3 */
    return b;
}

static int ds_parse(const uint8_t *r, DWORD n, int dualsense)
{
    const uint8_t *s;   /* the sticks */
    uint8_t b0, b1;
    if (n < 10) return 0;
    if (r[0] == 0x11 && n >= 12) { s = r + 3; b0 = s[4]; b1 = s[5]; }                       /* DualShock 4, Bluetooth full */
    else if (r[0] == 0x31 && n >= 13) { s = r + 2; b0 = s[7]; b1 = s[8]; }                  /* DualSense, Bluetooth full */
    else if (r[0] == 0x01 && dualsense && n >= 12 && n > 20) { s = r + 1; b0 = s[7]; b1 = s[8]; }   /* DualSense, USB */
    else if (r[0] == 0x01) { s = r + 1; b0 = s[4]; b1 = s[5]; }                             /* DualShock 4; DualSense short Bluetooth */
    else return 0;
    for (int i = 0; i < 4; i++) ds_sticks[i] = s[i];
    ds_buttons = ds_map(b0, b1);
    return 1;
}

static DWORD WINAPI ds_thread(LPVOID a)
{
    (void)a;
    static uint8_t buf[1024];
    for (;;) {
        int dualsense = 0;
        HANDLE h = ds_open(&dualsense);
        if (h == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
        {   /* asking a DualShock 4 on Bluetooth for its calibration switches it to the full reports (0x11) */
            static BOOLEAN (WINAPI *getf)(HANDLE, void *, ULONG);
            if (!getf) { HMODULE m = GetModuleHandleA("hid.dll"); if (m) getf = (void *)GetProcAddress(m, "HidD_GetFeature"); }
            if (getf && !dualsense) { uint8_t f[64]; memset(f, 0, sizeof f); f[0] = 0x05; getf(h, f, sizeof f); }
        }
        DWORD rlen = sizeof buf;   /* one report per read (a longer buffer gets several, oldest first) */
        {
            HMODULE m = GetModuleHandleA("hid.dll");
            BOOLEAN (WINAPI *gp)(HANDLE, void **) = m ? (void *)GetProcAddress(m, "HidD_GetPreparsedData") : 0;
            BOOLEAN (WINAPI *fp)(void *) = m ? (void *)GetProcAddress(m, "HidD_FreePreparsedData") : 0;
            LONG (WINAPI *caps)(void *, void *) = m ? (void *)GetProcAddress(m, "HidP_GetCaps") : 0;
            void *pd = 0;
            if (gp && fp && caps && gp(h, &pd)) {
                USHORT c[32]; memset(c, 0, sizeof c);
                if (caps(pd, c) == 0x00110000 && c[2] && c[2] <= sizeof buf) rlen = c[2];   /* HIDP_STATUS_SUCCESS; InputReportByteLength */
                fp(pd);
            }
            uya_log("gamepad: reports of %lu bytes", rlen);
        }
        int fails = 0;
        for (;;) {
            DWORD n = 0;
            if (!ReadFile(h, buf, rlen, &n, 0)) { uya_log("gamepad: read failed (error %lu)", GetLastError()); if (++fails > 3) break; Sleep(5); continue; }
            fails = 0;
            {   static int nlog;
                if (nlog < 3) { nlog++; uya_log("gamepad: report %02X, %lu bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", buf[0], n, buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7], buf[8], buf[9], buf[10]); }
            }
            if (ds_parse(buf, n, dualsense)) InterlockedExchange(&ds_ok, 1);
        }
        InterlockedExchange(&ds_ok, 0);
        ds_buttons = 0;
        CloseHandle(h);
        uya_log("gamepad: PlayStation controller disconnected");
        Sleep(1000);
    }
    return 0;
}

static void ds_start(void)
{
    static int tried;
    if (tried) return;
    tried = 1;
    HMODULE hid = LoadLibraryA("hid.dll"), sa = LoadLibraryA("setupapi.dll");
    if (!hid || !sa) return;
    p_HidD_GetHidGuid = (void *)GetProcAddress(hid, "HidD_GetHidGuid");
    p_HidD_GetAttributes = (void *)GetProcAddress(hid, "HidD_GetAttributes");
    p_SetupDiGetClassDevsA = (void *)GetProcAddress(sa, "SetupDiGetClassDevsA");
    p_SetupDiEnumDeviceInterfaces = (void *)GetProcAddress(sa, "SetupDiEnumDeviceInterfaces");
    p_SetupDiGetDeviceInterfaceDetailA = (void *)GetProcAddress(sa, "SetupDiGetDeviceInterfaceDetailA");
    p_SetupDiDestroyDeviceInfoList = (void *)GetProcAddress(sa, "SetupDiDestroyDeviceInfoList");
    if (!p_HidD_GetHidGuid || !p_HidD_GetAttributes || !p_SetupDiGetClassDevsA || !p_SetupDiEnumDeviceInterfaces ||
        !p_SetupDiGetDeviceInterfaceDetailA || !p_SetupDiDestroyDeviceInfoList) return;
    HANDLE t = CreateThread(0, 0, ds_thread, 0, 0, 0);
    if (t) CloseHandle(t);
}

static uint8_t ds_axis(uint8_t v)          /* a small dead zone; the game has its own on top */
{
    return (v > 0x80 - 12 && v < 0x80 + 12) ? 0x7F : v;
}

/* The same controllers through the Windows joystick API (winmm), when the
 * HID reader does not get them: Windows itself reads a DualShock 4 or
 * DualSense as a game controller, axes X Y = left stick, Z R = right stick,
 * U V = L2 R2, buttons 1-14 Square Cross Circle Triangle L1 R1 L2 R2
 * Share Options L3 R3 PS touchpad, the d-pad as the POV hat. */
static int jm_id = -1;
static uint64_t jm_next;
static int jm_read(JOYINFOEX *ji)
{
    uint64_t t = plat_time_us();
    if (jm_id < 0) {
        if (t < jm_next) return 0;
        jm_next = t + 2000000;                       /* looked for every 2 s */
        UINT n = joyGetNumDevs();
        for (UINT i = 0; i < n && i < 16; i++) {
            JOYCAPSA cp; memset(&cp, 0, sizeof cp);
            if (joyGetDevCapsA(i, &cp, sizeof cp) != JOYERR_NOERROR) continue;
            JOYINFOEX tj; memset(&tj, 0, sizeof tj); tj.dwSize = sizeof tj; tj.dwFlags = JOY_RETURNALL;
            if (joyGetPosEx(i, &tj) != JOYERR_NOERROR) continue;
            uya_log("gamepad: joystick %u: %04X:%04X \"%s\", %u axes, %u buttons%s", i, cp.wMid, cp.wPid, cp.szPname, cp.wNumAxes, cp.wNumButtons,
                    cp.wMid == 0x054C ? " (a PlayStation controller: used)" : "");
            if (cp.wMid == 0x054C) { jm_id = (int)i; break; }
        }
        if (jm_id < 0) return 0;
    }
    memset(ji, 0, sizeof *ji); ji->dwSize = sizeof *ji; ji->dwFlags = JOY_RETURNALL;
    if (joyGetPosEx((UINT)jm_id, ji) != JOYERR_NOERROR) { uya_log("gamepad: joystick %d gone", jm_id); jm_id = -1; jm_next = t + 2000000; return 0; }
    return 1;
}
static uint8_t jm_axis(DWORD v) { return ds_axis((uint8_t)(v >> 8)); }   /* 0..65535 -> 0..255, 0x80 centre */
static unsigned jm_buttons(const JOYINFOEX *ji)
{
    static const unsigned map[14] = { 0x8000, 0x4000, 0x2000, 0x1000, 0x0400, 0x0800, 0x0100, 0x0200, 0x0001, 0x0008, 0x0002, 0x0004, 0, 0 };
    unsigned b = 0;
    for (int i = 0; i < 14; i++) if (ji->dwButtons & (1u << i)) b |= map[i];
    DWORD pov = ji->dwPOV;
    if (pov != 0xFFFF && pov <= 36000) {
        int o = (int)((pov + 2250) / 4500) % 8;     /* 0 up, 1 up-right, ... clockwise */
        static const unsigned dir[8] = { 0x10, 0x30, 0x20, 0x60, 0x40, 0xC0, 0x80, 0x90 };
        b |= dir[o];
    }
    return b;
}

int plat_pad_sticks(uint8_t st[4])
{
    ds_start();
    if (ds_ok) {
        st[0] = ds_axis(ds_sticks[0]); st[1] = ds_axis(ds_sticks[1]);
        st[2] = ds_axis(ds_sticks[2]); st[3] = ds_axis(ds_sticks[3]);
        return 1;
    }
    xi_state_t s;
    if (xi_read(&s)) {
        st[0] = stick(s.Gamepad.sThumbLX, 0); st[1] = stick(s.Gamepad.sThumbLY, 1);   /* PS2: down is +Y */
        st[2] = stick(s.Gamepad.sThumbRX, 0); st[3] = stick(s.Gamepad.sThumbRY, 1);
        return 1;
    }
    JOYINFOEX ji;
    if (jm_read(&ji)) {
        st[0] = jm_axis(ji.dwXpos); st[1] = jm_axis(ji.dwYpos);
        st[2] = jm_axis(ji.dwZpos); st[3] = jm_axis(ji.dwRpos);
        return 1;
    }
    return 0;
}

/* testing: UYA_KEYS="start@600+10,cross@900" presses buttons from vsync 600 for 10 vsyncs (as on Linux) */
static unsigned test_keys(void)
{
    static const struct { const char *name; unsigned bit; } nm[] = {
        {"select", 0x0001}, {"start", 0x0008}, {"up", 0x0010}, {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080},
        {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400}, {"r1", 0x0800},
        {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000}, {"square", 0x8000},
        {"lup", 0x10000}, {"ldown", 0x20000}, {"lleft", 0x40000}, {"lright", 0x80000},
    };
    static const char *keys; static int init;
    if (!init) { init = 1; keys = getenv("UYA_KEYS"); }
    const char *s = keys;
    unsigned now = uya_frame_count(), bits = 0;
    while (s && *s) {
        char name[16]; unsigned at = 0, dur = 6; int k = 0;
        while (*s && *s != '@' && *s != ',' && k < 15) name[k++] = *s++;
        name[k] = 0;
        if (*s == '@') at = (unsigned)strtoul(s + 1, (char **)&s, 10);
        if (*s == '+') dur = (unsigned)strtoul(s + 1, (char **)&s, 10);
        if (now >= at && now < at + dur)
            for (unsigned i = 0; i < sizeof nm / sizeof nm[0]; i++) if (!strcmp(name, nm[i].name)) bits |= nm[i].bit;
        while (*s && *s != ',') s++;
        if (*s == ',') s++;
    }
    return bits;
}

unsigned plat_pad_buttons(void)
{
    xi_state_t s;
    JOYINFOEX ji;
    unsigned b = test_keys();
    ds_start();
    if (ds_ok) b |= ds_buttons;
    else if (xi_read(&s)) b |= xi_buttons(&s.Gamepad);
    else if (jm_read(&ji)) b |= jm_buttons(&ji);
    return b;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE: win_closed = 1; DestroyWindow(h); return 0;
    case WM_KEYDOWN:
        if (w == VK_F9 && !(l & (1 << 30))) { void uya_gs_rec_hotkey(void); uya_gs_rec_hotkey(); return 0; }   /* testing: record the GS input */
        return 0;
    case WM_ERASEBKGND:
        if (plat_gl_active) return 1;   /* GL draws the whole window */
        break;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
}

static void win_create(void)
{
    if (win) return;
    WNDCLASSA wc = {0};
    RECT r = {0, 0, 640, 480};
    wc.style = CS_OWNDC;                       /* (OpenGL keeps the window's DC) */
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "UYAWrapper";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    win = CreateWindowA("UYAWrapper", "Project Solana", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
}

/* ---- OpenGL (the GS's GPU renderer, host/src/gs/gs_gl.c) ----
 * A 3.3 core context on the window through WGL; opengl32.dll is loaded at
 * run time, so a machine without it just keeps the software renderer. */
int plat_gl_active;
static HMODULE gl_lib;
static HDC gl_dc;
static HGLRC gl_rc;
static HGLRC (WINAPI *p_wglCreateContext)(HDC);
static BOOL (WINAPI *p_wglMakeCurrent)(HDC, HGLRC);
static BOOL (WINAPI *p_wglDeleteContext)(HGLRC);
static PROC (WINAPI *p_wglGetProcAddress)(LPCSTR);
void *plat_gl_proc(const char *name)
{
    void *p = p_wglGetProcAddress ? (void *)p_wglGetProcAddress(name) : 0;
    if (p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) p = 0;
    if (!p && gl_lib) p = (void *)GetProcAddress(gl_lib, name);   /* the GL 1.1 functions */
    return p;
}
int plat_gl_init(void)
{
    win_create();
    if (!win) return 0;
    gl_lib = LoadLibraryA("opengl32.dll");
    if (!gl_lib) { uya_log("GL: no opengl32.dll"); return 0; }
    p_wglCreateContext = (HGLRC (WINAPI *)(HDC))GetProcAddress(gl_lib, "wglCreateContext");
    p_wglMakeCurrent = (BOOL (WINAPI *)(HDC, HGLRC))GetProcAddress(gl_lib, "wglMakeCurrent");
    p_wglDeleteContext = (BOOL (WINAPI *)(HGLRC))GetProcAddress(gl_lib, "wglDeleteContext");
    p_wglGetProcAddress = (PROC (WINAPI *)(LPCSTR))GetProcAddress(gl_lib, "wglGetProcAddress");
    if (!p_wglCreateContext || !p_wglMakeCurrent || !p_wglDeleteContext || !p_wglGetProcAddress) return 0;
    gl_dc = GetDC(win);
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(gl_dc, &pfd);
    if (!pf || !SetPixelFormat(gl_dc, pf, &pfd)) { uya_log("GL: no pixel format"); return 0; }
    HGLRC tmp = p_wglCreateContext(gl_dc);
    if (!tmp || !p_wglMakeCurrent(gl_dc, tmp)) { uya_log("GL: wglCreateContext failed"); return 0; }
    HGLRC (WINAPI *create_attribs)(HDC, HGLRC, const int *) = (HGLRC (WINAPI *)(HDC, HGLRC, const int *))plat_gl_proc("wglCreateContextAttribsARB");
    if (!create_attribs) { uya_log("GL: no wglCreateContextAttribsARB (OpenGL 3.3 needed)"); p_wglMakeCurrent(0, 0); p_wglDeleteContext(tmp); return 0; }
    const int attr[] = { 0x2091, 3, 0x2092, 3, 0x9126, 1, 0 };   /* 3.3 core */
    gl_rc = create_attribs(gl_dc, 0, attr);
    p_wglMakeCurrent(0, 0);
    p_wglDeleteContext(tmp);
    if (!gl_rc || !p_wglMakeCurrent(gl_dc, gl_rc)) { uya_log("GL: no OpenGL 3.3 core context"); return 0; }
    {   /* UYA_VSYNC=1: swaps wait for the monitor (the game paces itself at 50 Hz anyway) */
        BOOL (WINAPI *swap_interval)(int) = (BOOL (WINAPI *)(int))plat_gl_proc("wglSwapIntervalEXT");
        const char *e = getenv("UYA_VSYNC");
        if (swap_interval) swap_interval(e ? atoi(e) : 0);
    }
    return 1;
}
void plat_gl_current(int on) { if (p_wglMakeCurrent) p_wglMakeCurrent(on ? gl_dc : 0, on ? gl_rc : 0); }
void plat_gl_swap(void) { SwapBuffers(gl_dc); }
void plat_gl_size(int *w, int *h)
{
    RECT rc;
    if (win && GetClientRect(win, &rc)) { *w = rc.right; *h = rc.bottom; }
    else { *w = 640; *h = 480; }
}

int plat_present(const uint32_t *px, int w, int h)
{
    if (win_closed) return 0;
    win_create();
    pump();
    if (win_closed) return 0;
    if (plat_gl_active) {   /* the GS thread draws the window; here only the frame rate in the title */
        extern unsigned long long nsends;
        static uint64_t t0; static unsigned long long s0; static unsigned n0;
        uint64_t t = plat_time_us();
        n0++;
        if (!t0) { t0 = t; s0 = nsends; }
        else if (t - t0 >= 1000000) {
            double sec = (double)(t - t0) / 1e6;
            char title[64];
            snprintf(title, sizeof title, "Project Solana - %.0f FPS", (double)(nsends - s0) / sec);
            SetWindowTextA(win, title);
            t0 = t; s0 = nsends; n0 = 0;
        }
        return 1;
    }
    {
        BITMAPINFO bi = {0};
        RECT rc;
        HDC dc = GetDC(win);
        GetClientRect(win, &rc);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;            /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        {
            /* keep 4:3, black bars around */
            int cw = rc.right, ch = rc.bottom, vw = cw, vh = cw * 3 / 4;
            if (vh > ch) { vh = ch; vw = ch * 4 / 3; }
            SetStretchBltMode(dc, COLORONCOLOR);   /* HALFTONE filters nicer but costs milliseconds a frame */
            if (px) StretchDIBits(dc, (cw - vw) / 2, (ch - vh) / 2, vw, vh, 0, 0, w, h, px, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {   /* debug: frame rate in the corner and the title (game frames drawn, and vsyncs: 50 = full speed) */
            extern unsigned long long nsends;
            static uint64_t t0; static unsigned long long s0; static unsigned n0;
            static char text[96] = "";
            uint64_t t = plat_time_us();
            n0++;
            if (!t0) { t0 = t; s0 = nsends; }
            else if (t - t0 >= 1000000) {
                double sec = (double)(t - t0) / 1e6;
                snprintf(text, sizeof text, "%.1f fps  (%.1f vsyncs/s, full speed 50)", (double)(nsends - s0) / sec, n0 / sec);
                char title[64];
                snprintf(title, sizeof title, "Project Solana - %.0f FPS", (double)(nsends - s0) / sec);
                SetWindowTextA(win, title);
                t0 = t; s0 = nsends; n0 = 0;
            }
            if (text[0]) {
                SetBkMode(dc, OPAQUE); SetBkColor(dc, RGB(0, 0, 0)); SetTextColor(dc, RGB(255, 255, 0));
                TextOutA(dc, 4, 4, text, (int)strlen(text));
            }
        }
        ReleaseDC(win, dc);
    }
    return 1;
}

/* a movie starts: forget key presses from before it (the Enter that chose it) */
/* movies: a fresh press of Cross or Start on the controller skips (buttons already held when the movie starts do not) */
static unsigned skip_held;
void plat_movie_started(void) { pump(); skip_held = plat_pad_buttons() & 0x4008; }
int plat_key_skip(void)
{
    pump();
    unsigned b = plat_pad_buttons() & 0x4008, fresh = b & ~skip_held;
    skip_held = b;
    return fresh != 0 || (getenv("UYA_SKIPMOVIES") && getenv("UYA_KEYS"));   /* (testing: scripted runs skip movies) */
}

void plat_poll(void)
{
    if (!win) return;
    pump();
    if (win_closed) { uya_log("window closed"); exit(0); }
}

/* ---- sound: waveOut, one buffer per call ---- */
static HWAVEOUT wo;
static WAVEHDR wh;
static int16_t *wbuf;

void plat_audio_stop(void)
{
    if (!wo) return;
    waveOutReset(wo);
    waveOutUnprepareHeader(wo, &wh, sizeof wh);
    waveOutClose(wo);
    wo = 0;
    free(wbuf);
    wbuf = 0;
}

void plat_audio_play(const int16_t *lr, size_t frames, int rate, const char *tag)
{
    WAVEFORMATEX fmt = {0};
    (void)tag;
    plat_audio_stop();
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = (DWORD)rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = (DWORD)rate * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { wo = 0; uya_log("sound: no output device"); return; }
    wbuf = malloc(frames * 4);
    if (!wbuf) { waveOutClose(wo); wo = 0; return; }
    memcpy(wbuf, lr, frames * 4);
    memset(&wh, 0, sizeof wh);
    wh.lpData = (LPSTR)wbuf;
    wh.dwBufferLength = (DWORD)(frames * 4);
    waveOutPrepareHeader(wo, &wh, sizeof wh);
    waveOutWrite(wo, &wh, sizeof wh);
}

/* ---- sound effects stream: waveOut with a ring of buffers ---- */
#define SBUFS 32
#define SBUF_FRAMES 1024
static HWAVEOUT so;
static WAVEHDR sh[SBUFS];
static int16_t sbuf[SBUFS][SBUF_FRAMES * 2];
static int snext, sopen_failed;

static void stream_open(void)
{
    WAVEFORMATEX fmt = {0};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = 48000;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = 48000 * 4;
    if (waveOutOpen(&so, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { so = 0; sopen_failed = 1; return; }
    for (int i = 0; i < SBUFS; i++) {
        memset(&sh[i], 0, sizeof sh[i]);
        sh[i].lpData = (LPSTR)sbuf[i];
        sh[i].dwBufferLength = SBUF_FRAMES * 4;
        waveOutPrepareHeader(so, &sh[i], sizeof sh[i]);
        sh[i].dwFlags |= WHDR_DONE;
    }
}

int plat_stream_queued(void)
{
    int n = 0;
    if (!so) return sopen_failed ? 1 << 20 : 0;
    for (int i = 0; i < SBUFS; i++) if (!(sh[i].dwFlags & WHDR_DONE)) n += (int)(sh[i].dwBufferLength / 4);
    return n;
}

void plat_stream_write(const int16_t *lr, int frames)
{
    if (!so && !sopen_failed) stream_open();
    if (!so) return;
    while (frames > 0) {
        WAVEHDR *h = &sh[snext];
        if (!(h->dwFlags & WHDR_DONE)) return;             /* ring full: drop */
        int n = frames > SBUF_FRAMES ? SBUF_FRAMES : frames;
        memcpy(sbuf[snext], lr, (size_t)n * 4);
        h->dwBufferLength = (DWORD)n * 4;
        h->dwFlags &= ~WHDR_DONE;
        waveOutWrite(so, h, sizeof *h);
        snext = (snext + 1) % SBUFS;
        lr += n * 2;
        frames -= n;
    }
}

/* entry the launcher calls */
__declspec(dllexport) int uya_main_win32(void)
{
    int argc = 0;
    LPWSTR *wv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = calloc(argc + 1, sizeof *argv);
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, 0, 0, 0, 0);
        argv[i] = malloc(n);
        WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, argv[i], n, 0, 0);
    }
    return uya_main(argc, argv);
}
#endif
