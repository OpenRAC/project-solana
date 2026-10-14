#!/usr/bin/env python3
"""gen_host.py DECOMP_REPO OUT_DIR

RAC1 Wrapper: turns OpenRAC rac1-decomp's executable game code (src/game,
src/game/movie and the game-compiled parts of src/core) into sources a 32-bit
x86 compiler accepts. Adapted from the UYA Wrapper's generator.
The decomp repo is only read, never written.

Per file:
  - INCLUDE_ASM / ASM_FUNC entries become stubs in asm_stubs.c (they log and
    return 0) unless host/overrides/<func>.c provides a real version;
    LINKER_REMNANT, INCLUDE_RODATA, TEXT_PADDING and top-level __asm__ go away.
  - A function whose body uses inline asm (VU0, COP0, sync...), or calls a
    same-file static helper that does, gets a stub body, again unless an
    override exists.
  - `long` becomes `long long` (the EE's long is 64-bit), and
    `typedef int X __attribute__((mode(TI)))` becomes a 16-byte struct.
Also writes:
  - abs_syms.s: every symbol the decomp resolves by address and the host does
    not define (globals, engine functions, per-function aliases), as absolute
    symbols, so data lives at its PS2 address.
  - funcs.tsv: every frontbin function address and how the host provides it.

gen_host.py DECOMP_REPO OUT_DIR --level NN --exe-gen EXE_GEN_DIR

Level mode: the same passes over one level's program (docs/LEVELS.md): the
shared code and the level's own code in src/overlays, keeping only the
functions the level's program has (config/overlays/functions.tsv). Its
functions sit at the level's addresses, so the executable's main-segment
names (func_XXXXXXXX of 0x1E9080-0x23E740) mean that function's copy in the
level. Every name the level pass defines or places is given the suffix _lNN
by one header, lNN_names.h, forced into each of its C files (asm labels are
rewritten to match), so several levels and the executable link together.
Writes OUT_DIR/{*.c, asm_stubs.c, missing.c, tramp_table.c, abs_syms.s,
sigs.json, asm_entries.json, lNN_names.h, funcs.tsv}; the headers are the
executable pass's (EXE_GEN_DIR/include).
"""
import os, re, sys, glob, json

REPO, OUT = sys.argv[1], sys.argv[2]
LEVEL, EXEGEN = None, None
_a = sys.argv[3:]
while _a:
    _o = _a.pop(0)
    if _o == "--level": LEVEL = int(_a.pop(0))
    elif _o == "--exe-gen": EXEGEN = _a.pop(0)
    else: sys.exit("gen_host.py: unknown option " + _o)
LV = "%02d" % LEVEL if LEVEL is not None else None
SFX = "_l%s" % LV if LEVEL is not None else ""
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAT = os.environ.get("RAC_PLATFORM", os.environ.get("UYA_PLATFORM", "linux"))  # linux | windows (COFF i386 adds a leading _)
os.makedirs(OUT, exist_ok=True)
overrides = {os.path.basename(p)[:-2] for p in glob.glob(os.path.join(HERE, "host", "overrides", "func_*.c"))}
ASM_MACRO = re.compile(r"^\s*(INCLUDE_ASM|ASM_FUNC|LINKER_REMNANT|INCLUDE_RODATA|TEXT_PADDING)\s*\(([^()]*)\)\s*;?\s*(?:/\*.*?\*/|//.*)?\s*$", re.M)


def strip_comments_keep_len(s):
    """Blank out comments and string/char literals (same length) for scanning."""
    out = list(s); i = 0; n = len(s)
    while i < n:
        c = s[i]
        if s.startswith("/*", i):
            j = s.find("*/", i + 2); j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n": out[k] = " "
            i = j
        elif s.startswith("//", i):
            j = s.find("\n", i); j = n if j < 0 else j
            for k in range(i, j): out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and s[j] != c:
                j += 2 if s[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n": out[k] = "_"
            i = j + 1
        else:
            i += 1
    return "".join(out)


host_defined = set()
STATIC = os.environ.get("UYA_STATIC", "1") != "0"   # RAC1_STATICRECOMP: translate the whole game from its bytes
for p in glob.glob(os.path.join(HERE, "host", "src", "**", "*.c"), recursive=True):
    host_defined |= set(re.findall(r"^[\w \t\*]*\b(func_(?:L\d\d_)?[0-9A-Fa-f]{6,8})\s*\([^;{]*\)\s*\{", strip_comments_keep_len(open(p).read()), re.M))


def match_close(m, i, o="{", c="}"):
    d = 0
    for j in range(i, len(m)):
        if m[j] == o: d += 1
        elif m[j] == c:
            d -= 1
            if d == 0: return j
    raise ValueError("unbalanced")


ASM_RE = re.compile(r'\b(__asm__|__asm|asm)\b(?!\s*__volatile__\s*\(\s*""\s*:::\s*"_+"\s*\)\s*;)')
IDENT = re.compile(r"[A-Za-z_]\w*")


def top_level_items(src):
    """Yield (kind, start, end, extra) for top-level __asm__ statements and function definitions."""
    m = strip_comments_keep_len(src)
    i, n, last = 0, len(m), 0
    bounds = []
    while i < n:
        c = m[i]
        if c in ";}":
            last = i + 1; bounds.append(last); i += 1; continue
        if c == "#" and m[m.rfind("\n", 0, i) + 1:i].strip() == "":
            # preprocessor line (with continuations) is a boundary, never part of an item
            j = i
            while True:
                e = m.find("\n", j)
                if e < 0: e = len(m); break
                if m[e - 1] == "\\": j = e + 1; continue
                break
            i = e; last = e; bounds.append(last); continue
        if c == "(" :
            i = match_close(m, i, "(", ")") + 1; continue
        if c == "{":
            head = m[last:i]
            if not head.strip().endswith(")") and re.fullmatch(r"\s*([^;{}()#]+;\s*)*", head) and bounds:
                # K&R definition: `type name(a, b) T a; U b; {` -- the header ends at an earlier ';'
                for b in reversed(bounds):
                    h2 = m[b:i]
                    if re.fullmatch(r"\s*[^;{}#]*?\w+\s*\(\s*\w+(\s*,\s*\w+)*\s*\)\s*([^;{}()#]+;\s*)+", h2):
                        last = b; head = h2; break
                    if b < i - 2000: break
            bounds_snapshot = None
            j = match_close(m, i)
            hs = head.strip()
            is_func = hs.endswith(")") or re.search(r"\)\s*([^;{}()#]+;\s*)+$", hs) or re.search(r"\)\s*(__attribute__\s*\(\(.*\)\))\s*$", hs, re.S)
            if is_func and not re.match(r"\s*(typedef|struct|union|enum)\b", head) and "=" not in head.split("(")[0]:
                # name = identifier before the parameter list
                p = hs.rfind(")"); depth = 0; k = p
                while k >= 0:
                    if hs[k] == ")": depth += 1
                    elif hs[k] == "(":
                        depth -= 1
                        if depth == 0: break
                    k -= 1
                name = IDENT.findall(hs[:k])[-1]
                yield ("func", last, j + 1, (name, i))
                last = j + 1; bounds.append(last); i = j + 1
                continue
            i = j + 1; last = i; bounds.append(last); continue
        if m.startswith(("__asm__", "asm"), i) and (i == 0 or not (m[i - 1].isalnum() or m[i - 1] == "_")):
            w = IDENT.match(m, i).group(0)
            if w in ("__asm__", "asm", "__asm") and m[last:i].strip() == "":
                k = m.index("(", i); j = match_close(m, k, "(", ")")
                e = m.index(";", j) + 1
                yield ("asm", i, e, None)
                last = e; i = e; continue
        i += 1


def rewrite_types(s):
    s = re.sub(r"typedef\s+[\w ]+?\s+(\w+)\s+__attribute__\s*\(\(\s*mode\s*\(\s*TI\s*\)[^;]*?\)\)\s*;",
               r"typedef unsigned int \1 __attribute__((ext_vector_type(4), aligned(16)));", s)
    # long -> long long, leaving existing "long long" alone
    s = re.sub(r"\blong(\s+long)?\b", lambda q: q.group(0) if q.group(1) else "long long", s)
    # and the literal suffixes: 0x8000UL << 45 is a 64-bit value on the EE
    s = re.sub(r"\b(0[xX][0-9A-Fa-f]+|[0-9]+)([uU]?)[lL]\b(?![lL])", lambda q: q.group(1) + q.group(2) + "LL", s)
    s = re.sub(r"\b(0[xX][0-9A-Fa-f]+|[0-9]+)[lL][uU]\b", lambda q: q.group(1) + "ULL", s)
    return s


DECL = re.compile(r"^[ \t]*(?:extern[ \t]+)?(?P<t>[A-Za-z_][\w \t\*]*?)[ \t\*]*\b(?P<n>(?:func|D)_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)[ \t]*(?P<s>\((?:[^;{}]*)\)|\[[^;{}]*\])?[ \t]*;", re.M)


MAIN_TEXT = (0x001E9080, 0x0023E740)   # the executable's game program (.text of the main segment)
RESIDENT = 0x0015F000                   # below: core segment and core_lit, the same in every program
PLACES = {}                             # level mode: catalogue name -> its addresses in this level
CAT_KIND = {}
CAT_SIZE = {}
if LEVEL is not None:
    for _l in open(os.path.join(REPO, "config", "overlays", "functions.tsv")):
        if _l.startswith("#") or not _l.strip(): continue
        _f = _l.rstrip("\n").split("\t")
        CAT_KIND[_f[0]] = _f[1]
        CAT_SIZE[_f[0]] = int(_f[2]) if _f[2].isdigit() else 0
        _p = [int(x.split(":")[1], 16) for x in _f[5].split(",") if x.split(":")[0] == LV]
        if _p: PLACES[_f[0]] = _p
PLACE_ADDRS = {a for v in PLACES.values() for a in v}


# the level program's code (build/levels/level_NN/text.bin), for checking C against it
LTEXT = None
CALL_MISMATCH = []
if LEVEL is not None:
    try:
        _man = json.load(open(os.path.join(HERE, "build", "levels", "level_" + LV, "manifest.json")))
        _tr = [r for r in _man["records"] if r["name"] == "text"][0]
        LTEXT = (_tr["address"], open(os.path.join(HERE, "build", "levels", "level_" + LV, "text.bin"), "rb").read())
    except Exception as _e:
        print("level text not found for call checks:", _e)
def level_call_targets(a, size):
    base, d = LTEXT
    if not (base <= a and a + size <= base + len(d)): return None
    out = set()
    for o in range(a - base, a - base + size, 4):
        w = int.from_bytes(d[o:o + 4], "little")
        if (w >> 26) in (2, 3): out.add(((a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)))
    return out

def is_main_func(name):
    """an executable main-segment function name (its copy differs per program)"""
    q = re.fullmatch(r"func_([0-9A-Fa-f]{8})", name)
    return bool(q) and MAIN_TEXT[0] <= int(q.group(1), 16) < MAIN_TEXT[1]


def base_func(name):
    q = re.match(r"(func_(?:L\d\d_)?[0-9A-Fa-f]{8})", name)
    return q.group(1) if q else None


def ps2_addr(name):
    q = re.match(r"(func|D)_(?:L(\d\d)_)?([0-9A-Fa-f]{6,8})", name)
    if not q: return None
    a = int(q.group(3), 16)
    if LEVEL is None:
        return None if q.group(2) else a
    if q.group(1) == "D":
        return a
    if q.group(2) or (len(q.group(3)) == 8 and MAIN_TEXT[0] <= a < MAIN_TEXT[1]):
        p = PLACES.get(base_func(name))
        if p: return p[0]
        # func_LNN_X naming another place of a function the level has (identical copies)
        return a if q.group(2) == LV and a in PLACE_ADDRS else None
    return a


ALIASES = {}  # new name -> PS2 address


def split_conflicts(text, defined_here):
    """gcc 2.95 accepts a function or global declared with different types in
    one file (each localdecomp block brings its own prototypes); clang does
    not. A redeclaration that differs from the first one (or from the
    definition) gets its own name, used up to the next redeclaration; the new
    name is an absolute symbol at the same PS2 address."""
    m = strip_comments_keep_len(text)
    # top-level only: compute brace depth per position
    depth = [0] * (len(m) + 1); d = 0
    for i, ch in enumerate(m):
        depth[i] = d
        if ch == "{": d += 1
        elif ch == "}": d -= 1
    first = {}
    # definitions count as the first declaration
    for q in re.finditer(r"^[^\n;{}]*?\b((?:func)_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)[ \t]*\(([^;{}]*)\)\s*(?:[^;{}()#]+;\s*)*\{", m, re.M):
        if depth[q.start()] == 0: first.setdefault(q.group(1), "def:" + " ".join(q.group(0).split()))
    edits = []  # (decl_start, decl_end, name, newname)
    count = {}
    for q in DECL.finditer(m):
        if depth[q.start()] != 0 or "typedef" in q.group("t") or "return" in q.group("t"): continue
        if "__asm__" in (q.group("s") or ""): continue   # an asm-label alias, not a redeclaration
        n = q.group("n"); sig = " ".join((q.group("t") + "|" + (q.group("s") or "")).split())
        if n not in first:
            first[n] = sig; continue
        if first[n] == sig and not first[n].startswith("def:"): continue
        count[n] = count.get(n, 0) + 1
        nn = "%s__%sh%d" % (n, SFX[1:], count[n]) if SFX else "%s__h%d" % (n, count[n])
        a = ps2_addr(n)
        if a is None: continue
        ALIASES[nn] = a
        edits.append((q.start("n"), q.end("n"), n, nn, q.end()))
    # (a) a call before any declaration is an implicit `int f()`; if the file
    # later declares or defines f differently, give those early calls their own name
    declpos = {}
    for q in DECL.finditer(m):
        if depth[q.start()] == 0: declpos.setdefault(q.group("n"), q.start())
    for q in re.finditer(r"^[^\n;{}]*?\b((?:func)_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)[ \t]*\(([^;{}]*)\)\s*(?:[^;{}()#]+;\s*)*\{", m, re.M):
        if depth[q.start()] == 0: declpos[q.group(1)] = min(declpos.get(q.group(1), 1 << 30), q.start())
    early = []
    for q in re.finditer(r"\b(func_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)\s*\(", m):
        n = q.group(1)
        if n in declpos and q.start() < declpos[n] and depth[q.start()] > 0 and ps2_addr(n) is not None:
            early.append((q.start(1), len(n), n + "__%simp" % SFX[1:]))
            ALIASES[n + "__%simp" % SFX[1:]] = ps2_addr(n)
    if early:
        names = sorted({e[2] for e in early})
        edits_early = early
    else:
        edits_early = []
    if not edits and not edits_early: return text
    # each renamed decl renames uses of n up to the next decl of n (or end of file)
    by_name = {}
    for e in edits: by_name.setdefault(e[2], []).append(e)
    starts = {}
    for q in DECL.finditer(m):
        if depth[q.start()] == 0: starts.setdefault(q.group("n"), []).append(q.start())
    repl = []  # (pos, len, new)
    for n, es in by_name.items():
        for (ns, ne, _, nn, dend) in es:
            later = [p for p in starts.get(n, []) if p > ns]
            stop = later[0] if later else len(m)
            repl.append((ns, ne - ns, nn))
            for u in re.finditer(r"\b%s\b" % re.escape(n), m[dend:stop]):
                p = dend + u.start()
                # do not rename the in-file definition itself
                tail = m[p + len(n):p + len(n) + 400]
                if depth[p] == 0 and re.match(r"\s*\([^;{}]*\)\s*(?:[^;{}()#]+;\s*)*\{", tail): continue
                repl.append((p, len(n), nn))
    repl += edits_early
    repl.sort(reverse=True)
    t = text
    for p, l, new in repl:
        t = t[:p] + new + t[p + l:]
    if edits_early:
        hdr = "".join("extern int %s();\n" % nm for nm in sorted({e[2] for e in edits_early}))
        k = t.find("\n", t.find('#include')) + 1
        t = t[:k] + hdr + t[k:]
    return t


def ret_type(head, name):
    t = head[:head.rfind(name)]
    t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
    t = re.sub(r"\b(static|__inline__|inline|__inline|extern)\b", " ", t)
    t = re.sub(r"__attribute__\s*\(\(.*?\)\)", " ", t)
    return " ".join(t.split())


# ---- headers: gen/include/ ------------------------------------------------
# common.h is the host's own (host/include/rac1_common.h); include_asm.h is
# empty; the decomp's other headers get the same type rewrites as the C.
INC = os.path.join(OUT, "include")
if LEVEL is None: os.makedirs(INC, exist_ok=True)
for h in (glob.glob(os.path.join(REPO, "include", "*.h")) if LEVEL is None else []):
    bn = os.path.basename(h)
    if bn == "common.h":
        t = open(os.path.join(HERE, "host", "include", "rac1_common.h")).read()
    elif bn == "include_asm.h":
        t = "/* host: assembly inclusion has no meaning on the PC */\n"
    else:
        t = rewrite_types(open(h, errors="replace").read())
        if PLAT == "windows":
            t = re.sub(r'(__asm__|__asm|asm)(\s*\(\s*")((?:func|D)_\w+"\s*\))', r'\1\2_\3', t)
    open(os.path.join(INC, bn), "w").write(t)

stubbed, defined, asm_entries, report = [], set(), [], {}
gen_texts = {}
drafts = {os.path.basename(p)[:-2]: p for p in glob.glob(os.path.join(HERE, "host", "drafts", "func_*.c"))}
used_drafts = set()
files = sorted(os.path.relpath(p, REPO) for p in glob.glob(os.path.join(REPO, "src", "game", "*.c")))
files += sorted(os.path.relpath(p, REPO) for p in glob.glob(os.path.join(REPO, "src", "game", "movie", "*.c")))
files += ["src/core/989snd.c", "src/core/wad.c", "src/core/permcb.c"]
if LEVEL is not None:
    # the level's program: shared level code and the level's own
    files = sorted(os.path.relpath(p, REPO) for p in glob.glob(os.path.join(REPO, "src", "overlays", "shared", "*.c")))
    files += sorted(os.path.relpath(p, REPO) for p in glob.glob(os.path.join(REPO, "src", "overlays", "l%s_*" % LV, "*.c")))
files = [f.replace(os.sep, "/") for f in files]
dropped = []      # level mode: functions of other levels' programs
# tools/source_patches.py: small text fixes where the decomp's C is right for
# the EE ABI but not for x86 (each entry: file, old text, new text)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from source_patches import PATCHES
patches_applied = 0
# names.h: readable names are macros for the address names; expand them in
# the C (outside string literals) so every later pass sees func_/D_ names.
NAMES = {}
for q in re.finditer(r"^#define\s+(\w+)\s+((?:func|D)_\w+)\s*$", open(os.path.join(REPO, "include", "names.h")).read(), re.M):
    NAMES[q.group(1)] = q.group(2)
NAMES_RE = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, sorted(NAMES, key=len, reverse=True)))) if NAMES else None


def expand_names(src):
    if not NAMES_RE: return src
    m = strip_comments_keep_len(src)
    out, pos = [], 0
    for q in NAMES_RE.finditer(m):
        out.append(src[pos:q.start()]); out.append(NAMES[q.group(1)]); pos = q.end()
    out.append(src[pos:])
    return "".join(out)


for rel in files:
    src = expand_names(open(os.path.join(REPO, rel), errors="replace").read())
    for pf, old, new in PATCHES:
        if os.path.basename(rel) == pf:
            if old in src:
                src = src.replace(old, new); patches_applied += 1
            else:
                print("source patch not found in %s: %s" % (pf, old[:60]))
    # host/drafts/<func>.c: C for a function the decomp still has in assembly
    # (a near miss is fine, it only has to behave the same). It is pasted where
    # the INCLUDE_ASM line was, so it sees the same file context.
    def use_draft(mm):
        nm = re.findall(r"func_\w+", mm.group(2))
        if mm.group(1) == "INCLUDE_ASM" and nm and nm[-1] in drafts:
            used_drafts.add(nm[-1])
            return "\n/* host: draft for %s */\n%s\n" % (nm[-1], open(drafts[nm[-1]], errors="replace").read())
        return mm.group(0)
    src = ASM_MACRO.sub(use_draft, src)
    for mm in ASM_MACRO.finditer(src):
        kind, args = mm.group(1), mm.group(2)
        nm = re.findall(r"func_\w+", args)
        if kind in ("INCLUDE_ASM", "ASM_FUNC") and nm:
            if LEVEL is not None and nm[-1] not in PLACES: dropped.append(nm[-1]); continue
            asm_entries.append(nm[-1])
    src = ASM_MACRO.sub("", src)
    # MMI plzcw (leading sign bits) written as inline asm: a host helper
    src = re.sub(r'\b(?:__asm__|__asm|asm)\s*(?:__volatile__|volatile)?\s*\(\s*"plzcw\s+%0\s*,\s*%1"\s*:\s*"=r"\s*\(([^()]+)\)\s*:\s*"r"\s*\(((?:[^()]|\([^()]*\))+)\)\s*\)\s*;',
                 r"\1 = uya_plzcw((u32)(\2));", src)
    # matching-only asm that means nothing on the host: register pins
    # (`register s32 x __asm__("$16")`) and empty barriers
    src = re.sub(r'(\bregister\b[^;=(){}]*?)\s*(?:__asm__|__asm|asm)\s*\(\s*"\$\w+"\s*\)', r"\1", src)
    src = re.sub(r'\b(?:__asm__|__asm|asm)\s*(?:__volatile__|volatile)?\s*\(\s*""\s*(?::[^;]*)?\)\s*;', '__asm__ __volatile__("" ::: "memory");', src)
    # scheduling-only asm: nops (and .set directives) with no operands
    src = re.sub(r'\b(?:__asm__|__asm|asm)\s*(?:__volatile__|volatile)?\s*\(\s*((?:"(?:\s|nop|\\n|\\t|;|\.set\s+\w+)*"\s*)+)(?::[^;()]*)?\)\s*;', '__asm__ __volatile__("" ::: "memory");', src)
    items = list(top_level_items(src))
    # C names that are asm-label aliases of an address name (void f_r(...) __asm__("func_X");)
    lab = {q.group(1): q.group(2) for q in re.finditer(r"\b(\w+)\s*\((?:[^;{}()]|\([^()]*\))*\)\s*__asm__\s*\(\s*\"(func_(?:L\d\d_)?[0-9A-Fa-f]{8})\"\s*\)\s*;", src)}
    funcs = {x[3][0]: x for x in items if x[0] == "func"}
    masked = strip_comments_keep_len(src)
    # macros that expand to inline asm count as asm too
    asm_macros = set()
    for q in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)((?:[^\n]*\\\n)*[^\n]*)", src, re.M):
        if ASM_RE.search(q.group(2)): asm_macros.add(q.group(1))
    am = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, sorted(asm_macros)))) if asm_macros else None
    has_asm = {nm for nm, x in funcs.items() if ASM_RE.search(masked[x[3][1]:x[2]]) or (am and am.search(masked[x[3][1]:x[2]]))}
    # Level programs: a C function whose calls do not match its bytes (it calls a
    # function the bytes never call: the C was written for another level's copy, or
    # an identical-looking wrapper got the wrong name) is translated from the bytes.
    if LEVEL is not None and LTEXT is not None:
        for nm, x in funcs.items():
            b = base_func(nm)
            if not b or nm in has_asm or b not in PLACES or not CAT_SIZE.get(b): continue
            body = masked[x[3][1]:x[2]]
            callees = set(re.findall(r"\b(func_(?:L\d\d_)?[0-9A-Fa-f]{8})\w*\s*\(", body))
            callees |= {lab[c] for c in re.findall(r"\b(\w+)\s*\(", body) if c in lab}
            bad = []
            for a0 in PLACES[b][:1]:
                tg = level_call_targets(a0, CAT_SIZE[b])
                if tg is None: continue
                for c in callees:
                    if base_func(c) == b or not c.startswith("func_L"): continue   # the main program's functions have copies under other names
                    q = ps2_addr(c)                 # the address the host's call goes to
                    if q is None: continue
                    if q not in tg: bad.append(c)
            if bad:
                has_asm.add(nm); CALL_MISMATCH.append((nm, sorted(bad)))
    # testing (level programs): UYA_FORCE_ASM="file-substring,..." or "re:regex" translates
    # those functions from the level's bytes instead of compiling their C (to find a bad C function)
    # By default every level function is translated from the level's bytes
    # ("re:."): much of the overlay C is adapted and does not match them (the
    # hero files help_0020CDF0.c/help_00232560.c break Ratchet's walking, others
    # the right-stick camera and the camera's distance, a stray triangle).
    # UYA_FORCE_ASM="" compiles all the C; or a list: file substrings, re:regex,
    # !file (keep that file's C), e.g. "help_0020CDF0,help_00232560" (the old default).
    _fa = os.environ.get("UYA_FORCE_ASM", "re:.")
    if LEVEL is not None and _fa:
        _ex = [t[1:] for t in _fa.split(",") if t.startswith("!")]
        if any(x and x in rel for x in _ex): _fa = ""            # "!file-substring": that file keeps its C
        for tok in _fa.split(","):
            if tok.startswith("!"): continue
            kr = lambda nm: re.search(r"\)\s*[A-Za-z_][^{]*;\s*$", src[funcs[nm][1]:funcs[nm][3][1]].rstrip("{ \n\t"))   # K&R parameters: kept as C
            if tok.startswith("re:"):
                has_asm |= {nm for nm in funcs if base_func(nm) == nm and re.search(tok[3:], nm) and not kr(nm)}
                # C under another name with the function's asm label (impl_X(...) __asm__("func_LNN_X")): the same
                has_asm |= {nm for nm in funcs if lab.get(nm) in PLACES and re.search(tok[3:], lab[nm]) and not kr(nm)}
            elif tok and tok in rel:
                has_asm |= {nm for nm in funcs if base_func(nm) == nm and not kr(nm)}
    # propagate through same-file static helpers
    statics = {nm for nm, x in funcs.items() if re.search(r"\bstatic\b", src[x[1]:x[3][1]])}
    changed = LEVEL is None
    while changed:
        changed = False
        for nm, x in funcs.items():
            if nm in has_asm: continue
            body = masked[x[3][1]:x[2]]
            if any(re.search(r"\b%s\s*\(" % h, body) for h in has_asm & statics):
                has_asm.add(nm); changed = True
    out, pos = [], 0
    for kind, s, e, extra in items:
        out.append(src[pos:s])
        seg = src[s:e]
        if kind == "asm":
            seg = "/* host: top-level asm removed */"
        else:
            name, bpos = extra
            if LEVEL is not None and base_func(name) == name and (name.startswith("func_L") or is_main_func(name)) and name not in PLACES:
                dropped.append(name)
                out.append("/* host: %s is not in level %s's program */" % (name, LV)); pos = e
                continue
            # C written for another level (a shared function's C names its
            # canonical level's data, D_LNN_...) is not this level's: its bytes are used
            foreign = LEVEL is not None and re.match(r"func_L(\d\d)_", name) and name[6:8] != LV
            # static recompilation (RAC1_STATICRECOMP): every game function of the
            # executable is translated from the game's bytes too; only the host's
            # own implementations (host/src, host/overrides) stay
            static_main = (LEVEL is None and STATIC and base_func(name) == name and name.startswith("func_")
                           and name not in host_defined and name not in overrides
                           and not re.search(r"\)\s*[A-Za-z_][^{]*;\s*$", src[s:bpos].rstrip("{ \n\t")))
            if static_main or (LEVEL is not None and (name in has_asm or foreign) and name not in overrides):
                # inline asm (VU0, MMI...): the level's own bytes are translated
                # instead (tools/recomp.py); the definition becomes a declaration
                head = re.sub(r"\b(static|inline|__inline__|__inline)\b", " ", src[s:bpos])
                if re.search(r"\)\s*[A-Za-z_][^{]*;\s*$", head.rstrip("{ \n\t")):
                    head = head[:head.index("(")] + "()"            # K&R parameters: an old-style declaration
                out.append("/* host: %s %s: translated from the game's bytes */\n%s;" % (
                    name, "static" if static_main else ("has C for another level" if foreign else "has inline asm"), head.rstrip()))
                if lab.get(name) in PLACES: name = lab[name]          # labelled C: the label is the function
                asm_entries.append(name); report[name] = "static (recompiled)" if static_main else ("other level's C (recompiled)" if foreign else "inline asm (recompiled)")
                pos = e
                continue
            if lab.get(name) in overrides:
                seg = "/* host: %s (%s) is in host/overrides */" % (name, lab[name])
                report[lab[name]] = "override"
                out.append(seg); pos = e
                continue
            if name in has_asm or name in overrides:
                head = src[s:bpos]
                rt = ret_type(head, name)
                why = "override" if name in overrides else "inline asm"
                if name in overrides:
                    seg = "/* host: %s is in host/overrides */" % name
                else:
                    body = "{ uya_unimpl(\"%s\"); " % name
                    if rt != "void":
                        body += "{ %s uya_r; __builtin_memset(&uya_r, 0, sizeof uya_r); return uya_r; } " % rt
                    seg = head + body + "}"
                stubbed.append(name)
                report[name] = why
            else:
                report[name] = "c"
            if name.startswith("func_"): defined.add(name)
        out.append(seg)
        pos = e
    out.append(src[pos:])
    text = split_conflicts("".join(out), set(funcs))
    text = rewrite_types(text)
    # asm labels (`extern T x __asm__("D_00137C80");`) name a symbol directly;
    # COFF i386 (Windows) symbols carry a leading underscore
    if PLAT == "windows":
        text = re.sub(r'(__asm__|__asm|asm)(\s*\(\s*")((?:func|D)_\w+"\s*\))', r'\1\2_\3', text)
    gen_texts[rel[len("src/"):].replace("/", "_")] = text

# ---- argument pass-through -------------------------------------------------
# On the EE, arguments travel in $a0-$a3. A function the decomp writes as
# `void f(T *o) { g(o); }` can still hand its caller's 2nd and 3rd arguments
# to g, because f never touches $a1/$a2. On x86 those are lost. Where a call
# passes fewer arguments than the callee's definition takes, the missing ones
# are the enclosing function's own incoming arguments at those positions; the
# enclosing function gets extra `s32 uya_aN` parameters when it has too few.
FLOATY = re.compile(r"\b(f32|float|double|f64)\b")
HDR = re.compile(r"^[^\n;{}#]*?\b(func_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)[ \t]*\(([^;{}]*)\)\s*((?:[^;{}()#]+;\s*)*)\{", re.M)


def split_args(t):
    out, d, cur = [], 0, ""
    for ch in t:
        if ch in "([": d += 1
        elif ch in ")]": d -= 1
        if ch == "," and d == 0: out.append(cur); cur = ""
        else: cur += ch
    if cur.strip(): out.append(cur)
    return [x.strip() for x in out]


def params_of(plist, krdecls):
    ps = split_args(plist)
    if ps == ["void"] or ps == []: return []
    res = []
    for x in ps:
        if x == "...": res.append(("...", "...")); continue
        if re.fullmatch(r"\w+", x):  # K&R: type in the declarations
            q = re.search(r"([^;]*?)\b%s\b[^;]*;" % re.escape(x), krdecls)
            res.append((q.group(1) if q else "int", x))
        else:
            q = re.search(r"(\w+)\s*(\[[^\]]*\])?\s*$", x)
            res.append((x, q.group(1) if q else None))
    return res


def base_name(n):
    return re.sub(r"__(?:l\d\d)?(h\d+|imp)$", "", n)


def top_headers(t):
    """function definitions at file scope: (name, match) on the masked text"""
    m = strip_comments_keep_len(t)
    depth = [0] * (len(m) + 1); d = 0
    for i, ch in enumerate(m):
        depth[i] = d
        if ch == "{": d += 1
        elif ch == "}": d -= 1
    return m, [q for q in HDR.finditer(m) if depth[q.start()] == 0 and depth[q.end() - 1] == 0]


# 1. parameter lists of every definition
defs, info, rets = {}, {}, {}
for fn, t in gen_texts.items():
    m, hs = top_headers(t)
    info[fn] = (m, hs)
    for q in hs:
        defs[q.group(1)] = params_of(t[q.start(2):q.end(2)], t[q.start(3):q.end(3)])
        rets[q.group(1)] = ret_type(t[q.start():q.start(1) + len(q.group(1))], q.group(1))
# host functions (host/src, host/overrides) too: a decomp call that leaves out
# arguments the EE would still pass in registers gets them filled in
for hp in glob.glob(os.path.join(HERE, "host", "src", "**", "*.c"), recursive=True) + glob.glob(os.path.join(HERE, "host", "overrides", "*.c")):
    ht = open(hp, errors="replace").read()
    hm = strip_comments_keep_len(ht)
    for q in HDR.finditer(hm):
        if q.group(1) not in defs:
            defs[q.group(1)] = params_of(ht[q.start(2):q.end(2)], ht[q.start(3):q.end(3)])
            rets[q.group(1)] = ret_type(ht[q.start():q.start(1) + len(q.group(1))], q.group(1))
# level mode: the executable's definitions too (its main-segment functions
# have copies in the level; core functions are shared)
exe_def_names = set()
if LEVEL is not None and EXEGEN:
    _d = json.load(open(os.path.join(EXEGEN, "defs.json")))
    for n, v in _d.items():
        if n not in defs:
            defs[n] = [tuple(x) for x in v["params"]]; rets[n] = v["ret"]; exe_def_names.add(n)
# 2. short calls inside each definition
sites = {}   # fn -> list of (enclosing, name_start, callee_text, open, close, k, callee_base)
for fn, t in gen_texts.items():
    m, hs = info[fn]
    lst = []
    for q in hs:
        bstart = q.end() - 1
        bend = match_close(m, bstart)
        for c in re.finditer(r"(?<![\w)])\b(func_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)\s*\(", m[bstart:bend]):
            callee = base_name(c.group(1))
            if callee not in defs or callee == q.group(1): continue
            if any(t_ == "..." for t_, _ in defs[callee]): continue
            op = bstart + c.end() - 1
            cl = match_close(m, op, "(", ")")
            k = len(split_args(t[op + 1:cl]))
            lst.append((q.group(1), bstart + c.start(1), c.group(1), op, cl, k, callee))
    sites[fn] = lst
# 3. fixpoint: how many parameters each function needs
want = {n: len(p) for n, p in defs.items()}
for _ in range(20):
    ch = False
    for fn, lst in sites.items():
        for enc, _s, _c, _o, _l, k, callee in lst:
            mm_ = want[callee]
            if k >= mm_ or mm_ > 8: continue
            cp = defs[callee] + [("s32", "")] * (mm_ - len(defs[callee]))
            if any(FLOATY.search(cp[i][0]) or "struct" in cp[i][0] for i in range(k, mm_)): continue
            if any(defs[enc][i][1] is None for i in range(k, min(mm_, len(defs[enc])))): continue
            if want[enc] < mm_:
                want[enc] = mm_; ch = True
    if not ch: break
# 4. edit: extend headers, widen short calls
passthru_edits = 0
for fn, t in gen_texts.items():
    m, hs = info[fn]
    edits = []
    for q in hs:
        n = q.group(1); have = len(defs[n]); need = want[n]
        if need <= have: continue
        add = ["uya_a%d" % i for i in range(have, need)]
        plist = t[q.start(2):q.end(2)]
        if have == 0:
            edits.append((q.start(2), q.end(2), ", ".join("s32 " + a for a in add)))
        elif q.group(3).strip():  # K&R
            edits.append((q.end(2), q.end(2), ", " + ", ".join(add)))
            edits.append((q.end(3), q.end(3), " ".join("s32 %s;" % a for a in add) + " "))
        else:
            edits.append((q.end(2), q.end(2), ", " + ", ".join("s32 " + a for a in add)))
    for enc, cs, cname, op, cl, k, callee in sites[fn]:
        mm_ = want[callee]
        if k >= mm_ or mm_ > 8: continue
        cp = defs[callee] + [("s32", "")] * (mm_ - len(defs[callee]))
        if any(FLOATY.search(cp[i][0]) or "struct" in cp[i][0] for i in range(k, mm_)): continue
        own = [x[1] for x in defs[enc]] + ["uya_a%d" % i for i in range(len(defs[enc]), want[enc])]
        if mm_ > len(own) or any(x is None for x in own[k:mm_]): continue
        orig = t[op + 1:cl]
        full = orig + (", " if k else "") + ", ".join(own[k:mm_])
        rt = rets.get(callee, "")
        if re.fullmatch(r"(void|int|unsigned int|unsigned|short|char|float|double|long long|unsigned long long|[su](8|16|32|64)|f32|f64)(\s*\*)*", rt):
            edits.append((cs, op, "((%s (*)())%s)" % (rt, cname)))
        else:
            edits.append((cs, op, "((__typeof__(%s(%s)) (*)())%s)" % (cname, full, cname)))
        edits.append((cl, cl, (", " if k else "") + ", ".join(own[k:mm_])))
    for st, en, txt in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        t = t[:st] + txt + t[en:]
    gen_texts[fn] = t
    passthru_edits += len(edits)
print("argument pass-through edits:", passthru_edits)
# the extended definitions take the extra arguments (thunks pass them from the registers)
for n in list(defs):
    if n in want and want[n] > len(defs[n]) and not any(x[0] == "..." for x in defs[n]) and n not in exe_def_names:
        defs[n] = defs[n] + [("s32 uya_a%d" % i, "uya_a%d" % i) for i in range(len(defs[n]), want[n])]

# ---- 64-bit pass-through parameters ----------------------------------------
# A parameter the C declares as a 32-bit integer but only ever hands on, as a
# call argument, to a 64-bit parameter of another definition: on the EE the
# register carries all 64 bits from the caller to that call whatever the C
# says (retail moves it with daddu); on x86 the upper word is lost. So such a
# parameter becomes 64 bits. (func_001F69F0, FontPrintWindow's scaled-text
# path: the 64-bit TEX0 from GetEffectTex lost CBP/TCC/TH, so window titles --
# "Options", "Language" on the main menu -- drew nothing.)
def _defs_by_addr():
    r = {}
    for n, p in defs.items():
        if re.fullmatch(r"func_(?:L\d\d_)?[0-9A-Fa-f]{8}", n) and not any(x[0] == "..." for x in p):
            a = ps2_addr(n)
            if a is not None: r[a] = p
    return r
_DBA = _defs_by_addr()
_INTPAR = re.compile(r"\s*(?:const\s+)?(?:signed\s+|unsigned\s+)?(?:int|s32|u32|long|unsigned|signed)\s+(\w+)\s*")
_widened = []
for fn in list(gen_texts):
    t = gen_texts[fn]
    m, hs = top_headers(t)
    edits = []
    for q in hs:
        name = q.group(1)
        if q.group(3).strip(): continue                     # K&R
        plist = t[q.start(2):q.end(2)]
        parts = split_args(plist)
        if not parts or parts == ["void"] or "..." in parts: continue
        bstart = q.end() - 1
        bend = match_close(m, bstart)
        body = m[bstart:bend]
        wide = []
        for i, x in enumerate(parts):
            pm = _INTPAR.fullmatch(x)
            if not pm or re.search(r"\blong\s+long\b", x): continue
            pn = pm.group(1)
            occ = list(re.finditer(r"(?<![\w.>])%s\b" % re.escape(pn), body))
            if not occ: continue
            ok, hit = True, False
            for o in occ:
                pre = body[:o.start()].rstrip(); post = body[o.end():].lstrip()
                if not pre or pre[-1] not in "(," or not post or post[0] not in "),":
                    ok = False; break
                # the call this argument belongs to: the innermost unclosed '('
                d, j = 0, o.start() - 1
                while j >= 0:
                    if body[j] == ")": d += 1
                    elif body[j] == "(":
                        if d == 0: break
                        d -= 1
                    j -= 1
                cm = re.search(r"\b(func_(?:L\d\d_)?[0-9A-Fa-f]{8}\w*)\s*$", body[:j]) if j >= 0 else None
                if not cm: ok = False; break
                cdef = _DBA.get(ps2_addr(cm.group(1)))
                cl = match_close(body, j, "(", ")")
                args = split_args(body[j + 1:cl])
                if cdef is None or len(args) != len(cdef): ok = False; break
                k = [ai for ai, ax in enumerate(args) if re.fullmatch(r"\s*%s\s*" % re.escape(pn), ax)]
                if not k: ok = False; break
                for ai in k:
                    ct = cdef[ai][0][:-len(cdef[ai][1])] if cdef[ai][1] and cdef[ai][0].endswith(cdef[ai][1]) else cdef[ai][0]
                    if re.search(r"\blong\s+long\b|\b[su]64\b", ct) and "*" not in ct: hit = True
                    else: ok = False
                if not ok: break
            if ok and hit: wide.append((i, pn))
        if not wide: continue
        newp = list(parts)
        for i, pn in wide: newp[i] = "unsigned long long " + pn
        edits.append((q.start(2), q.end(2), ", ".join(newp)))
        if name in defs:
            _wi = {i for i, _ in wide}
            defs[name] = [("unsigned long long " + p[1], p[1]) if i in _wi else p for i, p in enumerate(defs[name])]
        _widened.append("%s(%s)" % (name, ", ".join(pn for _, pn in wide)))
    for st, en, txt in sorted(edits, reverse=True):
        t = t[:st] + txt + t[en:]
    gen_texts[fn] = t
print("64-bit pass-through parameters widened:", ", ".join(_widened) or "none")

# ---- float/int argument order ---------------------------------------------
# On the EE, integer arguments go in $a0-$a3/$t0-$t3 and float arguments in
# $f12-$f19, each class in its own order. So the decomp can declare
# f(float, int) at a call site for a function defined as f(int, float), and on
# the PS2 both mean the same. On x86 only the position counts. Where a call
# goes through a prototype (or a cast) whose float and int parameters are the
# definition's in the same per-class order but interleaved differently, the
# call's arguments and that prototype are put in the definition's order.
def is_float(ty):
    return bool(FLOATY.search(ty)) and "*" not in ty and "(" not in ty

def class_perm(decl, defn):
    """for each definition position, the decl index that supplies it (None:
    the call has no argument there, it gets 0), or None if the two cannot be
    matched class by class (float vs integer)"""
    if len(decl) > len(defn) or any(x[0] == "..." for x in decl + defn): return None
    di = [i for i, x in enumerate(decl) if not is_float(x[0])]
    df = [i for i, x in enumerate(decl) if is_float(x[0])]
    ei = [i for i, x in enumerate(defn) if not is_float(x[0])]
    ef = [i for i, x in enumerate(defn) if is_float(x[0])]
    if len(di) > len(ei) or len(df) > len(ef): return None
    perm = [None] * len(defn)
    for a, b in zip(ei, di): perm[a] = b
    for a, b in zip(ef, df): perm[a] = b
    return perm

def size_class(ty):
    if "*" in ty or "(" in ty: return "i"
    if is_float(ty): return "f"
    if re.search(r"\b(long\s+long|s64|u64)\b", ty): return "l"
    if re.search(r"\bstruct\b|\b[A-Z]\w*_[0-9A-F]{6}\b|\bu128\b|\bQ_|\bV16", ty): return "s"
    return "i"

CANON = {"f": "f32", "l": "unsigned long long", "i": "s32"}

fdefs = {}
for n, p in defs.items():
    a = ps2_addr(n)
    if a is not None and re.fullmatch(r"func_(?:L\d\d_)?[0-9A-Fa-f]{8}", n) and not any(x[0] == "..." for x in p):
        fdefs[a] = p
for op_ in glob.glob(os.path.join(HERE, "host", "overrides", "func_*.c")):
    ot = open(op_, errors="replace").read()
    for q in HDR.finditer(strip_comments_keep_len(ot)):
        n = q.group(1); a = ps2_addr(n)
        if a is None or not re.fullmatch(r"func_(?:L\d\d_)?[0-9A-Fa-f]{8}", n): continue
        fdefs[a] = params_of(ot[q.start(2):q.end(2)], ot[q.start(3):q.end(3)])
# definitions under another C name with an asm label to the address name
# (`void func_001F6668_r(...) __asm__("func_001F6668"); void func_001F6668_r(...) {`):
# the reference for the address's other prototypes too (FontPrint's 64-bit colour
# and texture were passed as 32-bit values by `(void *, ..., int, ...)` prototypes)
for fn_, t_ in gen_texts.items():
    for q in re.finditer(r"\b(func_(?:L\d\d_)?[0-9A-Fa-f]{8}\w+)\s*\((?:[^;{}()]|\([^()]*\))*\)\s*__asm__\s*\(\s*\"_?(func_(?:L\d\d_)?[0-9A-Fa-f]{8})\"\s*\)\s*;",
                         t_):
        a = ps2_addr(q.group(2))
        if a is None or a in fdefs or q.group(1) not in defs or ps2_addr(q.group(1)) != a: continue
        if any(x[0] == "..." for x in defs[q.group(1)]): continue
        fdefs[a] = defs[q.group(1)]
PROTO = re.compile(r"\b(func_(?:L\d\d_)?[0-9A-Fa-f]{8}\w*)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*(?:__asm__\s*\(\s*\"([^\"]*)\"\s*\)\s*)?;")
CAST = re.compile(r"\(\s*\(\s*[\w\s\*]+\(\s*\*\s*\)\s*\(((?:[^()]|\([^()]*\))*)\)\s*\)\s*(func_(?:L\d\d_)?[0-9A-Fa-f]{8}\w*)\s*\)\s*\(")
reorder_edits = 0
# Assembly-only functions have no definition: one of their declarations is
# made the reference instead (the longest plain extern prototype), the other
# prototypes are put in its float/int order, and recomp.py's C entry for the
# function takes it (sigs.json). Without this, `f(float, void *)` here and
# `f(void *, float)` there, the same call on the EE, reach the entry with the
# arguments swapped.
REF_DECL = {}
for fn, t_ in gen_texts.items():
    m_ = strip_comments_keep_len(t_)
    for q in PROTO.finditer(m_):
        if q.group(3): continue
        a = ps2_addr(q.group(1))
        if a is None or a in fdefs: continue
        if not re.search(r"\bextern\b[^;{}]*$", m_[max(0, q.start() - 200):q.start()]): continue
        parts = [x for x in split_args(t_[q.start(2):q.end(2)]) if x != "void"]
        if not parts or "..." in parts: continue
        if a not in REF_DECL or len(parts) > len(REF_DECL[a]): REF_DECL[a] = parts
# 64-bit parameters of assembly-only functions: the reference's 32-bit integer
# at a position becomes 64 bits where another view of the function (a labeled
# prototype with as many parameters) or OpenRAC's near-miss C for it
# (nonmatching/*/func_X.c) has a 64-bit type there. On the EE the register
# carries 64 bits whatever the C says; on x86 the reference decides what the
# C entry takes (func_001F7070's colour and texture).
def _views64():
    v = {}
    for fn_, t_ in gen_texts.items():
        for q in PROTO.finditer(t_):
            if not q.group(3): continue
            a = ps2_addr(q.group(3).lstrip("_"))
            if a is None or ps2_addr(q.group(1)) != a: continue
            v.setdefault(a, []).append([re.sub(r"\blong(?!\s+long)\b", "long long", x) for x in split_args(q.group(2)) if x != "void"])
    for p_ in glob.glob(os.path.join(REPO, "nonmatching", "*", "func_*.c")):
        t_ = re.sub(r"\blong(?!\s+long)\b", "long long", open(p_, errors="replace").read())   # EE long: 64 bits
        for q in re.finditer(r"\b(func_[0-9A-Fa-f]{8}\w*)\s*\(([^;{}()]*)\)\s*(?:__asm__\s*\(\s*\"(func_[0-9A-Fa-f]{8})\"\s*\))?\s*[;{]", t_):
            a = ps2_addr(q.group(3) or q.group(1))
            if a is None: continue
            v.setdefault(a, []).append([x for x in split_args(q.group(2)) if x != "void"])
    return v
_V64 = _views64()
_n64 = 0
for a, parts in REF_DECL.items():
    for view in _V64.get(a, []):
        if len(view) != len(parts) or "..." in view: continue
        for i, x in enumerate(view):
            vt = re.sub(r"\b\w+\s*$", "", x) if re.search(r"[\w\*\]]\s+\w+\s*$", x) and not re.fullmatch(r"\s*(unsigned|signed)?\s*\w+\s*", x) else x
            if size_class(vt) == "l" and size_class(parts[i]) == "i":
                parts[i] = "unsigned long long"; _n64 += 1
print("64-bit parameters of assembly-only functions from other views:", _n64)
for a, parts in REF_DECL.items():
    fdefs[a] = [(x, None) for x in parts]

def plan(types_text, a, labeled=False):
    """(new parameter text, mapping) for a prototype or cast, or None when it
    already agrees with the definition. mapping[i] is the argument index that
    goes to definition position i (None: the call has none there; 0 is passed).
    A prototype under its own name with an asm label (`f_x(...) __asm__("f")`)
    is a deliberate view of the function: only its float/int order is fixed."""
    parts = [x for x in split_args(types_text) if x != "void"]
    defn = fdefs[a]
    if not parts or "..." in parts or not defn: return None
    perm = class_perm([(x, None) for x in parts], defn)
    if perm is None: return None
    if labeled:
        # a view keeps its own types, put in the definition's float/int order; a
        # 32-bit integer where the definition has 64 bits is widened (the EE register
        # carries all 64: FontPrint's texture from GetEffectTex)
        if None in perm or len(perm) != len(parts): return None
        vt = []
        for i, src in enumerate(perm):
            dc, sc = size_class(fdefs[a][i][0]), size_class(parts[src])
            # and a 64-bit integer where the definition has 32 bits is narrowed:
            # on x86 a wider argument shifts every later one (FontPrintCenter's
            # colour, func_001F6F40, declared u64 in pause.c)
            vt.append(CANON["l"] if (dc == "l" and sc == "i") else (CANON["i"] if (dc == "i" and sc == "l") else parts[src]))
        if perm == list(range(len(parts))) and vt == parts: return None
        return ", ".join(vt), perm
    types = []
    for i, src in enumerate(perm):
        dc = size_class(defn[i][0])
        if src is None:
            if dc == "s": return None
            types.append(CANON[dc]); continue
        sc = size_class(parts[src])
        if "s" in (sc, dc) and sc != dc: return None
        types.append(parts[src] if sc == dc else CANON[dc])
    if perm == list(range(len(parts))) and types == parts: return None
    return ", ".join(types), perm

PROTO_VIEW = re.compile(r"\b([A-Za-z_]\w*)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*__asm__\s*\(\s*\"(_?func_(?:L\d\d_)?[0-9A-Fa-f]{8}\w*)\"\s*\)\s*;")
n_view_fix = [0]
def is_proto_pre(pre):
    """the text before a `name(...);` makes it a prototype: `extern ...`, or a
    file-scope declaration without it (`E *func_L00_0026DEA0(V *, int, float, ...);`
    at the start of a line: a call or a local declaration is indented)"""
    if re.search(r"\bextern\b[^;{}]*$", pre): return True
    return re.search(r"(?:^|\n)(?!return\b|else\b|case\b|do\b|goto\b)[A-Za-z_][\w \t\*]*$", pre) is not None
for fn, t in gen_texts.items():
    m = strip_comments_keep_len(t)
    edits = []
    perms = {}
    for q in PROTO.finditer(m):
        a = ps2_addr(q.group(1))
        if a not in fdefs: continue
        if q.group(3) and ps2_addr(t[q.start(3):q.end(3)].lstrip("_")) != a: continue     # an asm label to another address
        pre = m[max(0, q.start() - 200):q.start()]
        if not is_proto_pre(pre): continue
        r = plan(t[q.start(2):q.end(2)], a, labeled=bool(q.group(3)))
        if r is None: continue
        edits.append((q.start(2), q.end(2), r[0]))
        if r[1] != list(range(len(r[1]))): perms[q.group(1)] = r[1]
    # views under a name of their own (`float cb90_alt(float, int, float)
    # __asm__("func_L00_0025CB90");`): the same fix, keyed by that name
    for q in PROTO_VIEW.finditer(t):                          # t: the label is a string, blank in m
        if m[q.start():q.end(1)] != q.group(1): continue      # in a comment
        if q.group(1).startswith("func_"): continue          # done above
        a = ps2_addr(t[q.start(3):q.end(3)].lstrip("_"))
        if a not in fdefs: continue
        pre = m[max(0, q.start() - 200):q.start()]
        if not is_proto_pre(pre): continue
        r = plan(t[q.start(2):q.end(2)], a, labeled=True)
        if r is None: continue
        edits.append((q.start(2), q.end(2), r[0]))
        if r[1] != list(range(len(r[1]))): perms[q.group(1)] = r[1]
        n_view_fix[0] += 1
    def fix_call(op, perm):
        cl = match_close(m, op, "(", ")")
        args = split_args(t[op + 1:cl])
        if len(args) != sum(1 for x in perm if x is not None): return
        edits.append((op + 1, cl, ", ".join(args[i] if i is not None else "0" for i in perm)))
    cast_spans = []
    for q in CAST.finditer(m):
        a = ps2_addr(q.group(2))
        if a not in fdefs: continue
        r = plan(t[q.start(1):q.end(1)], a)
        if r is None: continue
        edits.append((q.start(1), q.end(1), r[0]))
        if r[1] != list(range(len(r[1]))): fix_call(q.end() - 1, r[1])
        cast_spans.append(q.start(2))
    for name, perm in perms.items():
        for c in re.finditer(r"(?<![\w.>])\b%s\s*\(" % re.escape(name), m):
            if c.start() in cast_spans: continue
            before = m[max(0, c.start() - 200):c.start()]
            if is_proto_pre(before): continue    # the prototype itself
            fix_call(c.end() - 1, perm)
    for st, en, txt in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        t = t[:st] + txt + t[en:]
    gen_texts[fn] = t
    reorder_edits += len(edits)
if CALL_MISMATCH:
    print("level %s: %d C functions call what their bytes do not; translated from the bytes instead:" % (LV, len(CALL_MISMATCH)))
    for nm, bad in CALL_MISMATCH: print("   %s (calls %s)" % (nm, ", ".join(bad)))
print("prototype fixes (float/int order, 64-bit sizes):", reorder_edits, "; of views under their own names:", n_view_fix[0])

# ---- function addresses as values (level mode) ------------------------------
# A function named as a value (a callback argument, a pointer stored in a
# moby) is its PS2 address on the PS2, and the game's code calls it, stores it
# and compares it as such. In C on the host the name is the host function;
# where the level's translated code or a table gets the value, it must be the
# PS2 address (which is a trampoline to the same host function, so a C call
# through it works too). So such uses become the PS2 address, typed as the
# function. Direct calls stay direct.
FVAL = re.compile(r"(&\s*)?\b(func_(?:L\d\d_)?[0-9A-Fa-f]{8}\w*)\b")
fval_edits = 0
if LEVEL is not None:
    for fn, t in gen_texts.items():
        m = strip_comments_keep_len(t)
        edits = []
        for q in FVAL.finditer(m):
            nm = q.group(2)
            a = ps2_addr(nm)
            if a is None: continue
            after = m[q.end():q.end() + 40].lstrip()
            before = m[max(0, q.start() - 40):q.start()].rstrip()
            if not after or after[0] == "(": continue                        # a call
            if after[0] == ")" and after[1:].lstrip().startswith("("): continue   # ((T (*)())f)(...): a call
            if after[0] not in ";,)}:?+-=!" : continue
            if not before or (before[-1] not in "=,({?:)!+-" and not re.search(r"\breturn$", before)): continue
            edits.append((q.start(), q.end(), "((__typeof__(%s) *)0x%08X)" % (nm, a)))
        for st, en, tx in sorted(edits, reverse=True):
            t = t[:st] + tx + t[en:]
        gen_texts[fn] = t
        fval_edits += len(edits)
    print("level %s: function values made PS2 addresses: %d" % (LV, fval_edits))
for fn, t in gen_texts.items():
    open(os.path.join(OUT, fn), "w").write(t)

for nm in overrides | host_defined: defined.add(nm)
# asm-only entries: one stub each (no prototype in scope, so any caller signature links)
with open(os.path.join(OUT, "asm_stubs.c"), "w") as f:
    f.write('#include "uya_host.h"\n/* Functions still in assembly in the decomp. Replace one by adding host/overrides/<name>.c. */\n')
    for nm in asm_entries:
        if nm in overrides or nm in defined: continue
        f.write('long long %s(void) { uya_unimpl("%s"); return 0; }\n' % (nm, nm))
        report[nm] = "asm stub"; defined.add(nm)
for nm in overrides | host_defined: defined.add(nm)
# every function the generated C defines (whatever form its header takes)
for fn_, t_ in gen_texts.items():
    for q in top_headers(t_)[1]:
        defined.add(q.group(1))
# functions defined under another C name with an asm label to the address name:
#   void func_00203B70_r(...) __asm__("func_00203B70"); void func_00203B70_r(...) { ... }
LABEL_ALIAS = {}
for fn_, t_ in gen_texts.items():
    m_ = strip_comments_keep_len(t_)
    hdrs = {q.group(1) for q in top_headers(t_)[1]}
    hdrs |= set(re.findall(r"^[^\n;{}#]*?\b(\w+)[ \t]*\([^;{}]*\)\s*\{", m_, re.M))
    for q in re.finditer(r"\b(\w+)\s*\((?:[^;{}()]|\([^()]*\))*\)\s*__asm__\s*\(\s*\"_?(func_(?:L\d\d_)?[0-9A-Fa-f]{8})\"\s*\)\s*;", t_):
        if q.group(1) in hdrs and q.group(1) != q.group(2):
            LABEL_ALIAS[q.group(2)] = q.group(1)
            defined.add(q.group(2))
# trampoline table: every host function with a PS2 address, so pointers to it work
TT = "uya_tramp_table" + SFX
with open(os.path.join(OUT, "tramp_table.c"), "w") as f:
    f.write('/* generated by gen_host.py: PS2 address -> host function */\n#include "uya_runtime.h"\n')
    tl = sorted((ps2_addr(n), n) for n in defined if ps2_addr(n) is not None and re.fullmatch(r"func_(?:L\d\d_)?[0-9A-Fa-f]{6,8}", n))
    if LEVEL is not None:
        # the level's code only (the core keeps the executable's), at every place it has
        tl = sorted((a, n) for n in defined if n in PLACES for a in PLACES[n])
    seen_a = set(); rows = []
    for a, n in tl:
        if a in seen_a: continue
        seen_a.add(a); rows.append((a, n))
    for a, n in rows: f.write("extern void %s(void);\n" % n)
    f.write("const uya_tramp_t %s[] = {\n" % TT)
    for a, n in rows: f.write("    {0x%08X, (void *)%s},\n" % (a, n))
    f.write("};\nconst int %s = %d;\n" % (TT.replace("table", "count"), len(rows)))

# absolute symbols
abs_syms = {}
for fn in ("config/symbol_addrs.txt", "config/port_symbols.txt"):
    p = os.path.join(REPO, fn)
    if not os.path.exists(p): continue
    for l in open(p):
        q = re.match(r"\s*(\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", l)
        if q and q.group(1) not in abs_syms: abs_syms[q.group(1)] = int(q.group(2), 16)
if LEVEL is not None:
    # the executable's names below the main segment only: the rest moved
    abs_syms = {n: a for n, a in abs_syms.items() if a < RESIDENT and not is_main_func(n)}
abs_syms.update(ALIASES)
# anything else the generated C names by address (labels the decomp defines in its data asm)
for p in glob.glob(os.path.join(OUT, "*.c")) + glob.glob(os.path.join(OUT, "include", "*.h")):
    if os.path.basename(p) in ("tramp_table.c",): continue
    for nm in set(re.findall(r"(?<![A-Za-z0-9])((?:D|func)_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)", open(p).read())):
        if nm not in abs_syms and nm not in defined and ps2_addr(nm): abs_syms[nm] = ps2_addr(nm)
pre = "_" if PLAT == "windows" else ""
if LEVEL is not None:
    # names used by the level C that the executable pass does not provide
    # (or provides with another value) get the suffix; so do every function the
    # level pass defines and every main-segment name
    exe_abs = {}
    for l in open(os.path.join(EXEGEN, "abs_syms.s")):
        q = re.match(r"\.set\s+_?(\w+),\s*0x([0-9A-Fa-f]+)", l)
        if q: exe_abs[q.group(1)] = int(q.group(2), 16)
    exe_defined = set(json.load(open(os.path.join(EXEGEN, "defined.json"))))
    texts_all = "".join(open(p).read() for p in glob.glob(os.path.join(OUT, "*.c")) if os.path.basename(p) != "tramp_table.c")
    used = set(re.findall(r"(?<![A-Za-z0-9])((?:D|func)_(?:L\d\d_)?[0-9A-Fa-f]{6,8}\w*)", texts_all))
    host_main = {n for n in host_defined if is_main_func(n)}
    # main-segment overrides (host/overrides) the level has: compiled again for it (build.py)
    lvl_overrides = sorted(n for n in overrides if is_main_func(n) and n in PLACES)
    open(os.path.join(OUT, "overrides.txt"), "w").write("".join(n + "\n" for n in lvl_overrides))
    # the host's main-segment functions (host/src and host/overrides): recomp.py
    # looks for the ones the catalogue does not place in the level's bytes
    open(os.path.join(OUT, "host_main.txt"), "w").write("".join(
        "%s %s\n" % (n, "src" if n in host_main else "override") for n in sorted(host_main | {o for o in overrides if is_main_func(o)})))
    lvl_defined = {n for n in defined if n not in host_defined and (n not in overrides or n in lvl_overrides)}
    RENAME = set()
    level_abs = {}
    for nm in sorted(used | set(abs_syms)):
        if nm in host_main: continue
        if nm in lvl_defined or is_main_func(nm) or base_func(nm) and base_func(nm).startswith("func_L") and ps2_addr(nm) is None:
            RENAME.add(nm); continue
        if nm in PLACES:            # a function of this level's program: recomp.py translates it from the bytes
            RENAME.add(nm); continue
        a = abs_syms.get(nm, ps2_addr(nm))
        if a is None or a == 0: continue
        if nm in exe_defined or exe_abs.get(nm) == a: continue        # the executable's, the same here
        if a >= RESIDENT and re.fullmatch(r"D_[0-9A-F]{8}", nm):
            print("level %s: warning: %s is an executable data name in the moved range" % (LV, nm))
        RENAME.add(nm); level_abs[nm] = a
    # main-segment and level names the level C calls but the level's program does not have
    missing = sorted(nm for nm in used if base_func(nm) == nm and nm not in host_main and (is_main_func(nm) or nm.startswith("func_L"))
                     and nm not in PLACES and nm not in lvl_defined and ps2_addr(nm) is None and nm not in overrides)
    with open(os.path.join(OUT, "missing.c"), "w") as f:
        f.write('#include "uya_host.h"\n/* names the level C uses that level %s\'s program does not have */\n' % LV)
        for nm in missing:
            f.write('long long %s(void) { uya_unimpl("%s (not in level %s)"); return 0; }\n' % (nm, nm, LV))
    RENAME |= set(missing)
    # every main-segment function the executable has: the level's copy, if it has one
    for u in json.load(open(os.path.join(REPO, "progress", "report.json")))["units"]:
        for fn_ in u.get("functions", []):
            if is_main_func(fn_["name"]) and fn_["name"] not in host_main: RENAME.add(fn_["name"])
    for nm in PLACES:
        if nm not in host_main: RENAME.add(nm)
    for nm in overrides:
        if is_main_func(nm): RENAME.add(nm)
    RENAME -= host_main
    with open(os.path.join(OUT, "l%s_names.h" % LV), "w") as f:
        f.write("/* generated by gen_host.py: level %s's names (forced into each of its C files) */\n" % LV)
        for nm in sorted(RENAME):
            f.write("#define %s %s%s\n" % (nm, nm, SFX))
    # asm labels name symbols directly: the same renames
    lab_re = re.compile(r'((?:__asm__|__asm|asm)\s*\(\s*")(_?)((?:func|D)_\w+)("\s*\))')
    for p in glob.glob(os.path.join(OUT, "*.c")):
        tx = open(p).read()
        t2 = lab_re.sub(lambda q: q.group(1) + q.group(2) + q.group(3) + (SFX if q.group(3) in RENAME else "") + q.group(4), tx)
        if t2 != tx: open(p, "w").write(t2)
    with open(os.path.join(OUT, "abs_syms.s"), "w") as f:
        f.write("# Level %s: symbols its C places by address (data at its PS2 address in the level's program).\n" % LV)
        for nm, a in sorted(level_abs.items()):
            f.write(".globl %s%s%s\n.set %s%s%s, 0x%X\n" % (pre, nm, SFX, pre, nm, SFX, a))
    abs_syms = level_abs
with open(os.path.join(OUT, "abs_syms.s") if LEVEL is None else os.devnull, "w") as f:
    f.write("# Symbols the decomp places by address (data at its PS2 address; calls go through trampolines).\n")
    for nm, a in sorted(abs_syms.items()):
        if nm in defined or nm in ("func_0",) or a == 0: continue
        f.write(".globl %s%s\n.set %s%s, 0x%X\n" % (pre, nm, pre, nm, a))
with open(os.path.join(OUT, "funcs.tsv"), "w") as f:
    for nm in sorted(report):
        f.write("%s\t%s\n" % (nm, report[nm]))
c = {}
for v in report.values(): c[v] = c.get(v, 0) + 1
print("drafts used:", len(used_drafts), "of", len(drafts))
print("functions:", c, "absolute symbols:", sum(1 for n in abs_syms if n not in defined))

# ---- signatures for tools/recomp.py ----------------------------------------
# For every function with a PS2 address: its ABI classes (i: 32-bit int or
# pointer, l: 64-bit int, f: float, d: double, q: 128-bit, s: struct by
# value, v: void) from its definition (generated C, host code) or, for the
# assembly-only ones, from the declarations the C makes of them.
def abi_class(t):
    t = " ".join(re.sub(r"\b(const|volatile|register|static|extern|inline|__inline__|signed)\b", " ", t or "").split())
    if not t or t == "void": return "v"
    if "*" in t or "(" in t or "[" in t: return "i"
    if re.search(r"\b(float|f32)\b", t): return "f"
    if re.search(r"\b(double|f64)\b", t): return "d"
    if re.search(r"\blong long\b|\b[su]64\b|\blong\b", t): return "l"
    if re.search(r"\bu128\b|\bqword\b|ext_vector|__int128|\bU128\b|\bvec128\b", t): return "q"
    if re.search(r"\b(struct|union)\b", t): return "s"
    return "i"

def param_type(p):
    ty = p[0]
    if p[1] and ty.endswith(p[1]): ty = ty[:-len(p[1])]
    return ty

for real, cname in LABEL_ALIAS.items():
    if cname in defs and real not in defs:
        defs[real] = defs[cname]; rets[real] = rets.get(cname, "int")
sigs = {}
for n, ps in defs.items():
    a = ps2_addr(n)
    if a is None: continue
    src_ = "def"
    if n in exe_def_names:
        if n not in host_defined and n not in overrides:
            if not is_main_func(n): continue          # core: the executable pass's
            src_ = "decl"                             # the level's copy is translated from its bytes
    sigs[n] = {"addr": a, "ret": abi_class(rets.get(n, "int")), "params": [abi_class(param_type(p)) for p in ps if p[0] != "..."],
               "var": any(p[0] == "..." for p in ps), "src": src_}
    if src_ == "decl": sigs[n]["unk"] = False
DECLP = re.compile(r"\bextern\s+([^;{}()]*?)\b(func_(?:L\d\d_)?[0-9A-Fa-f]{8})(\w*)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*(?:__asm__\s*\([^)]*\))?\s*;")
for fn_, t_ in gen_texts.items():
    m_ = strip_comments_keep_len(t_)
    for q in DECLP.finditer(m_):
        n = q.group(2)
        if q.group(3) and not re.fullmatch(r"__h\d+|__imp|_\w+", q.group(3)): pass
        if n in sigs and sigs[n]["src"] == "def": continue
        plist = t_[q.start(4):q.end(4)].strip()
        if plist in ("", "void"): ps = []
        else: ps = [x for x in split_args(plist)]
        cls = [abi_class(re.sub(r"\b\w+\s*$", "", x) if re.search(r"[\w\*\]]\s+\w+\s*$", x) and not re.fullmatch(r"\s*(unsigned|signed)?\s*\w+\s*", x) else x) for x in ps if x != "..."]
        cur = sigs.get(n)
        cand = {"addr": ps2_addr(n), "ret": abi_class(q.group(1)), "params": cls, "var": "..." in ps, "src": "decl", "unk": plist == ""}
        if cur is None or (cur.get("unk") and not cand["unk"]) or (not cand["unk"] and len(cand["params"]) > len(cur["params"])): sigs[n] = cand
def _decl_class(x):
    return abi_class(re.sub(r"\b\w+\s*$", "", x) if re.search(r"[\w\*\]]\s+\w+\s*$", x) and not re.fullmatch(r"\s*(unsigned|signed)?\s*\w+\s*", x) else x)
for n, s in sigs.items():
    if s.get("src") == "decl" and s.get("addr") in REF_DECL:
        s["params"] = [_decl_class(x) for x in REF_DECL[s["addr"]]]; s["var"] = False; s["unk"] = False
# A 64-bit return: the EE returns the whole $v0. If any declaration of an
# assembly-only function (plain or under another name with an asm label, e.g.
# `extern s64 get_effect_texture(s32) __asm__("func_001F4868")`) returns 64
# bits, its C entry returns all 64 (callers declaring int still read the low
# half). Without this GetEffectTex's TEX0 lost its upper word (CBP, TCC, TH).
RET64 = re.compile(r"\bextern\s+([^;{}()]*?)\b(\w+)\s*\(((?:[^;{}()]|\([^()]*\))*)\)\s*(?:__asm__\s*\(\s*\"_?(func_(?:L\d\d_)?[0-9A-Fa-f]{8})\"\s*\))?\s*;")
for fn_, t_ in gen_texts.items():
    for q in RET64.finditer(strip_comments_keep_len(t_)):
        n = q.group(4) or re.sub(r"^(func_(?:L\d\d_)?[0-9A-Fa-f]{8}).*$", r"\1", q.group(2))
        s = sigs.get(n)
        if s is None or s.get("src") != "decl" or s["ret"] == "l": continue
        if abi_class(q.group(1)) == "l" and s["ret"] in ("i", "v"):
            s["ret"] = "l"
# ... and the C that declares such a function as returning int, or keeps its
# value in an int (`int mode = GetEffectTex(1);` then passed on as the texture),
# gets 64 bits: on the EE the register keeps all of them whatever the C says.
_r64 = sorted(n for n, s in sigs.items() if s.get("src") == "decl" and s["ret"] == "l" and re.fullmatch(r"func_(?:L\d\d_)?[0-9A-Fa-f]{8}", n)
              and (s.get("addr") or 0) >= 0x0015F000)     # game code (the core's library calls are the host's)
if _r64:
    _names = "|".join(re.escape(n) for n in _r64)
    _INT = r"(?:int|s32|u32|unsigned int|unsigned|signed int|signed)"
    _P1 = re.compile(r"(\bextern\s+|^[ \t]*)%s(\s+(?:%s)\w*\s*\([^;{}]*\)\s*;)" % (_INT, _names), re.M)
    _P2 = re.compile(r"(\bextern\s+)%s(\s+\w+\s*\([^;{}]*\)\s*__asm__\s*\(\s*\"_?(?:%s)\"\s*\)\s*;)" % (_INT, _names))
    _P3 = re.compile(r"(^\s*)%s(\s+\w+\s*=\s*(?:%s)\w*\s*\()" % (_INT, _names), re.M)
    _nfix = 0
    for fn_ in list(gen_texts):
        p_ = os.path.join(OUT, fn_)
        if not os.path.exists(p_): continue
        t0 = open(p_).read()
        t1, k1 = _P1.subn(r"\1long long\2", t0)
        t1, k2 = _P2.subn(r"\1long long\2", t1)
        t1, k3 = _P3.subn(r"\1long long\2", t1)
        # `int glyphs; ... glyphs = GetEffectTex(kind);`: the nearest such declaration above
        _again = True
        while _again:
            _again = False
            for q in re.finditer(r"(?<![\w.>])(\w+)\s*=\s*(?:%s)\w*\s*\(" % _names, t1):
                d_ = list(re.finditer(r"^([ \t]*)%s([ \t]+%s[ \t]*;)" % (_INT, re.escape(q.group(1))), t1[:q.start()], re.M))
                if d_:
                    d_ = d_[-1]
                    t1 = t1[:d_.start()] + d_.group(1) + "long long" + d_.group(2) + t1[d_.end():]; k3 += 1
                    _again = True
                    break
        if t1 != t0:
            open(p_, "w").write(t1); gen_texts[fn_] = t1; _nfix += k1 + k2 + k3
    print("64-bit returns of assembly-only functions (%s): %d declarations and locals widened" % (", ".join(_r64), _nfix))
json.dump(sigs, open(os.path.join(OUT, "sigs.json"), "w"), indent=0)
json.dump(sorted(set(asm_entries)), open(os.path.join(OUT, "asm_entries.json"), "w"))
if LEVEL is None:
    # for the level passes: what this pass defines, and its definitions' parameters
    json.dump(sorted(defined), open(os.path.join(OUT, "defined.json"), "w"))
    json.dump({n: {"params": [list(x) for x in ps], "ret": rets.get(n, "int")} for n, ps in defs.items()},
              open(os.path.join(OUT, "defs.json"), "w"))
else:
    print("level %s: %d functions of other programs left out; %d names renamed" % (LV, len(dropped), len(RENAME)))
