#!/usr/bin/env python3
"""vu_refcheck.py DECOMP_REPO IMAGE VU [vucap_before.bin vucap_after.bin]

Testing aid for tools/vu_recomp.py: a plain interpreter of VU micro code that
replays a state the running wrapper captured (UYA_VUCAPTURE=image,n, see
host/src/gfx/vu.c) and compares its result with what the translated C
produced: registers, flags, Q and data memory. It works out stalls at run
time (the translator does it statically), so it also checks that model. The
MAC/status flags are only compared for images that read them (the
translation skips them otherwise). Needs numpy.
"""
import os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vu_isa import decode_pair, XYZW

f32 = np.float32
np.seterr(all="ignore")

def load_images():
    d = open(os.path.join(sys.argv[1], "baserom", "SCES_509.16"), "rb").read()
    e_shoff, = struct.unpack_from("<I", d, 0x20); shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<10I", d, e_shoff + i * shentsize) for i in range(shnum)]
    so = secs[shstrndx][4]
    sizes = {}
    for s in secs:
        n = d[so + s[0]:d.index(b"\0", so + s[0])].decode()
        if n.startswith(".DVP.overlay"): sizes[n] = s[5]
        if n == ".DVP.ovlytab": tab = d[s[4]:s[4] + s[5]]
        if n == ".DVP.ovlystrtab": st = d[s[4]:s[4] + s[5]]
    imgs = {}
    for i in range(0, len(tab) // 12 * 12, 12):
        no, lma, vma = struct.unpack_from("<3I", tab, i)
        n = st[no:st.index(b"\0", no)].decode()
        g = n.split(".")[5]
        imgs.setdefault(g, {})
        b = d[lma - 0x100080 + 0x1000: lma - 0x100080 + 0x1000 + sizes[n]]
        for k in range(0, len(b), 8):
            imgs[g][vma + k] = struct.unpack_from("<II", b, k)
    return imgs

FLAGREAD = {"fcand", "fcor", "fceq", "fcget", "fsand", "fsor", "fseq", "fmand", "fmor", "fmeq"}
def vi_written(lw):
    n = lw.name
    if n in ("iadd", "isub", "iand", "ior"): return lw.id_ & 15
    if n in ("iaddi", "iaddiu", "isubiu", "ilw", "ilwr", "mtir", "xtop", "xitop", "bal", "jalr"): return lw.it & 15
    if n in ("lqi", "lqd"): return lw.is_ & 15
    if n in ("sqi", "sqd"): return lw.it & 15
    return 0

SZ = 748   # sizeof(RcVu0), host/include/rc.h (keep in step with the struct)
def parse(path, vu):
    b = open(path, "rb").read()
    pc, = struct.unpack_from("<I", b, 0)
    st = b[4:4 + SZ]
    msz = 0x4000 if vu else 0x1000
    mem = bytearray(b[4 + SZ:4 + SZ + msz])
    top, itop = struct.unpack_from("<2I", b, 4 + SZ + msz) if len(b) >= 4 + SZ + msz + 8 else (0, 0)
    vf = np.frombuffer(st[0:512], dtype=np.uint32).reshape(32, 4).copy()
    acc = np.frombuffer(st[512:528], dtype=np.uint32).copy()
    q, i, p, r = struct.unpack_from("<4I", st, 528)
    vi = list(struct.unpack_from("<16I", st, 544))
    clip, status, mac = struct.unpack_from("<3I", st, 608)
    return dict(pc=pc, vf=vf, acc=acc, q=q, i=i, vi=vi, clip=clip, status=status, mac=mac, mem=mem, top=top, itop=itop)

def fl(u): return np.array([u], dtype=np.uint32).view(np.float32)[0]
def bits(x): return int(np.array([x], dtype=np.float32).view(np.uint32)[0])

class VU:
    def __init__(s, st, code, vu):
        s.vf = st["vf"]; s.acc = st["acc"]; s.vi = st["vi"]; s.q = st["q"]; s.i = st["i"]
        s.clip = st["clip"]; s.status = st["status"]; s.mac = st["mac"]; s.mem = st["mem"]
        s.code = code; s.vu = vu; s.qmask = 0x3FF if vu else 0xFF
        s.cyc = 0; s.ready = {}  # (reg, comp) -> cycle
        s.fpipe = []  # (rdy, mac, status)
        s.cpipe = []
        s.cl_int = s.clip; s.st_int = s.status
        s.qp = None  # (rdy, val)
        s.kicks = []
        s.top, s.itop = st["top"], st["itop"]
        s.vhist = []
        s.targets = set()
        for pc_, w_ in code.items():
            l_ = decode_pair(*w_)[1]
            if l_ is not None and l_.name in ("b", "bal", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"):
                s.targets.add((pc_ + 8 + l_.imm * 8) & (0x3FF8 if vu else 0xFF8))
    def F(s, r, c): return fl(int(s.vf[r][c]))
    def setF(s, r, c, x):
        if r: s.vf[r][c] = bits(x)
    def memw(s, qa, c): o = ((qa & s.qmask) << 4) + 4 * c; return struct.unpack_from("<I", s.mem, o)[0]
    def setmem(s, qa, c, v): o = ((qa & s.qmask) << 4) + 4 * c; struct.pack_into("<I", s.mem, o, v & 0xFFFFFFFF)
    def flsync(s):
        while s.fpipe and s.fpipe[0][0] <= s.cyc:
            _, s.mac, s.status = s.fpipe.pop(0)
        while s.cpipe and s.cpipe[0][0] <= s.cyc:
            _, s.clip = s.cpipe.pop(0)
        if s.qp and s.qp[0] <= s.cyc:
            s.q = s.qp[1]; s.qp = None
    def setvi(s, r, v):
        if r & 15: s.vi[r & 15] = v & 0xFFFF
    def run(s, pc, maxsteps=2000000):
        bpending = None; ending = 0
        steps = 0
        while True:
            steps += 1
            if steps > maxsteps: raise Exception("too many steps")
            lo, hi = s.code[pc]
            u, l, flg, imm = decode_pair(lo, hi)
            # stalls
            reads = s.reads(u, l)
            t = s.cyc + 1
            need = max([s.ready.get(x, 0) for x in reads] + [0])
            t = max(t, need)
            if l is not None and l.name in ("waitq", "div", "sqrt", "rsqrt") and s.qp:
                t = max(t, s.qp[0])
            stalled = t > s.cyc + 1
            s.cyc = t
            s.flsync()
            # upper
            ures = s.upper(u)
            # lower
            nb = None
            before_vi = list(s.vi)
            if imm is not None:
                s.i = imm
            elif l is not None:
                # branch VI delay (see tools/vu_recomp.py): the branch reads the value
                # from before a run of writes to its register right before it
                patch = {}
                if l.name in ("ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez", "jr", "jalr") and not stalled and pc not in s.targets:
                    regs = [l.is_ & 15] + ([l.it & 15] if l.name in ("ibeq", "ibne") else [])
                    for x in regs:
                        if not x: continue
                        h = s.vhist
                        if not h or h[-1][0] != x or h[-1][2]: continue
                        k = len(h) - 1
                        while k > 0 and len(h) - k < 4 and h[k - 1][0] == x and not h[k - 1][2] and not h[k][3] and not h[k][4]:
                            k -= 1
                        patch[x] = h[k][1]
                        break
                real = {x: s.vi[x] for x in patch}
                for x, val in patch.items(): s.vi[x] = val
                nb = s.lower(l, pc)
                for x, val in patch.items():
                    if s.vi[x] == val: s.vi[x] = real[x]
            # history of VI writes: (reg, value before, reads flags, stalled, branch target)
            if l is not None and imm is None:
                wr = vi_written(l)
                s.vhist.append((wr, before_vi[wr] if wr else 0, l.name in FLAGREAD, stalled, pc in s.targets))
                if len(s.vhist) > 8: s.vhist.pop(0)
            else:
                s.vhist.append((0, 0, False, stalled, pc in s.targets))
                if len(s.vhist) > 8: s.vhist.pop(0)
            # commit upper
            if ures: ures()
            for x in s.writes(u, l):
                s.ready[x] = s.cyc + 4
            # control
            if ending:
                return pc + 8
            if bpending is not None:
                tgt = bpending; bpending = None
                if flg["E"]: ending = 1
                pc = tgt
                continue
            if flg["E"]:
                ending = 1
            if nb is not None:
                bpending = nb
            pc = pc + 8
    def reads(s, u, l):
        r = []
        k = u.kind
        if k == "arith":
            for c in u.comps:
                r.append((u.fs, c))
                if u.bc in ("x","y","z","w"): r.append((u.ft, XYZW.index(u.bc)))
                elif u.bc is None: r.append((u.ft, c))
        elif k in ("opmula", "opmsub"):
            for c in range(3): r += [(u.fs, c), (u.ft, c)]
        elif k in ("itof", "ftoi", "abs"): r += [(u.fs, c) for c in u.comps]
        elif k == "clip": r += [(u.fs, 0), (u.fs, 1), (u.fs, 2), (u.ft, 3)]
        if l is not None:
            n = l.name
            if n in ("sq", "sqi", "sqd", "move"): r += [(l.fs, c) for c in l.comps]
            elif n == "mr32": r += [(l.fs, (c + 1) & 3) for c in l.comps]
            elif n == "mtir": r.append((l.fs, l.fsf))
            elif n == "div" or n == "rsqrt": r += [(l.fs, l.fsf), (l.ft, l.ftf)]
            elif n == "sqrt": r.append((l.ft, l.ftf))
        return [x for x in r if x[0]]
    def writes(s, u, l):
        w = []
        k = u.kind
        if k == "arith" and not u.acc: w += [(u.fd, c) for c in u.comps]
        elif k == "opmsub": w += [(u.fd, c) for c in u.comps if c < 3]
        elif k in ("itof", "ftoi", "abs"): w += [(u.ft, c) for c in u.comps]
        if l is not None and l.name in ("lq", "lqi", "lqd", "move", "mr32", "mfir"):
            w += [(l.ft, c) for c in l.comps]
        return [x for x in w if x[0]]
    def upper(s, u):
        k = u.kind
        if k == "nop": return None
        if k == "arith":
            base = u.base if u.acc else u.name
            def B(c):
                if u.bc in ("x","y","z","w"): return s.F(u.ft, XYZW.index(u.bc))
                if u.bc == "q": return fl(s.q)
                if u.bc == "i": return fl(s.i)
                return s.F(u.ft, c)
            res = {}
            for c in u.comps:
                a, b = s.F(u.fs, c), B(c)
                accv = fl(int(s.acc[c]))
                if base == "add": x = f32(a + b)
                elif base == "sub": x = f32(a - b)
                elif base == "mul": x = f32(a * b)
                elif base == "madd": x = f32(accv + f32(a * b))
                elif base == "msub": x = f32(accv - f32(a * b))
                elif base == "max": x = np.fmax(a, b)
                elif base == "mini": x = np.fmin(a, b)
                res[c] = x
            def commit():
                for c, x in res.items():
                    if u.acc: s.acc[c] = bits(x)
                    elif u.fd: s.vf[u.fd][c] = bits(x)
                if base in ("add", "sub", "mul", "madd", "msub"):
                    s.push_flags(res)
            return commit
        if k in ("opmula", "opmsub"):
            ex = {0: (1, 2), 1: (2, 0), 2: (0, 1)}
            res = {}
            for c in [c for c in u.comps if c < 3]:
                a, b = ex[c]
                pr = f32(s.F(u.fs, a) * s.F(u.ft, b))
                res[c] = pr if k == "opmula" else f32(fl(int(s.acc[c])) - pr)
            def commit():
                for c, x in res.items():
                    if k == "opmula": s.acc[c] = bits(x)
                    elif u.fd: s.vf[u.fd][c] = bits(x)
                s.push_flags(res)
            return commit
        if k == "itof":
            res = {c: bits(f32(np.int32(np.uint32(s.vf[u.fs][c]).view(np.int32)) / f32(1 << u.shift))) for c in u.comps}
            def commit():
                for c, x in res.items():
                    if u.ft: s.vf[u.ft][c] = x
            return commit
        if k == "ftoi":
            res = {}
            for c in u.comps:
                x = float(s.F(u.fs, c)) * (1 << u.shift)
                x = f32(x)
                if x != x: v = 0
                elif x >= 2147483647.0: v = 0x7FFFFFFF
                elif x <= -2147483648.0: v = -0x80000000
                else: v = int(x)
                res[c] = v & 0xFFFFFFFF
            def commit():
                for c, x in res.items():
                    if u.ft: s.vf[u.ft][c] = x
            return commit
        if k == "abs":
            res = {c: int(s.vf[u.fs][c]) & 0x7FFFFFFF for c in u.comps}
            def commit():
                for c, x in res.items():
                    if u.ft: s.vf[u.ft][c] = x
            return commit
        if k == "clip":
            w = abs(s.F(u.ft, 3)); x, y, z = s.F(u.fs, 0), s.F(u.fs, 1), s.F(u.fs, 2)
            f = (x > w) | ((x < -w) << 1) | ((y > w) << 2) | ((y < -w) << 3) | ((z > w) << 4) | ((z < -w) << 5)
            def commit():
                s.cl_int = ((s.cl_int << 6) | int(f)) & 0xFFFFFF
                s.cpipe.append((s.cyc + 4, s.cl_int))
            return commit
        raise Exception("upper " + u.name)
    def push_flags(s, res):
        mac = 0
        for c, x in res.items():
            b = bits(x)
            if (b & 0x7FFFFFFF) == 0: mac |= 8 >> c
            if b >> 31: mac |= 0x80 >> c
        st = (s.st_int & 0xFC0) | (1 if mac & 0xF else 0) | (2 if mac & 0xF0 else 0)
        st |= (st & 3) << 6
        s.st_int = st
        s.fpipe.append((s.cyc + 4, mac, st))
    def lower(s, l, pc):
        n = l.name; vi = s.vi
        V = lambda r: vi[r & 15] if r & 15 else 0
        if n == "move":
            vals = [int(s.vf[l.fs][c]) for c in range(4)]
            for c in l.comps:
                if l.ft: s.vf[l.ft][c] = vals[c]
        elif n == "mr32":
            vals = [int(s.vf[l.fs][(c + 1) & 3]) for c in range(4)]
            for c in l.comps:
                if l.ft: s.vf[l.ft][c] = vals[c]
        elif n in ("lq", "lqi", "lqd"):
            if n == "lqd": s.setvi(l.is_, V(l.is_) - 1)
            a = V(l.is_) + (l.imm if n == "lq" else 0)
            for c in l.comps:
                if l.ft: s.vf[l.ft][c] = s.memw(a, c)
            if n == "lqi": s.setvi(l.is_, V(l.is_) + 1)
        elif n in ("sq", "sqi", "sqd"):
            if n == "sqd": s.setvi(l.it, V(l.it) - 1)
            a = V(l.it) + (l.imm if n == "sq" else 0)
            for c in l.comps: s.setmem(a, c, int(s.vf[l.fs][c]))
            if n == "sqi": s.setvi(l.it, V(l.it) + 1)
        elif n in ("ilw", "ilwr"):
            a = V(l.is_) + (l.imm if n == "ilw" else 0)
            s.setvi(l.it, s.memw(a, l.comps[0]))
        elif n in ("isw", "iswr"):
            a = V(l.is_) + (l.imm if n == "isw" else 0)
            for c in l.comps: s.setmem(a, c, V(l.it))
        elif n == "iadd": s.setvi(l.id_, V(l.is_) + V(l.it))
        elif n == "isub": s.setvi(l.id_, V(l.is_) - V(l.it))
        elif n == "iand": s.setvi(l.id_, V(l.is_) & V(l.it))
        elif n == "ior": s.setvi(l.id_, V(l.is_) | V(l.it))
        elif n == "iaddi": s.setvi(l.it, V(l.is_) + l.imm)
        elif n == "iaddiu": s.setvi(l.it, V(l.is_) + l.imm)
        elif n == "isubiu": s.setvi(l.it, V(l.is_) - l.imm)
        elif n == "mtir": s.setvi(l.it, int(s.vf[l.fs][l.fsf]))
        elif n == "mfir":
            v = V(l.is_); v = v - 0x10000 if v & 0x8000 else v
            for c in l.comps:
                if l.ft: s.vf[l.ft][c] = v & 0xFFFFFFFF
        elif n in ("div", "sqrt", "rsqrt"):
            if s.qp: s.q = s.qp[1]; s.qp = None
            if n == "div":
                a, b = s.F(l.fs, l.fsf), s.F(l.ft, l.ftf)
                v = bits(f32(a / b)) if b != 0 else (0xFF7FFFFF if (bits(a) ^ bits(b)) & 0x80000000 else 0x7F7FFFFF)
                s.qp = (s.cyc + 7, v)
            elif n == "sqrt":
                s.qp = (s.cyc + 7, bits(f32(np.sqrt(abs(s.F(l.ft, l.ftf))))))
            else:
                a, b = s.F(l.fs, l.fsf), f32(np.sqrt(abs(s.F(l.ft, l.ftf))))
                v = bits(f32(a / b)) if b != 0 else (0xFF7FFFFF if bits(a) & 0x80000000 else 0x7F7FFFFF)
                s.qp = (s.cyc + 13, v)
        elif n == "waitq": pass
        elif n == "fcand": s.setvi(1, 1 if (s.clip & l.imm) else 0)
        elif n == "fcor": s.setvi(1, 1 if ((s.clip | l.imm) & 0xFFFFFF) == 0xFFFFFF else 0)
        elif n == "fceq": s.setvi(1, 1 if (s.clip & 0xFFFFFF) == l.imm else 0)
        elif n == "fcset":
            s.cpipe = []; s.clip = s.cl_int = l.imm
        elif n == "fcget": s.setvi(l.it, s.clip & 0xFFF)
        elif n == "fsand": s.setvi(l.it, s.status & l.imm)
        elif n == "fmand": s.setvi(l.it, V(l.is_) & s.mac)
        elif n == "fmor": s.setvi(l.it, V(l.is_) | s.mac)
        elif n == "xgkick": s.kicks.append(V(l.is_) & 0x3FF)
        elif n == "xtop": s.setvi(l.it, s.top)
        elif n == "xitop": s.setvi(l.it, s.itop)
        elif n == "b": return (pc + 8 + l.imm * 8) & (0x3FF8 if s.vu else 0xFF8)
        elif n == "bal": s.setvi(l.it, (pc + 16) // 8); return (pc + 8 + l.imm * 8)
        elif n == "jr": return (V(l.is_) * 8)
        elif n == "jalr":
            t = V(l.is_) * 8; s.setvi(l.it, (pc + 16) // 8); return t
        elif n in ("ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"):
            a, b = V(l.it), V(l.is_)
            sb = b - 0x10000 if b & 0x8000 else b
            c = {"ibeq": a == b, "ibne": a != b, "ibltz": sb < 0, "ibgtz": sb > 0, "iblez": sb <= 0, "ibgez": sb >= 0}[n]
            if c: return pc + 8 + l.imm * 8
        else:
            raise Exception("lower " + n)
        return None

def main():
    g, vu = sys.argv[2], int(sys.argv[3])
    bp = sys.argv[4] if len(sys.argv) > 4 else "vucap_before.bin"
    ap = sys.argv[5] if len(sys.argv) > 5 else "vucap_after.bin"
    code = load_images()[g]
    reads_flags = any(decode_pair(*w)[1] is not None and decode_pair(*w)[1].name in
                      ("fsand", "fsor", "fseq", "fmand", "fmor", "fmeq") for w in code.values())
    st = parse(bp, vu); af = parse(ap, vu)
    m = VU(st, code, vu)
    m.run(st["pc"])
    while m.fpipe: _, m.mac, m.status = m.fpipe.pop(0)
    while m.cpipe: _, m.clip = m.cpipe.pop(0)
    if m.qp: m.q = m.qp[1]
    diffs = 0
    for r in range(32):
        for c in range(4):
            if int(m.vf[r][c]) != int(af["vf"][r][c]):
                diffs += 1
                if diffs < 30: print("vf%d.%s ref %08X C %08X" % (r, XYZW[c], int(m.vf[r][c]), int(af["vf"][r][c])))
    for r in range(16):
        if m.vi[r] != af["vi"][r]: diffs += 1; print("vi%d ref %04X C %04X" % (r, m.vi[r], af["vi"][r]))
    for c in range(4):
        if int(m.acc[c]) != int(af["acc"][c]): diffs += 1; print("acc.%s ref %08X C %08X" % (XYZW[c], int(m.acc[c]), int(af["acc"][c])))
    for nm in ("q", "clip") + (("status", "mac") if reads_flags else ()):
        if getattr(m, nm) != af[nm]: diffs += 1; print("%s ref %08X C %08X" % (nm, getattr(m, nm), af[nm]))
    md = [k for k in range(0, len(m.mem), 4) if m.mem[k:k+4] != af["mem"][k:k+4]]
    if md:
        diffs += len(md)
        print("memory differs at %d words, first at qw 0x%X" % (len(md), md[0] // 16))
        for k in md[:10]:
            print("  qw %03X.%s ref %08X C %08X" % (k // 16, XYZW[(k // 4) & 3], struct.unpack_from("<I", m.mem, k)[0], struct.unpack_from("<I", af["mem"], k)[0]))
    print("kicks:", ["%03X" % k for k in m.kicks][:20], "cycles", m.cyc)
    print("DIFFS", diffs)

if __name__ == "__main__":
    main()
