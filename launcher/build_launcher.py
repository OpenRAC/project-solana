#!/usr/bin/env python3
"""build_launcher.py: builds Project Solana.exe (the launcher) with zig.

usage: python launcher/build_launcher.py [OUT_DIR]      (default: dist/)
Needs Python 3 and zig (pip install ziglang, or zig on PATH)."""
import os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "..", "dist")
os.makedirs(out, exist_ok=True)
zig = [shutil.which("zig")] if shutil.which("zig") else [sys.executable, "-m", "ziglang"]
exe = os.path.join(out, "Project Solana.exe")
cmd = zig + ["cc", "-target", "x86-windows-gnu", "-O2", "-s", "-municode", "-Wl,--subsystem,windows", "-o", exe,
             "solana.c", "solana.rc", "-lgdi32", "-luser32", "-lshell32", "-lcomdlg32", "-lole32", "-lmsimg32", "-lwinhttp", "-lbcrypt", "-lwindowscodecs", "-luuid"]
r = subprocess.run(cmd, cwd=HERE)
if r.returncode: sys.exit("build failed")
print("built", exe)
