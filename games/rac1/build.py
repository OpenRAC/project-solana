#!/usr/bin/env python3
"""build.py: generate host sources from OpenRAC rac1-decomp and build the RAC1 Wrapper.

usage: python build.py [--repo PATH_TO_DECOMP] [--platform linux|windows] [-j N]
                       [--iso PATH_TO_DISC_IMAGE] [--levels 00,01,...|none]

Needs Python 3 and zig (pip install ziglang, or zig on PATH). The decomp repo
is only read. Output: build/<platform>/rac1_wrapper[.exe]

Level programs (docs/LEVELS.md): each level listed in --levels (default 00,
Veldin 1) is compiled from src/overlays and its own bytes. Those bytes come
from your disc image (--iso, else RAC1_ISO, else iso= in
build/<platform>/rac1_wrapper.cfg), or from an OpenRAC dump in the decomp
repo (baserom/overlays/level_NN); they are extracted to build/levels/ and
never leave your machine.
"""
import argparse, glob, json, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--repo", default=os.environ.get("UYA_DECOMP", os.path.join(HERE, "..", "rac1-decomp-main")))
ap.add_argument("--platform", default="windows" if os.name == "nt" else "linux")
ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
ap.add_argument("-O", default="2")
ap.add_argument("--iso", default=None)
ap.add_argument("--levels", default="00")
a = ap.parse_args()

zig = [shutil.which("zig")] if shutil.which("zig") else [sys.executable, "-m", "ziglang"]
# Linux: 32-bit glibc 2.17 or newer (any distribution since 2013), linked dynamically so it can load
# the system's SDL2 and OpenGL driver (32-bit: i386 libraries)
target = {"linux": "x86-linux-gnu.2.17", "windows": "x86-windows-gnu"}[a.platform]
out = os.path.join(HERE, "build", a.platform)
gen = os.path.join(out, "gen")
stage = os.path.join(out, "gen.new")
obj = os.path.join(out, "obj")
os.makedirs(obj, exist_ok=True)
levels = [] if a.levels in ("", "none") else sorted({int(x) for x in a.levels.split(",")})


def find_iso():
    if a.iso: return a.iso
    if os.environ.get("RAC1_ISO"): return os.environ["RAC1_ISO"]
    cfg = os.path.join(out, "rac1_wrapper.cfg")
    if os.path.exists(cfg):
        for l in open(cfg, errors="replace"):
            if l.strip().startswith("iso="): return l.strip()[4:].strip()
    return None


def sync(src, dst):
    """copy the freshly generated tree over gen/, touching only files that
    changed, so unchanged sources keep their objects"""
    os.makedirs(dst, exist_ok=True)
    keep = set()
    for root, _, fs in os.walk(src):
        for f in fs:
            s = os.path.join(root, f)
            rel = os.path.relpath(s, src)
            d = os.path.join(dst, rel)
            keep.add(rel)
            os.makedirs(os.path.dirname(d), exist_ok=True)
            if not os.path.exists(d) or open(s, "rb").read() != open(d, "rb").read():
                shutil.copyfile(s, d)
    for root, _, fs in os.walk(dst):
        for f in fs:
            rel = os.path.relpath(os.path.join(root, f), dst)
            if rel not in keep: os.remove(os.path.join(root, f))
    shutil.rmtree(src)


def iso_file(iso, name):
    """a file in the disc image's root directory (ISO 9660)"""
    import struct
    with open(iso, "rb") as f:
        f.seek(16 * 2048); pvd = f.read(2048)
        if pvd[1:6] != b"CD001": return None
        dlsn, dlen = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        f.seek(dlsn * 2048); d = f.read(dlen)
        o = 0
        while o < len(d):
            n = d[o]
            if n == 0: o = (o // 2048 + 1) * 2048; continue
            fn = d[o + 33:o + 33 + d[o + 32]].decode("ascii", "replace").split(";")[0]
            if fn == name:
                lsn, size = struct.unpack_from("<I", d, o + 2)[0], struct.unpack_from("<I", d, o + 10)[0]
                f.seek(lsn * 2048); return f.read(size)
            o += n
    return None


env = dict(os.environ, UYA_PLATFORM=a.platform, RAC_PLATFORM=a.platform)
# the static recompilation is built from the game itself: the executable comes
# from the disc image (the decomp repo only lends its symbol and function lists)
if find_iso():
    _exe = iso_file(find_iso(), "SCES_509.16")
    if _exe:
        os.makedirs(os.path.join(HERE, "build", "disc"), exist_ok=True)
        _p = os.path.join(HERE, "build", "disc", "SCES_509.16")
        if not os.path.exists(_p) or open(_p, "rb").read() != _exe: open(_p, "wb").write(_exe)
        env["UYA_EXE"] = _p
        print("executable read from the disc image:", find_iso())
shutil.rmtree(stage, ignore_errors=True)
py = sys.executable
def progress(phase, i, n):
    """a line the Project Solana launcher reads for its progress bar"""
    print("[progress] %s %d %d" % (phase, i, n), flush=True)
progress("prepare", 0, 3)
subprocess.run([py, os.path.join(HERE, "tools", "gen_host.py"), a.repo, stage], check=True, env=env)
dumps = os.path.join(HERE, "build", "levels")
# the level programs first: the executable's translation looks at the core
# addresses their code calls (tools/recomp.py)
for lv in levels:
    cmd = [py, os.path.join(HERE, "tools", "level_dump.py"), dumps, str(lv), "--repo", a.repo]
    if find_iso(): cmd += ["--iso", find_iso()]
    subprocess.run(cmd, check=True)
progress("prepare", 1, 3)
subprocess.run([py, os.path.join(HERE, "tools", "recomp.py"), stage, a.repo], check=True, env=env)
progress("prepare", 2, 3)
# VU0/VU1 microprograms (.vutext) to C
subprocess.run([py, os.path.join(HERE, "tools", "vu_recomp.py"), stage, a.repo], check=True, env=env)
level_info = []
for k, lv in enumerate(levels):
    progress("levels", k, len(levels))
    LV = "%02d" % lv
    lg = os.path.join(stage, "l" + LV)
    subprocess.run([py, os.path.join(HERE, "tools", "gen_host.py"), a.repo, lg, "--level", LV, "--exe-gen", stage], check=True, env=env)
    subprocess.run([py, os.path.join(HERE, "tools", "recomp.py"), lg, a.repo, "--level", LV, "--dump", dumps, "--exe-gen", stage],
                   check=True, env=env)
    man = json.load(open(os.path.join(dumps, "level_" + LV, "manifest.json")))
    text = [r for r in man["records"] if r["name"] == "text"][0]
    level_info.append((LV, man["entry_point"], text["address"], text["bytes"]))
# the levels built in: ParseBin looks a loaded program up here (host/src/rac1/levels.c)
with open(os.path.join(stage, "level_table.c"), "w") as f:
    f.write('/* generated by build.py: the level programs in this build */\n#include "rac1_levels.h"\n')
    for LV, *_ in level_info:
        f.write("extern const uya_tramp_t uya_tramp_table_l%s[]; extern const int uya_tramp_count_l%s;\n" % (LV, LV))
        f.write("extern const rac1_rc_row_t rc%s_table[]; extern const int rc%s_table_count;\n" % (LV, LV))
    f.write("const rac1_level_t rac1_levels[] = {\n")
    for LV, entry, ta, tb in level_info:
        f.write("    {%d, 0x%08X, 0x%08X, 0x%X, uya_tramp_table_l%s, &uya_tramp_count_l%s, rc%s_table, &rc%s_table_count},\n"
                % (int(LV), entry, ta, tb, LV, LV, LV, LV))
    if not level_info: f.write("    {-1, 0, 0, 0, 0, 0, 0, 0},\n")
    f.write("};\nconst int rac1_level_count = %d;\n" % len(level_info))
sync(stage, gen)

common = ["-target", target, "-O" + a.O, "-g", "-march=nehalem", "-mfpmath=sse", "-malign-double", "-fno-strict-aliasing", "-fwrapv",
          "-fno-PIE", "-fno-stack-protector", "-I", os.path.join(gen, "include"), "-I", os.path.join(HERE, "host", "include")]
if a.platform == "linux": common += ["-D_FILE_OFFSET_BITS=64"]   # (32-bit glibc: disc images are over 2 GB)
# frontbin code: keep it permissive, it was written for gcc 2.95
menu_flags = ["-w", "-Wno-error=incompatible-pointer-types", "-Wno-error=int-conversion",
              "-Wno-error=implicit-function-declaration", "-Wno-error=implicit-int",
              "-Wno-error=incompatible-function-pointer-types", "-Wno-error=return-type",
              "-fno-builtin",
              # no inlining across the game's functions: the decomp names overlapping PS2 data
              # with separate symbols (D_001997D0 is a struct whose +0x2C is also D_001997FC),
              # which the compiler takes for distinct objects. A call keeps such stores and loads
              # in order; an inlined body does not (func_001EB300 inlined into func_001EABE8 kept
              # the help-text count in a register, so the text tables were never relocated)
              "-fno-inline-functions",
              # the game's C keeps 16-byte vectors (u128, qcopy) in locals; the x86 stack is
              # not 16-byte aligned on every path into it (translated code, thunks), and
              # an aligned load of a misaligned local faults (func_L00_001FE940 on Veldin's
              # first frame): every function realigns its own frame
              "-mstackrealign"]
jobs = []   # (source, extra flags, object name, forced header)
for c in sorted(glob.glob(os.path.join(gen, "*.c"))):
    jobs.append((c, menu_flags if os.path.basename(c) not in ("tramp_table.c", "level_table.c") else [], None, None))
jobs.append((os.path.join(gen, "abs_syms.s"), [], None, None))
for c in sorted(glob.glob(os.path.join(HERE, "host", "overrides", "*.c"))):
    jobs.append((c, menu_flags, None, None))
for c in sorted(glob.glob(os.path.join(HERE, "host", "src", "**", "*.c"), recursive=True)):
    jobs.append((c, ["-Wall", "-Wno-unused"], None, None))
for LV, *_ in level_info:
    lg = os.path.join(gen, "l" + LV)
    hdr = os.path.join(lg, "l%s_names.h" % LV)
    os.makedirs(os.path.join(obj, "l" + LV), exist_ok=True)
    for c in sorted(glob.glob(os.path.join(lg, "*.c"))):
        jobs.append((c, menu_flags + ["-include", hdr], os.path.join("l" + LV, os.path.basename(c) + ".o"), hdr))
    jobs.append((os.path.join(lg, "abs_syms.s"), [], os.path.join("l" + LV, "abs_syms.s.o"), None))
    # the executable's main-segment overrides that the level has: the level's copies
    for nm in open(os.path.join(lg, "overrides.txt")).read().split():
        c = os.path.join(HERE, "host", "overrides", nm + ".c")
        jobs.append((c, menu_flags + ["-include", hdr], os.path.join("l" + LV, "override_" + nm + ".c.o"), hdr))


# compiler flags: a change rebuilds everything
_stamp = os.path.join(obj, "flags.txt")
_flags = " ".join(common + menu_flags)
if not os.path.exists(_stamp) or open(_stamp).read() != _flags:
    os.makedirs(obj, exist_ok=True)
    open(_stamp, "w").write(_flags)
flags_time = os.path.getmtime(_stamp)

# headers every generated file sees: a change rebuilds them all
headers_time = max([flags_time] + [os.path.getmtime(h) for h in glob.glob(os.path.join(gen, "include", "*.h")) + glob.glob(os.path.join(HERE, "host", "include", "*.h"))])


def cc(job):
    src, extra, oname, hdr = job
    o = os.path.join(obj, oname or (os.path.basename(src) + ".o"))
    if os.path.exists(o) and src.endswith(".c") and os.path.join(HERE, "host") not in src:
        t = os.path.getmtime(o)
        if t > os.path.getmtime(src) and t > headers_time and (not hdr or t > os.path.getmtime(hdr)):
            return o, None
    r = subprocess.run(zig + ["cc"] + common + extra + ["-c", src, "-o", o], capture_output=True, text=True)
    return o, (r.stderr if r.returncode else None)

progress("compile", 0, len(jobs))
with ThreadPoolExecutor(a.j) as ex:
    futs = [ex.submit(cc, j) for j in jobs]
    done_n = 0
    for _ in as_completed(futs):
        done_n += 1
        if done_n % 8 == 0 or done_n == len(jobs): progress("compile", done_n, len(jobs))
    res = [f.result() for f in futs]
progress("link", 0, 1)


def objs_rsp(name):
    """the object files as a response file (@file): Windows limits a command line to
    32767 characters, and the ~5700 objects of a full build are far more than that"""
    p = os.path.join(obj, name + ".rsp")
    with open(p, "w") as f:
        for o, _ in res: f.write('"%s"\n' % o.replace("\\", "/"))
    return "@" + p.replace("\\", "/")
errs = [e for _, e in res if e]
for e in errs: print(e[-3000:])
if errs:
    sys.exit("%d files failed to compile" % len(errs))
if a.platform == "linux":
    exe = os.path.join(out, "rac1_wrapper")
    # at 0x40000000 like the Windows DLL (lower addresses are the PS2's); its symbols exported
    # (-rdynamic) so the crash handler can name functions
    link = zig + ["cc", "-target", target, "-o", exe, objs_rsp("link")] + ["-no-pie", "-rdynamic", "-Wl,--image-base=0x40000000",
                                                                            "-ldl", "-lpthread", "-lm"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-6000:]); sys.exit("link failed")
else:
    # the menu and runtime as a DLL at 0x40000000, plus the small launcher that owns PS2 RAM
    dll = os.path.join(out, "rac1_game.dll")
    link = zig + ["cc", "-shared", "-target", target, "-o", dll, objs_rsp("link")] + ["-Wl,--image-base=0x40000000", "-Wl,--no-dynamicbase", "-lshell32", "-lwinmm", "-lgdi32", "-luser32", "-ladvapi32"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-6000:]); sys.exit("link failed (dll)")
    # the function names for crash reports (the .pdb is deleted after a launcher build)
    try:
        sys.path.insert(0, os.path.join(HERE, "tools"))
        import pdb_syms
        print("symbols:", pdb_syms.write_sym(dll, os.path.join(out, "rac1_game.pdb"), os.path.join(out, "rac1_game.sym")))
    except Exception as e:
        print("no symbol map (%s): crash reports will show offsets only" % e)
    exe =os.path.join(out, "rac1_wrapper.exe")
    L = os.path.join(HERE, "launcher")
    link = zig + ["cc", "-target", target, "-O2", "-nostdlib", "-o", exe, os.path.join(L, "launcher_win32.c"),
                  os.path.join(L, "ps2ram_win32.c"), "-lkernel32", "-luser32",
                  "-Wl,--image-base=0x10000", "-Wl,--entry=launcher_start@0", "-Wl,--no-dynamicbase"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-6000:]); sys.exit("link failed (launcher)")
    # the library app (frontend/solana.c): build/Project Solana.exe, which finds the game in build/windows
    F = os.path.join(HERE, "frontend")
    if not os.path.isdir(F): F = None   # (the game kit players download has no launcher source)
    link = None if not F else zig + ["cc", "-target", target, "-O2", "-municode", "-Wl,--subsystem,windows", "-o", os.path.join(HERE, "build", "Project Solana.exe"),
                  os.path.join(F, "solana.c"), os.path.join(F, "solana.rc"), "-lgdi32", "-luser32", "-lshell32", "-lcomdlg32", "-lole32", "-lmsimg32", "-lwinhttp", "-lbcrypt", "-lwindowscodecs", "-luuid"]
    r = subprocess.run(link, capture_output=True, text=True, cwd=F) if link else None
    if r and r.returncode:
        print(r.stderr[-6000:]); sys.exit("link failed (Project Solana.exe)")
progress("done", 1, 1)
print("built", exe)
