/* rac1_wrapper.exe: reserves PS2 RAM through its own image, then runs rac1_game.dll.
 *
 * Built with no C runtime and image base 0x10000. Its .ps2ram section (see
 * ps2ram_win32.s) is uninitialized and runs past 0x02000000, so Windows maps it
 * with the image, before it places the main stack and the process heaps.
 * The few Win32 functions it needs are declared here, so it needs no SDK headers.
 *
 * Delivery in parts: with every level built in, rac1_game.dll is larger than the
 * file transfer allows, so it can arrive as rac1_game.dll.part00, .part01, ... plus
 * rac1_game.dll.manifest ("<parts> <total bytes>"). On start, if the manifest is
 * there, the parts are joined into rac1_game.dll (through a temporary file, the size
 * checked against the manifest) and the parts and manifest are deleted.
 * No globals here: they would land in .bss next to the PS2 RAM. */
typedef void *HMODULE;
typedef void *HANDLE;
__declspec(dllimport) HMODULE __stdcall LoadLibraryA(const char *);
__declspec(dllimport) void *__stdcall GetProcAddress(HMODULE, const char *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) int __stdcall MessageBoxA(void *, const char *, const char *, unsigned);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char *, unsigned, unsigned, void *, unsigned, unsigned, HANDLE);
__declspec(dllimport) int __stdcall ReadFile(HANDLE, void *, unsigned, unsigned *, void *);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void *, unsigned, unsigned *, void *);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE);
__declspec(dllimport) int __stdcall DeleteFileA(const char *);
__declspec(dllimport) int __stdcall MoveFileExA(const char *, const char *, unsigned);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *, unsigned long, unsigned, unsigned);
__declspec(dllimport) int __stdcall VirtualFree(void *, unsigned long, unsigned);
__declspec(dllimport) unsigned __stdcall GetModuleFileNameA(HMODULE, char *, unsigned);

#define INVALID ((HANDLE)(long)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define OPEN_EXISTING 3u
#define CREATE_ALWAYS 2u

typedef int (*uya_main_fn)(void);

/* dir (the exe's folder, with its trailing backslash) + name, plus a two-digit number when i >= 0 */
static void path(char *out, const char *dir, const char *name, int i)
{
    int k = 0;
    while (*dir) out[k++] = *dir++;
    while (*name) out[k++] = *name++;
    if (i >= 0) { out[k++] = (char)('0' + i / 10); out[k++] = (char)('0' + i % 10); }
    out[k] = 0;
}

static void fail(const char *m) { MessageBoxA(0, m, "RAC1", 0x10); ExitProcess(1); }

/* rac1_game.dll from its parts, when a manifest says they are there */
static void join_parts(void)
{
    char dir[600], man[700], tmp[700], dll[700], nm[700];
    unsigned dl = GetModuleFileNameA(0, dir, 520);
    while (dl > 0 && dir[dl - 1] != '\\' && dir[dl - 1] != '/') dl--;
    dir[dl] = 0;
    path(man, dir, "rac1_game.dll.manifest", -1); path(tmp, dir, "rac1_game.dll.tmp", -1); path(dll, dir, "rac1_game.dll", -1);
    HANDLE m = CreateFileA(man, GENERIC_READ, 1, 0, OPEN_EXISTING, 0x80, 0);
    if (m == INVALID) return;
    char txt[64]; unsigned got = 0;
    ReadFile(m, txt, sizeof txt - 1, &got, 0);
    CloseHandle(m);
    txt[got] = 0;
    unsigned parts = 0, k = 0; unsigned long long total = 0;
    while (txt[k] >= '0' && txt[k] <= '9') parts = parts * 10 + (unsigned)(txt[k++] - '0');
    while (txt[k] == ' ') k++;
    while (txt[k] >= '0' && txt[k] <= '9') total = total * 10 + (unsigned)(txt[k++] - '0');
    if (!parts || parts > 99 || !total) fail("rac1_game.dll.manifest is not valid.");
    unsigned char *buf = VirtualAlloc(0, 1u << 22, 0x3000, 4);
    if (!buf) fail("Out of memory joining rac1_game.dll.");
    HANDLE o = CreateFileA(tmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0x80, 0);
    if (o == INVALID) fail("Could not write rac1_game.dll.tmp next to rac1_wrapper.exe.");
    unsigned long long done = 0;
    for (unsigned i = 0; i < parts; i++) {
        path(nm, dir, "rac1_game.dll.part", (int)i);
        HANDLE p = CreateFileA(nm, GENERIC_READ, 1, 0, OPEN_EXISTING, 0x80, 0);
        if (p == INVALID) { CloseHandle(o); DeleteFileA(tmp); fail("A part of rac1_game.dll (rac1_game.dll.partNN) is missing."); }
        for (;;) {
            unsigned n = 0, w = 0;
            if (!ReadFile(p, buf, 1u << 22, &n, 0) || !n) break;
            if (!WriteFile(o, buf, n, &w, 0) || w != n) { CloseHandle(p); CloseHandle(o); fail("Writing rac1_game.dll.tmp failed (disk full?)."); }
            done += n;
        }
        CloseHandle(p);
    }
    CloseHandle(o);
    VirtualFree(buf, 0, 0x8000);
    if (done != total) { DeleteFileA(tmp); fail("The parts of rac1_game.dll do not add up to its size; copy them again."); }
    if (!MoveFileExA(tmp, dll, 1 /* REPLACE_EXISTING */)) fail("Could not replace rac1_game.dll (is the game still running?).");
    for (unsigned i = 0; i < parts; i++) { path(nm, dir, "rac1_game.dll.part", (int)i); DeleteFileA(nm); }
    DeleteFileA(man);
}

void __stdcall launcher_start(void)
{
    join_parts();
    HMODULE h = LoadLibraryA("rac1_game.dll");
    if (!h) {
        MessageBoxA(0, "rac1_game.dll was not found next to rac1_wrapper.exe.", "UYA Wrapper", 0x10);
        ExitProcess(1);
    }
    uya_main_fn f = (uya_main_fn)GetProcAddress(h, "uya_main_win32");
    ExitProcess(f ? (unsigned)f() : 1u);
}
