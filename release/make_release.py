#!/usr/bin/env python3
"""make_release.py: makes the files of a game's GitHub release (the tag is the game, e.g. "rac1").

usage: python release/make_release.py rac1 --decomp PATH_TO_RAC1_DECOMP [--version V]
                                      [--toolchain-zip toolchain-win64.zip]

Writes dist/rac1/:
  rac1.json             what the launcher's Build button fetches (version, files, SHA-256s, levels)
  rac1-build.zip        games/rac1 (build.py, tools/, host/, launcher/, docs/) plus the decomp's
                        source as decomp/ (everything but baserom/ and .git: no game files)
  toolchain-win64.zip   Python, rabbitizer and zig for Windows x64 (release/toolchain.json); reused
                        from --toolchain-zip when given, else fetched (PyPI and GitHub) and assembled

Upload the three files to the release named after the game (gh release create rac1 dist/rac1/*).
Players never download anything from the game itself: the build reads their own disc image."""
import argparse, datetime, glob, hashlib, io, json, os, shutil, subprocess, sys, tarfile, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ap = argparse.ArgumentParser()
ap.add_argument("game")
ap.add_argument("--decomp", required=True, help="the game's decomp repo (only its source is packed; baserom/ is left out)")
ap.add_argument("--version", default=datetime.date.today().strftime("%Y.%m.%d"))
ap.add_argument("--toolchain-zip", default=None)
ap.add_argument("--levels", default=",".join(str(i) for i in range(19)))
a = ap.parse_args()

game = a.game
src = os.path.join(ROOT, "games", game)
if not os.path.isdir(src): sys.exit("no games/%s" % game)
out = os.path.join(ROOT, "dist", game)
os.makedirs(out, exist_ok=True)

SKIP_DIRS = {"__pycache__", ".git", "build", "dev", "decomp", "zig-cache"}
DECOMP_SKIP = {"baserom", ".git", ".github"}


def add_tree(z, top, arc, skip):
    for root, dirs, files in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d not in skip)
        for f in sorted(files):
            if f.endswith((".pyc", ".o", ".obj")) or f == "build_log.txt": continue
            p = os.path.join(root, f)
            # one fixed, old timestamp for every file: files the player compiled before are
            # newer, so a retry of the same kit reuses them (a new kit version starts clean)
            zi = zipfile.ZipInfo(os.path.join(arc, os.path.relpath(p, top)).replace(os.sep, "/"), date_time=(2020, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
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

# the toolchain
tcj = json.load(open(os.path.join(HERE, "toolchain.json")))
tcz = os.path.join(out, "toolchain-win64.zip")
if a.toolchain_zip:
    if os.path.abspath(a.toolchain_zip) != os.path.abspath(tcz): shutil.copyfile(a.toolchain_zip, tcz)
else:
    import urllib.request
    with tempfile.TemporaryDirectory() as t:
        print("fetching", tcj["python"])
        data = urllib.request.urlopen(tcj["python"]).read()
        tarfile.open(fileobj=io.BytesIO(data)).extractall(t)
        py = os.path.join(t, "python")
        for pat in ("*.pdb", "**/*.pdb"):
            for p in glob.glob(os.path.join(py, pat), recursive=True): os.remove(p)
        for d in ("tcl", "Lib/test", "Lib/idlelib", "Lib/tkinter", "Lib/turtledemo", "Lib/ensurepip", "include", "libs", "Scripts"):
            shutil.rmtree(os.path.join(py, d), ignore_errors=True)
        w = os.path.join(t, "wheels")
        subprocess.run([sys.executable, "-m", "pip", "download", "--no-deps", "--platform", "win_amd64", "--only-binary=:all:", "-d", w,
                        tcj["ziglang_wheel"], tcj["rabbitizer_wheel"]], check=True)
        for whl in glob.glob(os.path.join(w, "rabbitizer-*.whl")): zipfile.ZipFile(whl).extractall(os.path.join(py, "Lib", "site-packages"))
        for whl in glob.glob(os.path.join(w, "ziglang-*.whl")): zipfile.ZipFile(whl).extractall(os.path.join(t, "zw"))
        zg = os.path.join(t, "zig")
        shutil.move(os.path.join(t, "zw", "ziglang"), zg)
        for f in ("__init__.py", "__main__.py"):
            if os.path.exists(os.path.join(zg, f)): os.remove(os.path.join(zg, f))
        # the C library sources and headers of other systems are not needed for x86-windows-gnu
        lc = os.path.join(zg, "lib", "libc")
        for d in os.listdir(lc):
            if d in ("glibc", "musl", "darwin", "wasi", "freebsd", "netbsd", "openbsd"): shutil.rmtree(os.path.join(lc, d))
        inc = os.path.join(lc, "include")
        for d in os.listdir(inc):
            if "windows" not in d: shutil.rmtree(os.path.join(inc, d))
        for d in ("libcxx", "libcxxabi", "libunwind", "libtsan", "docs", "build-web", "init"):
            shutil.rmtree(os.path.join(zg, "lib", d), ignore_errors=True)
        with zipfile.ZipFile(tcz, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            add_tree(z, py, "python", set())
            add_tree(z, zg, "zig", set())
print("toolchain:", tcz, os.path.getsize(tcz) // (1 << 20), "MB")

# what the launcher reads (a flat object: it is read without a JSON library)
man = {
    "game": game,
    "version": a.version,
    "levels": a.levels,
    "kit_file": os.path.basename(kit), "kit_size": os.path.getsize(kit), "kit_sha256": sha256(kit),
    "toolchain_file": os.path.basename(tcz), "toolchain_size": os.path.getsize(tcz), "toolchain_sha256": sha256(tcz),
    "toolchain_version": tcj["version"],
}
with open(os.path.join(out, "%s.json" % game), "w") as f: json.dump(man, f, indent=1)
print(json.dumps(man, indent=1))
