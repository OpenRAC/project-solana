#!/usr/bin/env python3
"""recomp.py GEN_DIR DECOMP_REPO

Ahead-of-time recompiler for the game's assembly-only functions.

Each function the decomp still has as assembly (gen/asm_entries.json) is read
from the user's own executable and translated, instruction by instruction,
into a C function over an RcCtx (the EE's registers, host/include/rc.h). The
C is compiled with the rest of the program: nothing is translated or
interpreted at run time.

Calls are bridged both ways:
  - C -> recompiled: a wrapper with the function's C signature (from the
    declarations the decomp's C makes of it, gen/sigs.json) loads the
    arguments into a0-t3 / f12-f19, runs the translation and returns v0/f0.
    It takes the function's own name, so it replaces the stub in asm_stubs.c
    and gets the trampoline at the function's PS2 address.
  - recompiled -> C: a thunk per C function reads the arguments from the
    registers by its signature and calls it.
  - recompiled -> recompiled: a direct C call sharing the register context.
  - by address (jalr, function pointers): rc_dispatch() looks the address up.

Writes GEN_DIR/rc_funcs.c, rc_wrappers.c, rc_table.c, and rewrites
asm_stubs.c without the functions it covers.

recomp.py LEVEL_GEN_DIR DECOMP_REPO --level NN --dump DUMP_DIR --exe-gen EXE_GEN_DIR

Level mode (docs/LEVELS.md): every function of level NN's program that the
level pass (gen_host.py --level) does not define in C is translated from the
level's own bytes (DUMP_DIR/level_NN/text.bin, tools/level_dump.py): its
assembly-only functions, the ones whose C has inline asm, and the copies of
the executable's game functions (their relocations are the level's own).
Functions and sizes come from config/overlays/functions.tsv; a function the
level has at several addresses is translated at each (func_X__lNNatADDR).
The generated symbols take the prefix rcNN_ (the executable's are rc_), the
dispatch table is rcNN_table, and the level's trampoline table
(uya_tramp_table_lNN) gets a row for every translated function.
"""
import glob, json, os, re, struct, sys

import rabbitizer
rabbitizer.config.pseudos_enablePseudos = False

GEN, REPO = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGION = os.environ.get("UYA_REGION", "pal")
HOSTDIR = os.environ.get("UYA_HOST") or os.path.join(HERE, "host")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import region as _region
RC = _region.consts(REGION)
LEVEL = DUMP = EXEGEN = None
_a = sys.argv[3:]
while _a:
    _o = _a.pop(0)
    if _o == "--level": LEVEL = int(_a.pop(0))
    elif _o == "--dump": DUMP = _a.pop(0)
    elif _o == "--exe-gen": EXEGEN = _a.pop(0)
    else: sys.exit("recomp.py: unknown option " + _o)
LV = "%02d" % LEVEL if LEVEL is not None else None
P = "rc_" if LEVEL is None else "rc%s_" % LV        # prefix of the generated symbols
RESIDENT = RC["resident"]   # PAL 0x0015F000

# ---- inputs -----------------------------------------------------------------
# testing: UYA_RC_TRACE="func_A,func_B" (build time) logs every call of those translated functions
TRACE_FUNCS = set(x for x in os.environ.get("UYA_RC_TRACE", "").split(",") if x)
EXE = os.environ.get("UYA_EXE") or os.path.join(REPO, "baserom", RC["exe"])   # build.py: the copy read from the disc image
img = open(EXE, "rb").read()
TEXT_LO = 0x00100080   # the executable's one loadable segment (PAL size 0x13E6B0)
TEXT_HI = TEXT_LO + struct.unpack_from("<I", img, struct.unpack_from("<I", img, 28)[0] + 16)[0]
LTEXT_LO = LTEXT_HI = 0
if LEVEL is not None:
    _ld = os.path.join(DUMP, "level_%s" % LV)
    _man = json.load(open(os.path.join(_ld, "manifest.json")))
    _tr = [r for r in _man["records"] if r["name"] == "text"][0]
    limg = open(os.path.join(_ld, "text.bin"), "rb").read() + b"\0" * 16
    LTEXT_LO, LTEXT_HI = _tr["address"], _tr["address"] + _tr["bytes"]


def word(a):
    if LTEXT_LO <= a < LTEXT_HI + 16:
        return struct.unpack_from("<I", limg, a - LTEXT_LO)[0]
    o = a - 0x100080 + 0x1000
    return struct.unpack_from("<I", img, o)[0]


report = json.load(open(os.path.join(REPO, "progress", "report.json")))
fsize = {}
for u in report["units"]:
    for f in u.get("functions", []):
        md = f.get("metadata") or {}
        if "virtual_address" in md:
            fsize[f["name"]] = (int(md["virtual_address"]), int(f["size"]))
sigs = json.load(open(os.path.join(GEN, "sigs.json")))
entries = json.load(open(os.path.join(GEN, "asm_entries.json")))
overrides = {os.path.basename(p)[:-2] for p in os.listdir(os.path.join(HOSTDIR, "overrides")) if p.endswith(".c")} if os.path.isdir(os.path.join(HOSTDIR, "overrides")) else set()

todo = [n for n in entries if n in fsize and n not in overrides and re.fullmatch(r"func_[0-9A-F]{8}", n)
        and not (n in sigs and sigs[n].get("src") == "def")
        and TEXT_LO <= fsize[n][0] < TEXT_HI]
# Sony libraries that are pure computation are translated too (libvu0: the
# sceVu0 matrix and vector routines, core_text 0x1252A0-0x125630)
EXTRA_RANGES = RC["extra_ranges"]   # PAL 0x001252A0-0x00125630, and libgcc 0x0011DF10-0x001207B8
                # (libgcc: 64-bit integer and soft-float helpers; level code calls some the executable's C does not)
todo += [n for n, (a, sz) in fsize.items() if any(lo <= a < hi for lo, hi in EXTRA_RANGES)
         and re.fullmatch(r"func_[0-9A-F]{8}", n) and not (n in sigs and sigs[n].get("src") == "def") and n not in overrides]
base_of = {}
if LEVEL is not None:
    # level mode: the level's functions from the catalogue, at the level's addresses
    fsize_exe = fsize
    fsize = {}
    PLACES = {}
    for l in open(os.path.join(REPO, "config", "overlays", "functions.tsv")):
        if l.startswith("#") or not l.strip(): continue
        f = l.rstrip("\n").split("\t")
        pl = [int(x.split(":")[1], 16) for x in f[5].split(",") if x.split(":")[0] == LV]
        if not pl: continue
        PLACES[f[0]] = pl
        for k, a in enumerate(pl):
            key = f[0] if k == 0 else "%s__l%sat%08X" % (f[0], LV, a)
            fsize[key] = (a, int(f[2])); base_of[key] = f[0]
    # dispatch-table pointers that are not a catalogued start (docs/OVERLAYS.md,
    # Roles: InitCamera_7 is 0x10 into func_L00_002EDB58): an entry of their own,
    # translated from there to the end of the function they fall in
    _starts = sorted((a, a + s) for a, s in fsize.values())
    _is_start = {a for a, _ in _starts}
    for _r in _man["records"]:
        if _r["name"] not in ("vtbl", "camvtbl", "sndvtbl"): continue
        _b = open(os.path.join(_ld, _r["name"] + ".bin"), "rb").read()
        for _i in range(0, len(_b) - 3, 4):
            _v = struct.unpack_from("<I", _b, _i)[0]
            if LTEXT_LO <= _v < LTEXT_HI and _v % 4 == 0 and _v not in _is_start:
                _own = [(a, e) for a, e in _starts if a < _v < e]
                if _own:
                    key = "func_l%s_entry_%08X" % (LV, _v)
                    fsize[key] = (_v, _own[0][1] - _v); base_of[key] = key; _is_start.add(_v)
                    print("level %s: table entry 0x%08X inside the function at 0x%08X: translated as %s" % (LV, _v, _own[0][0], key))
    # the host's versions of executable functions the catalogue does not place
    # in this level (func_001F9988, the busy wait the DMA loops call, is split
    # into an uncatalogued run of nops and func_L00_001FEFE0): found by the
    # executable's bytes, which have no relocations
    _host_rows = {}
    _exe_size = {n: s for n, (a, s) in fsize_exe.items()}
    for _l in open(os.path.join(GEN, "host_main.txt")):
        _n, _kind = _l.split()
        if _n in PLACES or _n not in fsize_exe: continue
        _a, _s = fsize_exe[_n]
        _pat = bytes(img[_a - 0x100080 + 0x1000:_a - 0x100080 + 0x1000 + _s])
        _o = limg.find(_pat)
        _cat = {a: s for a, s in fsize.values()}
        if _o >= 0 and _o % 4 == 0 and _o < LTEXT_HI - LTEXT_LO and _cat.get(LTEXT_LO + _o, _s) == _s:
            _host_rows[LTEXT_LO + _o] = _n
            print("level %s: %s (host) found at 0x%08X by its bytes" % (LV, _n, LTEXT_LO + _o))
            if _kind == "override":
                with open(os.path.join(GEN, "overrides.txt"), "a") as _f: _f.write(_n + "\n")
    _is_start |= set(_host_rows)
    import bisect
    # branches past a function's catalogued end into bytes no catalogued function
    # covers (level 16's func_L16_002E7138 branches to 0x2E715C): the function is
    # longer than the catalogue says, up to the next start
    _changed = True
    while _changed:
        _changed = False
        _spans = sorted((a, a + sz) for a, sz in fsize.values())
        _span_lo = [a for a, _ in _spans]
        def _covered(v):
            k = bisect.bisect_right(_span_lo, v) - 1
            return any(_spans[j][0] <= v < _spans[j][1] for j in range(max(0, k - 8), k + 1))
        for key, (a, sz) in list(fsize.items()):
            for pc in range(a, a + sz, 4):
                if not (LTEXT_LO <= pc < LTEXT_HI): break
                w = struct.unpack_from("<I", limg, pc - LTEXT_LO)[0]
                d = rabbitizer.Instruction(w, vram=pc, category=rabbitizer.InstrCategory.R5900)
                if d.getOpcodeName() == "j": t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                elif d.isBranch() and not d.doesLink(): t = d.getBranchVramGeneric()
                else: continue
                if not (a + sz <= t < LTEXT_HI) or _covered(t): continue
                k = bisect.bisect_right(_span_lo, t)
                end = _spans[k][0] if k < len(_spans) else LTEXT_HI
                fsize[key] = (a, end - a); _changed = True
                print("level %s: %s branches to 0x%08X past its end: extended to 0x%08X" % (LV, key, t, end))
                break
            if _changed: break
    # function pointers to an address inside a catalogued function: an entry of their
    # own (level 06's boss health bar, func_L06_0024A080, really starts at 0x24A0A0, 32
    # bytes into the catalogued function, and the HUD calls it through a pointer the code
    # builds with lui + addiu): addresses the code builds that way (~20 a level)
    # (not the data records' words: those are mostly switch tables, ~900 case labels a level)
    _cands = set()
    _hi = {}
    for _i in range(0, LTEXT_HI - LTEXT_LO, 4):
        _w = struct.unpack_from("<I", limg, _i)[0]
        _op, _rs, _rt, _imm = _w >> 26, (_w >> 21) & 31, (_w >> 16) & 31, _w & 0xFFFF
        if _op == 0x0F: _hi[_rt] = (_imm << 16, _i); continue              # lui
        if _op in (0x09, 0x0D) and _rs in _hi and _i - _hi[_rs][1] <= 64:  # addiu / ori within 16 instructions
            _v = (_hi[_rs][0] + (_imm - 0x10000 if (_op == 0x09 and _imm & 0x8000) else _imm)) & 0xFFFFFFFF
            _cands.add(_v)
    _starts_sorted = sorted((a, e) for a, e in _starts)
    _lo_list = [a for a, _ in _starts_sorted]
    _n_ptr = 0
    for _v in sorted(_cands):
        if not (LTEXT_LO <= _v < LTEXT_HI) or _v % 4 or _v in _is_start: continue
        _k = bisect.bisect_right(_lo_list, _v) - 1
        if _k < 0 or not (_starts_sorted[_k][0] < _v < _starts_sorted[_k][1]): continue
        key = "func_l%s_entry_%08X" % (LV, _v)
        fsize[key] = (_v, _starts_sorted[_k][1] - _v); base_of[key] = key; _is_start.add(_v); _n_ptr += 1
    print("level %s: %d function pointers into catalogued functions: entries of their own" % (LV, _n_ptr))
    # calls (jal) to an address that starts no catalogued function: an entry of
    # their own, up to the next start
    _sorted_starts = sorted(_is_start)
    for _i in range(0, LTEXT_HI - LTEXT_LO, 4):
        _w = struct.unpack_from("<I", limg, _i)[0]
        if _w >> 26 != 3: continue
        _v = ((LTEXT_LO + _i + 4) & 0xF0000000) | ((_w & 0x3FFFFFF) << 2)
        if not (LTEXT_LO <= _v < LTEXT_HI) or _v in _is_start: continue
        _k = bisect.bisect_right(_sorted_starts, _v)
        _end = _sorted_starts[_k] if _k < len(_sorted_starts) else LTEXT_HI
        key = "func_l%s_entry_%08X" % (LV, _v)
        fsize[key] = (_v, _end - _v); base_of[key] = key; _is_start.add(_v)
        print("level %s: call target 0x%08X starts no catalogued function: translated as %s" % (LV, _v, key))
    exe_sigs = json.load(open(os.path.join(EXEGEN, "sigs.json")))
    lsigs = sigs
    level_c = {n for n, s in lsigs.items() if s.get("src") == "def" and s.get("addr") is not None and s["addr"] >= RESIDENT
               and re.fullmatch(r"func_(L\d\d_)?[0-9A-F]{8}", n)}
    todo = [k for k in fsize if base_of[k] not in level_c]
    # signatures: the level's, the executable's definitions of the core it calls
    sigs = dict(lsigs)
    for n, s in exe_sigs.items():
        if s.get("src") == "def" and s.get("addr") is not None and s["addr"] < RESIDENT: sigs[n] = s
    c_funcs = {}
    # a C function stands for each of its places only where the bytes are the
    # same as at its first place: the catalogue's fingerprints leave out call
    # targets, so tiny wrappers that call different functions share a name
    # (func_L00_00237B70, the HUD's, is also at 0x0025B458, a colour blend's).
    # The other places are translated from their own bytes.
    _lb = lambda a, n_: bytes(limg[a - LTEXT_LO:a - LTEXT_LO + n_])
    _n_diff = 0
    for n in level_c:
        pl = PLACES.get(n, [lsigs[n]["addr"]])
        for k, a in enumerate(pl):
            if k and n in PLACES and LTEXT_LO <= a < LTEXT_HI and LTEXT_LO <= pl[0] < LTEXT_HI:
                sz = fsize[n][1]
                if _lb(a, sz) != _lb(pl[0], sz):
                    todo.append("%s__l%sat%08X" % (n, LV, a)); _n_diff += 1
                    continue
            c_funcs[a] = n
    if _n_diff: print("level %s: %d places of C functions differ from the first place's bytes: translated" % (LV, _n_diff))
    for a, n in _host_rows.items():
        c_funcs[a] = n
        if n not in sigs and n in exe_sigs: sigs[n] = exe_sigs[n]
    for n, s in sigs.items():
        if s.get("src") == "def" and s.get("addr") is not None and s["addr"] < RESIDENT and re.fullmatch(r"func_[0-9A-F]{8}", n):
            c_funcs.setdefault(s["addr"], n)
    # the core's translated functions and thunks (the executable pass's): the level calls them directly
    core_rc = {int(a, 16): f for a, f in re.findall(r"\{0x([0-9A-F]{8}), (rc_\w+)\},", open(os.path.join(EXEGEN, "rc_table.c")).read())
               if int(a, 16) < RESIDENT}
else:
    c_funcs = {s["addr"]: n for n, s in sigs.items() if s.get("src") == "def" and re.fullmatch(r"func_[0-9A-F]{8}", n)}
    core_rc = {}
    # code addresses the C names as data (`m->f74 = D_0023B578`, a second
    # entry point 0x68 into the assembly-only func_0023B510): an entry of
    # their own, translated from there to the end of the function they fall in
    _tset = set(todo)
    _starts_x = sorted((fsize[n][0], fsize[n][0] + fsize[n][1]) for n in _tset)
    _is_start_x = {a for a, _ in _starts_x}
    _abs = os.path.join(GEN, "abs_syms.s")
    _cand = {int(x, 16) for x in re.findall(r"\.set\s+_?D_([0-9A-F]{8})\w*,", open(_abs).read())} if os.path.exists(_abs) else set()
    _cand = {v for v in _cand if RC["main_text"][0] <= v < RC["main_text"][1]}
    # and functions the C (the executable's and the levels') calls by an address
    # inside an assembly-only function: func_001252C0 is 0x20 into libvu0's
    # func_001252A0 (the levels' hero code calls it)
    for _root, _ds, _fs in os.walk(os.path.join(REPO, "src")):
        for _f in _fs:
            if _f.endswith(".c"):
                _cand |= {int(x, 16) for x in re.findall(r"\bfunc_([0-9A-F]{8})\b", open(os.path.join(_root, _f), errors="replace").read())}
    # and core addresses the level programs call with jal inside a function (level 17
    # calls 0x125328, 0x88 into libvu0's func_001252A0): from the levels' code that
    # build.py extracted (build/levels/level_NN/text.bin; the core, below 0x15F000,
    # is the same in every program)
    for _tb in sorted(glob.glob(os.path.join(HERE, "build", "levels", "level_*", "text.bin"))):
        _d = open(_tb, "rb").read()
        for _i in range(0, len(_d) - 3, 4):
            _w = struct.unpack_from("<I", _d, _i)[0]
            if _w >> 26 == 3:
                _t = (_w & 0x3FFFFFF) << 2
                if TEXT_LO <= _t < RESIDENT: _cand.add(_t)
    for _v in sorted(_cand):
        if not (TEXT_LO <= _v < TEXT_HI) or _v % 4 or _v in _is_start_x: continue
        _own = [(a, e) for a, e in _starts_x if a < _v < e]
        if not _own: continue
        key = "func_entry_%08X" % _v
        fsize[key] = (_v, _own[0][1] - _v); base_of[key] = key; todo.append(key); _is_start_x.add(_v)
        print("address 0x%08X named in the C is inside the function at 0x%08X: translated as %s" % (_v, _own[0][0], key))
todo = sorted(set(todo), key=lambda n: fsize[n][0])
rc_addr = {fsize[n][0]: n for n in todo}
# every known function start (for "is this a call to another function")
starts = {}
for n, (a, s) in fsize.items():
    if re.fullmatch(r"func_[0-9A-F]{8}", n): starts[a] = n
for n, s in sigs.items():
    if s.get("addr") is not None and re.fullmatch(r"func_[0-9A-F]{8}", n): starts.setdefault(s["addr"], n)


# ---- helpers ----------------------------------------------------------------
def R(n):
    return "0ULL" if n == 0 else "c->r[%d].d[0]" % n


def W(n, expr):
    return "" if n == 0 else "c->r[%d].d[0] = %s;" % (n, expr)


def S16(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def EA(w):
    rs = (w >> 21) & 31
    return "(u32)(%s + %d)" % (R(rs), S16(w))


warnings = {}


def warn(op, pc):
    warnings.setdefault(op, []).append(pc)


DESTS = "xyzw"


def dest_of(w):
    d = (w >> 21) & 15
    return [i for i in range(4) if d & (8 >> i)]


def vu_parts(w):
    return (w >> 6) & 31, (w >> 11) & 31, (w >> 16) & 31   # fd, fs, ft


def vset(fd, comps, exprs):
    """assign VF[fd][i] = exprs[i] for i in comps, via temporaries (sources may alias fd)"""
    if fd == 0:
        return "/* write to vf0 ignored */"
    s = "{ " + " ".join("f32 t%d = %s;" % (i, exprs[i]) for i in comps)
    s += " " + " ".join("VF(%d)[%d] = t%d;" % (fd, i, i) for i in comps) + " }"
    return s


def aset(comps, exprs):
    s = "{ " + " ".join("f32 t%d = %s;" % (i, exprs[i]) for i in comps)
    s += " " + " ".join("VACC[%d] = t%d;" % (i, i) for i in comps) + " }"
    return s


def vu_op(name, w, pc):
    fd, fs, ft = vu_parts(w)
    comps = dest_of(w)
    bc = w & 3
    m = re.match(r"v(add|sub|mul|madd|msub|max|mini)(a?)(x|y|z|w|q|i)?$", name)
    if m:
        op, acc, src = m.group(1), m.group(2), m.group(3)
        if acc:  # special2 layout: no fd
            pass

        def b(i):
            if src in ("x", "y", "z", "w"): return "VF(%d)[%d]" % (ft, DESTS.index(src))
            if src == "q": return "rc_vu0.q"
            if src == "i": return "rc_vu0.i"
            return "VF(%d)[%d]" % (ft, i)
        ex = {}
        for i in comps:
            a_, b_ = "VF(%d)[%d]" % (fs, i), b(i)
            if op == "add": e = "rc_fc(%s + %s)" % (a_, b_)            # rc_fc: the VU clamps to +-MAX
            elif op == "sub": e = "rc_fc(%s - %s)" % (a_, b_)
            elif op == "mul": e = "rc_fc(%s * %s)" % (a_, b_)
            elif op == "madd": e = "rc_fc(VACC[%d] + rc_fc(%s * %s))" % (i, a_, b_)
            elif op == "msub": e = "rc_fc(VACC[%d] - rc_fc(%s * %s))" % (i, a_, b_)
            elif op == "max": e = "fmaxf(%s, %s)" % (a_, b_)
            else: e = "fminf(%s, %s)" % (a_, b_)
            ex[i] = e
        if not acc and fd == 0:
            # vf0 stays (0,0,0,1), but the op still sets the MAC/status flags: the game compares this way
            # (`vsubw.w vf0, ...` then `cfc2 $at, $vi17; andi 0x10` skips point lights out of range)
            if not NEED_VU0_FLAGS or op in ("max", "mini"): return "/* write to vf0 ignored */"
            return "{ f32 vz[4] = { 0, 0, 0, 0 }; " + " ".join("vz[%d] = %s;" % (i, ex[i]) for i in comps) + \
                " rc_vu0_flags(vz, %du); }" % ((w >> 21) & 15)
        code = aset(comps, ex) if acc else vset(fd, comps, ex)
        if NEED_VU0_FLAGS and op not in ("max", "mini") and (acc or fd != 0):      # FMAC ops set the MAC/status flags
            code += " rc_vu0_flags(%s, %du);" % ("VACC" if acc else "VF(%d)" % fd, (w >> 21) & 15)
        return code
    if name == "vopmula":
        ex = {0: "rc_fc(VF(%d)[1] * VF(%d)[2])" % (fs, ft), 1: "rc_fc(VF(%d)[2] * VF(%d)[0])" % (fs, ft), 2: "rc_fc(VF(%d)[0] * VF(%d)[1])" % (fs, ft)}
        return aset([i for i in comps if i < 3], ex) + (" rc_vu0_flags(VACC, %du);" % (((w >> 21) & 15) & 14) if NEED_VU0_FLAGS else "")
    if name == "vopmsub":
        ex = {0: "rc_fc(VACC[0] - rc_fc(VF(%d)[1] * VF(%d)[2]))" % (fs, ft), 1: "rc_fc(VACC[1] - rc_fc(VF(%d)[2] * VF(%d)[0]))" % (fs, ft), 2: "rc_fc(VACC[2] - rc_fc(VF(%d)[0] * VF(%d)[1]))" % (fs, ft)}
        cs = [i for i in comps if i < 3]
        if fd == 0 and NEED_VU0_FLAGS:   # vf0 stays; the flags are set (see the FMAC ops above)
            return "{ f32 vz[4] = { 0, 0, 0, 0 }; " + " ".join("vz[%d] = %s;" % (i, ex[i]) for i in cs) + " rc_vu0_flags(vz, %du); }" % (((w >> 21) & 15) & 14)
        return vset(fd, cs, ex) + ("" if fd == 0 or not NEED_VU0_FLAGS else " rc_vu0_flags(VF(%d), %du);" % (fd, ((w >> 21) & 15) & 14))
    if name in ("vmove", "vmr32", "vabs"):
        # special2: ft is the destination, fs the source
        if name == "vmove": ex = {i: "VF(%d)[%d]" % (fs, i) for i in comps}
        elif name == "vabs": ex = {i: "__builtin_fabsf(VF(%d)[%d])" % (fs, i) for i in comps}
        else: ex = {i: "VF(%d)[%d]" % (fs, (i + 1) & 3) for i in comps}
        return vset(ft, comps, ex)
    m = re.match(r"v(ftoi|itof)(0|4|12|15)$", name)
    if m:
        sh = int(m.group(2))
        if m.group(1) == "ftoi":
            if ft == 0: return "/* ftoi to vf0 */"
            return "{ " + " ".join("s32 t%d = rc_ftoi(VF(%d)[%d], %d);" % (i, fs, i, sh) for i in comps) + " " + \
                " ".join("__builtin_memcpy(&VF(%d)[%d], &t%d, 4);" % (ft, i, i) for i in comps) + " }"
        ex = {}
        for i in comps:
            ex[i] = "rc_itof(*(s32 *)&VF(%d)[%d], %d)" % (fs, i, sh)
        return vset(ft, comps, ex)
    if name == "vclipw":
        return ("{ f32 w_ = __builtin_fabsf(VF(%d)[3]); u32 fl = (VF(%d)[0] > w_) | ((VF(%d)[0] < -w_) << 1) | ((VF(%d)[1] > w_) << 2) | "
                "((VF(%d)[1] < -w_) << 3) | ((VF(%d)[2] > w_) << 4) | ((VF(%d)[2] < -w_) << 5); "
                "rc_vu0.clip = ((rc_vu0.clip << 6) | fl) & 0xFFFFFF; }") % (ft, fs, fs, fs, fs, fs, fs)
    if name in ("vdiv", "vsqrt", "vrsqrt"):
        fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
        if name == "vdiv": return "rc_vu0.q = rc_fc(rc_fdiv(VF(%d)[%d], VF(%d)[%d]));" % (fs, fsf, ft, ftf)
        if name == "vsqrt": return "rc_vu0.q = rc_fsqrt(VF(%d)[%d]);" % (ft, ftf)
        return "rc_vu0.q = rc_fdiv(VF(%d)[%d], rc_fsqrt(VF(%d)[%d]));" % (fs, fsf, ft, ftf)
    if name in ("vwaitq", "vnop"): return ";"
    if name in ("vsqi", "vsqd", "vlqi", "vlqd"):
        # vsqi fs, (it++): VU0 data memory at 0x11004000, 16 bytes per address unit
        is_, it = (w >> 11) & 31, (w >> 16) & 31
        if name in ("vsqi", "vsqd"):
            pre = "rc_vu0.vi[%d] = (rc_vu0.vi[%d] - 1) & 0xFFFF; " % (it, it) if name == "vsqd" and it else ""
            post = " if (%d) rc_vu0.vi[%d] = (rc_vu0.vi[%d] + 1) & 0xFFFF;" % (it, it, it) if name == "vsqi" else ""
            return "{ %sf32 *p = (f32 *)(uintptr_t)(0x11004000u + ((rc_vu0.vi[%d] & 0xFF) << 4)); %s%s }" % (
                pre, it, " ".join("p[%d] = VF(%d)[%d];" % (i, is_, i) for i in comps), post)
        pre = "rc_vu0.vi[%d] = (rc_vu0.vi[%d] - 1) & 0xFFFF; " % (is_, is_) if name == "vlqd" and is_ else ""
        post = " if (%d) rc_vu0.vi[%d] = (rc_vu0.vi[%d] + 1) & 0xFFFF;" % (is_, is_, is_) if name == "vlqi" else ""
        if it == 0: return "/* vlq to vf0 */"
        return "{ %sf32 *p = (f32 *)(uintptr_t)(0x11004000u + ((rc_vu0.vi[%d] & 0xFF) << 4)); %s%s }" % (
            pre, is_, " ".join("VF(%d)[%d] = p[%d];" % (it, i, i) for i in comps), post)
    if name in ("viadd", "visub", "viand", "vior"):
        id_, is_, it = (w >> 6) & 31, (w >> 11) & 31, (w >> 16) & 31
        op = {"viadd": "+", "visub": "-", "viand": "&", "vior": "|"}[name]
        return "" if id_ == 0 else "rc_vu0.vi[%d] = (rc_vu0.vi[%d] %s rc_vu0.vi[%d]) & 0xFFFF;" % (id_ & 15, is_ & 15, op, it & 15)
    if name == "viaddi":
        it, is_, imm = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        if imm & 16: imm -= 32
        return "" if it == 0 else "rc_vu0.vi[%d] = (rc_vu0.vi[%d] + %d) & 0xFFFF;" % (it & 15, is_ & 15, imm)
    if name == "vmtir":
        it, fs_, fsf = (w >> 16) & 31, (w >> 11) & 31, (w >> 21) & 3
        return "" if it == 0 else "rc_vu0.vi[%d] = rc_fbits(VF(%d)[%d]) & 0xFFFF;" % (it & 15, fs_, fsf)
    if name == "vmfir":
        ft_, is_ = (w >> 16) & 31, (w >> 11) & 31
        return "" if ft_ == 0 else " ".join("{ s32 v = (s16)rc_vu0.vi[%d]; __builtin_memcpy(&VF(%d)[%d], &v, 4); }" % (is_ & 15, ft_, i) for i in comps)
    if name in ("vcallms",):
        return "rc_vcallms(c, 0x%X);" % (((w >> 6) & 0x7FFF) * 8)
    if name in ("vcallmsr",):
        return "rc_vcallms(c, (rc_vu0.cmsar0 & 0xFFFF) * 8);"   # CMSAR0 (ctc2 $27 below)
    warn(name, pc)
    return 'rc_bad(0x%08X, "%s");' % (pc, name)


def mmi_op(name, w, pc):
    rs, rt, rd, sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    S, T = "c->r[%d]" % rs, "c->r[%d]" % rt

    def out(body):  # body computes R128 x from s, t
        if rd == 0: return "/* %s to $0 */" % name
        return "{ R128 s = %s, t = %s, x; (void)s; (void)t; %s c->r[%d] = x; }" % (S if rs else "(R128){{0,0}}", T if rt else "(R128){{0,0}}", body, rd)
    lanes = {
        "pextlw": "x.w[0]=t.w[0]; x.w[1]=s.w[0]; x.w[2]=t.w[1]; x.w[3]=s.w[1];",
        "pextuw": "x.w[0]=t.w[2]; x.w[1]=s.w[2]; x.w[2]=t.w[3]; x.w[3]=s.w[3];",
        "pextlh": "for (int i=0;i<4;i++){x.h[2*i]=t.h[i]; x.h[2*i+1]=s.h[i];}",
        "pextuh": "for (int i=0;i<4;i++){x.h[2*i]=t.h[4+i]; x.h[2*i+1]=s.h[4+i];}",
        "pextlb": "for (int i=0;i<8;i++){x.b[2*i]=t.b[i]; x.b[2*i+1]=s.b[i];}",
        "pextub": "for (int i=0;i<8;i++){x.b[2*i]=t.b[8+i]; x.b[2*i+1]=s.b[8+i];}",
        "ppacw": "x.w[0]=t.w[0]; x.w[1]=t.w[2]; x.w[2]=s.w[0]; x.w[3]=s.w[2];",
        "ppach": "for (int i=0;i<4;i++){x.h[i]=t.h[2*i]; x.h[4+i]=s.h[2*i];}",
        "ppacb": "for (int i=0;i<8;i++){x.b[i]=t.b[2*i]; x.b[8+i]=s.b[2*i];}",
        "pcpyld": "x.d[0]=t.d[0]; x.d[1]=s.d[0];",
        "pcpyud": "x.d[0]=s.d[1]; x.d[1]=t.d[1];",
        "pcpyh": "for (int i=0;i<4;i++){x.h[i]=t.h[0]; x.h[4+i]=t.h[4];}",
        "pexew": "x.w[0]=t.w[2]; x.w[1]=t.w[1]; x.w[2]=t.w[0]; x.w[3]=t.w[3];",
        "pexcw": "x.w[0]=t.w[0]; x.w[1]=t.w[2]; x.w[2]=t.w[1]; x.w[3]=t.w[3];",
        "pexch": "x.h[0]=t.h[0];x.h[1]=t.h[2];x.h[2]=t.h[1];x.h[3]=t.h[3];x.h[4]=t.h[4];x.h[5]=t.h[6];x.h[6]=t.h[5];x.h[7]=t.h[7];",
        "pexeh": "x.h[0]=t.h[2];x.h[1]=t.h[1];x.h[2]=t.h[0];x.h[3]=t.h[3];x.h[4]=t.h[6];x.h[5]=t.h[5];x.h[6]=t.h[4];x.h[7]=t.h[7];",
        "prot3w": "x.w[0]=t.w[1]; x.w[1]=t.w[2]; x.w[2]=t.w[0]; x.w[3]=t.w[3];",
        "pext5": "for (int i=0;i<4;i++){u32 v=t.w[i]; x.w[i]=((v&0x1F)<<3)|(((v>>5)&0x1F)<<11)|(((v>>10)&0x1F)<<19)|((v&0x8000)<<16);}",
        "ppac5": "for (int i=0;i<4;i++){u32 v=t.w[i]; x.w[i]=((v>>3)&0x1F)|(((v>>11)&0x1F)<<5)|(((v>>19)&0x1F)<<10)|((v>>16)&0x8000);}",
        "paddw": "for (int i=0;i<4;i++) x.w[i]=s.w[i]+t.w[i];",
        "psubw": "for (int i=0;i<4;i++) x.w[i]=s.w[i]-t.w[i];",
        "paddh": "for (int i=0;i<8;i++) x.h[i]=s.h[i]+t.h[i];",
        "psubh": "for (int i=0;i<8;i++) x.h[i]=s.h[i]-t.h[i];",
        "paddb": "for (int i=0;i<16;i++) x.b[i]=s.b[i]+t.b[i];",
        "psubb": "for (int i=0;i<16;i++) x.b[i]=s.b[i]-t.b[i];",
        "padduw": "for (int i=0;i<4;i++){u64 v=(u64)s.w[i]+t.w[i]; x.w[i]=v>0xFFFFFFFFu?0xFFFFFFFFu:(u32)v;}",
        "paddsw": "for (int i=0;i<4;i++){s64 v=(s64)s.sw[i]+t.sw[i]; x.sw[i]=v>0x7FFFFFFF?0x7FFFFFFF:v<-0x7FFFFFFF-1?(-0x7FFFFFFF-1):(s32)v;}",
        "psubsb": "for (int i=0;i<16;i++){int v=s.sb[i]-t.sb[i]; x.sb[i]=v>127?127:v<-128?-128:v;}",
        "paddsb": "for (int i=0;i<16;i++){int v=s.sb[i]+t.sb[i]; x.sb[i]=v>127?127:v<-128?-128:v;}",
        "paddub": "for (int i=0;i<16;i++){int v=s.b[i]+t.b[i]; x.b[i]=v>255?255:v;}",
        "psubub": "for (int i=0;i<16;i++){int v=s.b[i]-t.b[i]; x.b[i]=v<0?0:v;}",
        "paddsh": "for (int i=0;i<8;i++) x.sh[i]=rc_clamp16(s.sh[i]+t.sh[i]);",
        "psubsh": "for (int i=0;i<8;i++) x.sh[i]=rc_clamp16(s.sh[i]-t.sh[i]);",
        "pmaxw": "for (int i=0;i<4;i++) x.sw[i]=s.sw[i]>t.sw[i]?s.sw[i]:t.sw[i];",
        "pminw": "for (int i=0;i<4;i++) x.sw[i]=s.sw[i]<t.sw[i]?s.sw[i]:t.sw[i];",
        "pmaxh": "for (int i=0;i<8;i++) x.sh[i]=s.sh[i]>t.sh[i]?s.sh[i]:t.sh[i];",
        "pminh": "for (int i=0;i<8;i++) x.sh[i]=s.sh[i]<t.sh[i]?s.sh[i]:t.sh[i];",
        "pceqw": "for (int i=0;i<4;i++) x.w[i]=s.w[i]==t.w[i]?0xFFFFFFFFu:0;",
        "pcgtw": "for (int i=0;i<4;i++) x.w[i]=s.sw[i]>t.sw[i]?0xFFFFFFFFu:0;",
        "pceqh": "for (int i=0;i<8;i++) x.h[i]=s.h[i]==t.h[i]?0xFFFF:0;",
        "pcgth": "for (int i=0;i<8;i++) x.h[i]=s.sh[i]>t.sh[i]?0xFFFF:0;",
        "pceqb": "for (int i=0;i<16;i++) x.b[i]=s.b[i]==t.b[i]?0xFF:0;",
        "pcgtb": "for (int i=0;i<16;i++) x.b[i]=s.sb[i]>t.sb[i]?0xFF:0;",
        "pand": "x.d[0]=s.d[0]&t.d[0]; x.d[1]=s.d[1]&t.d[1];",
        "por": "x.d[0]=s.d[0]|t.d[0]; x.d[1]=s.d[1]|t.d[1];",
        "pxor": "x.d[0]=s.d[0]^t.d[0]; x.d[1]=s.d[1]^t.d[1];",
        "pnor": "x.d[0]=~(s.d[0]|t.d[0]); x.d[1]=~(s.d[1]|t.d[1]);",
        "pabsw": "for (int i=0;i<4;i++) x.sw[i]=t.sw[i]<0?-t.sw[i]:t.sw[i];",
        "pabsh": "for (int i=0;i<8;i++) x.sh[i]=t.sh[i]<0?-t.sh[i]:t.sh[i];",
        "plzcw": "x=c->r[%d]; x.w[0]=(u32)rc_plzcw1(s.w[0]); x.w[1]=(u32)rc_plzcw1(s.w[1]);" % rd,
        "psllw": "for (int i=0;i<4;i++) x.w[i]=t.w[i]<<%d;" % sa,
        "psrlw": "for (int i=0;i<4;i++) x.w[i]=t.w[i]>>%d;" % sa,
        "psraw": "for (int i=0;i<4;i++) x.sw[i]=t.sw[i]>>%d;" % sa,
        "psllh": "for (int i=0;i<8;i++) x.h[i]=(u16)(t.h[i]<<%d);" % (sa & 15),
        "psrlh": "for (int i=0;i<8;i++) x.h[i]=t.h[i]>>%d;" % (sa & 15),
        "psrah": "for (int i=0;i<8;i++) x.sh[i]=t.sh[i]>>%d;" % (sa & 15),
        "psllvw": "x.d[0]=SE32(t.w[0]<<(s.w[0]&31)); x.d[1]=SE32(t.w[2]<<(s.w[2]&31));",
        "psrlvw": "x.d[0]=SE32(t.w[0]>>(s.w[0]&31)); x.d[1]=SE32(t.w[2]>>(s.w[2]&31));",
        "psravw": "x.d[0]=SE32(t.sw[0]>>(s.w[0]&31)); x.d[1]=SE32(t.sw[2]>>(s.w[2]&31));",
        "pinth": "for (int i=0;i<4;i++){x.h[2*i]=t.h[i]; x.h[2*i+1]=s.h[4+i];}",
        "pinteh": "for (int i=0;i<4;i++){x.h[2*i]=t.h[2*i]; x.h[2*i+1]=s.h[2*i];}",
        "prevh": "x.h[0]=t.h[3];x.h[1]=t.h[2];x.h[2]=t.h[1];x.h[3]=t.h[0];x.h[4]=t.h[7];x.h[5]=t.h[6];x.h[6]=t.h[5];x.h[7]=t.h[4];",
        "pmultuw": "x.d[0]=(u64)s.w[0]*t.w[0]; x.d[1]=(u64)s.w[2]*t.w[2]; c->lo.d[0]=SE32((u32)x.d[0]); c->hi.d[0]=SE32((u32)(x.d[0]>>32)); c->lo.d[1]=SE32((u32)x.d[1]); c->hi.d[1]=SE32((u32)(x.d[1]>>32));",
        "pmultw": "x.sd[0]=(s64)s.sw[0]*t.sw[0]; x.sd[1]=(s64)s.sw[2]*t.sw[2]; c->lo.d[0]=SE32((u32)x.d[0]); c->hi.d[0]=SE32((u32)(x.d[0]>>32)); c->lo.d[1]=SE32((u32)x.d[1]); c->hi.d[1]=SE32((u32)(x.d[1]>>32));",
        "pmulth": "s32 p[8]; for (int i=0;i<8;i++) p[i]=(s32)s.sh[i]*t.sh[i]; x.sw[0]=p[0];x.sw[1]=p[2];x.sw[2]=p[4];x.sw[3]=p[6]; c->lo.sw[0]=p[0];c->lo.sw[1]=p[1];c->lo.sw[2]=p[4];c->lo.sw[3]=p[5]; c->hi.sw[0]=p[2];c->hi.sw[1]=p[3];c->hi.sw[2]=p[6];c->hi.sw[3]=p[7];",
    }
    if name in lanes:
        if name in ("pmultuw", "pmultw", "pmulth") and rd == 0:
            return "{ R128 s = %s, t = %s, x; %s }" % (S, T, lanes[name])
        return out(lanes[name])
    if name == "pmfhl.sh":
        return out("x.sh[0]=rc_clamp16(c->lo.sw[0]); x.sh[1]=rc_clamp16(c->lo.sw[1]); x.sh[2]=rc_clamp16(c->hi.sw[0]); x.sh[3]=rc_clamp16(c->hi.sw[1]); "
                   "x.sh[4]=rc_clamp16(c->lo.sw[2]); x.sh[5]=rc_clamp16(c->lo.sw[3]); x.sh[6]=rc_clamp16(c->hi.sw[2]); x.sh[7]=rc_clamp16(c->hi.sw[3]);")
    if name == "pmfhl.lw":
        return out("x.w[0]=c->lo.w[0]; x.w[1]=c->hi.w[0]; x.w[2]=c->lo.w[2]; x.w[3]=c->hi.w[2];")
    if name == "pmfhl.uw":
        return out("x.w[0]=c->lo.w[1]; x.w[1]=c->hi.w[1]; x.w[2]=c->lo.w[3]; x.w[3]=c->hi.w[3];")
    if name == "pmflo": return out("x=c->lo;")
    if name == "pmfhi": return out("x=c->hi;")
    if name == "pmtlo": return "c->lo = c->r[%d];" % rs
    if name == "pmthi": return "c->hi = c->r[%d];" % rs
    warn(name, pc)
    return 'rc_bad(0x%08X, "%s");' % (pc, name)


# ---- one instruction --------------------------------------------------------
def simple(ins, w, pc):
    """C for an instruction that is not a branch or jump"""
    name = ins.getOpcodeName()
    rs, rt, rd, sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm, uimm = S16(w), w & 0xFFFF
    ft, fs, fd = rt, rd, sa
    Rs, Rt = R(rs), R(rt)
    L = lambda: "(u32)%s" % Rs
    if name == "nop" or name in ("sync", "sync.l", "sync.p", "pref", "cache", "ei", "di", "mtc0", "mtsab", "mtsah"): return ";"
    # ALU
    if name in ("addu", "add"): return W(rd, "SE32((u32)%s + (u32)%s)" % (Rs, Rt))
    if name in ("subu", "sub"): return W(rd, "SE32((u32)%s - (u32)%s)" % (Rs, Rt))
    if name in ("addiu", "addi"): return W(rt, "SE32((u32)%s + %d)" % (Rs, imm))
    if name in ("daddu", "dadd"): return W(rd, "%s + %s" % (Rs, Rt))
    if name in ("dsubu", "dsub"): return W(rd, "%s - %s" % (Rs, Rt))
    if name in ("daddiu", "daddi"): return W(rt, "%s + (u64)(s64)%d" % (Rs, imm))
    if name == "and": return W(rd, "%s & %s" % (Rs, Rt))
    if name == "or": return W(rd, "%s | %s" % (Rs, Rt))
    if name == "xor": return W(rd, "%s ^ %s" % (Rs, Rt))
    if name == "nor": return W(rd, "~(%s | %s)" % (Rs, Rt))
    if name == "andi": return W(rt, "%s & 0x%X" % (Rs, uimm))
    if name == "ori": return W(rt, "%s | 0x%X" % (Rs, uimm))
    if name == "xori": return W(rt, "%s ^ 0x%X" % (Rs, uimm))
    if name == "lui": return W(rt, "SE32(0x%X)" % (uimm << 16))
    if name == "slt": return W(rd, "(s64)%s < (s64)%s" % (Rs, Rt))
    if name == "sltu": return W(rd, "%s < %s" % (Rs, Rt))
    if name == "slti": return W(rt, "(s64)%s < %d" % (Rs, imm))
    if name == "sltiu": return W(rt, "%s < (u64)(s64)%d" % (Rs, imm))
    if name == "sll": return W(rd, "SE32((u32)%s << %d)" % (Rt, sa))
    if name == "srl": return W(rd, "SE32((u32)%s >> %d)" % (Rt, sa))
    if name == "sra": return W(rd, "SE32((s32)(u32)%s >> %d)" % (Rt, sa))
    if name == "sllv": return W(rd, "SE32((u32)%s << (%s & 31))" % (Rt, Rs))
    if name == "srlv": return W(rd, "SE32((u32)%s >> (%s & 31))" % (Rt, Rs))
    if name == "srav": return W(rd, "SE32((s32)(u32)%s >> (%s & 31))" % (Rt, Rs))
    if name == "dsll": return W(rd, "%s << %d" % (Rt, sa))
    if name == "dsrl": return W(rd, "%s >> %d" % (Rt, sa))
    if name == "dsra": return W(rd, "(u64)((s64)%s >> %d)" % (Rt, sa))
    if name == "dsll32": return W(rd, "%s << %d" % (Rt, sa + 32))
    if name == "dsrl32": return W(rd, "%s >> %d" % (Rt, sa + 32))
    if name == "dsra32": return W(rd, "(u64)((s64)%s >> %d)" % (Rt, sa + 32))
    if name == "dsllv": return W(rd, "%s << (%s & 63)" % (Rt, Rs))
    if name == "dsrlv": return W(rd, "%s >> (%s & 63)" % (Rt, Rs))
    if name == "dsrav": return W(rd, "(u64)((s64)%s >> (%s & 63))" % (Rt, Rs))
    if name == "movz": return "if (%s == 0) { %s }" % (Rt, W(rd, Rs))
    if name == "movn": return "if (%s != 0) { %s }" % (Rt, W(rd, Rs))
    if name in ("mult", "multu", "mult1", "multu1", "madd", "maddu", "madd1", "maddu1"):
        sgn = "u" not in name.replace("mult", "").replace("madd", "")
        k = 1 if name.endswith("1") else 0
        prod = "(s64)(s32)(u32)%s * (s32)(u32)%s" % (Rs, Rt) if sgn else "(s64)((u64)(u32)%s * (u32)%s)" % (Rs, Rt)
        acc = "((u64)c->hi.w[%d] << 32 | c->lo.w[%d]) + " % (2 * k, 2 * k) if name.startswith("madd") else ""
        return "{ u64 p = %s(u64)(%s); c->lo.d[%d] = SE32((u32)p); c->hi.d[%d] = SE32((u32)(p >> 32)); %s }" % (acc, prod, k, k, W(rd, "c->lo.d[%d]" % k))
    if name in ("div", "divu", "div1", "divu1"):
        k = 1 if name.endswith("1") else 0
        if name.startswith("divu"):
            return "{ u32 a = (u32)%s, b = (u32)%s; if (b) { c->lo.d[%d] = SE32(a / b); c->hi.d[%d] = SE32(a %% b); } }" % (Rs, Rt, k, k)
        return ("{ s32 a = (s32)(u32)%s, b = (s32)(u32)%s; if (b == -1 && a == (s32)0x80000000) { c->lo.d[%d] = SE32(a); c->hi.d[%d] = 0; } "
                "else if (b) { c->lo.d[%d] = SE32(a / b); c->hi.d[%d] = SE32(a %% b); } else { c->lo.d[%d] = SE32(a < 0 ? 1 : -1); c->hi.d[%d] = SE32(a); } }") % (Rs, Rt, k, k, k, k, k, k)
    if name == "mfhi": return W(rd, "c->hi.d[0]")
    if name == "mflo": return W(rd, "c->lo.d[0]")
    if name == "mfhi1": return W(rd, "c->hi.d[1]")
    if name == "mflo1": return W(rd, "c->lo.d[1]")
    if name == "mthi": return "c->hi.d[0] = %s;" % Rs
    if name == "mtlo": return "c->lo.d[0] = %s;" % Rs
    if name == "mthi1": return "c->hi.d[1] = %s;" % Rs
    if name == "mtlo1": return "c->lo.d[1] = %s;" % Rs
    if name == "mfsa": return W(rd, "c->sa")
    if name == "mtsa": return "c->sa = (u32)%s;" % Rs
    # loads / stores
    ea = EA(w)
    if name == "lb": return W(rt, "(u64)(s64)(s8)M8(%s)" % ea)
    if name == "lbu": return W(rt, "M8(%s)" % ea)
    if name == "lh": return W(rt, "(u64)(s64)(s16)M16(%s)" % ea)
    if name == "lhu": return W(rt, "M16(%s)" % ea)
    if name == "lw": return W(rt, "SE32(M32(%s))" % ea)
    if name == "lwu": return W(rt, "M32(%s)" % ea)
    if name == "ld": return W(rt, "M64(%s)" % ea)
    if name == "lq": return "" if rt == 0 else "rc_lq(&c->r[%d], %s);" % (rt, ea)
    if name == "sb": return "M8(%s) = (u8)%s;" % (ea, Rt)
    if name == "sh": return "M16(%s) = (u16)%s;" % (ea, Rt)
    if name == "sw": return "rc_sw(%s, (u32)%s);" % (ea, Rt)
    if name == "sd": return "rc_sd(%s, %s);" % (ea, Rt)
    if name == "sq": return ("{ R128 z = {{0,0}}; rc_sq(&z, %s); }" % ea) if rt == 0 else "rc_sq(&c->r[%d], %s);" % (rt, ea)
    if name in ("lwl", "lwr", "ldl", "ldr"): return W(rt, "rc_%s(%s, %s)" % (name, Rt, ea))
    if name in ("swl", "swr", "sdl", "sdr"): return "rc_%s(%s, %s);" % (name, Rt, ea)
    if name == "lwc1": return "c->f[%d] = MF(%s);" % (ft, ea)
    if name == "swc1": return "MF(%s) = c->f[%d];" % (ea, ft)
    if name == "lqc2": return "" if rt == 0 else "__builtin_memcpy(VF(%d), (void *)RC_QA(%s), 16);" % (rt, ea)
    if name == "sqc2": return "__builtin_memcpy((void *)RC_QA(%s), VF(%d), 16);" % (ea, rt)
    # FPU
    F = lambda n: "c->f[%d]" % n
    if name == "mfc1": return W(rt, "SE32(rc_fbits(c->f[%d]))" % fs)
    if name == "mtc1": return "c->f[%d] = rc_bitsf((u32)%s);" % (fs, Rt)
    if name == "cfc1": return W(rt, "SE32(c->fcc ? 0x00800000u : 0)")
    if name == "ctc1": return "c->fcc = ((u32)%s >> 23) & 1;" % Rt
    if name == "add.s": return "%s = rc_fc(%s + %s);" % (F(fd), F(fs), F(ft))      # rc_fc: no infinities on the EE
    if name == "sub.s": return "%s = rc_fc(%s - %s);" % (F(fd), F(fs), F(ft))
    if name == "mul.s": return "%s = rc_fc(%s * %s);" % (F(fd), F(fs), F(ft))
    if name == "div.s": return "%s = rc_fc(rc_fdiv(%s, %s));" % (F(fd), F(fs), F(ft))
    if name == "sqrt.s": return "%s = rc_fsqrt(%s);" % (F(fd), F(ft))
    if name == "rsqrt.s": return "%s = rc_fdiv(%s, rc_fsqrt(%s));" % (F(fd), F(fs), F(ft))
    if name == "abs.s": return "%s = __builtin_fabsf(%s);" % (F(fd), F(fs))
    if name == "neg.s": return "%s = -%s;" % (F(fd), F(fs))
    if name == "mov.s": return "%s = %s;" % (F(fd), F(fs))
    if name == "max.s": return "%s = fmaxf(%s, %s);" % (F(fd), F(fs), F(ft))
    if name == "min.s": return "%s = fminf(%s, %s);" % (F(fd), F(fs), F(ft))
    if name == "adda.s": return "c->facc = rc_fc(%s + %s);" % (F(fs), F(ft))
    if name == "suba.s": return "c->facc = %s - %s;" % (F(fs), F(ft))
    if name == "mula.s": return "c->facc = %s * %s;" % (F(fs), F(ft))
    if name == "madda.s": return "c->facc = rc_fc(c->facc + rc_fc(%s * %s));" % (F(fs), F(ft))
    if name == "msuba.s": return "c->facc = rc_fc(c->facc - rc_fc(%s * %s));" % (F(fs), F(ft))
    if name == "madd.s": return "%s = rc_fc(c->facc + rc_fc(%s * %s));" % (F(fd), F(fs), F(ft))
    if name == "msub.s": return "%s = rc_fc(c->facc - rc_fc(%s * %s));" % (F(fd), F(fs), F(ft))
    if name == "cvt.s.w": return "%s = (f32)(s32)rc_fbits(%s);" % (F(fd), F(fs))
    if name == "cvt.w.s": return "%s = rc_bitsf((u32)rc_cvtws(%s));" % (F(fd), F(fs))
    if name == "c.f.s": return "c->fcc = 0;"
    if name == "c.eq.s": return "c->fcc = %s == %s;" % (F(fs), F(ft))
    if name == "c.lt.s": return "c->fcc = %s < %s;" % (F(fs), F(ft))
    if name == "c.le.s": return "c->fcc = %s <= %s;" % (F(fs), F(ft))
    # COP2 transfers
    if name in ("qmfc2", "qmfc2.ni", "qmfc2.i"): return ("rc_vu0_resume(); " if name.endswith(".i") else "") + ("" if rt == 0 else "__builtin_memcpy(&c->r[%d], VF(%d), 16);" % (rt, rd))
    if name in ("qmtc2", "qmtc2.ni", "qmtc2.i"): return "" if rd == 0 else "__builtin_memcpy(VF(%d), &c->r[%d], 16);" % (rd, rt)
    if name in ("cfc2", "cfc2.ni", "cfc2.i"):
        src = {16: "rc_vu0.status", 17: "rc_vu0.mac", 18: "rc_vu0.clip", 20: "rc_fbits(rc_vu0.r)", 21: "rc_fbits(rc_vu0.i)", 22: "rc_fbits(rc_vu0.q)",
               29: "0"}.get(rd, "rc_vu0.vi[%d]" % (rd & 15) if rd < 16 else "0")
        return W(rt, "SE32(%s)" % src)
    if name in ("ctc2", "ctc2.ni", "ctc2.i"):
        if rd < 16: return "" if rd == 0 else "rc_vu0.vi[%d] = (u32)%s & 0xFFFF; rc_vu0_resume();" % (rd, Rt)
        dst = {18: "rc_vu0.clip = (u32)%s & 0xFFFFFF;", 20: "rc_vu0.r = rc_bitsf((u32)%s);", 21: "rc_vu0.i = rc_bitsf((u32)%s);",
               22: "rc_vu0.q = rc_bitsf((u32)%s);", 16: "rc_vu0.status = (u32)%s;", 27: "rc_vu0.cmsar0 = (u32)%s & 0xFFFF;",
               31: "vu_run(1, ((u32)%s & 0xFFFF) * 8);"}.get(rd)   # CMSAR1: starts VU1 there
        return (dst % Rt) if dst else ";"
    if name == "mfc0":
        return W(rt, "SE32(rc_cop0(%d))" % rd)
    if name == "syscall": return "rc_syscall(c);"
    if name in ("break", "teq", "tne", "tge", "tlt", "teqi", "tnei"):
        if name == "teq": return "if (%s == %s) rc_break(0x%08X);" % (Rs, Rt, pc)
        if name == "tne": return "if (%s != %s) rc_break(0x%08X);" % (Rs, Rt, pc)
        return "rc_break(0x%08X);" % pc
    if name.startswith("v"): return vu_op(name, w, pc)
    if name.startswith("p") or name.startswith("pm"): return mmi_op(name, w, pc)
    warn(name, pc)
    return 'rc_bad(0x%08X, "%s");' % (pc, name)


def branch_cond(name, w):
    rs, rt = (w >> 21) & 31, (w >> 16) & 31
    Rs, Rt = R(rs), R(rt)
    base = name[:-1] if name.endswith("l") and name not in ("bal",) and name[:-1] in (
        "beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal", "bc1f", "bc1t", "bc0f", "bc0t", "bc2f", "bc2t") else name
    return {
        "beq": "%s == %s" % (Rs, Rt), "bne": "%s != %s" % (Rs, Rt),
        "blez": "(s64)%s <= 0" % Rs, "bgtz": "(s64)%s > 0" % Rs,
        "bltz": "(s64)%s < 0" % Rs, "bgez": "(s64)%s >= 0" % Rs,
        "bltzal": "(s64)%s < 0" % Rs, "bgezal": "(s64)%s >= 0" % Rs,
        "bc1f": "!c->fcc", "bc1t": "c->fcc",
        "bc0f": "0", "bc0t": "1",          # CPCOND0 (DMA done): always done on the host
        "bc2f": "0", "bc2t": "0",
    }.get(base), name != base


def call_target(addr):
    """C statement that calls the function at addr with the shared context"""
    if addr in rc_addr: return P + "%s(c);" % rc_addr[addr]
    if addr in c_funcs: return P + "thunk_%s(c);" % c_funcs[addr]
    if addr in core_rc: return "%s(c);" % core_rc[addr]
    return "rc_dispatch(c, 0x%08X);" % addr


thunks_needed = set()


NEED_VU0_FLAGS = True
def translate(name, rng=None, entries=None):
    va, size = rng if rng else fsize[name]
    n = size // 4
    ins = [(va + 4 * i, word(va + 4 * i)) for i in range(n + 1)]   # + the word after: a last delay slot
    dec = {pc: rabbitizer.Instruction(w, vram=pc, category=rabbitizer.InstrCategory.R5900) for pc, w in ins}
    ins_all, ins = ins, ins[:n]
    # the COP2 MAC/status flags cost a computation per FMAC op: only functions
    # that read them (cfc2 from $vi16 or $vi17) get them
    global NEED_VU0_FLAGS
    NEED_VU0_FLAGS = any(dec[pc].getOpcodeName().startswith("cfc2") and ((w >> 11) & 31) in (16, 17) for pc, w in ins)
    labels = set()
    jr_reg = False
    for pc, w in ins:
        d = dec[pc]
        nm = d.getOpcodeName()
        if d.isBranch() or nm in ("j",):
            t = d.getBranchVramGeneric() if d.isBranch() else ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if va <= t < va + size: labels.add(t)
        if nm == "jr" and ((w >> 21) & 31) != 31: jr_reg = True
    if jr_reg:
        labels = {pc for pc, w in ins}
    if entries:
        labels |= set(entries)
        out = ["static void %sregion_%s(RcCtx *c, u32 entry)\n{" % (P, name),
               "    switch (entry) { %s }" % " ".join("case 0x%08X: goto L_%08X;" % (e, e) for e in sorted(entries))]
    else:
        out = ["void %s%s(RcCtx *c)\n{" % (P, name)]
    if name in TRACE_FUNCS:       # testing: UYA_RC_TRACE at build time (rc_trace logs the calls)
        out.append('    { extern void rc_trace(RcCtx *, const char *); rc_trace(c, "%s"); }' % name)
    if name == "func_001F7EF8":   # testing: drawquad's GS words checked (UYA_QUADCHECK)
        out.append("    { extern void uya_dbg_quad(RcCtx *); uya_dbg_quad(c); }")
    if jr_reg:
        # the return address on entry: `jr $t6` after `move $t6, $ra` (func_L00_001FE688)
        # is a return, not a jump to the caller's code as a function of its own
        out.append("    u32 rc_ra0 = (u32)c->r[31].d[0];")
    i = 0
    while i < n:
        pc, w = ins[i]
        d = dec[pc]
        nm = d.getOpcodeName()
        if pc in labels: out.append("L_%08X:;" % pc)
        has_delay = d.hasDelaySlot() and i + 1 <= n
        delay = ""
        if has_delay:
            dpc, dw = ins_all[i + 1]
            delay = simple(dec[dpc], dw, dpc) or ";"
        if nm in ("jr",):
            rs = (w >> 21) & 31
            if rs == 31:
                out.append("    %s return;" % delay)
            else:
                out.append("    { u32 tgt = (u32)%s; %s if (tgt == rc_ra0) return; switch (tgt) { @CASES@ default: rc_dispatch(c, tgt); return; } }" % (R(rs), delay))
            if has_delay and pc + 4 in labels: out.append("    if (0) { L_%08X:; %s }" % (pc + 4, delay))
            i += 2; continue
        if nm == "jalr":
            rs, rd = (w >> 21) & 31, (w >> 11) & 31
            out.append("    { u32 tgt = (u32)%s; %s %s rc_dispatch(c, tgt); }" % (R(rs), W(rd, "0x%08XULL" % (pc + 8)), delay))
            if has_delay and pc + 4 in labels: out.append("    if (0) { L_%08X:; %s }" % (pc + 4, delay))
            i += 2; continue
        if nm in ("jal", "j"):
            t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if nm == "jal":
                out.append("    c->r[31].d[0] = 0x%08XULL; %s %s" % (pc + 8, delay, call_target(t)))
            elif va <= t < va + size:
                out.append("    %s goto L_%08X;" % (delay, t))
            else:
                out.append("    %s %s return;" % (delay, call_target(t)))
            if has_delay and pc + 4 in labels: out.append("    if (0) { L_%08X:; %s }" % (pc + 4, delay))
            i += 2; continue
        if d.isBranch():
            cond, likely = branch_cond(nm, w)
            t = d.getBranchVramGeneric()
            link = nm in ("bltzal", "bgezal", "bltzall", "bgezall")
            lk = "c->r[31].d[0] = 0x%08XULL; " % (pc + 8) if link else ""
            if cond is None:
                warn(nm, pc); out.append('    rc_bad(0x%08X, "%s");' % (pc, nm)); i += 1; continue
            inside = va <= t < va + size
            go = ("goto L_%08X;" % t) if inside and not link else ("%s return;" % call_target(t) if not link else call_target(t))
            if likely:
                out.append("    if (%s) { %s%s %s }" % (cond, lk, delay, go))
            else:
                out.append("    { int bt = %s; %s%s if (bt) { %s } }" % (cond, lk, delay, go))
            if has_delay and pc + 4 in labels: out.append("    if (0) { L_%08X:; %s }" % (pc + 4, delay))
            i += 2; continue
        out.append("    " + (simple(d, w, pc) or ";"))
        i += 1
    # the last instruction does not leave: it runs on into the next function
    last = n - 2 if n >= 2 and dec[ins[n - 2][0]].hasDelaySlot() else n - 1
    lnm = dec[ins[last][0]].getOpcodeName() if n else "jr"
    lw = ins[last][1] if n else 0
    if LEVEL is not None and not (lnm in ("j", "jr") or (lnm == "beq" and (lw >> 16) & 0x3FF == 0) or lnm in ("b", "teq", "break")):
        out.append("    %s return;" % call_target(va + size))
    out.append("}\n")
    txt = "\n".join(out)
    emitted = set(int(x, 16) for x in re.findall(r"\bL_([0-9A-F]{8}):;", txt))   # also the delay-slot labels inside `if (0) { ... }`
    txt = txt.replace("@CASES@", " ".join("case 0x%08X: goto L_%08X;" % (l, l) for l in sorted(emitted)))
    for t in set(int(x, 16) for x in re.findall(r"goto L_([0-9A-F]{8});", txt)) - emitted:
        warn("branch into a delay slot", t)
        txt = txt.replace("goto L_%08X;" % t, 'rc_bad(0x%08X, "branch into a delay slot"); return;' % t)
    return txt


# ---- emit -------------------------------------------------------------------
cls_c = {"i": "s32", "l": "s64", "f": "f32", "d": "double", "q": "s32", "s": "s32", "v": "void"}


def arg_regs(params):
    """(gpr or fpr expression) per parameter, EE argument order"""
    gi, fi, res = 4, 12, []
    for p in params:
        if p == "f":
            res.append(("f", fi)); fi += 1
        else:
            res.append(("g", gi)); gi += 1
    return res


# ---- regions: hand-written functions that jump into each other's middles are one routine
def jump_targets(nm):
    va, size = fsize[nm]
    res = []
    for i in range(size // 4):
        pc = va + 4 * i; w = word(pc)
        d = rabbitizer.Instruction(w, vram=pc, category=rabbitizer.InstrCategory.R5900)
        if d.getOpcodeName() == "j": res.append(((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
        elif d.isBranch() and not d.doesLink(): res.append(d.getBranchVramGeneric())
    return res
by_start = sorted((fsize[n][0], fsize[n][0] + fsize[n][1], n) for n in todo)
def owner(a):
    for lo, hi, n in by_start:
        if lo <= a < hi: return n
    return None
parent = {n: n for n in todo}
def find(n):
    while parent[n] != n: parent[n] = parent[parent[n]]; n = parent[n]
    return n
for n in todo:
    for t in jump_targets(n):
        o = owner(t)
        if o and o != n and t != fsize[o][0]:
            parent[find(o)] = find(n)
groups = {}
for n in todo: groups.setdefault(find(n), []).append(n)
region_of = {}
spans = sorted((min(fsize[n][0] for n in g), max(fsize[n][0] + fsize[n][1] for n in g)) for g in groups.values() if len(g) > 1)
merged = []                     # overlapping regions are one region (a function falling through into the next group)
for lo, hi in spans:
    if merged and lo < merged[-1][1]: merged[-1][1] = max(merged[-1][1], hi)
    else: merged.append([lo, hi])
for lo, hi in merged:
    # everything in between belongs to the region too
    mem = [n for n in todo if lo <= fsize[n][0] < hi]
    hi = max([hi] + [fsize[n][0] + fsize[n][1] for n in mem])
    for n in mem: region_of[n] = (lo, hi, sorted(mem, key=lambda x: fsize[x][0]))

hdr = ['/* generated by tools/recomp.py */\n#include "rc.h"\nstatic inline double rc_d(u64 v) { double d; __builtin_memcpy(&d, &v, 8); return d; }\nu32 rc_cop0(int r);']
for nm in todo:
    hdr.append("void %s%s(RcCtx *c);" % (P, nm))
for f in sorted(set(core_rc.values())):
    hdr.append("void %s(RcCtx *c);" % f)
thunk_text = ['#include "rc_decls.h"']
_seen_thunk = set()
for a, cn in sorted(c_funcs.items()):
    if cn in _seen_thunk: continue          # one C function at several places
    _seen_thunk.add(cn)
    s = sigs[cn]
    ps = s["params"]
    regs = arg_regs(ps)
    if s["var"]: decl_args = (", ".join(cls_c[p] for p in ps) + ", ...") if ps else "..."
    else: decl_args = ", ".join(cls_c[p] for p in ps) if ps else "void"
    call_args = []
    for p, (k, n_) in zip(ps, regs):
        if k == "f": call_args.append("c->f[%d]" % n_)
        elif n_ > 11 and p == "l": call_args.append("(s64)M64((u32)c->r[29].d[0] + %d)" % (8 * (n_ - 12)))   # a 64-bit stack slot
        elif n_ > 11: call_args.append("(%s)M32((u32)c->r[29].d[0] + %d)" % (cls_c[p], 8 * (n_ - 12)))
        elif p == "l": call_args.append("(s64)c->r[%d].d[0]" % n_)
        elif p == "d": call_args.append("rc_d(c->r[%d].d[0])" % n_)
        else: call_args.append("(s32)c->r[%d].d[0]" % n_)
    if s["var"]:
        gi = 4 + sum(1 for p in ps if p != "f")
        call_args += ["(s32)c->r[%d].d[0]" % g for g in range(gi, 12)]
    ret = s["ret"]
    hdr.append("void %sthunk_%s(RcCtx *c);" % (P, cn))
    thunk_text.append("extern %s %s(%s);" % (cls_c[ret], cn, decl_args))
    body = "%s(%s)" % (cn, ", ".join(call_args))
    if ret == "v": store = "%s;" % body
    elif ret == "f": store = "c->f[0] = %s;" % body
    elif ret == "l": store = "c->r[2].d[0] = (u64)%s;" % body
    elif ret == "d": store = "{ double v_ = %s; __builtin_memcpy(&c->r[2].d[0], &v_, 8); }" % body
    else: store = "c->r[2].d[0] = SE32(%s);" % body
    thunk_text.append("void %sthunk_%s(RcCtx *c) { u32 sp0 = rc_sp; rc_sp = (u32)c->r[29].d[0] & ~15u; %s rc_sp = sp0; }" % (P, cn, store))
open(os.path.join(GEN, "rc_decls.h"), "w").write("\n".join(hdr) + "\n")
open(os.path.join(GEN, "rc_thunks.c"), "w").write("\n".join(thunk_text) + "\n")
for old in [f for f in os.listdir(GEN) if re.fullmatch(r"rc_funcs_\d+\.c", f)]:
    os.remove(os.path.join(GEN, old))
CH = 20
for k in range(0, len(todo), CH):
    txt = ['/* generated by tools/recomp.py from the user\'s own %s: do not edit */\n#include "rc_decls.h"\n' % ("executable" if LEVEL is None else "disc (level %s's program)" % LV)]
    for nm in todo[k:k + CH]:
        if nm in region_of:
            lo, hi, mem = region_of[nm]
            if nm == mem[0]:
                txt.append(translate(nm, (lo, hi - lo), [fsize[m][0] for m in mem]))
                for m in mem:
                    txt.append("void %s%s(RcCtx *c) { %sregion_%s(c, 0x%08X); }" % (P, m, P, nm, fsize[m][0]))
            continue
        txt.append(translate(nm))
    open(os.path.join(GEN, "rc_funcs_%03d.c" % (k // CH)), "w").write("\n".join(txt))

# wrappers: the C entry for each translated function, under its own name
wr = ['/* generated by tools/recomp.py: C-callable entries for the translated functions */\n#include "rc.h"\n']
for nm in todo:
    s = sigs.get(base_of.get(nm, nm))
    wr.append("void %s%s(RcCtx *c);" % (P, nm))
    if s and s.get("src") == "decl" and s["params"] is not None and not s.get("unk"):
        ps, ret = s["params"], s["ret"]
    else:
        ps, ret = ["i"] * 8, "l"
    regs = arg_regs(ps)
    plist = ", ".join("%s a%d" % (cls_c[p], k) for k, p in enumerate(ps)) or "void"
    sets = []
    for k, (p, (kind, n_)) in enumerate(zip(ps, regs)):
        if kind == "f": sets.append("c.f[%d] = a%d;" % (n_, k))
        elif n_ > 11 and p == "l": sets.append("M64((u32)c.r[29].d[0] + %d) = (u64)a%d;" % (8 * (n_ - 12), k))
        elif n_ > 11: sets.append("M32((u32)c.r[29].d[0] + %d) = (u32)a%d;" % (8 * (n_ - 12), k))
        elif p == "l": sets.append("c.r[%d].d[0] = (u64)a%d;" % (n_, k))
        elif p == "d": sets.append("__builtin_memcpy(&c.r[%d].d[0], &a%d, 8);" % (n_, k))
        else: sets.append("c.r[%d].d[0] = SE32(a%d);" % (n_, k))
    if ret == "v": rr = ""
    elif ret == "f": rr = "return c.f[0];"
    elif ret == "l": rr = "return (s64)c.r[2].d[0];"
    elif ret == "d": rr = "{ double v_; __builtin_memcpy(&v_, &c.r[2].d[0], 8); return v_; }"
    else: rr = "return (s32)c.r[2].d[0];"
    wr.append("%s %s(%s) { RcCtx c; rc_enter(&c); %s %s%s(&c); %s }" % (
        cls_c[ret], nm, plist, " ".join(sets), P, nm, rr))
open(os.path.join(GEN, "rc_wrappers.c"), "w").write("\n".join(wr) + "\n")

# dispatch table: every translated function and every thunk, by address
tb = ['/* generated by tools/recomp.py */\n#include "rc.h"\ntypedef void (*rc_fn)(RcCtx *);']
rows = []
for a, nm in sorted(rc_addr.items()):
    rows.append((a, P + nm))
for a, cn in sorted(c_funcs.items()):
    if a not in rc_addr: rows.append((a, P + "thunk_" + cn))
have_rows = {a for a, _ in rows}
for a, f in sorted(core_rc.items()):
    if a not in have_rows: rows.append((a, f))
rows.sort()
for a, f in rows: tb.append("void %s(RcCtx *c);" % f)
tb.append("const struct { u32 addr; rc_fn fn; } %stable[] = {" % P)
for a, f in rows: tb.append("    {0x%08X, %s}," % (a, f))
tb.append("};\nconst int %stable_count = %d;" % (P, len(rows)))
open(os.path.join(GEN, "rc_table.c"), "w").write("\n".join(tb) + "\n")

# asm_stubs.c without the translated functions
p = os.path.join(GEN, "asm_stubs.c")
if os.path.exists(p):
    keep = [l for l in open(p) if not any(("long long %s(void)" % nm) in l for nm in todo)]
    if LEVEL is not None:
        tset = set(todo)
        keep = [l for l in open(p) if not (re.match(r"long long (\w+)\(void\)", l) and re.match(r"long long (\w+)\(void\)", l).group(1) in tset)]
    open(p, "w").write("".join(keep))
print("recompiled %d functions (%d instructions); %d C thunks; unsupported instructions: %s" % (
    len(todo), sum(fsize[n][1] // 4 for n in todo), len(c_funcs),
    ", ".join("%s x%d" % (k, len(v)) for k, v in sorted(warnings.items())) or "none"))

# functions translated here that the generator did not know as defined (the
# extra library ranges): no absolute symbol for them, and a trampoline each
known = set(entries)
extra = [n for n in todo if n not in known]
if LEVEL is not None:
    # the level's trampolines: its C functions (gen_host.py) and every translated one
    p = os.path.join(GEN, "tramp_table.c")
    t = open(p).read()
    rows = {int(a, 16): n for a, n in re.findall(r"\{0x([0-9A-F]{8}), \(void \*\)(\w+)\},", t)}
    for n in todo: rows[fsize[n][0]] = n
    rows.update(_host_rows)
    TT = "uya_tramp_table_l%s" % LV
    t = '/* generated by gen_host.py and recomp.py: level %s, PS2 address -> host function */\n#include "uya_runtime.h"\n' % LV
    t += "".join("extern void %s(void);\n" % n for n in sorted(set(rows.values())))
    t += "const uya_tramp_t %s[] = {\n" % TT + "".join("    {0x%08X, (void *)%s},\n" % r for r in sorted(rows.items()))
    t += "};\nconst int uya_tramp_count_l%s = %d;\n" % (LV, len(rows))
    open(p, "w").write(t)
    extra = []
if extra:
    p = os.path.join(GEN, "abs_syms.s")
    lines = open(p).read().split("\n")
    drop = set(extra) | {"_" + n for n in extra}
    out, skip = [], False
    for l in lines:
        q = re.match(r"\.(globl|set)\s+(\w+)", l)
        if q and q.group(2) in drop: continue
        out.append(l)
    open(p, "w").write("\n".join(out))
    p = os.path.join(GEN, "tramp_table.c")
    t = open(p).read()
    rows = re.findall(r"\{0x([0-9A-F]{8}), \(void \*\)(\w+)\},", t)
    have = {int(a, 16) for a, _ in rows}
    rows += [("%08X" % fsize[n][0], n) for n in extra if fsize[n][0] not in have]
    rows.sort()
    decls = sorted({n for _, n in rows})
    t = '/* generated by gen_host.py and recomp.py: PS2 address -> host function */\n#include "uya_runtime.h"\n'
    t += "".join("extern void %s(void);\n" % n for n in decls)
    t += "const uya_tramp_t uya_tramp_table[] = {\n" + "".join("    {0x%s, (void *)%s},\n" % r for r in rows)
    t += "};\nconst int uya_tramp_count = %d;\n" % len(rows)
    open(p, "w").write(t)
