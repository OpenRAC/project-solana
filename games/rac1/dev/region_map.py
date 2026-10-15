#!/usr/bin/env python3
"""region_map.py DECOMP_REPO PAL_EXE US_EXE PAL_LEVELS US_LEVELS OUT_JSON

Developer tool (needs both executables): the PAL-to-US address map that lets the
build make the US version (SCUS-97199) from the same sources as PAL (SCES-50916).
The output is addresses only (no game bytes) and ships in the build kit as
region/us.json; tools/region.py reads it.

  functions  the executable's: OpenRAC's config/overlays/us_map.tsv (code alone),
             PAL name -> US address and size, with how it was paired; and each
             level's, from the same table and the catalogue's places
             (config/overlays/functions.tsv), PAL place -> US place
  data       the executable's data sections: PAL and US words aligned (an address
             in either one counts as equal: the pointers moved), giving runs of
             PAL addresses that share one shift. Sections whose size did not
             change move as one. Checked against the data addresses the paired
             functions form with lui/addiu (both builds load the same data).
  levels     each level's data records (lit, bss, data, vtbl, camvtbl, sndvtbl)
             aligned the same way (PAL_LEVELS and US_LEVELS: level_dump.py output)
"""
import json, os, re, struct, sys
from collections import Counter, defaultdict

REPO, PAL_EXE, US_EXE, PAL_LV, US_LV, OUT = sys.argv[1:7]
pal, us = open(PAL_EXE, "rb").read(), open(US_EXE, "rb").read()


def sections(e):
    shoff, _, shnum, shstr = struct.unpack_from("<I", e, 32)[0], *struct.unpack_from("<HHH", e, 46)
    S = [struct.unpack_from("<10I", e, shoff + i * 40) for i in range(shnum)]
    names = e[S[shstr][4]:S[shstr][4] + S[shstr][5]]
    out = {}
    for s in S:
        n = names[s[0]:names.index(b"\0", s[0])].decode()
        if s[3]: out[n] = {"addr": s[3], "off": s[4], "size": s[5], "nobits": s[1] == 8}
    return out


SP, SU = sections(pal), sections(us)


def seg_bytes(e, a, n):
    phoff, phnum = struct.unpack_from("<I", e, 28)[0], struct.unpack_from("<H", e, 44)[0]
    for i in range(phnum):
        t, off, va, pa, fs = struct.unpack_from("<5I", e, phoff + 32 * i)
        if va <= a and a + n <= va + fs: return e[off + a - va: off + a - va + n]


# ---- functions -----------------------------------------------------------------------------------
fsize = {}
for u in json.load(open(os.path.join(REPO, "progress", "report.json")))["units"]:
    for f in u.get("functions", []):
        md = f.get("metadata") or {}
        if "virtual_address" in md: fsize[f["name"]] = (int(md["virtual_address"]), int(f["size"]))
places = {}   # catalogue name -> {level: [PAL addresses]}
for l in open(os.path.join(REPO, "config", "overlays", "functions.tsv")):
    if l.startswith("#") or not l.strip(): continue
    f = l.rstrip("\n").split("\t")
    d = defaultdict(list)
    for x in f[5].split(","):
        if ":" in x: d[x.split(":")[0]].append(int(x.split(":")[1], 16))
    places[f[0]] = d

exe_funcs, exe_us_only = {}, []
lv_rows = defaultdict(list)
for l in open(os.path.join(REPO, "config", "overlays", "us_map.tsv")):
    if l.startswith("#") or not l.strip(): continue
    prog, ua, sz, name, method, sim = l.rstrip("\n").split("\t")[:6]
    ua, sz = int(ua, 16), int(sz)
    if prog == "boot":
        q = re.fullmatch(r"func_([0-9A-F]{8})", name)
        if q: exe_funcs["%08X" % int(q.group(1), 16)] = [ua, sz, method]
        else: exe_us_only.append([ua, sz])
    else:
        lv_rows[prog[6:]].append((ua, sz, name, method))
levels = {}
for lv, rows in sorted(lv_rows.items()):
    by_name = defaultdict(list)
    only = []
    for ua, sz, name, method in sorted(rows):
        if name in places and places[name].get(lv): by_name[name].append((ua, sz, method))
        else: only.append([ua, sz])
    m = {}
    for name, us_pl in by_name.items():
        for pa, (ua, sz, method) in zip(sorted(places[name][lv]), us_pl):   # several copies: paired in address order
            m["%08X" % pa] = [ua, sz, method]
    levels[lv] = {"funcs": m, "us_only": only}

# ---- data ----------------------------------------------------------------------------------------
def words(b): return list(struct.unpack("<%dI" % (len(b) // 4), b[:len(b) // 4 * 4]))
def tok(w): return -1 if 0x00100000 <= w < 0x02000000 else w


def align(a_words, b_words, a_base, b_base, W=8):
    """runs [pal_lo, pal_hi, shift] (bytes) of a PAL range onto a US range"""
    a, b = [tok(x) for x in a_words], [tok(x) for x in b_words]
    d, runs, i = 0, [], 0
    while i < len(a):
        if 0 <= i + d < len(b) and a[i] == b[i + d]:
            if runs and runs[-1][2] == d and runs[-1][1] == i: runs[-1][1] = i + 1
            else: runs.append([i, i + 1, d])
            i += 1; continue
        best = None
        for k in range(0, 1024):
            for dd in (d - k, d + k):
                if 0 <= i + dd and i + dd + W <= len(b) and a[i:i + W] == b[i + dd:i + dd + W] and \
                        sum(1 for x in a[i:i + W] if x not in (0, -1)) >= 3:
                    best = dd; break
            if best is not None: break
        if best is None: i += 1; continue
        d = best
    # runs of a few words that only match zeros or pointers are noise: drop the short ones
    # between two runs of one shift
    out = []
    for s, e, dd in runs:
        sh = (b_base + 4 * (s + dd)) - (a_base + 4 * s)
        if out and out[-1][2] == sh: out[-1][1] = a_base + 4 * e
        else: out.append([a_base + 4 * s, a_base + 4 * e, sh])
    clean = []
    for k, r in enumerate(out):
        if 0 < k < len(out) - 1 and r[1] - r[0] <= 16 and out[k - 1][2] == out[k + 1][2] != r[2]:
            continue
        if clean and clean[-1][2] == r[2]: clean[-1][1] = r[1]
        else: clean.append(r)
    # close the gaps: a gap takes the shift of the run before it
    for k in range(len(clean) - 1): clean[k][1] = clean[k + 1][0]
    if clean: clean[0][0], clean[-1][1] = a_base, a_base + 4 * len(a_words)
    return clean


data = []
for name in ("core.data", "core.rdata", "core.bss", "core.lit", ".lit", ".bss", ".data", "lvl.vtbl", "lvl.camvtbl", "lvl.sndvtbl"):
    p, u = SP[name], SU[name]
    if p["nobits"]:   # (no bytes to compare: it moves as one; checked against the code below)
        data.append([p["addr"], p["addr"] + p["size"], u["addr"] - p["addr"], name])
        continue
    for lo, hi, sh in align(words(pal[p["off"]:p["off"] + p["size"]]), words(us[u["off"]:u["off"] + u["size"]]), p["addr"], u["addr"]):
        data.append([lo, hi, sh, name])


def data_shift(a):
    for lo, hi, sh, _ in data:
        if lo <= a < hi: return sh


# the data addresses paired functions build (lui + addiu/ori/load/store): both builds load the
# same data, so each is a PAL -> US pair. The executable's functions and every level's (level code
# uses the executable's data a lot).
def refs(b):
    w = struct.unpack("<%dI" % (len(b) // 4), b); hi = {}; out = {}
    for i, x in enumerate(w):
        op, rs, rt, imm = x >> 26, (x >> 21) & 31, (x >> 16) & 31, x & 0xFFFF
        if op == 0x0F: hi[rt] = imm << 16; continue
        if rs in hi and (op == 0x09 or 0x20 <= op <= 0x3F or op in (0x1A, 0x1B, 0x1E, 0x1F)):
            out[i] = (hi[rs] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
        if op in (0x09, 0x0D, 0x0F) or 0x20 <= op <= 0x27: hi.pop(rt, None)
    return out


anchor_votes = defaultdict(Counter)
RESIDENT = 0x0015F000   # PAL: from here up, each level program has its own lit/bss/data/tables
def collect(b1, b2, top):
    if not b1 or not b2: return
    r1, r2 = refs(b1), refs(b2)
    for i, x in r1.items():
        if i in r2 and SP["core.data"]["addr"] <= x < top:
            anchor_votes[x][r2[i] - x] += 1


for pa_s, (ua, sz, method) in exe_funcs.items():
    if method == "fingerprint":
        pa = int(pa_s, 16); collect(seg_bytes(pal, pa, sz), seg_bytes(us, ua, sz), SP[".text"]["addr"])
lv_text = {}
for lv in levels:
    try:
        tp = [r for r in json.load(open(os.path.join(PAL_LV, "level_" + lv, "manifest.json")))["records"] if r["name"] == "text"][0]
        tu = [r for r in json.load(open(os.path.join(US_LV, "level_" + lv, "manifest.json")))["records"] if r["name"] == "text"][0]
    except (OSError, IndexError):
        continue
    bp = open(os.path.join(PAL_LV, "level_" + lv, "text.bin"), "rb").read(); bu = open(os.path.join(US_LV, "level_" + lv, "text.bin"), "rb").read()
    lv_text[lv] = (tp["address"], bp, tu["address"], bu)
    for pa_s, (ua, sz, method) in levels[lv]["funcs"].items():
        if method != "fingerprint": continue
        pa = int(pa_s, 16)
        collect(bp[pa - tp["address"]:pa - tp["address"] + sz], bu[ua - tu["address"]:ua - tu["address"] + sz], RESIDENT)

# data sections move down (by 0x100 to 0x400): a "shift" outside that is a constant, not an address
def align_shift(a):
    for lo, hi, sh, _ in data:
        if lo <= a < hi: return sh


anchors = {}
for x, c in anchor_votes.items():
    sh, n = c.most_common(1)[0]
    if not (-0x600 <= sh <= -0x40 and sh % 4 == 0): continue
    # trusted when several references agree and none disagrees, or when the bytes agree
    # (a lone or contested pair is often base + a constant the compiler folded differently)
    if (n >= 2 and len(c) == 1) or sh == align_shift(x): anchors[x] = sh


def build(sec_runs):
    """the final runs for one section: anchors where the code has them; between two anchors of
    one shift, that shift; between anchors of different shifts, the alignment's boundary if it
    has one there, else halfway"""
    lo, hi = sec_runs[0][0], sec_runs[-1][1]
    pts = sorted((a, s) for a, s in anchors.items() if lo <= a < hi)
    if not pts: return [r[:3] for r in sec_runs]
    def align_at(a):
        for r in sec_runs:
            if r[0] <= a < r[1]: return r[2]
    out = []
    def put(a0, a1, sh):
        if a0 >= a1: return
        if out and out[-1][2] == sh and out[-1][1] == a0: out[-1][1] = a1
        else: out.append([a0, a1, sh])
    # before the first anchor: the alignment, as long as it agrees with that anchor, else the anchor's
    put(lo, pts[0][0], pts[0][1])
    for (a, s), (b, t) in zip(pts, pts[1:]):
        if s == t: put(a, b, s); continue
        # the boundary: the first address from a where the alignment says t
        cut = None
        for r in sec_runs:
            if r[2] == t and a < r[0] <= b: cut = r[0]; break
        if cut is None: cut = (a + (b - a) // 2) & ~3
        put(a, cut, s); put(cut, b, t)
    put(pts[-1][0], hi, pts[-1][1])
    return out


final = []
for name in ("core.data", "core.rdata", "core.bss", "core.lit", ".lit", ".bss", ".data", "lvl.vtbl", "lvl.camvtbl", "lvl.sndvtbl"):
    rs = [r for r in data if r[3] == name]
    for r in build(rs): final.append(r + [name])
data = final


def data_shift(a):
    for lo, hi, sh, _ in data:
        if lo <= a < hi: return sh


agree = sum(1 for x, s in anchors.items() if data_shift(x) == s)
print("data map: %d runs; %d code anchors (%d agree with the map)" % (len(data), len(anchors), agree))
for lo, hi, sh, n in data[:80]: print("   %-11s %08X-%08X %+X" % (n, lo, hi, sh))

# ---- level data ----------------------------------------------------------------------------------
level_data = {}
for lv in sorted(levels):
    pm = os.path.join(PAL_LV, "level_" + lv, "manifest.json"); um = os.path.join(US_LV, "level_" + lv, "manifest.json")
    if not (os.path.exists(pm) and os.path.exists(um)): continue
    P = {r["name"]: r for r in json.load(open(pm))["records"]}; U = {r["name"]: r for r in json.load(open(um))["records"]}
    runs = []
    for rec in ("lit", "bss", "data", "vtbl", "camvtbl", "sndvtbl"):
        p, u = P[rec], U[rec]
        if p["bytes"] == u["bytes"] or rec == "bss":
            runs.append([p["address"], p["address"] + p["bytes"], u["address"] - p["address"], rec]); continue
        pb = open(os.path.join(PAL_LV, "level_" + lv, rec + ".bin"), "rb").read()
        ub = open(os.path.join(US_LV, "level_" + lv, rec + ".bin"), "rb").read()
        for lo, hi, sh in align(words(pb), words(ub), p["address"], u["address"]): runs.append([lo, hi, sh, rec])
    level_data[lv] = runs

json.dump({"region": "us", "exe": "SCUS_971.99", "pal_exe": "SCES_509.16",
           "sections": {n: [SP[n]["addr"], SP[n]["size"], SU[n]["addr"], SU[n]["size"]] for n in SP if n in SU},
           "exe_funcs": exe_funcs, "exe_us_only": exe_us_only, "data": data,
           "levels": levels, "level_data": level_data}, open(OUT, "w"), separators=(",", ":"))
print("wrote %s: %d executable functions (%d US-only), %d levels" % (OUT, len(exe_funcs), len(exe_us_only), len(levels)))
