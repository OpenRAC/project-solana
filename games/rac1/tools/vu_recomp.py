#!/usr/bin/env python3
"""vu_recomp.py GEN_DIR DECOMP_REPO [--dis OUT_DIR]

Ahead-of-time translator for the game's VU microprograms (VU0 and VU1 micro
mode). The microcode lives in the executable's .vutext section, listed by its
.DVP.ovlytab overlay table: one entry per MPG upload (VU address, size, load
address). Uploads of one program image share a group id in the overlay's
name (".DVP.overlay..<vu address>.<group>.<line>.<n>").

Each image is translated to one C function over an RcVu0 (host/include/vu.h):
a switch on the start address, then one labelled piece of C per instruction
pair. The function is compiled with the rest of the host program; nothing is
decoded at run time. Images that fit VU0's 4 KB micro memory are translated
for both VUs, the rest for VU1 only. The bytes of every upload are kept too,
so the runtime (host/src/gfx/vu.c) can tell which image is in micro memory
after the game's MPG uploads.

Semantics kept:
  - upper and lower ops of a pair both read the registers as they were before
    the pair; the upper op's result wins when both write the same register;
    with the I bit the lower word is LOI's immediate (the pair's own upper
    op still reads the old I: the game's sine code depends on it).
  - branches and E (end) have one delay slot pair.
  - FMAC results that overflow are +-MAX, as on the VU (which has no inf
    or NaN): x86 would give inf, and then NaN (inf * 0, inf - inf), where
    the VU keeps finite numbers (vu_fc).
  - MAC/status/clip flags reach FSAND/FMAND/FCAND... 4 cycles after the op
    issues, Q 7 cycles after DIV/SQRT and 13 after RSQRT: the code counts
    cycles, including the stalls on VF registers that are still being written
    (4-cycle latency, per component), which are worked out here.

Writes GEN_DIR/vu_prog_<n>.c (one per image and VU) and GEN_DIR/vu_table.c.
"""
import os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vu_isa import decode_pair, BRANCHES, XYZW

GEN, REPO = sys.argv[1], sys.argv[2]
DIS = sys.argv[sys.argv.index("--dis") + 1] if "--dis" in sys.argv else None
EXE = os.environ.get("UYA_EXE") or os.path.join(REPO, "baserom", "SCES_509.16")   # build.py: the copy read from the disc image
img = open(EXE, "rb").read()

# ---- the executable's sections ------------------------------------------------
e_shoff, = struct.unpack_from("<I", img, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", img, 0x2E)
secs = [struct.unpack_from("<10I", img, e_shoff + i * shentsize) for i in range(shnum)]
strtab_off = secs[shstrndx][4]


def secname(s):
    o = strtab_off + s[0]
    return img[o:img.index(b"\0", o)].decode()


bysec = {secname(s): s for s in secs}
vutext = bysec[".vutext"]
VT_ADDR, VT_OFF, VT_SIZE = vutext[3], vutext[4], vutext[5]
tab = bysec[".DVP.ovlytab"]
ostr = bysec[".DVP.ovlystrtab"]
tabd = img[tab[4]:tab[4] + tab[5]]
strd = img[ostr[4]:ostr[4] + ostr[5]]

groups = {}
order = []
for k in range(0, len(tabd) // 12 * 12, 12):
    no, lma, vma = struct.unpack_from("<3I", tabd, k)
    name = strd[no:strd.index(b"\0", no)].decode()
    size = bysec[name][5]
    parts = name.split(".")          # ['', 'DVP', 'overlay', '', '0x0', group, line, n]
    g = parts[5]
    o = VT_OFF + (lma - VT_ADDR)
    data = img[o:o + size]
    if g not in groups:
        groups[g] = []
        order.append(g)
    groups[g].append((vma, data))


# ---- register use (for stalls) ------------------------------------------------
def up_rw(u):
    """VF (reg, comp) read and written by an upper op"""
    r, w = [], []
    k = u.kind
    if k in ("nop", "bad"):
        return r, w
    if k == "arith":
        for c in u.comps:
            r.append((u.fs, c))
            if u.bc in ("x", "y", "z", "w"):
                r.append((u.ft, XYZW.index(u.bc)))
            elif u.bc is None:
                r.append((u.ft, c))
            if not u.acc:
                w.append((u.fd, c))
    elif k in ("opmula", "opmsub"):
        for c in (0, 1, 2):
            r += [(u.fs, c), (u.ft, c)]
        if k == "opmsub":
            w += [(u.fd, c) for c in u.comps if c < 3]
    elif k in ("itof", "ftoi", "abs"):
        r += [(u.fs, c) for c in u.comps]
        w += [(u.ft, c) for c in u.comps]
    elif k == "clip":
        r += [(u.fs, 0), (u.fs, 1), (u.fs, 2), (u.ft, 3)]
    return r, w


def lo_rw(l):
    r, w = [], []
    if l is None:
        return r, w
    n = l.name
    if n in ("lq", "lqi", "lqd"):
        w += [(l.ft, c) for c in l.comps]
    elif n in ("sq", "sqi", "sqd"):
        r += [(l.fs, c) for c in l.comps]
    elif n == "move":
        r += [(l.fs, c) for c in l.comps]
        w += [(l.ft, c) for c in l.comps]
    elif n == "mr32":
        r += [(l.fs, (c + 1) & 3) for c in l.comps]
        w += [(l.ft, c) for c in l.comps]
    elif n == "mtir":
        r.append((l.fs, l.fsf))
    elif n == "mfir":
        w += [(l.ft, c) for c in l.comps]
    elif n == "div":
        r += [(l.fs, l.fsf), (l.ft, l.ftf)]
    elif n == "sqrt":
        r.append((l.ft, l.ftf))
    elif n == "rsqrt":
        r += [(l.fs, l.fsf), (l.ft, l.ftf)]
    return [x for x in r if x[0]], [x for x in w if x[0]]


FLAG_UPPER = {"add", "sub", "mul", "madd", "msub", "adda", "suba", "mula", "madda", "msuba"}


class Image:
    def __init__(self, gid, blocks, vu):
        self.gid, self.blocks, self.vu = gid, sorted(blocks), vu
        self.mem = {}
        for vma, data in self.blocks:
            for k in range(0, len(data), 8):
                self.mem[vma + k] = struct.unpack_from("<II", data, k)
        self.warn = []


def dis_image(im, out):
    with open(out, "w") as f:
        for pc in sorted(im.mem):
            lo, hi = im.mem[pc]
            u, l, fl, imm = decode_pair(lo, hi)
            fs = "".join(x for x in "IEMDT" if fl[x])
            ls = "loi %08X" % imm if imm is not None else l.text()
            f.write("%04X %-3s %-45s | %s\n" % (pc, fs, u.text(), ls))


# ---- translation ----------------------------------------------------------------
def gen_image(im):
    vu = im.vu
    MEM = "VU%d_MEM" % vu
    PCMASK = 0x3FF8 if vu == 1 else 0xFF8
    pcs = sorted(im.mem)
    dec = {}
    for pc in pcs:
        lo, hi = im.mem[pc]
        dec[pc] = decode_pair(lo, hi)

    reads_sm = any(d[1] is not None and d[1].name in ("fsand", "fsor", "fseq", "fmand", "fmor", "fmeq") for d in dec.values())
    # ---- static cycles: stalls on VF registers still being written ----
    stall, qstall = {}, {}
    ready, t, qready = {}, 0, None
    prev_end = True
    for pc in pcs:
        u, l, fl, imm = dec[pc]
        if prev_end or (pc - 8) not in dec:
            ready, t, qready = {}, 0, None
        ti = t + 1
        r1, w1 = up_rw(u)
        r2, w2 = lo_rw(l)
        need = max([ready.get(x, 0) for x in r1 + r2] + [0])
        s = max(0, need - ti)
        ti += s
        qs = 0
        if l is not None and l.name in ("waitq", "div", "sqrt", "rsqrt") and qready is not None and qready > ti:
            qs = qready - ti
            ti += qs
        if l is not None and l.name in ("div", "sqrt", "rsqrt"):
            qready = ti + (13 if l.name == "rsqrt" else 7)
        stall[pc] = s + qs
        for x in w1 + w2:
            ready[x] = ti + 4
        t = ti
        # a new straight-line run starts after an unconditional jump's or E's delay slot
        prev_end = False
        p = pc - 8
        if p in dec:
            pu, pl, pf, pimm = dec[p]
            if pf["E"] or (pl is not None and pl.name in ("b", "jr")):
                prev_end = True

    used_q = False

    # ---- branch VI delay: a conditional branch whose VI register was written
    # by the instruction right before it reads the value the register had
    # before that write (before the earliest of a run of such writes, up to 4
    # back), unless the branch stalls or the write reads flags (FCAND and
    # friends). This is the hardware's integer pipeline; PCSX2's microVU models
    # it the same way (analyzeBranchVI, called for conditional branches only).
    # JR/JALR read the new value (microVU's mVUanalyzeJump has no such delay):
    # the moby clipper (13859 at 0x22F8) sets vi1 and jumps through it in the
    # next instruction, and only the new value reaches its edge-going-out code. ----
    FLAGREAD = {"fcand", "fcor", "fceq", "fcget", "fsand", "fsor", "fseq", "fmand", "fmor", "fmeq"}
    def vi_written(lw):
        if lw is None: return 0
        n = lw.name
        if n in ("iadd", "isub", "iand", "ior"): return lw.id_ & 15
        if n in ("iaddi", "iaddiu", "isubiu", "ilw", "ilwr", "mtir", "xtop", "xitop", "bal", "jalr"): return lw.it & 15
        if n in ("lqi", "lqd"): return lw.is_ & 15
        if n in ("sqi", "sqd"): return lw.it & 15
        return 0
    targets = set()
    for pc_ in pcs:
        l_ = dec[pc_][1]
        if l_ is not None and l_.name in ("b", "bal", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"):
            targets.add((pc_ + 8 + l_.imm * 8) & PCMASK)
    vib_save, vib_use = {}, {}
    for pc_ in pcs:
        l_ = dec[pc_][1]
        if l_ is None or l_.name not in ("ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"): continue
        if stall[pc_] or pc_ in targets: continue
        regs = [l_.is_ & 15] + ([l_.it & 15] if l_.name in ("ibeq", "ibne") else [])
        for x in regs:
            if not x: continue
            p_ = pc_ - 8
            if p_ not in dec or dec[p_][1] is None or dec[p_][1].name in FLAGREAD or vi_written(dec[p_][1]) != x: continue
            j_ = p_
            for k_ in range(3):
                q_ = j_ - 8
                if j_ in targets or q_ not in dec or stall[j_]: break
                lq_ = dec[q_][1]
                if lq_ is None or lq_.name in FLAGREAD or vi_written(lq_) != x: break
                j_ = q_
            vib_save[j_] = x
            vib_use[pc_] = x
            break

    def VI(r):
        r &= 15
        return "0u" if r == 0 else "v->vi[%d]" % r

    def setvi(r, e):
        r &= 15
        return "" if r == 0 else "v->vi[%d] = (%s) & 0xFFFF;" % (r, e)

    def U32(r):
        return "((u32 *)v->vf[%d])" % r

    def upper(pc, u):
        """(compute, commit) code for the upper op"""
        nonlocal used_q
        k = u.kind
        if k == "nop":
            return "", ""
        if k == "bad":
            im.warn.append("%04X upper %s" % (pc, u.name))
            return 'vu_bad(%d, 0x%04X, "%s");' % (vu, pc, u.name), ""
        comp, commit = [], []
        F = lambda r, c: "v->vf[%d][%d]" % (r, c)
        if k == "arith":
            base = u.base if u.acc else u.name
            if u.bc == "q":
                used_q = True
                comp.append("f32 qv = vu_q(v, cyc);")

            def B(c):
                if u.bc in ("x", "y", "z", "w"): return F(u.ft, XYZW.index(u.bc))
                if u.bc == "q": return "qv"
                if u.bc == "i": return "v->i"
                return F(u.ft, c)
            for c in u.comps:
                a, b = F(u.fs, c), B(c)
                e = {"add": "%s + %s" % (a, b), "sub": "%s - %s" % (a, b), "mul": "%s * %s" % (a, b),
                     "madd": "v->acc[%d] + vu_fc(%s * %s)" % (c, a, b), "msub": "v->acc[%d] - vu_fc(%s * %s)" % (c, a, b),
                     "max": "fmaxf(%s, %s)" % (a, b), "mini": "fminf(%s, %s)" % (a, b)}[base]
                comp.append("f32 u%d = %s;" % (c, e))
            for c in u.comps:
                if u.acc: commit.append("v->acc[%d] = vu_fc(u%d);" % (c, c))
                elif u.fd: commit.append("v->vf[%d][%d] = vu_fc(u%d);" % (u.fd, c, c))
            if reads_sm and base in ("add", "sub", "mul", "madd", "msub"):
                commit.append("vu_fl_push(v, cyc, %s);" % (" | ".join("vu_macbits(u%d, %d)" % (c, c) for c in u.comps) or "0"))
        elif k in ("opmula", "opmsub"):
            ex = {0: (1, 2), 1: (2, 0), 2: (0, 1)}
            cs = [c for c in u.comps if c < 3]
            for c in cs:
                a, b = ex[c]
                p = "%s * %s" % (F(u.fs, a), F(u.ft, b))
                comp.append("f32 u%d = %s;" % (c, p if k == "opmula" else "v->acc[%d] - vu_fc(%s)" % (c, p)))
            for c in cs:
                if k == "opmula": commit.append("v->acc[%d] = vu_fc(u%d);" % (c, c))
                elif u.fd: commit.append("v->vf[%d][%d] = vu_fc(u%d);" % (u.fd, c, c))
            if reads_sm:
                commit.append("vu_fl_push(v, cyc, %s);" % (" | ".join("vu_macbits(u%d, %d)" % (c, c) for c in cs) or "0"))
        elif k == "itof":
            for c in u.comps:
                comp.append("u32 n%d = vu_itof(%s[%d], %.9ef);" % (c, U32(u.fs), c, 1.0 / (1 << u.shift)))
            if u.ft:
                commit += ["%s[%d] = n%d;" % (U32(u.ft), c, c) for c in u.comps]
        elif k == "ftoi":
            for c in u.comps:
                comp.append("s32 n%d = vu_ftoi(%s, %.1ff);" % (c, F(u.fs, c), float(1 << u.shift)))
            if u.ft:
                commit += ["%s[%d] = (u32)n%d;" % (U32(u.ft), c, c) for c in u.comps]
        elif k == "abs":
            for c in u.comps:
                comp.append("u32 n%d = %s[%d] & 0x7FFFFFFFu;" % (c, U32(u.fs), c))
            if u.ft:
                commit += ["%s[%d] = n%d;" % (U32(u.ft), c, c) for c in u.comps]
        elif k == "clip":
            comp.append("f32 cw = v->vf[%d][3];" % u.ft)
            comp.append("f32 cs[3] = { v->vf[%d][0], v->vf[%d][1], v->vf[%d][2] };" % (u.fs, u.fs, u.fs))
            commit.append("vu_clipw(v, cyc, cs, cw);")
        return " ".join(comp), " ".join(commit)

    def lower(pc, l):
        """(pre, code, kind, target) ; kind in None, 'b' (static target), 'j' (npc)"""
        nonlocal used_q
        n = l.name
        c0 = l.comps[0] if l.comps else 0
        if n.startswith("?"):
            im.warn.append("%04X lower %s" % (pc, n))
            return "", 'vu_bad(%d, 0x%04X, "%s");' % (vu, pc, n), None, None
        if n == "move":
            if not l.comps or not l.ft: return "", "", None, None
            return "", " ".join("%s[%d] = %s[%d];" % (U32(l.ft), c, U32(l.fs), c) for c in l.comps), None, None
        if n == "mr32":
            if not l.ft: return "", "", None, None
            s = "{ u32 m0 = %s[1], m1 = %s[2], m2 = %s[3], m3 = %s[0]; " % (U32(l.fs), U32(l.fs), U32(l.fs), U32(l.fs))
            return "", s + " ".join("%s[%d] = m%d;" % (U32(l.ft), c, c) for c in l.comps) + " }", None, None
        if n in ("lq", "lqi", "lqd"):
            reg = l.is_
            pre = setvi(reg, VI(reg) + " - 1") + " " if n == "lqd" else ""
            post = " " + setvi(reg, VI(reg) + " + 1") if n == "lqi" else ""
            addr = VI(reg) + (" + %d" % l.imm if n == "lq" else "")
            body = " ".join("%s[%d] = m[%d];" % (U32(l.ft), c, c) for c in l.comps) if l.ft else ""
            return "", "%s{ const u32 *m = %s(%s); %s }%s" % (pre, MEM, addr, body, post), None, None
        if n in ("sq", "sqi", "sqd"):
            reg = l.it
            pre = setvi(reg, VI(reg) + " - 1") + " " if n == "sqd" else ""
            post = " " + setvi(reg, VI(reg) + " + 1") if n == "sqi" else ""
            addr = VI(reg) + (" + %d" % l.imm if n == "sq" else "")
            body = " ".join("m[%d] = %s[%d];" % (c, U32(l.fs), c) for c in l.comps)
            return "", "%s{ u32 *m = %s(%s); %s }%s" % (pre, MEM, addr, body, post), None, None
        if n in ("ilw", "ilwr"):
            addr = VI(l.is_) + (" + %d" % l.imm if n == "ilw" else "")
            return "", setvi(l.it, "%s(%s)[%d]" % (MEM, addr, c0)), None, None
        if n in ("isw", "iswr"):
            addr = VI(l.is_) + (" + %d" % l.imm if n == "isw" else "")
            return "", "{ u32 *m = %s(%s); %s }" % (MEM, addr, " ".join("m[%d] = %s;" % (c, VI(l.it)) for c in l.comps)), None, None
        if n in ("iadd", "isub", "iand", "ior"):
            op = {"iadd": "+", "isub": "-", "iand": "&", "ior": "|"}[n]
            return "", setvi(l.id_, "%s %s %s" % (VI(l.is_), op, VI(l.it))), None, None
        if n == "iaddi":
            return "", setvi(l.it, "%s + (%d)" % (VI(l.is_), l.imm)), None, None
        if n == "iaddiu":
            return "", setvi(l.it, "%s + %d" % (VI(l.is_), l.imm)), None, None
        if n == "isubiu":
            return "", setvi(l.it, "%s - %d" % (VI(l.is_), l.imm)), None, None
        if n == "mtir":
            return "", setvi(l.it, "%s[%d]" % (U32(l.fs), l.fsf)), None, None
        if n == "mfir":
            if not l.ft: return "", "", None, None
            return "", " ".join("%s[%d] = (u32)(s32)(s16)%s;" % (U32(l.ft), c, VI(l.is_)) for c in l.comps), None, None
        F = lambda r, c: "v->vf[%d][%d]" % (r, c)
        if n == "div":
            used_q = True
            return "cyc = vu_qwait(v, cyc);", "vu_qset(v, cyc, 7, vu_div(%s, %s));" % (F(l.fs, l.fsf), F(l.ft, l.ftf)), None, None
        if n == "sqrt":
            used_q = True
            return "cyc = vu_qwait(v, cyc);", "vu_qset(v, cyc, 7, vu_sqrt(%s));" % F(l.ft, l.ftf), None, None
        if n == "rsqrt":
            used_q = True
            return "cyc = vu_qwait(v, cyc);", "vu_qset(v, cyc, 13, vu_rsqrt(%s, %s));" % (F(l.fs, l.fsf), F(l.ft, l.ftf)), None, None
        if n == "waitq":
            return "cyc = vu_qwait(v, cyc);", "", None, None
        if n == "fcand": return "", setvi(1, "(vu_clip(v, cyc) & 0x%X) != 0" % l.imm), None, None
        if n == "fcor": return "", setvi(1, "((vu_clip(v, cyc) | 0x%X) & 0xFFFFFF) == 0xFFFFFF" % l.imm), None, None
        if n == "fceq": return "", setvi(1, "(vu_clip(v, cyc) & 0xFFFFFF) == 0x%X" % l.imm), None, None
        if n == "fcset": return "", "vu_fcset(v, cyc, 0x%X);" % l.imm, None, None
        if n == "fcget": return "", setvi(l.it, "vu_clip(v, cyc) & 0xFFF"), None, None
        if n == "fsand": return "", setvi(l.it, "vu_status(v, cyc) & 0x%X" % l.imm), None, None
        if n == "fsor": return "", setvi(l.it, "vu_status(v, cyc) | 0x%X" % l.imm), None, None
        if n == "fseq": return "", setvi(l.it, "(vu_status(v, cyc) & 0xFFF) == 0x%X" % l.imm), None, None
        if n == "fsset": return "", "vu_fsset(v, cyc, 0x%X);" % l.imm, None, None
        if n == "fmand": return "", setvi(l.it, "%s & vu_mac(v, cyc)" % VI(l.is_)), None, None
        if n == "fmor": return "", setvi(l.it, "%s | vu_mac(v, cyc)" % VI(l.is_)), None, None
        if n == "fmeq": return "", setvi(l.it, "(%s & 0xFFFF) == vu_mac(v, cyc)" % VI(l.is_)), None, None
        if n == "xgkick":
            if vu != 1:
                im.warn.append("%04X xgkick on VU0" % pc)
                return "", 'vu_bad(0, 0x%04X, "xgkick");' % pc, None, None
            return "", "vu_xgkick(%s);" % VI(l.is_), None, None
        if n == "xtop": return "", setvi(l.it, "vu_vif_top(%d)" % vu), None, None
        if n == "xitop": return "", setvi(l.it, "vu_vif_itop(%d)" % vu), None, None
        if n in BRANCHES:
            link = (pc + 16) // 8
            if n == "b":
                return "", "", "b", (pc + 8 + l.imm * 8) & PCMASK
            if n == "bal":
                return "", setvi(l.it, "%d" % link), "b", (pc + 8 + l.imm * 8) & PCMASK
            JV = "vib" if vib_use.get(pc) == (l.is_ & 15) else VI(l.is_)
            if n == "jr":
                return "", "npc = (%s << 3) & 0x%X;" % (JV, PCMASK), "j", None
            if n == "jalr":
                return "", "npc = (%s << 3) & 0x%X; %s" % (JV, PCMASK, setvi(l.it, "%d" % link)), "j", None
            BV = (lambda r: "vib" if vib_use.get(pc) == (r & 15) else VI(r))
            cond = {"ibeq": "%s == %s" % (BV(l.it), BV(l.is_)), "ibne": "%s != %s" % (BV(l.it), BV(l.is_)),
                    "ibltz": "(s16)%s < 0" % BV(l.is_), "ibgtz": "(s16)%s > 0" % BV(l.is_),
                    "iblez": "(s16)%s <= 0" % BV(l.is_), "ibgez": "(s16)%s >= 0" % BV(l.is_)}[n]
            return "", "bt = %s;" % cond, "c", (pc + 8 + l.imm * 8) & PCMASK
        im.warn.append("%04X lower %s not translated" % (pc, n))
        return "", 'vu_bad(%d, 0x%04X, "%s");' % (vu, pc, n), None, None

    def pair(pc):
        """C for one pair; returns (code, flow kind, target)"""
        u, l, fl, imm = dec[pc]
        out = ["VU_TRACE(0x%04X); cyc += %d;" % (pc, 1 + stall[pc])]
        if pc in vib_save: out.append("vib = %s;" % VI(vib_save[pc]))
        uc, ucommit = upper(pc, u)
        pre = code = ""
        kind = tgt = None
        if l is not None:
            pre, code, kind, tgt = lower(pc, l)
        if pre: out.append(pre)
        if imm is not None:   # LOI: the pair's upper op still reads the old I
            code = "v->i = vu_f(0x%08Xu);" % imm
        if uc or ucommit:
            out.append("{ %s %s %s }" % (uc, code, ucommit))
        elif code:
            out.append(code)
        return " ".join(x for x in out if x), kind, tgt, fl["E"]

    body = []
    lab = lambda pc: "L_%04X" % pc
    import re as _re
    def idle(pc):   # a pair that does nothing but count its cycle
        c_, k_, t_, e_ = pair(pc)
        return k_ is None and not e_ and not _re.sub(r"VU_TRACE\(0x[0-9A-F]+\);|cyc \+= \d+;|\s", "", c_)
    for pc in pcs:
        code, kind, tgt, ebit = pair(pc)
        body.append("%s: %s" % (lab(pc), code))
        nxt = pc + 8
        if kind is None and not ebit:
            if nxt not in dec:
                body.append("    vu_bad(%d, 0x%04X, \"ran off the end\"); goto done;" % (vu, nxt))
            continue
        # a branch or the end: its delay slot pair runs here as well
        if nxt in dec:
            dcode, dkind, dtgt, debit = pair(nxt)
            if dkind is not None:
                im.warn.append("%04X branch in a delay slot (ignored)" % nxt)
            if debit and not ebit:
                im.warn.append("%04X E bit in a branch delay slot" % nxt)
            body.append("    /* delay slot */ " + dcode)
        else:
            im.warn.append("%04X delay slot outside the image" % pc)
        if ebit:
            if kind is not None:
                im.warn.append("%04X branch with the E bit" % pc)
            body.append("    v->tpc = 0x%04X; goto done;" % ((pc + 16) & PCMASK))
            continue
        if kind == "b" and tgt == pc - 16 and tgt in dec and pair(tgt)[1] == "c" and idle(tgt + 8) and (nxt not in dec or idle(nxt)) \
                and "vi[" in pair(tgt)[0] and not _re.search(r"vf\[|acc\[|VU\d_MEM", pair(tgt)[0]):
            # a wait loop on a VI register the EE writes (ctc2): park here and
            # let the EE resume the program (rc_vu0_resume) when it writes one
            body.append("    v->tpc = 0x%04X; v->susp = 1; goto done;   /* wait for the EE */" % tgt)
        elif kind == "b":
            body.append("    goto %s;" % lab(tgt) if tgt in dec else "    pc = 0x%04X; goto dispatch;" % tgt)
        elif kind == "c":
            body.append("    if (bt) %s" % ("goto %s;" % lab(tgt) if tgt in dec else "{ pc = 0x%04X; goto dispatch; }" % tgt))
            if nxt + 8 in dec:
                body.append("    goto %s;" % lab(nxt + 8))
            else:
                body.append("    goto done;")
        elif kind == "j":
            body.append("    pc = npc; goto dispatch;")

    name = "vup_%s_vu%d" % (im.gid, vu)
    out = ["/* VU%d microprogram image %s, translated from the executable by tools/vu_recomp.py */" % (vu, im.gid),
           '#include "vu.h"', "",
           "void %s(RcVu0 *v, u32 pc)" % name, "{",
           "    u32 cyc = 0, npc = 0, vib = 0; int bt = 0; (void)npc; (void)bt; (void)vib;",
           "    v->st_int = v->status; v->cl_int = v->clip;",
           "dispatch:",
           "    switch (pc & 0x%X) {" % PCMASK]
    for pc in pcs:
        out.append("    case 0x%04X: goto %s;" % (pc, lab(pc)))
    out.append('    default: vu_bad(%d, pc, "start address"); goto done;' % vu)
    out.append("    }")
    out += body
    out += ["done:", "    vu_settle(v);", "}"]
    return name, "\n".join(out) + "\n"


def extra_images():
    """Microcode the game uploads that is not in .vutext (none is known for
    RAC1): the runtime reports such an upload and, with UYA_VUDUMP=dir, saves
    it as vu<N>_<addr>_<hash>.bin. Files of that name in RAC1_VU_EXTRA (or
    the vu_extra folder next to build.py) are translated as images of their
    own, one upload each."""
    d = os.environ.get("RAC1_VU_EXTRA") or os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "vu_extra")
    out = []
    if not os.path.isdir(d):
        return out
    for fn in sorted(os.listdir(d)):
        m = re.fullmatch(r"vu([01])_([0-9A-Fa-f]{4})_([0-9A-Fa-f]{8})\.bin", fn)
        if not m:
            continue
        data = open(os.path.join(d, fn), "rb").read()
        data = data[:len(data) // 8 * 8]
        out.append(Image("x%s_%s" % (m.group(2).upper(), m.group(3).upper()), [(int(m.group(2), 16), data)], int(m.group(1))))
    return out


def main():
    os.makedirs(GEN, exist_ok=True)
    images = []
    for g in order:
        blocks = groups[g]
        size = max(vma + len(d) for vma, d in blocks)
        kicks = any(decode_pair(*struct.unpack_from("<II", d, k))[1] is not None and
                    decode_pair(*struct.unpack_from("<II", d, k))[1].name == "xgkick"
                    for vma, d in blocks for k in range(0, len(d), 8))
        # XGKICK only exists on VU1; anything bigger than 4 KB is VU1 code too
        for vu in ((0, 1) if size <= 0x1000 and not kicks else (1,)):
            images.append(Image(g, blocks, vu))
    images += extra_images()
    if DIS:
        os.makedirs(DIS, exist_ok=True)
        for im in images:
            if im.vu == 1 or len([i for i in images if i.gid == im.gid]) == 1:
                dis_image(im, os.path.join(DIS, "%s.txt" % im.gid))
    tbl = ['/* VU microprogram images (tools/vu_recomp.py) */', '#include "vu.h"', "",
           "typedef struct { u32 vma, size; const u32 *words; } VuBlock;",
           "typedef struct { const char *name; int vu; void (*fn)(RcVu0 *, u32); int nblk; const VuBlock *blk; } VuImage;", ""]
    names = []
    nwarn = 0
    for im in images:
        name, src = gen_image(im)
        path = os.path.join(GEN, "%s.c" % name)
        old = open(path).read() if os.path.exists(path) else None
        if old != src:
            open(path, "w").write(src)
        tbl.append("void %s(RcVu0 *, u32);" % name)
        for vma, d in im.blocks:
            ws = struct.unpack("<%dI" % (len(d) // 4), d)
            tbl.append("static const u32 %s_b%04X[%d] = {" % (name, vma, len(ws)))
            for k in range(0, len(ws), 8):
                tbl.append("    " + ", ".join("0x%08X" % x for x in ws[k:k + 8]) + ",")
            tbl.append("};")
        tbl.append("static const VuBlock %s_blk[] = { %s };" % (name, ", ".join(
            "{ 0x%04X, 0x%X, %s_b%04X }" % (vma, len(d), name, vma) for vma, d in im.blocks)))
        names.append((name, im))
        for w in im.warn:
            nwarn += 1
            print("vu_recomp: image %s VU%d: %s" % (im.gid, im.vu, w))
    tbl.append("const VuImage vu_images[] = {")
    for name, im in names:
        tbl.append('    { "%s", %d, %s, %d, %s_blk },' % (im.gid, im.vu, name, len(im.blocks), name))
    tbl.append("};")
    tbl.append("const int vu_image_count = %d;" % len(names))
    src = "\n".join(tbl) + "\n"
    path = os.path.join(GEN, "vu_table.c")
    if not os.path.exists(path) or open(path).read() != src:
        open(path, "w").write(src)
    print("vu_recomp: %d microprogram images (%d pairs) translated, %d warnings" % (
        len(names), sum(len(im.mem) for _, im in names), nwarn))


main()
