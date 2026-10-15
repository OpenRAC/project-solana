/* solana_build.h: the Build button (included once by solana.c).
 *
 * A game is built on the player's own PC from their own disc: nothing from the
 * game is downloaded. From the game's GitHub release (RELEASE_URL + the game's
 * tag, e.g. .../releases/download/rac1/) it fetches:
 *   <tag>.json            what to fetch: version, files, sizes, SHA-256s
 *   toolchain-win64.zip   Python and zig (the compiler); shared by all games,
 *                         kept in toolchain\ and fetched again only when its version changes
 *   <tag>-build.zip       the game's build kit: build.py, the recompilers, the
 *                         host engine and the decomp's C, unpacked to games\<tag>\
 * then runs build.py on the disc image and keeps only what the game needs
 * (games\<tag>\build\windows). The zips are checked against their SHA-256s and
 * unpacked by the small inflate below (no tar.exe needed).
 *
 * solana.ini [build] release_url=<url or folder> and levels=<list> override the
 * source and the levels (testing, offline installs). */
#include <winhttp.h>
#include <bcrypt.h>

#define RELEASE_URL L"https://github.com/OpenRAC/project-solana/releases/download/"
#define WM_BUILD_UPDATE (WM_APP + 2)

typedef struct {
    volatile int active, cancel, failed, disc;
    volatile double frac;              /* 0..1 for the bar */
    wchar_t status[256];               /* what it is doing */
    wchar_t err[512];                  /* why it stopped */
    HANDLE job;                        /* the build's processes (closed: they end) */
} build_t;
static build_t bld = { 0, 0, 0, -1, 0, {0}, {0}, 0 };

static void bld_post(void) { static DWORD last; DWORD t = GetTickCount(); if (t - last > 80) { last = t; PostMessageW(hwnd, WM_BUILD_UPDATE, 0, 0); } }
static void bld_status(double frac, const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(bld.status, 255, fmt, ap); bld.status[255] = 0;
    va_end(ap);
    if (frac >= 0) bld.frac = frac;
    bld_post();
}
static int bld_fail(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    _vsnwprintf(bld.err, 511, fmt, ap); bld.err[511] = 0;
    va_end(ap);
    bld.failed = 1;
    return 0;
}

/* ---- paths ---- */
static void path_join(wchar_t *out, const wchar_t *a, const wchar_t *b) { swprintf(out, MAX_PATH, L"%ls\\%ls", a, b); }
static void mkdirs(const wchar_t *dir)
{
    wchar_t p[MAX_PATH]; wcsncpy(p, dir, MAX_PATH - 1); p[MAX_PATH - 1] = 0;
    for (wchar_t *s = p + 3; *s; s++) if (*s == L'\\' || *s == L'/') { wchar_t c = *s; *s = 0; CreateDirectoryW(p, 0); *s = c; }
    CreateDirectoryW(p, 0);
}
static void rm_tree(const wchar_t *path)
{
    wchar_t pat[MAX_PATH]; WIN32_FIND_DATAW fd;
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) return;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) { SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL); DeleteFileW(path); return; }
    swprintf(pat, MAX_PATH, L"%ls\\*", path);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t c[MAX_PATH]; path_join(c, path, fd.cFileName); rm_tree(c);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

/* ---- fetching: https (WinHTTP, redirects followed) or a local folder ---- */
/* to a file (path) or to memory (*mem, *memn); progress from f0 to f1 of the bar */
static int fetch(const wchar_t *url, const wchar_t *path, char **mem, size_t *memn, double f0, double f1, const wchar_t *what)
{
    HANDLE out = INVALID_HANDLE_VALUE;
    size_t cap = 0, got = 0; char *buf = 0;
    if (path) { out = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0); if (out == INVALID_HANDLE_VALUE) return bld_fail(L"Could not write %ls.", path); }
    int ok = 0;
    if (_wcsnicmp(url, L"http", 4)) {   /* a folder */
        HANDLE in = CreateFileW(url, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (in == INVALID_HANDLE_VALUE) { bld_fail(L"%ls is not there.", url); goto end; }
        LARGE_INTEGER sz; GetFileSizeEx(in, &sz);
        static char chunk[1 << 20]; DWORD n;
        while (ReadFile(in, chunk, sizeof chunk, &n, 0) && n) {
            if (bld.cancel) { CloseHandle(in); goto end; }
            if (path) { DWORD w; WriteFile(out, chunk, n, &w, 0); }
            else { if (got + n + 1 > cap) { cap = (got + n + 1) * 2; buf = realloc(buf, cap); } memcpy(buf + got, chunk, n); }
            got += n;
            if (sz.QuadPart) bld_status(f0 + (f1 - f0) * (double)got / (double)sz.QuadPart, L"%ls (%.0f of %.0f MB)", what, got / 1048576.0, sz.QuadPart / 1048576.0);
        }
        CloseHandle(in);
        ok = 1;
        goto end;
    }
    {
        URL_COMPONENTSW uc; wchar_t host[256], upath[2048];
        memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
        uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = upath; uc.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(url, 0, 0, &uc)) { bld_fail(L"Bad address: %ls", url); goto end; }
        HINTERNET s = WinHttpOpen(L"ProjectSolana/1.0", 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY (8.1+) */, 0, 0, 0);
        if (!s) s = WinHttpOpen(L"ProjectSolana/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0);
        HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : 0;
        HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", upath, 0, 0, 0, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : 0;
        DWORD status = 0, sl = sizeof status; unsigned long long total = 0;
        if (!r || !WinHttpSendRequest(r, 0, 0, 0, 0, 0, 0) || !WinHttpReceiveResponse(r, 0)) {
            bld_fail(L"Could not download %ls (error %lu). Check your internet connection.", what, GetLastError());
        } else {
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 0, &status, &sl, 0);
            wchar_t lenb[32]; DWORD ll = sizeof lenb;
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, 0, lenb, &ll, 0)) total = _wcstoui64(lenb, 0, 10);
            if (status != 200) bld_fail(L"Could not download %ls (the server answered %lu).", what, status);
            else {
                static char chunk[1 << 16];
                for (;;) {
                    DWORD n = 0;
                    if (bld.cancel) break;
                    if (!WinHttpReadData(r, chunk, sizeof chunk, &n)) { bld_fail(L"The download of %ls stopped (error %lu).", what, GetLastError()); break; }
                    if (!n) { ok = 1; break; }
                    if (path) { DWORD w; if (!WriteFile(out, chunk, n, &w, 0) || w != n) { bld_fail(L"Could not write %ls (disk full?).", path); break; } }
                    else { if (got + n + 1 > cap) { cap = (got + n + 1) * 2; buf = realloc(buf, cap); } memcpy(buf + got, chunk, n); }
                    got += n;
                    if (total) bld_status(f0 + (f1 - f0) * (double)got / (double)total, L"%ls (%.0f of %.0f MB)", what, got / 1048576.0, total / 1048576.0);
                    else bld_status(-1, L"%ls (%.0f MB)", what, got / 1048576.0);
                }
            }
        }
        if (r) WinHttpCloseHandle(r);
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
    }
end:
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok || bld.cancel) { free(buf); if (path) DeleteFileW(path); return 0; }
    if (mem) { if (!buf) buf = malloc(1); buf[got] = 0; *mem = buf; *memn = got; } else free(buf);
    return 1;
}

/* ---- SHA-256 of a file (CNG) ---- */
static int sha256_file(const wchar_t *path, char hex[65])
{
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; unsigned char d[32]; int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, 0, 0)) return 0;
    if (!BCryptCreateHash(alg, &h, 0, 0, 0, 0, 0)) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
        if (f != INVALID_HANDLE_VALUE) {
            static unsigned char b[1 << 20]; DWORD n;
            while (ReadFile(f, b, sizeof b, &n, 0) && n) BCryptHashData(h, b, n, 0);
            CloseHandle(f);
            if (!BCryptFinishHash(h, d, 32, 0)) { for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]); hex[64] = 0; ok = 1; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* ---- unzip: stored and deflated entries (RFC 1951 inflate) ---- */
typedef struct { const unsigned char *in; size_t inn, ip; unsigned bb; int bc; unsigned char *out; size_t outn, op; int err; } inf_t;
typedef struct { short count[16], symbol[320]; } huff_t;
static int ibits(inf_t *s, int n)
{
    unsigned v = s->bb;
    while (s->bc < n) { if (s->ip >= s->inn) { s->err = 1; return 0; } v |= (unsigned)s->in[s->ip++] << s->bc; s->bc += 8; }
    s->bb = v >> n; s->bc -= n;
    return (int)(v & ((1u << n) - 1));
}
static int hdecode(inf_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= ibits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
        if (s->err) return -1;
    }
    s->err = 1; return -1;
}
static int hbuild(huff_t *h, const short *len, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return left; }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return left;
}
static int inflate_codes(inf_t *s, const huff_t *lc, const huff_t *dc)
{
    static const short lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const short dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    for (;;) {
        int sym = hdecode(s, lc);
        if (s->err || sym < 0) return 0;
        if (sym < 256) { if (s->op >= s->outn) return 0; s->out[s->op++] = (unsigned char)sym; }
        else if (sym == 256) return 1;
        else {
            sym -= 257;
            if (sym >= 29) return 0;
            int len = lbase[sym] + ibits(s, lext[sym]);
            int ds = hdecode(s, dc);
            if (s->err || ds < 0 || ds >= 30) return 0;
            size_t dist = (size_t)(dbase[ds] + ibits(s, dext[ds]));
            if (dist > s->op || s->op + (size_t)len > s->outn) return 0;
            for (int i = 0; i < len; i++, s->op++) s->out[s->op] = s->out[s->op - dist];
        }
    }
}
static int inflate_mem(const unsigned char *in, size_t inn, unsigned char *out, size_t outn)
{
    inf_t s; memset(&s, 0, sizeof s); s.in = in; s.inn = inn; s.out = out; s.outn = outn;
    static huff_t lc, dc;
    int last;
    do {
        last = ibits(&s, 1);
        int type = ibits(&s, 2);
        if (s.err) return 0;
        if (type == 0) {
            s.bb = 0; s.bc = 0;
            if (s.ip + 4 > s.inn) return 0;
            unsigned len = s.in[s.ip] | (s.in[s.ip + 1] << 8); s.ip += 4;
            if (s.ip + len > s.inn || s.op + len > s.outn) return 0;
            memcpy(s.out + s.op, s.in + s.ip, len); s.ip += len; s.op += len;
        } else if (type == 1) {
            static int fixed_done; static huff_t flc, fdc;
            if (!fixed_done) {
                short l[288]; int i;
                for (i = 0; i < 144; i++) l[i] = 8;
                for (; i < 256; i++) l[i] = 9;
                for (; i < 280; i++) l[i] = 7;
                for (; i < 288; i++) l[i] = 8;
                hbuild(&flc, l, 288);
                for (i = 0; i < 30; i++) l[i] = 5;
                hbuild(&fdc, l, 30);
                fixed_done = 1;
            }
            if (!inflate_codes(&s, &flc, &fdc)) return 0;
        } else if (type == 2) {
            static const unsigned char ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            short l[320]; int nlen = ibits(&s, 5) + 257, ndist = ibits(&s, 5) + 1, ncode = ibits(&s, 4) + 4, i;
            if (s.err || nlen > 286 || ndist > 30) return 0;
            for (i = 0; i < ncode; i++) l[ord[i]] = (short)ibits(&s, 3);
            for (; i < 19; i++) l[ord[i]] = 0;
            if (hbuild(&lc, l, 19)) return 0;
            for (i = 0; i < nlen + ndist;) {
                int sym = hdecode(&s, &lc);
                if (s.err || sym < 0) return 0;
                if (sym < 16) l[i++] = (short)sym;
                else {
                    int rep, v = 0;
                    if (sym == 16) { if (!i) return 0; v = l[i - 1]; rep = 3 + ibits(&s, 2); }
                    else if (sym == 17) rep = 3 + ibits(&s, 3);
                    else rep = 11 + ibits(&s, 7);
                    if (i + rep > nlen + ndist) return 0;
                    while (rep--) l[i++] = (short)v;
                }
            }
            if (l[256] == 0) return 0;
            if (hbuild(&lc, l, nlen) < 0 && nlen - lc.count[0] != 1) return 0;
            if (hbuild(&dc, l + nlen, ndist) < 0 && ndist - dc.count[0] != 1) return 0;
            if (!inflate_codes(&s, &lc, &dc)) return 0;
        } else return 0;
    } while (!last);
    return s.op == s.outn;
}
static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
/* every entry of the zip into dir (which it creates); progress f0..f1 */
static int unzip(const wchar_t *zip, const wchar_t *dir, double f0, double f1, const wchar_t *what)
{
    HANDLE f = CreateFileW(zip, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return bld_fail(L"Could not open %ls.", zip);
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    size_t n = (size_t)sz.QuadPart;
    unsigned char *z = malloc(n ? n : 1);
    DWORD got = 0; size_t at = 0;
    while (z && at < n && ReadFile(f, z + at, (DWORD)((n - at) > (1u << 30) ? (1u << 30) : (n - at)), &got, 0) && got) at += got;
    CloseHandle(f);
    if (!z || at != n) { free(z); return bld_fail(L"Could not read %ls.", zip); }
    size_t e = n >= 22 ? n - 22 : 0, stop = n > 65557 ? n - 65557 : 0;
    while (e > stop && rd32(z + e) != 0x06054b50) e--;
    if (rd32(z + e) != 0x06054b50) { free(z); return bld_fail(L"%ls is not a zip file.", what); }
    unsigned cnt = rd16(z + e + 10); size_t cd = rd32(z + e + 16);
    mkdirs(dir);
    int ok = 1;
    for (unsigned i = 0; i < cnt && ok; i++) {
        if (bld.cancel) { ok = 0; break; }
        if (cd + 46 > n || rd32(z + cd) != 0x02014b50) { ok = bld_fail(L"%ls is damaged.", what); break; }
        unsigned method = rd16(z + cd + 10), nl = rd16(z + cd + 28), xl = rd16(z + cd + 30), cl = rd16(z + cd + 32);
        WORD dtime = (WORD)rd16(z + cd + 12), ddate = (WORD)rd16(z + cd + 14);   /* (kept: a retry of the same kit reuses the compiled files) */
        size_t csize = rd32(z + cd + 20), usize = rd32(z + cd + 24), lo = rd32(z + cd + 42);
        char name[512]; if (nl >= sizeof name) { ok = bld_fail(L"%ls has a name too long.", what); break; }
        memcpy(name, z + cd + 46, nl); name[nl] = 0;
        cd += 46 + nl + xl + cl;
        if (strstr(name, "..") || name[0] == '/' || name[0] == '\\' || strchr(name, ':')) continue;   /* (never outside dir) */
        wchar_t wn[512], p[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 512);
        for (wchar_t *c = wn; *c; c++) if (*c == L'/') *c = L'\\';
        path_join(p, dir, wn);
        size_t wl = wcslen(p);
        if (wl && p[wl - 1] == L'\\') { p[wl - 1] = 0; mkdirs(p); continue; }
        { wchar_t d2[MAX_PATH]; wcscpy(d2, p); wchar_t *s2 = wcsrchr(d2, L'\\'); if (s2) { *s2 = 0; mkdirs(d2); } }
        if (lo + 30 > n || rd32(z + lo) != 0x04034b50) { ok = bld_fail(L"%ls is damaged.", what); break; }
        size_t data = lo + 30 + rd16(z + lo + 26) + rd16(z + lo + 28);
        if (data + csize > n) { ok = bld_fail(L"%ls is damaged.", what); break; }
        unsigned char *u = method == 0 ? z + data : malloc(usize ? usize : 1);
        if (method == 8 && (!u || !inflate_mem(z + data, csize, u, usize))) { free(u); ok = bld_fail(L"%ls is damaged (%hs).", what, name); break; }
        if (method != 0 && method != 8) { ok = bld_fail(L"%ls uses an unknown compression.", what); break; }
        HANDLE o = CreateFileW(p, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
        if (o == INVALID_HANDLE_VALUE) { if (method == 8) free(u); ok = bld_fail(L"Could not write %ls.", p); break; }
        size_t w0 = 0; DWORD w;
        while (w0 < usize && WriteFile(o, u + w0, (DWORD)((usize - w0) > (1u << 30) ? (1u << 30) : (usize - w0)), &w, 0) && w) w0 += w;
        { FILETIME lt, ft; if (DosDateTimeToFileTime(ddate, dtime, &lt) && LocalFileTimeToFileTime(&lt, &ft)) SetFileTime(o, 0, 0, &ft); }
        CloseHandle(o);
        if (method == 8) free(u);
        if (w0 != usize) { ok = bld_fail(L"Could not write %ls (disk full?).", p); break; }
        if (i % 16 == 0) bld_status(f0 + (f1 - f0) * (double)(i + 1) / cnt, L"Unpacking %ls (%u of %u files)", what, i + 1, cnt);
    }
    free(z);
    return ok && !bld.cancel;
}

/* ---- the release's description: a flat JSON object ---- */
static int json_get(const char *js, const char *key, char *out, size_t n)
{
    char k[96]; snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p) return 0;
    p += strlen(k);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':') p++;
    size_t i = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && i + 1 < n) out[i++] = *p++; }
    else while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\r' && *p != '\n' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
    return 1;
}

/* ---- running build.py ---- */
static void build_line(const char *l, double f0, double f1)
{
    char ph[32]; int a, b;
    if (sscanf(l, "[progress] %31s %d %d", ph, &a, &b) != 3 || b <= 0) return;
    double x = (double)a / b, lo, hi;   /* where each phase sits between f0 and f1 */
    const wchar_t *what;
    if (!strcmp(ph, "prepare")) { lo = 0.00; hi = 0.08; what = L"Translating the game's executable"; }
    else if (!strcmp(ph, "levels")) { lo = 0.08; hi = 0.30; what = L"Translating the levels"; }
    else if (!strcmp(ph, "compile")) { lo = 0.30; hi = 0.97; what = L"Compiling"; }
    else if (!strcmp(ph, "link")) { lo = 0.97; hi = 0.99; what = L"Linking"; }
    else { lo = 1; hi = 1; what = L"Finishing"; }
    double f = f0 + (f1 - f0) * (lo + (hi - lo) * x);
    if (!strcmp(ph, "levels")) bld_status(f, L"%ls (%d of %d)", what, a + 1, b);
    else if (!strcmp(ph, "compile")) bld_status(f, L"%ls (%d of %d files)", what, a, b);
    else bld_status(f, L"%ls", what);
}
static int run_build(const wchar_t *gdir, const wchar_t *iso, const wchar_t *levels, const wchar_t *rgn, double f0, double f1)
{
    wchar_t py[MAX_PATH], zig[MAX_PATH], tc[MAX_PATH], cmd[4096], logp[MAX_PATH];
    path_join(tc, base_dir, L"toolchain");
    swprintf(py, MAX_PATH, L"%ls\\python\\python.exe", tc);
    swprintf(zig, MAX_PATH, L"%ls\\zig", tc);
    /* jobs: the cores, but no more than the memory allows (a big generated file can take 1.5 GB to compile) */
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    int j = (int)si.dwNumberOfProcessors, jm = (int)(ms.ullTotalPhys / (1536ull << 20));
    if (j > jm) j = jm;
    if (j < 1) j = 1;
    swprintf(cmd, 4096, L"\"%ls\" -u build.py --platform windows --region %ls --levels %ls --iso \"%ls\" --repo decomp -j %d", py, rgn, levels, iso, j);
    static wchar_t path[8192]; static int have_path;   /* (the PATH this program started with) */
    if (!have_path) { DWORD k = GetEnvironmentVariableW(L"PATH", path, 8192); if (k >= 8192) path[0] = 0; have_path = 1; }
    {   /* the toolchain first on PATH; Python in UTF-8 mode (the sources are UTF-8) */
        wchar_t np[9000];
        swprintf(np, 9000, L"%ls;%ls\\python;%ls", zig, tc, path);
        SetEnvironmentVariableW(L"PATH", np);
        SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
        SetEnvironmentVariableW(L"PYTHONDONTWRITEBYTECODE", L"1");
        SetEnvironmentVariableW(L"PYTHONHOME", 0); SetEnvironmentVariableW(L"PYTHONPATH", 0);
        swprintf(np, 9000, L"%ls\\zig-cache", gdir);
        SetEnvironmentVariableW(L"ZIG_GLOBAL_CACHE_DIR", np);
        SetEnvironmentVariableW(L"ZIG_LOCAL_CACHE_DIR", np);
    }
    path_join(logp, gdir, L"build_log.txt");
    HANDLE lf = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    SECURITY_ATTRIBUTES sa = { sizeof sa, 0, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf); return bld_fail(L"Could not start the build (pipe)."); }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW st; PROCESS_INFORMATION pi;
    memset(&st, 0, sizeof st); st.cb = sizeof st;
    st.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; st.wShowWindow = SW_HIDE;
    st.hStdOutput = wr; st.hStdError = wr; st.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    bld.job = CreateJobObjectW(0, 0);
    if (bld.job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li); li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(bld.job, JobObjectExtendedLimitInformation, &li, sizeof li); }
    if (!CreateProcessW(py, cmd, 0, 0, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, gdir, &st, &pi)) {
        CloseHandle(rd); CloseHandle(wr); if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
        return bld_fail(L"Could not start the build tools (error %lu).", GetLastError());
    }
    if (bld.job) AssignProcessToJobObject(bld.job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    CloseHandle(wr);
    static char tail[16][300]; int nt = 0;
    char line[4096]; size_t ln = 0; char b[4096]; DWORD n;
    while (ReadFile(rd, b, sizeof b, &n, 0) && n) {
        DWORD w; if (lf != INVALID_HANDLE_VALUE) WriteFile(lf, b, n, &w, 0);
        for (DWORD i = 0; i < n; i++) {
            if (b[i] == '\n' || ln + 1 >= sizeof line) {
                line[ln] = 0; if (ln && line[ln - 1] == '\r') line[ln - 1] = 0;
                if (!strncmp(line, "[progress]", 10)) build_line(line, f0, f1);
                else if (line[0]) { snprintf(tail[nt % 16], 300, "%s", line); nt++; }
                ln = 0;
            } else line[ln++] = b[i];
        }
        if (bld.cancel) TerminateJobObject(bld.job, 1);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (bld.job) { CloseHandle(bld.job); bld.job = 0; }
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    if (bld.cancel) return 0;
    if (code) {
        const char *last = nt ? tail[(nt - 1) % 16] : "no output";
        return bld_fail(L"The build stopped (code %lu): %hs", code, last);
    }
    return 1;
}

/* ---- the Build button's thread ---- */
static DWORD WINAPI build_thread(LPVOID unused)
{
    (void)unused;
    disc_t *d = &discs[bld.disc];
    const game_t *g = &games[d->game];
    wchar_t rel[1024], url[1200], dl[MAX_PATH], tcdir[MAX_PATH], gdir[MAX_PATH], zp[MAX_PATH], p[MAX_PATH];
    char *js = 0; size_t jn = 0;
    GetPrivateProfileStringW(L"build", L"release_url", RELEASE_URL, rel, 1024, ini_path);
    int local = _wcsnicmp(rel, L"http", 4) != 0;
    const wchar_t *sep = local ? L"\\" : L"/";
    size_t rl = wcslen(rel); if (rl && (rel[rl - 1] == L'/' || rel[rl - 1] == L'\\')) rel[--rl] = 0;
    path_join(dl, base_dir, L"downloads"); mkdirs(dl);
    path_join(tcdir, base_dir, L"toolchain");
    swprintf(gdir, MAX_PATH, L"%ls\\games\\%ls", base_dir, g->kit);

    /* 1. what to fetch */
    bld_status(0.0, L"Checking for the latest build files");
    swprintf(url, 1200, L"%ls%ls%ls%ls%ls.json", rel, sep, g->kit, sep, g->kit);
    if (!fetch(url, 0, &js, &jn, 0.0, 0.01, L"the build description")) goto out;
    char version[64] = "", tcv[64] = "", tcf[128] = "", tcs[80] = "", kitf[128] = "", kits[80] = "", lv[256] = "";
    json_get(js, "version", version, sizeof version);
    json_get(js, "toolchain_version", tcv, sizeof tcv); json_get(js, "toolchain_file", tcf, sizeof tcf); json_get(js, "toolchain_sha256", tcs, sizeof tcs);
    json_get(js, "kit_file", kitf, sizeof kitf); json_get(js, "kit_sha256", kits, sizeof kits);
    json_get(js, "levels", lv, sizeof lv);
    if (!version[0] || !tcf[0] || !kitf[0]) { bld_fail(L"The build description from the release is not valid."); goto out; }

    /* 2. the toolchain, when it is missing or another version */
    {
        wchar_t have[64] = L"", want[64], vf[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, tcv, -1, want, 64);
        path_join(vf, tcdir, L"version.txt");
        { FILE *f = _wfopen(vf, L"r"); if (f) { char b[64] = ""; if (fgets(b, sizeof b, f)) { b[strcspn(b, "\r\n")] = 0; MultiByteToWideChar(CP_UTF8, 0, b, -1, have, 64); } fclose(f); } }
        swprintf(p, MAX_PATH, L"%ls\\python\\python.exe", tcdir);
        if (wcscmp(have, want) || GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
            wchar_t wf[128]; MultiByteToWideChar(CP_UTF8, 0, tcf, -1, wf, 128);
            swprintf(url, 1200, L"%ls%ls%ls%ls%ls", rel, sep, g->kit, sep, wf);
            path_join(zp, dl, wf);
            if (!fetch(url, zp, 0, 0, 0.01, 0.20, L"Downloading the build tools")) goto out;
            bld_status(0.20, L"Checking the build tools");
            char h[65]; if (tcs[0] && (!sha256_file(zp, h) || _stricmp(h, tcs))) { DeleteFileW(zp); bld_fail(L"The downloaded build tools are damaged (checksum). Try again."); goto out; }
            rm_tree(tcdir);
            if (!unzip(zp, tcdir, 0.20, 0.26, L"the build tools")) goto out;
            FILE *f = _wfopen(vf, L"w"); if (f) { fputs(tcv, f); fclose(f); }
            DeleteFileW(zp);
        }
    }
    /* 3. the game's build kit (a fresh copy; the game's own folder, build\, stays: saves live there) */
    {
        wchar_t wf[128]; MultiByteToWideChar(CP_UTF8, 0, kitf, -1, wf, 128);
        swprintf(url, 1200, L"%ls%ls%ls%ls%ls", rel, sep, g->kit, sep, wf);
        path_join(zp, dl, wf);
        if (!fetch(url, zp, 0, 0, 0.26, 0.30, L"Downloading the game's build kit")) goto out;
        char h[65]; if (kits[0] && (!sha256_file(zp, h) || _stricmp(h, kits))) { DeleteFileW(zp); bld_fail(L"The downloaded build kit is damaged (checksum). Try again."); goto out; }
        mkdirs(gdir);
        {   /* another kit version than the files compiled last time: none of them can be reused */
            wchar_t kv[64] = L"", wv[64];
            GetPrivateProfileStringW(g->kit, L"kit_version", L"", kv, 64, ini_path);
            MultiByteToWideChar(CP_UTF8, 0, version, -1, wv, 64);
            if (kv[0] && wcscmp(kv, wv)) {   /* (none recorded: a first build, or one from before this was kept) */
                static const wchar_t *old[] = { L"build\\windows\\gen", L"build\\windows\\gen.new", L"build\\windows\\obj",
                                                L"build\\windows_us\\gen", L"build\\windows_us\\gen.new", L"build\\windows_us\\obj", L"zig-cache" };
                for (unsigned i = 0; i < sizeof old / sizeof old[0]; i++) { path_join(p, gdir, old[i]); rm_tree(p); }
            }
            WritePrivateProfileStringW(g->kit, L"kit_version", wv, ini_path);
        }
        {   /* everything but build\ */
            wchar_t pat[MAX_PATH]; WIN32_FIND_DATAW fd; swprintf(pat, MAX_PATH, L"%ls\\*", gdir);
            HANDLE hf = FindFirstFileW(pat, &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                do { if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !_wcsicmp(fd.cFileName, L"build")) continue;
                     path_join(p, gdir, fd.cFileName); rm_tree(p); } while (FindNextFileW(hf, &fd));
                FindClose(hf);
            }
        }
        if (!unzip(zp, gdir, 0.30, 0.32, L"the build kit")) goto out;
        DeleteFileW(zp);
    }
    /* 4. build */
    {
        wchar_t levels[256];
        GetPrivateProfileStringW(L"build", L"levels", L"", levels, 256, ini_path);
        if (!levels[0]) MultiByteToWideChar(CP_UTF8, 0, lv[0] ? lv : "0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18", -1, levels, 256);
        bld_status(0.32, L"Starting the build");
        if (!run_build(gdir, d->path, levels, g->rgn ? g->rgn : L"pal", 0.32, 0.99)) goto out;
    }
    /* 5. keep what the game needs: the intermediate files (about 1.3 GB) and the bytes read from the disc go */
    bld_status(0.99, L"Cleaning up");
    {
        static const wchar_t *junk[] = { L"gen", L"gen.new", L"obj", L"region", L"rac1_game.pdb", L"rac1_wrapper.pdb", L"asm_stubs.c.lib" };   /* (in the build's own folder) */
        static const wchar_t *junk2[] = { L"build\\levels", L"build\\levels_us", L"build\\disc", L"zig-cache" };
        const wchar_t *bd = g->rgn && wcscmp(g->rgn, L"pal") ? L"build\\windows_" : L"build\\windows";
        for (unsigned i = 0; i < sizeof junk / sizeof junk[0]; i++) {
            swprintf(p, MAX_PATH, L"%ls\\%ls%ls\\%ls", gdir, bd, g->rgn && wcscmp(g->rgn, L"pal") ? g->rgn : L"", junk[i]); rm_tree(p); }
        for (unsigned i = 0; i < sizeof junk2 / sizeof junk2[0]; i++) { path_join(p, gdir, junk2[i]); rm_tree(p); }
    }
    {   wchar_t wv[64]; MultiByteToWideChar(CP_UTF8, 0, version, -1, wv, 64);
        WritePrivateProfileStringW(g->kit, g->rgn ? L"built_version_us" : L"built_version", wv, ini_path); }
    bld_status(1.0, L"Built");
out:
    free(js);
    if (bld.cancel && !bld.failed) bld_fail(L"Build cancelled.");
    bld.active = 0;
    PostMessageW(hwnd, WM_BUILD_UPDATE, 1, 0);
    return 0;
}
static void build_start(int i)
{
    if (bld.active || i < 0 || discs[i].game < 0 || !games[discs[i].game].kit) return;
    bld.active = 1; bld.cancel = 0; bld.failed = 0; bld.disc = i; bld.frac = 0; bld.err[0] = 0;
    wcscpy(bld.status, L"Starting");
    message[0] = 0;
    CloseHandle(CreateThread(0, 0, build_thread, 0, 0, 0));
}
