/* Project Solana launcher: a game library for the static recompilations.
 *
 * Add disc images (Add disc, Scan folder, or drag and drop). Each one is identified by
 * the boot executable its SYSTEM.CNF names (BOOT2) and that executable's MD5, then
 * matched against the games this build knows. A supported disc gets a Play button that
 * starts that game's wrapper (rac1_wrapper.exe - <disc>) in its own folder with the
 * settings below as environment variables. The library and settings are kept in
 * solana.ini next to this program.
 *
 * Plain Win32 + GDI, everything custom-drawn (double-buffered), DPI-aware.
 * Build: tools/build_frontend.py (zig cc, x86-windows-gnu). */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- the games this build knows ---------------------------------------------------- */
typedef struct {
    const char *serial;          /* BOOT2's file name, as in the disc's root */
    const wchar_t *title, *region;
    const char *md5;             /* the executable this recompilation was made from (0: any) */
    const wchar_t *dir;          /* the game's folder, relative to this program's */
    const wchar_t *exe;          /* its wrapper in that folder (0: not supported yet) */
    const wchar_t *kit;          /* its GitHub release tag and folder under games\ (the Build button) */
    const wchar_t *const *levels;
} game_t;

static const wchar_t *const rac1_levels[] = {
    L"Veldin", L"Novalis", L"Aridia", L"Kerwan", L"Eudora", L"Rilgar", L"Blarg Station", L"Umbris",
    L"Batalia", L"Gaspar", L"Orxon", L"Pokitaru", L"Hoven", L"Gemlik Base", L"Oltanis", L"Quartu",
    L"Kalebo III", L"Drek's Fleet", L"Veldin (Drek)", 0 };

static const game_t games[] = {
    { "SCES_509.16", L"Ratchet & Clank", L"PAL", "d86429e2eca4e1da7d7f1cc656af2574", L"games\\rac1\\build\\windows", L"rac1_wrapper.exe", L"rac1", rac1_levels },
    { "SCUS_971.99", L"Ratchet & Clank", L"NTSC-U", 0, 0, 0, 0, 0 },
    { "SCPS_150.37", L"Ratchet & Clank", L"NTSC-J", 0, 0, 0, 0, 0 },
    { "SCES_516.07", L"Ratchet & Clank 2", L"PAL", 0, 0, 0, 0, 0 },
    { "SCUS_972.68", L"Ratchet & Clank: Going Commando", L"NTSC-U", 0, 0, 0, 0, 0 },
    { "SCES_524.56", L"Ratchet & Clank 3", L"PAL", 0, 0, 0, 0, 0 },
    { "SCUS_973.53", L"Ratchet & Clank: Up Your Arsenal", L"NTSC-U", 0, 0, 0, 0, 0 },
    { "SCES_532.85", L"Ratchet: Gladiator", L"PAL", 0, 0, 0, 0, 0 },
    { "SCUS_974.65", L"Ratchet: Deadlocked", L"NTSC-U", 0, 0, 0, 0, 0 },
};
#define NGAMES (int)(sizeof games / sizeof games[0])

/* ---- the library ------------------------------------------------------------------- */
enum { ST_OK, ST_REVISION, ST_NOTYET, ST_UNKNOWN, ST_NOTDISC, ST_MISSING };
typedef struct {
    wchar_t path[MAX_PATH];
    char serial[32], md5[33];
    int game;      /* index into games[], or -1 */
    int status;
} disc_t;
#define MAXDISCS 64
static disc_t discs[MAXDISCS];
static int ndiscs, sel = -1;

/* settings */
static int set_upscale = 1, set_vsync, set_nomtgs, set_noglt, set_fpslog, set_capsec = 3, set_level = -1, set_minimize = 1;

static wchar_t base_dir[MAX_PATH], ini_path[MAX_PATH];
static HWND hwnd;
static float dpi = 1.0f;
#define S(x) ((int)((x) * dpi + 0.5f))

/* ---- MD5 (RFC 1321) ----------------------------------------------------------------- */
typedef struct { unsigned a, b, c, d; unsigned long long n; unsigned char buf[64]; } md5_t;
static const unsigned md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
static const unsigned char md5_r[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
static void md5_block(md5_t *m, const unsigned char *p)
{
    unsigned w[16], a = m->a, b = m->b, c = m->c, d = m->d;
    for (int i = 0; i < 16; i++) w[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | (unsigned)p[i * 4 + 3] << 24;
    for (int i = 0; i < 64; i++) {
        unsigned f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        unsigned t = d; d = c; c = b;
        unsigned x = a + f + md5_k[i] + w[g];
        b = b + ((x << md5_r[i]) | (x >> (32 - md5_r[i])));
        a = t;
    }
    m->a += a; m->b += b; m->c += c; m->d += d;
}
static void md5_init(md5_t *m) { m->a = 0x67452301; m->b = 0xefcdab89; m->c = 0x98badcfe; m->d = 0x10325476; m->n = 0; }
static void md5_add(md5_t *m, const unsigned char *p, size_t n)
{
    while (n--) { m->buf[m->n++ & 63] = *p++; if (!(m->n & 63)) md5_block(m, m->buf); }
}
static void md5_hex(md5_t *m, char out[33])
{
    unsigned long long bits = m->n * 8;
    unsigned char one = 0x80, zero = 0, len[8];
    md5_add(m, &one, 1);
    while ((m->n & 63) != 56) md5_add(m, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (8 * i));
    md5_add(m, len, 8);
    unsigned v[4] = { m->a, m->b, m->c, m->d };
    for (int i = 0; i < 16; i++) sprintf(out + 2 * i, "%02x", (v[i / 4] >> (8 * (i & 3))) & 255);
    out[32] = 0;
}

/* ---- disc images (ISO 9660, 2048-byte sectors) -------------------------------------- */
static int read_at(HANDLE f, unsigned long long off, void *buf, DWORD n)
{
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(f, li, 0, FILE_BEGIN) && ReadFile(f, buf, n, &got, 0) && got == n;
}
/* a file in the root directory: its first sector and size */
static int iso_find(HANDLE f, const unsigned char *root, unsigned rootn, const char *name, unsigned *lba, unsigned *size)
{
    size_t nl = strlen(name);
    for (unsigned o = 0; o < rootn;) {
        unsigned len = root[o];
        if (!len) { o = (o / 2048 + 1) * 2048; continue; }
        unsigned fl = root[o + 32];
        const char *fn = (const char *)root + o + 33;
        if (fl >= nl && !_strnicmp(fn, name, nl) && (fl == nl || fn[nl] == ';')) {
            memcpy(lba, root + o + 2, 4); memcpy(size, root + o + 10, 4);
            return 1;
        }
        o += len;
    }
    (void)f;
    return 0;
}
static void identify(disc_t *d)
{
    d->game = -1; d->serial[0] = d->md5[0] = 0;
    HANDLE f = CreateFileW(d->path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
    if (f == INVALID_HANDLE_VALUE) { d->status = ST_MISSING; return; }
    unsigned char pvd[2048];
    d->status = ST_NOTDISC;
    if (!read_at(f, 16 * 2048ull, pvd, 2048) || memcmp(pvd + 1, "CD001", 5)) { CloseHandle(f); return; }
    unsigned rlba, rsize;
    memcpy(&rlba, pvd + 156 + 2, 4); memcpy(&rsize, pvd + 156 + 10, 4);
    if (rsize > (1u << 20)) rsize = 1u << 20;
    unsigned char *root = malloc(rsize);
    if (!root || !read_at(f, rlba * 2048ull, root, rsize)) { free(root); CloseHandle(f); return; }
    unsigned lba, size;
    if (iso_find(f, root, rsize, "SYSTEM.CNF", &lba, &size) && size < 4096) {
        char cnf[4096] = { 0 };
        if (read_at(f, lba * 2048ull, cnf, size)) {
            char *b = strstr(cnf, "BOOT2");
            if (b && (b = strchr(b, '\\'))) {
                int k = 0;
                for (b++; *b && *b != ';' && *b != '\r' && *b != '\n' && k < 31; b++) d->serial[k++] = *b;
                d->serial[k] = 0;
            }
        }
    }
    d->status = ST_UNKNOWN;
    for (int i = 0; i < NGAMES; i++) if (!_stricmp(games[i].serial, d->serial)) d->game = i;
    if (d->game >= 0 && iso_find(f, root, rsize, d->serial, &lba, &size) && size < (64u << 20)) {
        unsigned char *exe = malloc(size);
        if (exe && read_at(f, lba * 2048ull, exe, size)) { md5_t m; md5_init(&m); md5_add(&m, exe, size); md5_hex(&m, d->md5); }
        free(exe);
    }
    free(root);
    CloseHandle(f);
    if (d->game < 0) return;
    const game_t *g = &games[d->game];
    if (!g->exe) d->status = ST_NOTYET;
    else if (g->md5 && strcmp(g->md5, d->md5)) d->status = ST_REVISION;
    else d->status = ST_OK;
}

/* ---- game folders --------------------------------------------------------------------- */
static void game_dir(const game_t *g, wchar_t *out)
{
    /* next to this program, in its games\\<kit> folder (built by the Build button), or in
     * build\\windows (where development builds put it) */
    const wchar_t *tries[3] = { L"", g->dir, L"build\\windows" };
    for (int i = 0; i < 3; i++) {
        wchar_t d[MAX_PATH], p[MAX_PATH];
        if (tries[i][0]) swprintf(d, MAX_PATH, L"%ls\\%ls", base_dir, tries[i]); else wcscpy(d, base_dir);
        swprintf(p, MAX_PATH, L"%ls\\%ls", d, g->exe);
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) { wcscpy(out, d); return; }
    }
    swprintf(out, MAX_PATH, L"%ls\\%ls", base_dir, g->dir);
}
static int game_installed(const game_t *g)
{
    wchar_t d[MAX_PATH], p[MAX_PATH];
    if (!g->exe) return 0;
    game_dir(g, d);
    swprintf(p, MAX_PATH, L"%ls\\%ls", d, g->exe);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return 0;
    swprintf(p, MAX_PATH, L"%ls\\rac1_game.dll", d);
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return 1;
    swprintf(p, MAX_PATH, L"%ls\\rac1_game.dll.manifest", d);    /* still in parts: the wrapper joins them */
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* ---- settings and library on disk ------------------------------------------------------ */
static int ini_int(const wchar_t *sec, const wchar_t *key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, ini_path); }
static void ini_set_int(const wchar_t *sec, const wchar_t *key, int v) { wchar_t b[16]; swprintf(b, 16, L"%d", v); WritePrivateProfileStringW(sec, key, b, ini_path); }
static void save_all(void)
{
    WritePrivateProfileStringW(L"library", 0, 0, ini_path);
    ini_set_int(L"library", L"count", ndiscs);
    for (int i = 0; i < ndiscs; i++) { wchar_t k[16]; swprintf(k, 16, L"disc%d", i); WritePrivateProfileStringW(L"library", k, discs[i].path, ini_path); }
    ini_set_int(L"library", L"selected", sel);
    ini_set_int(L"settings", L"upscaled", set_upscale);
    ini_set_int(L"settings", L"vsync", set_vsync);
    ini_set_int(L"settings", L"no_gs_thread", set_nomtgs);
    ini_set_int(L"settings", L"no_gl_thread", set_noglt);
    ini_set_int(L"settings", L"fps_log", set_fpslog);
    ini_set_int(L"settings", L"capture_seconds", set_capsec);
    ini_set_int(L"settings", L"start_level", set_level);
    ini_set_int(L"settings", L"minimize", set_minimize);
}
static int add_disc(const wchar_t *path, int select)
{
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, 0)) wcscpy(full, path);
    for (int i = 0; i < ndiscs; i++) if (!_wcsicmp(discs[i].path, full)) { if (select) sel = i; return i; }
    if (ndiscs >= MAXDISCS) return -1;
    disc_t *d = &discs[ndiscs];
    memset(d, 0, sizeof *d);
    wcsncpy(d->path, full, MAX_PATH - 1);
    identify(d);
    if (select) sel = ndiscs;
    return ndiscs++;
}
static void load_all(void)
{
    int n = ini_int(L"library", L"count", 0);
    for (int i = 0; i < n && i < MAXDISCS; i++) {
        wchar_t k[16], p[MAX_PATH];
        swprintf(k, 16, L"disc%d", i);
        GetPrivateProfileStringW(L"library", k, L"", p, MAX_PATH, ini_path);
        if (p[0]) add_disc(p, 0);
    }
    sel = ini_int(L"library", L"selected", ndiscs ? 0 : -1);
    if (sel >= ndiscs) sel = ndiscs - 1;
    set_upscale = ini_int(L"settings", L"upscaled", 1);
    set_vsync = ini_int(L"settings", L"vsync", 0);
    set_nomtgs = ini_int(L"settings", L"no_gs_thread", 0);
    set_noglt = ini_int(L"settings", L"no_gl_thread", 0);
    set_fpslog = ini_int(L"settings", L"fps_log", 0);
    set_capsec = ini_int(L"settings", L"capture_seconds", 3);
    set_level = ini_int(L"settings", L"start_level", -1);
    set_minimize = ini_int(L"settings", L"minimize", 1);
    if (set_capsec < 1 || set_capsec > 10) set_capsec = 3;
}

/* ---- running a game ---------------------------------------------------------------------- */
static HANDLE game_proc;
static int running = -1;
static wchar_t message[256];          /* a line under the Play button */
static int message_bad;
#define WM_GAME_EXIT (WM_APP + 1)

static DWORD WINAPI watch(LPVOID p)
{
    HANDLE h = (HANDLE)p; DWORD code = 0;
    WaitForSingleObject(h, INFINITE);
    GetExitCodeProcess(h, &code);
    PostMessageW(hwnd, WM_GAME_EXIT, code, 0);
    return 0;
}
static void env(const wchar_t *k, const wchar_t *v) { SetEnvironmentVariableW(k, v); }
/* Upscaled: the smallest whole multiple of the PS2's 448 lines that covers the height of
 * the monitor this window is on (1080p: 3x, 1440p and 4K: 4x), at least 2x */
static int screen_h(void)
{
    MONITORINFO mi; mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return mi.rcMonitor.bottom - mi.rcMonitor.top;
    return GetSystemMetrics(SM_CYSCREEN);
}
static int upscale_factor(void)
{
    int f = (screen_h() + 447) / 448;
    return f < 2 ? 2 : f > 4 ? 4 : f;
}
static void play(int i)
{
    if (i < 0 || running >= 0) return;
    disc_t *d = &discs[i];
    identify(d);
    if (d->status != ST_OK && d->status != ST_REVISION) return;
    const game_t *g = &games[d->game];
    wchar_t dir[MAX_PATH], exe[MAX_PATH], cmd[2 * MAX_PATH + 64];
    game_dir(g, dir);
    swprintf(exe, MAX_PATH, L"%ls\\%ls", dir, g->exe);
    if (!game_installed(g)) {
        swprintf(message, 256, L"The game files are not in %ls", dir); message_bad = 1;
        InvalidateRect(hwnd, 0, 0); return;
    }
    wchar_t b[32];
    env(L"UYA_RENDERER", L"gl");
    swprintf(b, 32, L"%d", set_upscale ? upscale_factor() : 1); env(L"UYA_SCALE", b);
    env(L"UYA_VSYNC", set_vsync ? L"1" : 0);
    env(L"UYA_MTGS", set_nomtgs ? L"0" : 0);
    env(L"UYA_GLTHREAD", set_noglt ? L"0" : 0);
    env(L"UYA_FPSLOG", set_fpslog ? L"1" : 0);
    swprintf(b, 32, L"%d", set_capsec * 50); env(L"UYA_GSREC_N", b);
    if (set_level >= 0) { swprintf(b, 32, L"%d@1", set_level); env(L"UYA_LEVEL", b); } else env(L"UYA_LEVEL", 0);
    swprintf(cmd, sizeof cmd / sizeof cmd[0], L"\"%ls\" - \"%ls\"", exe, d->path);
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, 0, 0, FALSE, 0, 0, dir, &si, &pi)) {
        swprintf(message, 256, L"Could not start %ls (error %lu)", g->exe, GetLastError()); message_bad = 1;
        InvalidateRect(hwnd, 0, 0); return;
    }
    CloseHandle(pi.hThread);
    game_proc = pi.hProcess; running = i;
    message[0] = 0; message_bad = 0;
    CloseHandle(CreateThread(0, 0, watch, pi.hProcess, 0, 0));
    if (set_minimize) ShowWindow(hwnd, SW_MINIMIZE);
    InvalidateRect(hwnd, 0, 0);
}
#include "solana_build.h"

static void open_log(int i)
{
    if (i < 0 || discs[i].game < 0 || !games[discs[i].game].exe) return;
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    const game_t *g = &games[discs[i].game];
    game_dir(g, dir);
    swprintf(p, MAX_PATH, L"%ls\\rac1_log.txt", dir);
    if (!game_installed(g) || (bld.failed && bld.disc == i)) swprintf(p, MAX_PATH, L"%ls\\games\\%ls\\build_log.txt", base_dir, g->kit);   /* (the build's) */
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { wcscpy(message, L"No log yet: it is written when the game runs."); message_bad = 0; InvalidateRect(hwnd, 0, 0); return; }
    ShellExecuteW(hwnd, L"open", p, 0, 0, SW_SHOWNORMAL);
}
static void open_folder(int i)
{
    if (i < 0 || discs[i].game < 0 || !games[discs[i].game].exe) return;
    wchar_t dir[MAX_PATH];
    game_dir(&games[discs[i].game], dir);
    ShellExecuteW(hwnd, L"open", dir, 0, 0, SW_SHOWNORMAL);
}

/* ---- adding discs ---------------------------------------------------------------------------- */
static void pick_files(void)
{
    static wchar_t buf[32768];
    OPENFILENAMEW of; memset(&of, 0, sizeof of);
    buf[0] = 0;
    of.lStructSize = sizeof of; of.hwndOwner = hwnd;
    of.lpstrFilter = L"PS2 disc images (*.iso)\0*.iso\0All files\0*.*\0";
    of.lpstrFile = buf; of.nMaxFile = 32768;
    of.lpstrTitle = L"Add disc images";
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return;
    wchar_t *p = buf + wcslen(buf) + 1;
    if (!*p) add_disc(buf, 1);
    else for (; *p; p += wcslen(p) + 1) { wchar_t f[MAX_PATH]; swprintf(f, MAX_PATH, L"%ls\\%ls", buf, p); add_disc(f, 1); }
    save_all();
}
static int scan_dir(const wchar_t *dir, int depth)
{
    wchar_t pat[MAX_PATH]; WIN32_FIND_DATAW fd; int n = 0;
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.cFileName[0] == L'.') continue;
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) n += scan_dir(p, depth + 1); }
        else { size_t l = wcslen(fd.cFileName); if (l > 4 && !_wcsicmp(fd.cFileName + l - 4, L".iso")) { int before = ndiscs; add_disc(p, 0); n += ndiscs > before; } }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
static void pick_folder(void)
{
    BROWSEINFOW bi; memset(&bi, 0, sizeof bi);
    bi.hwndOwner = hwnd; bi.lpszTitle = L"Find disc images (.iso) in this folder and its subfolders";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t dir[MAX_PATH];
    if (SHGetPathFromIDListW(id, dir)) {
        HCURSOR c = SetCursor(LoadCursor(0, IDC_WAIT));
        int n = scan_dir(dir, 0);
        SetCursor(c);
        swprintf(message, 256, n == 1 ? L"Found 1 new disc image." : L"Found %d new disc images.", n); message_bad = 0;
        if (n && sel < 0) sel = 0;
        save_all();
    }
    CoTaskMemFree(id);
    InvalidateRect(hwnd, 0, 0);
}

/* ---- drawing ----------------------------------------------------------------------------------- */
/* the palette (assets/palette.json, "Horizon") */
#define C_BG        RGB(0x0A, 0x0E, 0x1A)
#define C_SIDE      RGB(0x0F, 0x14, 0x24)
#define C_CARD      RGB(0x15, 0x1C, 0x30)
#define C_CARDHI    RGB(0x1C, 0x25, 0x40)
#define C_SELECT    RGB(0x1C, 0x25, 0x40)
#define C_LINE      RGB(0x26, 0x30, 0x4D)
#define C_TEXT      RGB(0xF4, 0xF6, 0xFB)
#define C_MUTED     RGB(0x9A, 0xA6, 0xC6)
#define C_DIM       RGB(0x5E, 0x68, 0x88)
#define C_ACCENT    RGB(0xFF, 0x7A, 0x3D)
#define C_ACCENTHI  RGB(0xFF, 0x94, 0x61)
#define C_ONACCENT  RGB(0x0A, 0x0E, 0x1A)
#define C_CYAN      RGB(0x7F, 0xD1, 0xFF)
#define C_OK        RGB(0x3D, 0xD6, 0x8C)
#define C_WARN      RGB(0xFF, 0xB2, 0x38)
#define C_BAD       RGB(0xFF, 0x5A, 0x5F)
#include "solana_img.h"

enum { W_NONE, W_ADD, W_SCAN, W_ITEM, W_PLAY, W_SETTINGS, W_LOG, W_FOLDER, W_REMOVE, W_BACK,
       W_RENDER, W_TOGGLE, W_CAPDOWN, W_CAPUP, W_LVLDOWN, W_LVLUP, W_CANCEL, W_REBUILD };
typedef struct { RECT r; int id, arg; } widget_t;
static widget_t wid[96];
static int nwid, hover_id = W_NONE, hover_arg, press_id = W_NONE, press_arg;
static int view_settings, scroll;
static HFONT f_title, f_head, f_body, f_bold, f_small, f_logo, f_btn;

static void add_w(RECT r, int id, int arg) { if (nwid < 96) { wid[nwid].r = r; wid[nwid].id = id; wid[nwid].arg = arg; nwid++; } }
static int is_hover(int id, int arg) { return hover_id == id && hover_arg == arg; }
static HFONT mkfont(int px, int weight) { return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI"); }
static void make_fonts(void)
{
    HFONT *all[] = { &f_title, &f_head, &f_body, &f_bold, &f_small, &f_logo, &f_btn };
    for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) if (*all[i]) DeleteObject(*all[i]);
    f_title = mkfont(30, FW_SEMIBOLD); f_head = mkfont(15, FW_SEMIBOLD); f_body = mkfont(14, FW_NORMAL);
    f_bold = mkfont(15, FW_SEMIBOLD); f_small = mkfont(12, FW_NORMAL); f_logo = mkfont(17, FW_BOLD); f_btn = mkfont(14, FW_SEMIBOLD);
}
static void fill(HDC dc, RECT r, COLORREF c) { SetDCBrushColor(dc, c); FillRect(dc, &r, GetStockObject(DC_BRUSH)); }
static void rrect(HDC dc, RECT r, int rad, COLORREF fillc, COLORREF edge)
{
    SelectObject(dc, GetStockObject(DC_BRUSH)); SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fillc); SetDCPenColor(dc, edge);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(rad), S(rad));
}
static void text(HDC dc, const wchar_t *s, RECT r, HFONT f, COLORREF c, UINT fmt)
{
    SelectObject(dc, f); SetTextColor(dc, c); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
}
static int text_w(HDC dc, const wchar_t *s, HFONT f) { SIZE z; SelectObject(dc, f); GetTextExtentPoint32W(dc, s, (int)wcslen(s), &z); return z.cx; }
static RECT rc(int l, int t, int r, int b) { RECT x = { l, t, r, b }; return x; }

static void status_text(const disc_t *d, wchar_t *out, COLORREF *c)
{
    switch (d->status) {
    case ST_OK:       if (bld.active && bld.disc == (int)(d - discs)) { wcscpy(out, L"Building\u2026"); *c = C_ACCENT; }
                      else if (games[d->game].exe && !game_installed(&games[d->game])) { wcscpy(out, games[d->game].kit ? L"Ready to build" : L"Game files missing"); *c = games[d->game].kit ? C_ACCENT : C_BAD; }
                      else { wcscpy(out, L"Ready to play"); *c = C_OK; } break;
    case ST_REVISION: wcscpy(out, L"Different disc revision"); *c = C_WARN; break;
    case ST_NOTYET:   wcscpy(out, L"Not supported yet"); *c = C_DIM; break;
    case ST_UNKNOWN:  wcscpy(out, L"Unknown PS2 disc"); *c = C_DIM; break;
    case ST_MISSING:  wcscpy(out, L"Disc image not found"); *c = C_BAD; break;
    default:          wcscpy(out, L"Not a PS2 disc image"); *c = C_BAD; break;
    }
}
static void disc_title(const disc_t *d, wchar_t *out)
{
    if (d->game >= 0) { wcscpy(out, games[d->game].title); return; }
    const wchar_t *n = wcsrchr(d->path, L'\\'); n = n ? n + 1 : d->path;
    wcsncpy(out, n, 127); out[127] = 0;
}

/* a button: label, and an icon (assets/png/icons, white, tinted here) left of it */
static void button_i(HDC dc, RECT r, const wchar_t *label, int id, int arg, int primary, int enabled, int icon)
{
    COLORREF bg = primary ? C_ACCENT : C_CARD, fg = primary ? C_ONACCENT : C_TEXT, edge = primary ? C_ACCENT : C_LINE;
    if (!enabled) { bg = primary ? RGB(0x5A, 0x3A, 0x2C) : C_CARD; fg = primary ? RGB(0x9A, 0x7E, 0x70) : C_DIM; edge = primary ? bg : C_LINE; }
    else if (is_hover(id, arg)) { bg = primary ? C_ACCENTHI : C_CARDHI; edge = primary ? bg : RGB(0x34, 0x40, 0x66); }
    if (enabled && press_id == id && press_arg == arg && is_hover(id, arg)) OffsetRect(&r, 0, S(1));
    rrect(dc, r, 6, bg, edge);
    HFONT f = primary ? f_bold : f_btn;
    int isz = S(18), gap = S(8), tw = text_w(dc, label, f);
    int total = tw + (icon ? isz + gap : 0), x = (r.left + r.right - total) / 2;
    if (icon) { img_icon(dc, icon, x, (r.top + r.bottom - isz) / 2, isz, fg); x += isz + gap; }
    text(dc, label, rc(x, r.top, x + tw + S(2), r.bottom), f, fg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (enabled) add_w(r, id, arg);
}
static void button(HDC dc, RECT r, const wchar_t *label, int id, int arg, int primary, int enabled) { button_i(dc, r, label, id, arg, primary, enabled, 0); }
static void toggle(HDC dc, int x, int y, int on, int arg)
{
    RECT r = rc(x, y, x + S(40), y + S(22));
    rrect(dc, r, 22, on ? C_ACCENT : C_LINE, on ? C_ACCENT : C_LINE);
    int kx = on ? r.right - S(19) : r.left + S(3);
    RECT k = rc(kx, y + S(3), kx + S(16), y + S(19));
    rrect(dc, k, 16, on ? C_ONACCENT : C_TEXT, on ? C_ONACCENT : C_TEXT);
}
/* one row of the settings list: title, explanation, and the control area on the right */
static int setting_row(HDC dc, RECT area, int y, const wchar_t *title, const wchar_t *help)
{
    RECT t = rc(area.left, y, area.right - S(220), y + S(22));
    text(dc, title, t, f_bold, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    RECT h = rc(area.left, y + S(22), area.right - S(220), y + S(58));
    text(dc, help, h, f_small, C_MUTED, DT_LEFT | DT_WORDBREAK);
    return y + S(66);
}
static void draw_settings(HDC dc, RECT area)
{
    int y = area.top;
    text(dc, L"Settings", rc(area.left, y, area.right, y + S(40)), f_title, C_TEXT, DT_LEFT | DT_SINGLELINE);
    button(dc, rc(area.right - S(90), y + S(4), area.right, y + S(36)), L"Done", W_BACK, 0, 0, 1);
    y += S(52);
    text(dc, L"Apply the next time a game starts.", rc(area.left, y, area.right, y + S(20)), f_body, C_MUTED, DT_LEFT | DT_SINGLELINE);
    y += S(34);
    int cx = area.right - S(200);

    int y0 = y;
    {   wchar_t help[200];
        swprintf(help, 200, L"Native draws at the PS2's own resolution, as the game was. Upscaled draws at %dx for this %d-line screen: sharper, more GPU work.", upscale_factor(), screen_h());
        y = setting_row(dc, area, y, L"Resolution", help);
    }
    {   const wchar_t *opt[2] = { L"Native", L"Upscaled" };
        RECT box = rc(cx, y0 + S(4), area.right, y0 + S(36));
        rrect(dc, box, 6, C_CARD, C_LINE);
        for (int k = 0; k < 2; k++) {
            RECT s = rc(box.left + S(3) + k * (S(197) / 2), box.top + S(3), box.left + S(3) + (k + 1) * (S(197) / 2) - S(3), box.bottom - S(3));
            int on = (k == 1) == (set_upscale != 0);
            if (on) rrect(dc, s, 6, C_ACCENT, C_ACCENT); else if (is_hover(W_RENDER, k)) rrect(dc, s, 6, C_CARDHI, C_CARDHI);
            text(dc, opt[k], s, f_btn, on ? C_ONACCENT : C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            add_w(s, W_RENDER, k);
        }
    }
    struct { const wchar_t *t, *h; int *v; int invert; } rows[] = {
        { L"VSync", L"Waits for the monitor's refresh. Off lets the game pace itself at 50 fps.", &set_vsync, 0 },
        { L"Graphics on its own thread", L"Runs the PS2 graphics chip beside the game. Turn off only to test a problem.", &set_nomtgs, 1 },
        { L"OpenGL calls on their own thread", L"Sends drawing to the driver from a separate thread. Turn off only to test a problem.", &set_noglt, 1 },
        { L"Performance statistics", L"Writes frame timings to rac1_log.txt every 2 seconds, for bug reports.", &set_fpslog, 0 },
        { L"Minimize while playing", L"Tucks this window away when a game starts.", &set_minimize, 0 },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        int ry = y;
        y = setting_row(dc, area, y, rows[i].t, rows[i].h);
        int on = rows[i].invert ? !*rows[i].v : *rows[i].v;
        RECT hit = rc(area.right - S(48), ry + S(4), area.right, ry + S(30));
        toggle(dc, area.right - S(44), ry + S(4), on, (int)i);
        add_w(hit, W_TOGGLE, (int)i);
    }
    /* steppers */
    {   int ry = y; wchar_t b[64];
        y = setting_row(dc, area, y, L"F9 capture length", L"F9 in the game records this much graphics input to gsrec_NN.bin, for glitch reports (about 260 MB a second).");
        swprintf(b, 64, L"%d second%ls", set_capsec, set_capsec == 1 ? L"" : L"s");
        RECT box = rc(cx, ry + S(4), area.right, ry + S(36));
        rrect(dc, box, 6, C_CARD, C_LINE);
        text(dc, b, box, f_btn, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT l = rc(box.left + S(3), box.top + S(3), box.left + S(35), box.bottom - S(3)), r = rc(box.right - S(35), box.top + S(3), box.right - S(3), box.bottom - S(3));
        if (is_hover(W_CAPDOWN, 0)) rrect(dc, l, 6, C_CARDHI, C_CARDHI);
        if (is_hover(W_CAPUP, 0)) rrect(dc, r, 6, C_CARDHI, C_CARDHI);
        text(dc, L"−", l, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"+", r, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_w(l, W_CAPDOWN, 0); add_w(r, W_CAPUP, 0);
    }
    {   int ry = y; wchar_t b[64];
        y = setting_row(dc, area, y, L"Start in a level (testing)", L"Skips the menus and loads this level with a fresh save. Off for normal play.");
        if (set_level < 0) wcscpy(b, L"Off"); else swprintf(b, 64, L"%02d %ls", set_level, rac1_levels[set_level]);
        RECT box = rc(cx, ry + S(4), area.right, ry + S(36));
        rrect(dc, box, 6, set_level >= 0 ? C_SELECT : C_CARD, set_level >= 0 ? C_ACCENT : C_LINE);
        RECT mid = rc(box.left + S(36), box.top, box.right - S(36), box.bottom);
        text(dc, b, mid, f_btn, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT l = rc(box.left + S(3), box.top + S(3), box.left + S(35), box.bottom - S(3)), r = rc(box.right - S(35), box.top + S(3), box.right - S(3), box.bottom - S(3));
        if (is_hover(W_LVLDOWN, 0)) rrect(dc, l, 6, C_CARDHI, C_CARDHI);
        if (is_hover(W_LVLUP, 0)) rrect(dc, r, 6, C_CARDHI, C_CARDHI);
        text(dc, L"‹", l, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        text(dc, L"›", r, f_bold, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        add_w(l, W_LVLDOWN, 0); add_w(r, W_LVLUP, 0);
    }
    {   /* building the selected game again (a newer build kit, or after a problem) */
        int ry = y;
        int ok = sel >= 0 && discs[sel].game >= 0 && games[discs[sel].game].kit && (discs[sel].status == ST_OK || discs[sel].status == ST_REVISION) && !bld.active && running < 0;
        y = setting_row(dc, area, y, L"Rebuild the game", L"Downloads the latest build kit and builds the selected game from your disc again. Saves are kept.");
        button_i(dc, rc(cx, ry + S(4), area.right, ry + S(36)), L"Rebuild", W_REBUILD, 0, 0, ok, IMG_I_REBUILD);
    }
}

static void info_row(HDC dc, int x, int *y, int w, const wchar_t *k, const wchar_t *v, COLORREF vc)
{
    text(dc, k, rc(x, *y, x + S(130), *y + S(22)), f_body, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    text(dc, v, rc(x + S(130), *y, x + w, *y + S(22)), f_body, vc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
    *y += S(30);
}
static void draw_details(HDC dc, RECT area)
{
    if (sel < 0 || sel >= ndiscs) {
        int cy = (area.top + area.bottom) / 2 + S(70);
        img_draw(dc, IMG_EMPTY, (area.left + area.right) / 2 - S(160), cy - S(290), S(320), S(200));
        text(dc, L"Your library is empty", rc(area.left, cy - S(60), area.right, cy - S(24)), f_title, C_TEXT, DT_CENTER | DT_SINGLELINE);
        text(dc, L"Add your own copy of the game as a disc image (.iso), or drop one on this window.", rc(area.left, cy - S(14), area.right, cy + S(10)), f_body, C_MUTED, DT_CENTER | DT_SINGLELINE);
        int bw = S(176);
        button_i(dc, rc((area.left + area.right) / 2 - bw - S(6), cy + S(30), (area.left + area.right) / 2 - S(6), cy + S(70)), L"Add disc image", W_ADD, 1, 1, 1, IMG_I_ADD);
        button_i(dc, rc((area.left + area.right) / 2 + S(6), cy + S(30), (area.left + area.right) / 2 + bw + S(6), cy + S(70)), L"Scan a folder", W_SCAN, 1, 0, 1, IMG_I_SCAN);
        return;
    }
    disc_t *d = &discs[sel];
    const game_t *g = d->game >= 0 ? &games[d->game] : 0;
    wchar_t t[160], st[64], b[MAX_PATH + 64]; COLORREF sc;
    disc_title(d, t); status_text(d, st, &sc);
    int y = area.top, w = area.right - area.left;

    /* banner */
    RECT ban = rc(area.left, y, area.right, y + S(150));
    {   /* the Horizon banner (assets/png/default_banner.png): filling the area, anchored right, under rounded corners */
        HRGN rg = CreateRoundRectRgn(ban.left, ban.top, ban.right + 1, ban.bottom + 1, S(10), S(10));
        SelectClipRgn(dc, rg);
        fill(dc, ban, C_BG);
        img_cover(dc, IMG_BANNER, ban, 1.0);
        SelectClipRgn(dc, 0); DeleteObject(rg);
    }
    text(dc, t, rc(ban.left + S(24), ban.top + S(30), ban.right - S(200), ban.top + S(74)), f_title, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    {   int x = ban.left + S(24), by = ban.top + S(84);
        if (g) {
            int tw = text_w(dc, g->region, f_small) + S(16);
            RECT chip = rc(x, by, x + tw, by + S(22));
            rrect(dc, chip, 22, C_CARD, C_LINE);
            text(dc, g->region, chip, f_small, C_TEXT, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            x += tw + S(10);
        }
        SelectObject(dc, GetStockObject(DC_BRUSH)); SelectObject(dc, GetStockObject(NULL_PEN));
        SetDCBrushColor(dc, sc); Ellipse(dc, x, by + S(7), x + S(9), by + S(16));
        text(dc, st, rc(x + S(15), by, ban.right - S(200), by + S(22)), f_body, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y = ban.bottom + S(24);

    /* actions */
    int supported = (d->status == ST_OK || d->status == ST_REVISION) && g && g->exe;
    int installed = supported && game_installed(g);
    int building = bld.active && bld.disc == sel;
    if (building) {   /* the build's progress in place of the buttons */
        RECT pc = rc(area.left, y, area.right, y + S(48));
        int bw = (area.right - S(110)) - area.left;
        RECT track = rc(area.left, y + S(30), area.left + bw - S(12), y + S(38));
        rrect(dc, track, 8, C_CARD, C_LINE);
        int fw = (int)((track.right - track.left) * (bld.frac < 0 ? 0 : bld.frac > 1 ? 1 : bld.frac));
        if (fw > S(8)) rrect(dc, rc(track.left, track.top, track.left + fw, track.bottom), 8, C_ACCENT, C_ACCENT);
        wchar_t pcnt[16]; swprintf(pcnt, 16, L"%d%%", (int)(bld.frac * 100 + 0.5));
        text(dc, bld.status, rc(area.left, y, area.left + bw - S(70), y + S(24)), f_bold, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        text(dc, pcnt, rc(area.left + bw - S(70), y, area.left + bw - S(12), y + S(24)), f_bold, C_MUTED, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        button_i(dc, rc(area.right - S(110), y + S(4), area.right, y + S(44)), bld.cancel ? L"Stopping\u2026" : L"Cancel", W_CANCEL, 0, 0, !bld.cancel, IMG_I_CANCEL);
        (void)pc;
    } else {
        int can = supported && running < 0 && (installed || (g->kit && !bld.active));
        const wchar_t *pl = running == sel ? L"Running\u2026" : (supported && !installed && g->kit) ? L"Build" : L"Play";
        button_i(dc, rc(area.left, y, area.left + S(170), y + S(48)), pl, W_PLAY, 0, 1, can, (supported && !installed && g->kit) ? IMG_I_BUILD : IMG_I_PLAY);
        int bx = area.left + S(182);
        int has_files = g && g->exe;
        button_i(dc, rc(bx, y + S(4), bx + S(128), y + S(44)), L"Settings", W_SETTINGS, 0, 0, 1, IMG_I_SETTINGS); bx += S(140);
        button_i(dc, rc(bx, y + S(4), bx + S(128), y + S(44)), L"Open log", W_LOG, 0, 0, has_files, IMG_I_LOG); bx += S(140);
        button_i(dc, rc(bx, y + S(4), bx + S(150), y + S(44)), L"Game folder", W_FOLDER, 0, 0, has_files, IMG_I_FOLDER);
        button_i(dc, rc(area.right - S(120), y + S(4), area.right, y + S(44)), L"Remove", W_REMOVE, 0, 0, running != sel && !(bld.active && bld.disc == sel), IMG_I_REMOVE);
    }
    y += S(60);
    if (bld.failed && bld.disc == sel && !building) text(dc, bld.err, rc(area.left, y, area.right, y + S(22)), f_body, C_BAD, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    else if (message[0]) { text(dc, message, rc(area.left, y, area.right, y + S(22)), f_body, message_bad ? C_BAD : C_MUTED, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS); }
    else if (building) text(dc, L"Building from your disc. You can keep using your PC; closing this window stops the build.", rc(area.left, y, area.right, y + S(22)), f_body, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    else if (supported && !installed && g->kit) text(dc, L"First time: downloads about 95 MB of tools and source, then builds the game from your disc (10 to 30 minutes).", rc(area.left, y, area.right, y + S(22)), f_body, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    else if (d->status == ST_REVISION) text(dc, L"This disc's executable differs from the one this build was made from. It may not run correctly.", rc(area.left, y, area.right, y + S(22)), f_body, C_WARN, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    else if (d->status == ST_NOTYET) text(dc, L"This game is recognised, but its recompilation is not built yet.", rc(area.left, y, area.right, y + S(22)), f_body, C_MUTED, DT_LEFT | DT_SINGLELINE);
    y += S(36);

    /* the disc */
    RECT card = rc(area.left, y, area.right, y + S(190));
    rrect(dc, card, 10, C_CARD, C_LINE);
    int x = card.left + S(20), iy = card.top + S(16), iw = w - S(40);
    text(dc, L"Disc", rc(x, iy, x + iw, iy + S(22)), f_head, C_TEXT, DT_LEFT | DT_SINGLELINE); iy += S(32);
    info_row(dc, x, &iy, iw, L"Image", d->path, C_TEXT);
    MultiByteToWideChar(CP_ACP, 0, d->serial[0] ? d->serial : "none", -1, b, 64);
    info_row(dc, x, &iy, iw, L"Boot executable", b, C_TEXT);
    if (d->md5[0]) {
        wchar_t m[64]; MultiByteToWideChar(CP_ACP, 0, d->md5, -1, m, 64);
        swprintf(b, MAX_PATH + 64, L"%ls  %ls", m, d->status == ST_OK ? L"(matches)" : d->status == ST_REVISION ? L"(different revision)" : L"");
        info_row(dc, x, &iy, iw, L"MD5", b, d->status == ST_REVISION ? C_WARN : C_TEXT);
    } else info_row(dc, x, &iy, iw, L"MD5", L"–", C_DIM);
    if (g && g->exe) {
        wchar_t gd[MAX_PATH]; game_dir(g, gd);
        if (game_installed(g)) info_row(dc, x, &iy, iw, L"Game files", gd, C_TEXT);
        else info_row(dc, x, &iy, iw, L"Game files", g->kit ? L"Not built yet" : L"Missing", g->kit ? C_MUTED : C_BAD);
    }
    else info_row(dc, x, &iy, iw, L"Game files", L"–", C_DIM);
    y = card.bottom + S(12);

    /* the controller: the PS2's buttons and the same buttons on an Xbox pad (assets/png/controller_colour) */
    if (g && g->exe && y + S(232) < area.bottom) {
        RECT c2 = rc(area.left, y, area.right, y + S(232));
        rrect(dc, c2, 10, C_CARD, C_LINE);
        x = c2.left + S(20); iy = c2.top + S(16);
        text(dc, L"Controller", rc(x, iy, x + S(200), iy + S(22)), f_head, C_TEXT, DT_LEFT | DT_SINGLELINE);
        text(dc, L"Xbox, DualShock 4 and DualSense, over USB or Bluetooth", rc(x + S(110), iy, x + iw, iy + S(22)), f_small, C_MUTED, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        iy += S(36);
        static const struct { int ps, xb; const wchar_t *pn, *xn; } rows[10] = {
            { IMG_PS_CROSS, IMG_XB_A, L"Cross", L"A" }, { IMG_PS_CIRCLE, IMG_XB_B, L"Circle", L"B" }, { IMG_PS_SQUARE, IMG_XB_X, L"Square", L"X" },
            { IMG_PS_TRIANGLE, IMG_XB_Y, L"Triangle", L"Y" }, { IMG_PS_START, IMG_XB_MENU, L"Start", L"Menu" },
            { IMG_PS_L1, IMG_XB_LB, L"L1", L"LB" }, { IMG_PS_R1, IMG_XB_RB, L"R1", L"RB" }, { IMG_PS_L2, IMG_XB_LT, L"L2", L"LT" },
            { IMG_PS_R2, IMG_XB_RT, L"R2", L"RT" }, { IMG_PS_SELECT, IMG_XB_VIEW, L"Select", L"View" } };
        int col = iw / 2, gs = S(24), rh = S(30);
        for (int i = 0; i < 10; i++) {
            int cx0 = x + (i / 5) * col, ry = iy + (i % 5) * rh;
            img_draw(dc, rows[i].ps, cx0, ry, gs, gs);
            text(dc, rows[i].pn, rc(cx0 + gs + S(10), ry, cx0 + gs + S(100), ry + gs), f_body, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            text(dc, L"\u2192", rc(cx0 + S(120), ry, cx0 + S(144), ry + gs), f_body, C_DIM, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            img_draw(dc, rows[i].xb, cx0 + S(154), ry, gs, gs);
            text(dc, rows[i].xn, rc(cx0 + S(154) + gs + S(10), ry, cx0 + col - S(8), ry + gs), f_bold, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    }
}

static void paint(HDC dc, RECT cr)
{
    nwid = 0;
    {   /* the starfield tile (512 x 512 at 3x: 171 px at 100%) behind the page */
        HBRUSH tb = img_tile_brush(IMG_BGTILE, S(171));
        if (tb) { SetBrushOrgEx(dc, 0, 0, 0); FillRect(dc, &cr, tb); } else fill(dc, cr, C_BG);
    }
    int sw = S(300), hh = S(64);
    /* sidebar */
    RECT side = rc(0, 0, sw, cr.bottom);
    fill(dc, side, C_SIDE);
    fill(dc, rc(sw - 1, 0, sw, cr.bottom), C_LINE);
    /* the wordmark (assets/png/wordmark.png, 260 x 40 at 100%) */
    if (!img_draw(dc, IMG_WORDMARK, S(20), S(14), S(260), S(40)))
        text(dc, L"PROJECT SOLANA", rc(S(20), S(18), sw - S(10), S(46)), f_logo, C_TEXT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text(dc, L"LIBRARY", rc(S(20), hh + S(6), sw - S(20), hh + S(26)), f_small, C_DIM, DT_LEFT | DT_SINGLELINE);
    int ly = hh + S(32) - scroll, ih = S(68);
    HRGN clip = CreateRectRgn(0, hh + S(30), sw - 1, cr.bottom - S(116));
    SelectClipRgn(dc, clip);
    for (int i = 0; i < ndiscs; i++, ly += ih + S(6)) {
        RECT it = rc(S(12), ly, sw - S(12), ly + ih);
        if (it.bottom < hh || it.top > cr.bottom) continue;
        disc_t *d = &discs[i];
        COLORREF bg = i == sel ? C_SELECT : is_hover(W_ITEM, i) ? C_CARD : C_SIDE;
        rrect(dc, it, 10, bg, i == sel ? C_LINE : bg);
        if (i == sel) fill(dc, rc(it.left, it.top + S(14), it.left + S(3), it.bottom - S(14)), C_ACCENT);
        wchar_t t[160], st[64], sub[128]; COLORREF sc;
        disc_title(d, t); status_text(d, st, &sc);
        text(dc, t, rc(it.left + S(16), it.top + S(10), it.right - S(12), it.top + S(32)), f_bold, C_TEXT, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (d->game >= 0) swprintf(sub, 128, L"%ls", games[d->game].region); else sub[0] = 0;
        int x = it.left + S(16);
        if (sub[0]) { text(dc, sub, rc(x, it.top + S(36), it.right, it.top + S(56)), f_small, C_MUTED, DT_LEFT | DT_SINGLELINE); x += text_w(dc, sub, f_small) + S(10); }
        SelectObject(dc, GetStockObject(DC_BRUSH)); SelectObject(dc, GetStockObject(NULL_PEN));
        SetDCBrushColor(dc, sc); Ellipse(dc, x, it.top + S(42), x + S(7), it.top + S(49));
        text(dc, st, rc(x + S(12), it.top + S(36), it.right - S(12), it.top + S(56)), f_small, C_MUTED, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        add_w(it, W_ITEM, i);
    }
    SelectClipRgn(dc, 0); DeleteObject(clip);
    if (!ndiscs) text(dc, L"No games yet", rc(S(20), hh + S(36), sw - S(20), hh + S(60)), f_body, C_DIM, DT_LEFT | DT_SINGLELINE);
    fill(dc, rc(0, cr.bottom - S(116), sw - 1, cr.bottom - S(115)), C_LINE);
    button_i(dc, rc(S(16), cr.bottom - S(100), sw - S(16), cr.bottom - S(60)), L"Add disc image", W_ADD, 0, 0, 1, IMG_I_ADD);
    button_i(dc, rc(S(16), cr.bottom - S(52), sw - S(16), cr.bottom - S(16)), L"Scan a folder", W_SCAN, 0, 0, 1, IMG_I_SCAN);

    RECT area = rc(sw + S(24), S(24), cr.right - S(24), cr.bottom - S(24));
    if (view_settings) draw_settings(dc, area); else draw_details(dc, area);
}

/* ---- input ---------------------------------------------------------------------------------- */
static widget_t *hit(int x, int y)
{
    POINT p = { x, y };
    for (int i = nwid - 1; i >= 0; i--) if (PtInRect(&wid[i].r, p)) return &wid[i];
    return 0;
}
static void click(int id, int arg)
{
    switch (id) {
    case W_ADD: pick_files(); view_settings = 0; break;
    case W_SCAN: pick_folder(); view_settings = 0; break;
    case W_ITEM: sel = arg; view_settings = 0; message[0] = 0; identify(&discs[sel]); save_all(); break;
    case W_PLAY:
        if (sel >= 0 && discs[sel].game >= 0 && games[discs[sel].game].exe && !game_installed(&games[discs[sel].game])) build_start(sel);
        else play(sel);
        break;
    case W_CANCEL: bld.cancel = 1; if (bld.job) TerminateJobObject(bld.job, 1); break;
    case W_REBUILD: if (sel >= 0 && running < 0) { view_settings = 0; build_start(sel); } break;
    case W_SETTINGS: view_settings = 1; break;
    case W_BACK: view_settings = 0; save_all(); break;
    case W_LOG: open_log(sel); break;
    case W_FOLDER: open_folder(sel); break;
    case W_REMOVE:
        if (sel >= 0 && running != sel) {
            memmove(&discs[sel], &discs[sel + 1], (size_t)(ndiscs - sel - 1) * sizeof discs[0]);
            ndiscs--;
            if (running > sel) running--;
            if (sel >= ndiscs) sel = ndiscs - 1;
            message[0] = 0; save_all();
        }
        break;
    case W_RENDER: set_upscale = arg; save_all(); break;
    case W_TOGGLE: {
        int *v[] = { &set_vsync, &set_nomtgs, &set_noglt, &set_fpslog, &set_minimize };
        *v[arg] = !*v[arg]; save_all(); break; }
    case W_CAPDOWN: if (set_capsec > 1) set_capsec--; save_all(); break;
    case W_CAPUP: if (set_capsec < 10) set_capsec++; save_all(); break;
    case W_LVLDOWN: set_level = set_level < 0 ? 18 : set_level - 1; save_all(); break;
    case W_LVLUP: set_level = set_level >= 18 ? -1 : set_level + 1; save_all(); break;
    }
    InvalidateRect(hwnd, 0, 0);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ old = SelectObject(mem, bm);
        paint(mem, cr);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        widget_t *x = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        int id = x ? x->id : W_NONE, arg = x ? x->arg : 0;
        if (id != hover_id || arg != hover_arg) { hover_id = id; hover_arg = arg; InvalidateRect(h, 0, 0); }
        SetCursor(LoadCursor(0, id != W_NONE ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0; }
    case WM_MOUSELEAVE: hover_id = W_NONE; InvalidateRect(h, 0, 0); return 0;
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return 1; break;
    case WM_LBUTTONDOWN: {
        widget_t *x = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        press_id = x ? x->id : W_NONE; press_arg = x ? x->arg : 0;
        SetCapture(h); InvalidateRect(h, 0, 0);
        return 0; }
    case WM_LBUTTONUP: {
        widget_t *x = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        int id = press_id, arg = press_arg;
        press_id = W_NONE; ReleaseCapture();
        if (x && x->id == id && x->arg == arg) click(id, arg); else InvalidateRect(h, 0, 0);
        return 0; }
    case WM_LBUTTONDBLCLK: {
        widget_t *x = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (x && x->id == W_ITEM) { sel = x->arg; if (discs[sel].game >= 0 && games[discs[sel].game].exe && game_installed(&games[discs[sel].game])) play(sel); else InvalidateRect(h, 0, 0); }
        return 0; }
    case WM_MOUSEWHEEL: {
        RECT cr; GetClientRect(h, &cr);
        int total = ndiscs * (S(68) + S(6)), view = cr.bottom - S(116) - S(96);
        scroll -= GET_WHEEL_DELTA_WPARAM(w) / 120 * S(60);
        if (scroll > total - view) scroll = total - view;
        if (scroll < 0) scroll = 0;
        InvalidateRect(h, 0, 0);
        return 0; }
    case WM_KEYDOWN:
        if (w == VK_RETURN && sel >= 0 && discs[sel].game >= 0 && games[discs[sel].game].exe && game_installed(&games[discs[sel].game])) play(sel);
        else if ((w == VK_DOWN || w == VK_UP) && ndiscs) { sel += w == VK_DOWN ? 1 : -1; if (sel < 0) sel = 0; if (sel >= ndiscs) sel = ndiscs - 1; view_settings = 0; message[0] = 0; save_all(); InvalidateRect(h, 0, 0); }
        else if (w == VK_ESCAPE && view_settings) { view_settings = 0; InvalidateRect(h, 0, 0); }
        return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)w; wchar_t p[MAX_PATH];
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, 0, 0);
        for (UINT i = 0; i < n; i++) if (DragQueryFileW(d, i, p, MAX_PATH)) {
            if (GetFileAttributesW(p) & FILE_ATTRIBUTE_DIRECTORY) scan_dir(p, 0); else add_disc(p, 1);
        }
        DragFinish(d);
        view_settings = 0; save_all(); InvalidateRect(h, 0, 0);
        return 0; }
    case WM_BUILD_UPDATE:
        if (w == 1) {   /* finished */
            if (!bld.failed) { wcscpy(message, L"Built. Ready to play."); message_bad = 0; FlashWindow(h, TRUE); }
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        }
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_CLOSE:
        if (bld.active && MessageBoxW(h, L"A game is being built. Stop the build and close?", L"Project Solana", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        DestroyWindow(h);
        return 0;
    case WM_GAME_EXIT:
        if (game_proc) { CloseHandle(game_proc); game_proc = 0; }
        if (w) { swprintf(message, 256, L"The game closed with an error (code 0x%lX). Open log shows what happened.", (unsigned long)w); message_bad = 1; }
        running = -1;
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        InvalidateRect(h, 0, 0);
        return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mi = (MINMAXINFO *)l; mi->ptMinTrackSize.x = S(940); mi->ptMinTrackSize.y = S(800); return 0; }
    case WM_DPICHANGED: {
        dpi = HIWORD(w) / 96.0f; make_fonts(); img_flush();
        RECT *r = (RECT *)l; SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0; }
    case WM_SIZE: InvalidateRect(h, 0, 0); return 0;
    case WM_DESTROY: save_all(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline;
    {   /* per-monitor DPI when Windows has it (10 1703+), else system DPI */
        typedef BOOL (WINAPI *ctx_t)(HANDLE);
        ctx_t f = (ctx_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
        if (!f || !f((HANDLE)-4)) SetProcessDPIAware();
    }
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(0, base_dir, MAX_PATH);
    wchar_t *s = wcsrchr(base_dir, L'\\'); if (s) *s = 0;
    swprintf(ini_path, MAX_PATH, L"%ls\\solana.ini", base_dir);
    {   HDC dc = GetDC(0); dpi = GetDeviceCaps(dc, LOGPIXELSX) / 96.0f; ReleaseDC(0, dc); }
    make_fonts();
    load_all();

    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.style = CS_DBLCLKS; wc.lpfnWndProc = wndproc; wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.lpszClassName = L"ProjectSolanaLauncher";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Project Solana", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, S(1100), S(860), 0, 0, inst, 0);
    {   /* the window's DPI may differ from the primary monitor's */
        typedef UINT (WINAPI *gdw_t)(HWND);
        gdw_t f = (gdw_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (f) { float d2 = f(hwnd) / 96.0f; if (d2 != dpi && d2 > 0) { dpi = d2; make_fonts(); SetWindowPos(hwnd, 0, 0, 0, S(1100), S(860), SWP_NOMOVE | SWP_NOZORDER); } }
    }
    {   /* dark title bar (Windows 10 2004+ / 11) */
        HMODULE dw = LoadLibraryW(L"dwmapi.dll");
        if (dw) {
            typedef HRESULT (WINAPI *set_t)(HWND, DWORD, LPCVOID, DWORD);
            set_t f = (set_t)(void *)GetProcAddress(dw, "DwmSetWindowAttribute");
            BOOL on = TRUE; if (f) { f(hwnd, 20, &on, sizeof on); COLORREF cap = C_SIDE; f(hwnd, 35, &cap, sizeof cap); }
        }
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
