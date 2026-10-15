#!/usr/bin/env python3
"""region.py: build another region's version of the game from the PAL sources.

  python tools/region.py us DECOMP_REPO OUT_DIR

The decomp, the host sources and every tool here work in the PAL executable's
addresses (SCES-50916), and they name functions and data by address
(func_0012F4A8, D_0013A548). region/us.json (made by tools/region_map.py from both
executables; addresses only) maps every PAL address to the US one (SCUS-97199).
This writes, under OUT_DIR:

  repo/   the decomp as the tools read it, in US addresses: its catalogues
          (progress/report.json, config/*, config/overlays/functions.tsv), headers and
          sources with every func_/D_ name and address literal rewritten
  host/   the host sources (host/src, host/include, host/overrides) the same way
  region_report.txt   every literal rewritten, and what could not be mapped

A PAL function the US build does not have is renamed func_palonly_X, so nothing is
ever placed at its PAL address; a US function the PAL build does not have is added to
the catalogue (and translated from the bytes like the rest). The build then runs the
usual tools on these trees (build.py --region us)."""
import bisect, json, os, re, shutil, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGIONS = {"us": {"map": os.path.join(HERE, "region", "us.json"), "exe": "SCUS_971.99", "pal_exe": "SCES_509.16",
                  "video": "ntsc"}}
RESIDENT_PAL = 0x0015F000
CORE_TEXT = (0x00112380, 0x0012F580)   # PAL core.text: the Sony libraries


class RegionMap:
    def __init__(self, path):
        m = json.load(open(path))
        self.m = m
        self.exe = {int(k, 16): v for k, v in m["exe_funcs"].items()}
        self.exe_starts = sorted(self.exe)
        self.data = sorted(m["data"])
        self.lv = {lv: {int(k, 16): v for k, v in d["funcs"].items()} for lv, d in m["levels"].items()}
        self.lv_starts = {lv: sorted(d) for lv, d in self.lv.items()}
        self.lv_data = {lv: sorted(r) for lv, r in m["level_data"].items()}
        self.sections = m["sections"]
        self.us_only = m["exe_us_only"]
        self.lv_us_only = {lv: d["us_only"] for lv, d in m["levels"].items()}

    @staticmethod
    def _func(table, starts, a, interior=True):
        if a in table: return table[a][0]
        if not interior: return None
        k = bisect.bisect_right(starts, a) - 1
        if k >= 0:
            s = starts[k]; ua, sz, method = table[s]
            if s < a < s + sz and method == "fingerprint": return ua + (a - s)   # same instructions: same offsets
        return None

    @staticmethod
    def _runs(runs, a):
        for r in runs:
            if r[0] <= a < r[1]: return a + r[2]
        return None

    def section_edge(self, a):
        """a PAL section's first address or its end: the US section's"""
        if a == 0x0023E740:                      # the end of PAL's game program as the tools give it (consts)
            t = self.sections[".text"]; return (t[2] + t[3] + 15) & ~15
        for n, (pa, ps, ua, us_) in self.sections.items():
            if not pa: continue
            if a == pa: return ua
            if a == pa + ps: return ua + us_
            if a == (pa + ps + 15) & ~15: return (ua + us_ + 15) & ~15     # (an end rounded up to 16)
        return None

    def exe_addr(self, a, interior=True):
        """the US address of a PAL executable address, or None"""
        if 0x00100000 <= a < 0x00112380: return a                # .reginfo, .vutext: the same in both
        f = self._func(self.exe, self.exe_starts, a, interior)
        if f is not None: return f
        return self._runs(self.data, a)

    def level_addr(self, lv, a, interior=True):
        if a < RESIDENT_PAL: return self.exe_addr(a, interior)  # the core: shared by every program
        f = self._func(self.lv.get(lv, {}), self.lv_starts.get(lv, []), a, interior)
        if f is not None: return f
        return self._runs(self.lv_data.get(lv, []), a)

    def exe_func(self, a): return self.exe[a][0] if a in self.exe else None
    def level_func(self, lv, a): return self.lv[lv][a][0] if lv in self.lv and a in self.lv[lv] else None


def consts(region):
    """what the tools need to know about the executable, for this region"""
    pal = {"exe": "SCES_509.16", "main_text": (0x001E9080, 0x0023E740), "resident": 0x0015F000,
           "core_text": CORE_TEXT, "extra_ranges": [(0x001252A0, 0x00125630), (0x0011DF10, 0x001207B8)]}
    if region in (None, "", "pal"): return pal
    rm = RegionMap(REGIONS[region]["map"])
    t = rm.sections[".text"]; lit = rm.sections[".lit"]; ct = rm.sections["core.text"]
    return {"exe": REGIONS[region]["exe"],
            "main_text": (t[2], (t[2] + t[3] + 15) & ~15),
            "resident": lit[2],
            "core_text": (ct[2], ct[2] + ct[3]),
            "extra_ranges": [(rm.exe_addr(lo), rm.exe_addr(hi) if rm.exe_addr(hi) else rm.exe_addr(hi - 4) + 4) for lo, hi in pal["extra_ranges"]]}


# ---- rewriting ---------------------------------------------------------------------------------------
NAME = re.compile(r"\b(func|D)_(?:L(\d\d)_)?([0-9A-Fa-f]{6,8})(?![0-9A-Fa-f])")
LIT = re.compile(r"\b0x(00[1-9][0-9A-Fa-f]{5}|0[1-9][0-9A-Fa-f]{5}|[1-9][0-9A-Fa-f]{5})(?=[uUlL]*\b)")
# a line marked region:keep holds values that only look like addresses (IOP addresses, handles, lui values)


class Rewriter:
    def __init__(self, rm):
        self.rm = rm
        self.log, self.unmapped = [], []
        self.taken = {}

    def name(self, kind, lv_name, hexpart, lv_ctx, where):
        a = int(hexpart, 16)
        up = hexpart.isupper() or not any(c.isalpha() for c in hexpart)
        fmt = "%08X" if len(hexpart) == 8 else ("%06X" if up else "%06x")
        if kind == "func":
            if lv_name is not None:   # a level catalogue name: its address in that level
                ua = self.rm.level_func(lv_name, a)
                if ua is None: self.unmapped.append((where, "func_L%s_%s" % (lv_name, hexpart))); return "func_palonly_L%s_%s" % (lv_name, hexpart)
                return "func_L%s_%s" % (lv_name, fmt % ua)
            ua = self.rm.exe_func(a)
            if ua is None and CORE_TEXT[0] <= a < CORE_TEXT[1]: ua = self.rm.exe_addr(a)   # a library entry inside a split unit
            if ua is None:
                # the executable's main-segment functions are also catalogue names of level copies
                self.unmapped.append((where, "func_" + hexpart)); return "func_palonly_" + hexpart
            return "func_" + (fmt % ua)
        ctx = lv_name or lv_ctx                      # D_LNN_X: level NN's own data
        pre = "D_L%s_" % lv_name if lv_name else "D_"
        ua = self.rm.level_addr(ctx, a) if ctx else self.rm.exe_addr(a)
        if ua is None: self.unmapped.append((where, pre + hexpart)); return pre.replace("D_", "D_palonly_") + hexpart
        # one to one: a second PAL address landing on a US address already taken (two neighbours either
        # side of a shift boundary) is not trusted; its uses then fail to build instead of reading other data
        key = (ctx, bool(lv_name), ua)
        if self.taken.setdefault(key, a) != a:
            self.unmapped.append((where, "%s%s (clashes with PAL %08X)" % (pre, hexpart, self.taken[key]))); return pre.replace("D_", "D_palonly_") + hexpart
        return pre + (fmt % ua)

    def text(self, s, where, lv_ctx=None, literals=True):
        def nm(q):
            r = self.name(q.group(1), q.group(2), q.group(3), lv_ctx, where)
            return r
        s = NAME.sub(nm, s)
        if literals:
            def lit(q):
                h = q.group(1); a = int(h, 16)
                if not (0x00100000 <= a < 0x02000000): return q.group(0)
                # only what is surely an address: a section's edge, a function's first instruction, data
                # (a constant inside a function's range, such as a size of 0x200000, stays as it is)
                ua = self.rm.section_edge(a)
                if ua is None:
                    core = CORE_TEXT[0] <= a < CORE_TEXT[1]   # the Sony libraries: entries inside split units, same code in both
                    ua = self.rm.level_addr(lv_ctx, a, interior=core) if lv_ctx else self.rm.exe_addr(a, interior=core)
                if ua is None: self.unmapped.append((where, "0x" + h)); return q.group(0)
                if ua != a: self.log.append((where, "0x%s -> 0x%08X" % (h, ua)))
                return "0x%0*X" % (len(h), ua)
            s = "".join(l if "region:keep" in l else LIT.sub(lit, l) for l in s.splitlines(True))
        return s


def level_of(rel):
    q = re.search(r"overlays[/\\]l(\d\d)_", rel)
    return q.group(1) if q else None


def write_repo(rm, rw, repo, out):
    """the decomp, as the tools read it, in US addresses"""
    for sub in ("include", "src", "nonmatching", "tools"):
        top = os.path.join(repo, sub)
        for root, _, files in os.walk(top):
            for f in files:
                p = os.path.join(root, f); rel = os.path.relpath(p, repo); d = os.path.join(out, rel)
                os.makedirs(os.path.dirname(d), exist_ok=True)
                if sub == "tools" or not f.endswith((".c", ".h", ".inc", ".s")):
                    shutil.copyfile(p, d); continue
                d_name = os.path.join(os.path.dirname(d), rw.text(f, rel, literals=False))
                open(d_name, "w").write(rw.text(open(p, errors="replace").read(), rel, level_of(rel)))
    os.makedirs(os.path.join(out, "config", "overlays"), exist_ok=True)
    os.makedirs(os.path.join(out, "progress"), exist_ok=True)
    # symbols
    for fn in ("config/symbol_addrs.txt", "config/port_symbols.txt"):
        p = os.path.join(repo, fn)
        if not os.path.exists(p): continue
        lines = []
        for l in open(p, errors="replace"):
            q = re.match(r"^(\s*)(\w+)(\s*=\s*)0x([0-9A-Fa-f]+)(.*)$", l.rstrip("\n"))
            if q:
                a = int(q.group(4), 16); ua = rm.exe_addr(a)
                if ua is None:
                    rw.unmapped.append((fn, q.group(2))); continue
                l = "%s%s%s0x%08X%s" % (q.group(1), rw.text(q.group(2), fn, literals=False), q.group(3), ua, q.group(5))
            else:
                l = rw.text(l.rstrip("\n"), fn)
            lines.append(l)
        open(os.path.join(out, fn), "w").write("\n".join(lines) + "\n")
    for fn in ("config/names.tsv", "config/symbol_names.txt", "config/file_cflags.txt", "config/linker_remnants.txt", "config/handwritten_asm.txt"):
        p = os.path.join(repo, fn)
        if os.path.exists(p): open(os.path.join(out, fn), "w").write(rw.text(open(p, errors="replace").read(), fn, literals=False))
    # the executable's functions (progress/report.json): US addresses and sizes
    rep = json.load(open(os.path.join(repo, "progress", "report.json")))
    seen = set()
    for u in rep.get("units", []):
        fl = []
        for f in u.get("functions", []):
            md = f.get("metadata") or {}
            if "virtual_address" not in md: fl.append(f); continue
            a = int(md["virtual_address"])
            if a in rm.exe:
                ua, sz, _ = rm.exe[a]
                f = dict(f); f["name"] = rw.text(f["name"], "report.json", literals=False); f["size"] = str(sz)
                md = dict(md); md["virtual_address"] = str(ua); f["metadata"] = md
                fl.append(f); seen.add(ua)
            # (PAL-only: dropped)
        u["functions"] = fl
    extra = [{"name": "func_%08X" % ua, "size": str(sz), "metadata": {"virtual_address": str(ua)}}
             for ua, sz in rm.us_only if ua not in seen and sz >= 8]
    rep.setdefault("units", []).append({"name": "us_only", "functions": extra})
    json.dump(rep, open(os.path.join(out, "progress", "report.json"), "w"))
    # the US-only functions: assembly entries in a source file of their own
    with open(os.path.join(out, "src", "game", "us_only.c"), "w") as f:
        f.write('/* region.py: functions the US executable has and PAL does not (translated from the bytes) */\n#include "common.h"\n\n')
        for e in extra: f.write('INCLUDE_ASM("asm/us_only", %s);\n' % e["name"])
    # the level catalogue: each function's places in every level, in US addresses
    rows = []
    for l in open(os.path.join(repo, "config", "overlays", "functions.tsv")):
        if l.startswith("#") or not l.strip(): rows.append(l.rstrip("\n")); continue
        f = l.rstrip("\n").split("\t")
        pl, size = [], None
        for x in f[5].split(","):
            if ":" not in x: continue
            lv, a = x.split(":"); a = int(a, 16)
            ua = rm.level_func(lv, a)
            if ua is None: continue
            pl.append("%s:%08X" % (lv, ua))
            if size is None: size = rm.lv[lv][a][1]
        if not pl: continue
        name = rw.text(f[0], "functions.tsv", literals=False)
        if name.startswith(("func_palonly", "D_palonly")):   # (named after a level copy the US build lacks: name it after one it has)
            lv0, a0 = pl[0].split(":"); name = "func_L%s_%s" % (lv0, a0)
        f[0], f[2], f[4], f[5] = name, str(size or f[2]), str(len({p.split(':')[0] for p in pl})), ",".join(pl)
        rows.append("\t".join(f))
    open(os.path.join(out, "config", "overlays", "functions.tsv"), "w").write("\n".join(rows) + "\n")
    for fn in ("config/overlays/names.tsv",):
        p = os.path.join(repo, fn)
        if os.path.exists(p): open(os.path.join(out, fn), "w").write(rw.text(open(p, errors="replace").read(), fn, literals=False))
    os.makedirs(os.path.join(out, "baserom"), exist_ok=True)


# host sources that use the game's addresses (the rest: the platform, the GS, VU and movie code)
HOST_GAME = ("src/rac1/", "src/sdk/", "overrides/", "src/main.c", "src/runtime.c", "include/")


def write_host(rm, rw, src_host, out):
    for root, dirs, files in os.walk(src_host):
        dirs[:] = [d for d in dirs if d not in ("uya_only", "drafts")]
        for f in files:
            p = os.path.join(root, f); rel = os.path.relpath(p, src_host).replace(os.sep, "/")
            if rel.startswith("region/"):
                # host/region/<region>/: code only that region has, already in its addresses
                if rel.startswith("region/%s/" % REGION):
                    d = os.path.join(out, "src", "region", rel[len("region/%s/" % REGION):])
                    os.makedirs(os.path.dirname(d), exist_ok=True); shutil.copyfile(p, d)
                continue
            d = os.path.join(out, os.path.dirname(rel), rw.text(f, "host/" + rel, literals=False))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            if not f.endswith((".c", ".h")): shutil.copyfile(p, d); continue
            s = open(p, errors="replace").read()
            s = s.replace("SCES_509.16", REGIONS[REGION]["exe"])   # the executable's file name on the disc
            open(d, "w").write(rw.text(s, "host/" + rel, literals=rel.startswith(HOST_GAME)) if rel.startswith(HOST_GAME) else s)


REGION = None


def main():
    global REGION
    region, repo, out = sys.argv[1], sys.argv[2], sys.argv[3]
    REGION = region
    cfg = REGIONS[region]
    rm = RegionMap(cfg["map"])
    rw = Rewriter(rm)
    shutil.rmtree(os.path.join(out, "repo"), ignore_errors=True); shutil.rmtree(os.path.join(out, "host"), ignore_errors=True)
    write_repo(rm, rw, repo, os.path.join(out, "repo"))
    write_host(rm, rw, os.path.join(HERE, "host"), os.path.join(out, "host"))
    with open(os.path.join(out, "region_report.txt"), "w") as f:
        f.write("region %s: %d literals rewritten, %d names and literals not mapped\n\n== not mapped\n" % (region, len(rw.log), len(rw.unmapped)))
        for w, x in sorted(set(rw.unmapped)): f.write("%s\t%s\n" % (w, x))
        f.write("\n== literals rewritten\n")
        for w, x in rw.log: f.write("%s\t%s\n" % (w, x))
    hosts = [(w, x) for w, x in set(rw.unmapped) if w.startswith("host/")]
    print("region %s: %d literals rewritten; not mapped: %d (host sources: %d) -> %s" % (
        region, len(rw.log), len(set(rw.unmapped)), len(hosts), os.path.join(out, "region_report.txt")))


if __name__ == "__main__":
    main()
