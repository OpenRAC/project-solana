#!/usr/bin/env python3
"""make_release.py: makes the files of a game's GitHub release (the tag is the game, e.g. "rac1").

usage: python release/make_release.py rac1 --decomp PATH_TO_RAC1_DECOMP [--version V]
                                      [--toolchain-zip toolchain-win64.zip] [--toolchain-linux-zip toolchain-linux64.zip]

Writes dist/rac1/:
  rac1.json             what the launcher's Build button fetches (version, files, SHA-256s, levels)
  rac1-build.zip        games/rac1 (build.py, tools/, host/, launcher/, docs/) plus the decomp's
                        source as decomp/ (everything but baserom/ and .git: no game files)
  toolchain-win64.zip   Python, rabbitizer and zig for Windows x64 (release/toolchain.json); reused
                        from --toolchain-zip when given, else fetched (PyPI and GitHub) and assembled
  toolchain-linux64.zip the same for Linux x86-64 (--toolchain-linux-zip to reuse one)

Upload the four files to the release named after the game (gh release create rac1 dist/rac1/*).
Players never download anything from the game itself: the build reads their own disc image."""
import argparse, datetime, glob, hashlib, io, json, os, shutil, subprocess, sys, tarfile, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ap = argparse.ArgumentParser()
ap.add_argument("game")
ap.add_argument("--decomp", required=True, help="the game's decomp repo (only its source is packed; baserom/ is left out)")
ap.add_argument("--version", default=datetime.date.today().strftime("%Y.%m.%d"))
ap.add_argument("--toolchain-zip", default=None, help="reuse this toolchain-win64.zip")
ap.add_argument("--toolchain-linux-zip", default=None, help="reuse this toolchain-linux64.zip")
ap.add_argument("--levels", default=",".join(str(i) for i in range(19)))
a = ap.parse_args()

game = a.game
src = os.path.join(ROOT, "games", game)
if not os.path.isdir(src): sys.exit("no games/%s" % game)
out = os.path.join(ROOT, "dist", game)
os.makedirs(out, exist_ok=True)

SKIP_DIRS = {"__pycache__", ".git", "build", "dev", "decomp", "zig-cache"}
DECOMP_SKIP = {"baserom", ".git", ".github"}


def add_tree(z, top, arc, skip, modes=False):
    for root, dirs, files in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d not in skip)
        for f in sorted(files):
            if f.endswith((".pyc", ".o", ".obj")) or f == "build_log.txt": continue
            p = os.path.join(root, f)
            # one fixed, old timestamp for every file: files the player compiled before are
            # newer, so a retry of the same kit reuses them (a new kit version starts clean)
            zi = zipfile.ZipInfo(os.path.join(arc, os.path.relpath(p, top)).replace(os.sep, "/"), date_time=(2020, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = ((os.stat(p).st_mode & 0o777) if modes else 0o644) << 16   # (Linux: executables stay executable)
            with open(p, "rb") as fh: z.writestr(zi, fh.read(), compresslevel=9)


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()


# the build kit
kit = os.path.join(out, "%s-build.zip" % game)
with zipfile.ZipFile(kit, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    add_tree(z, src, "", SKIP_DIRS)
    add_tree(z, a.decomp, "decomp", DECOMP_SKIP)
    for f in ("THIRD_PARTY_NOTICES.md",):
        if os.path.exists(os.path.join(ROOT, f)): z.write(os.path.join(ROOT, f), f)
    for n in z.namelist():   # (nothing from the game: no executable, no overlays)
        if n.startswith("decomp/baserom/") or n.endswith(("SCES_509.16", ".iso")): sys.exit("refusing to pack %s" % n)
print("kit:", kit, os.path.getsize(kit) // 1024, "KB")

# the toolchains: Python, rabbitizer and zig, for building on Windows (x64) and on Linux (x86-64)
tcj = json.load(open(os.path.join(HERE, "toolchain.json")))


def make_toolchain(plat, out_zip):
    import urllib.request
    with tempfile.TemporaryDirectory() as t:
        print("fetching", tcj["python"][plat])
        data = urllib.request.urlopen(tcj["python"][plat]).read()
        tarfile.open(fileobj=io.BytesIO(data)).extractall(t)
        py = os.path.join(t, "python")
        for pat in ("*.pdb", "**/*.pdb"):
            for p in glob.glob(os.path.join(py, pat), recursive=True): os.remove(p)
        site = os.path.join(py, "Lib", "site-packages") if plat == "windows" else glob.glob(os.path.join(py, "lib", "python3.*", "site-packages"))[0]
        stdlib = os.path.join(py, "Lib") if plat == "windows" else os.path.dirname(site)
        for d in ("test", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3", "config-3.12-x86_64-linux-gnu"):
            shutil.rmtree(os.path.join(stdlib, d), ignore_errors=True)
        for d in ("tcl", "include", "libs", "Scripts", "share"):
            shutil.rmtree(os.path.join(py, d), ignore_errors=True)
        for p in glob.glob(os.path.join(py, "lib", "libtcl*")) + glob.glob(os.path.join(py, "lib", "libtk*")) + glob.glob(os.path.join(py, "lib", "tcl*")) + glob.glob(os.path.join(py, "lib", "tk*")):
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
        w = os.path.join(t, "wheels")
        tags = ["--platform", "win_amd64"] if plat == "windows" else ["--platform", "manylinux_2_17_x86_64", "--platform", "manylinux2014_x86_64", "--platform", "manylinux_2_12_x86_64"]
        subprocess.run([sys.executable, "-m", "pip", "download", "--no-deps", *tags, "--python-version", "3.12", "--only-binary=:all:", "-d", w,
                        tcj["ziglang_wheel"], tcj["rabbitizer_wheel"]], check=True)
        for whl in glob.glob(os.path.join(w, "rabbitizer-*.whl")): zipfile.ZipFile(whl).extractall(site)
        for whl in glob.glob(os.path.join(w, "ziglang-*.whl")): zipfile.ZipFile(whl).extractall(os.path.join(t, "zw"))
        zg = os.path.join(t, "zig")
        shutil.move(os.path.join(t, "zw", "ziglang"), zg)
        for f in ("__init__.py", "__main__.py"):
            if os.path.exists(os.path.join(zg, f)): os.remove(os.path.join(zg, f))
        if plat == "linux":
            os.chmod(os.path.join(zg, "zig"), 0o755)
            # symlinks would be stored as second copies, and the binaries carry debug symbols (hundreds of MB)
            for root, dirs, files in os.walk(py):
                for f in files:
                    q = os.path.join(root, f)
                    if os.path.islink(q) and f.startswith("libpython"): os.remove(q)   # (libpython3.12.so: for linking against it)
            for q in glob.glob(os.path.join(py, "lib", "libpython3*.so*")) + [os.path.join(zg, "zig")]:
                subprocess.run(["strip", "--strip-unneeded", q], check=True)
        # the C library sources and headers of other systems: x86-windows-gnu needs mingw, x86-linux-gnu glibc
        keep_libc = ("mingw", "include") if plat == "windows" else ("glibc", "include", "include-linux") 
        lc = os.path.join(zg, "lib", "libc")
        for d in os.listdir(lc):
            if d in ("glibc", "musl", "darwin", "wasi", "freebsd", "netbsd", "openbsd", "mingw") and d not in keep_libc: shutil.rmtree(os.path.join(lc, d))
        inc = os.path.join(lc, "include")
        for d in os.listdir(inc):
            want = "windows" in d if plat == "windows" else (d.endswith(("linux-gnu", "linux-any", "-glibc")) or d in ("generic-glibc", "any-linux-any", "x86-linux-any", "x86-linux-gnu"))
            if not want: shutil.rmtree(os.path.join(inc, d))
        for d in ("libcxx", "libcxxabi", "libunwind", "libtsan", "docs", "build-web", "init"):
            shutil.rmtree(os.path.join(zg, "lib", d), ignore_errors=True)
        with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            add_tree(z, py, "python", set(), modes=plat == "linux")
            add_tree(z, zg, "zig", set(), modes=plat == "linux")


tc = {}
for plat, name, reuse in (("windows", "toolchain-win64.zip", a.toolchain_zip), ("linux", "toolchain-linux64.zip", a.toolchain_linux_zip)):
    tc[plat] = os.path.join(out, name)
    if reuse:
        if os.path.abspath(reuse) != os.path.abspath(tc[plat]): shutil.copyfile(reuse, tc[plat])
    else:
        make_toolchain(plat, tc[plat])
    print("toolchain (%s):" % plat, tc[plat], os.path.getsize(tc[plat]) // (1 << 20), "MB")

# what the launcher reads (a flat object: it is read without a JSON library)
man = {
    "game": game,
    "version": a.version,
    "levels": a.levels,
    "kit_file": os.path.basename(kit), "kit_size": os.path.getsize(kit), "kit_sha256": sha256(kit),
    "toolchain_file": os.path.basename(tc["windows"]), "toolchain_size": os.path.getsize(tc["windows"]), "toolchain_sha256": sha256(tc["windows"]),
    "toolchain_linux_file": os.path.basename(tc["linux"]), "toolchain_linux_size": os.path.getsize(tc["linux"]), "toolchain_linux_sha256": sha256(tc["linux"]),
    "toolchain_version": tcj["version"],
}
with open(os.path.join(out, "%s.json" % game), "w") as f: json.dump(man, f, indent=1)
print(json.dumps(man, indent=1))
