"""vu_isa.py: decoding of PS2 VU micro-mode instructions (shared by vu_recomp.py).

A VU micro instruction is a 64-bit pair: the lower word (at the lower address)
holds the lower-unit op (load/store, integer, branch, DIV/EFU...), the upper
word the FMAC op plus the I/E/M/D/T bits. With the I bit set the lower word is
a float immediate for the I register (LOI).
"""

XYZW = "xyzw"


def dest(w):
    d = (w >> 21) & 15
    return [i for i in range(4) if d & (8 >> i)]


def dstr(w):
    return "".join(XYZW[i] for i in dest(w))


class Up:
    """decoded upper op: name, kind, fd/fs/ft, comps, bc ('x'..'w', 'q', 'i' or None), acc (writes ACC)"""
    def __init__(self, name, w, **kw):
        self.name, self.w = name, w
        self.comps = dest(w)
        self.ft, self.fs, self.fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        self.bc = None
        self.__dict__.update(kw)

    def text(self):
        return "%s.%s ft=%d fs=%d fd=%d%s" % (self.name, dstr(self.w), self.ft, self.fs, self.fd,
                                             (" bc=" + self.bc) if self.bc else "")


_ARITH1 = {0x00: "add", 0x04: "sub", 0x08: "madd", 0x0C: "msub", 0x10: "max", 0x14: "mini", 0x18: "mul"}
_S1 = {0x1C: ("mul", "q"), 0x1D: ("max", "i"), 0x1E: ("mul", "i"), 0x1F: ("mini", "i"),
       0x20: ("add", "q"), 0x21: ("madd", "q"), 0x22: ("add", "i"), 0x23: ("madd", "i"),
       0x24: ("sub", "q"), 0x25: ("msub", "q"), 0x26: ("sub", "i"), 0x27: ("msub", "i"),
       0x28: ("add", None), 0x29: ("madd", None), 0x2A: ("mul", None), 0x2B: ("max", None),
       0x2C: ("sub", None), 0x2D: ("msub", None), 0x2F: ("mini", None)}
_ARITH2 = {0x00: "add", 0x04: "sub", 0x08: "madd", 0x0C: "msub", 0x18: "mul"}
_S2 = {0x1C: ("mul", "q"), 0x20: ("add", "q"), 0x21: ("madd", "q"), 0x22: ("add", "i"), 0x23: ("madd", "i"),
       0x24: ("sub", "q"), 0x25: ("msub", "q"), 0x26: ("sub", "i"), 0x27: ("msub", "i"), 0x1E: ("mul", "i"),
       0x28: ("add", None), 0x29: ("madd", None), 0x2A: ("mul", None), 0x2C: ("sub", None), 0x2D: ("msub", None)}


def decode_upper(w):
    op = w & 0x3F
    if op < 0x3C:
        if op < 0x1C:
            base = op & 0x3C
            if base in _ARITH1:
                return Up(_ARITH1[base], w, kind="arith", bc=XYZW[op & 3], acc=False)
        if op in _S1:
            n, bc = _S1[op]
            return Up(n, w, kind="arith", bc=bc, acc=False)
        if op == 0x2E:
            return Up("opmsub", w, kind="opmsub", acc=False)
        return Up("?up%02X" % op, w, kind="bad")
    op2 = ((w >> 6) & 31) << 2 | (w & 3)
    if op2 < 0x1C and (op2 & 0x3C) in _ARITH2 and op2 not in range(0x10, 0x18):
        return Up(_ARITH2[op2 & 0x3C] + "a", w, kind="arith", bc=XYZW[op2 & 3], acc=True, base=_ARITH2[op2 & 0x3C])
    if op2 in _S2:
        n, bc = _S2[op2]
        return Up(n + "a", w, kind="arith", bc=bc, acc=True, base=n)
    if 0x10 <= op2 <= 0x13:
        return Up("itof%d" % (0, 4, 12, 15)[op2 & 3], w, kind="itof", shift=(0, 4, 12, 15)[op2 & 3])
    if 0x14 <= op2 <= 0x17:
        return Up("ftoi%d" % (0, 4, 12, 15)[op2 & 3], w, kind="ftoi", shift=(0, 4, 12, 15)[op2 & 3])
    if op2 == 0x1D: return Up("abs", w, kind="abs")
    if op2 == 0x1F: return Up("clipw", w, kind="clip")
    if op2 == 0x2E: return Up("opmula", w, kind="opmula")
    if op2 == 0x2F: return Up("nop", w, kind="nop")
    return Up("?up2_%02X" % op2, w, kind="bad")


class Lo:
    def __init__(self, name, w, **kw):
        self.name, self.w = name, w
        self.comps = dest(w)
        self.it, self.is_, self.id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        self.ft, self.fs, self.fd = self.it, self.is_, self.id_
        self.fsf, self.ftf = (w >> 21) & 3, (w >> 23) & 3
        self.__dict__.update(kw)

    def text(self):
        return "%s.%s it=%d is=%d id=%d %s" % (self.name, dstr(self.w), self.it, self.is_, self.id_,
                                              " ".join("%s=%s" % (k, getattr(self, k)) for k in ("imm",) if hasattr(self, k)))


def s11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


_LOWER_S2 = {0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi", 0x36: "lqd", 0x37: "sqd", 0x38: "div",
             0x39: "sqrt", 0x3A: "rsqrt", 0x3B: "waitq", 0x3C: "mtir", 0x3D: "mfir", 0x3E: "ilwr", 0x3F: "iswr",
             0x40: "rnext", 0x41: "rget", 0x42: "rinit", 0x43: "rxor", 0x64: "mfp", 0x68: "xtop", 0x69: "xitop",
             0x6C: "xgkick", 0x70: "esadd", 0x71: "ersadd", 0x72: "eleng", 0x73: "erleng", 0x74: "eatanxy",
             0x75: "eatanxz", 0x76: "esum", 0x78: "esqrt", 0x79: "ersqrt", 0x7A: "ercpr", 0x7B: "waitp",
             0x7C: "esin", 0x7D: "eatan", 0x7E: "eexp"}
_LOWER = {0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw", 0x08: "iaddiu", 0x09: "isubiu", 0x10: "fceq",
          0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x14: "fseq", 0x15: "fsset", 0x16: "fsand", 0x17: "fsor",
          0x18: "fmeq", 0x1A: "fmand", 0x1B: "fmor", 0x1C: "fcget", 0x20: "b", 0x21: "bal", 0x24: "jr",
          0x25: "jalr", 0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz", 0x2E: "iblez", 0x2F: "ibgez"}
BRANCHES = {"b", "bal", "jr", "jalr", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"}


def decode_lower(w):
    op = w >> 25
    if op == 0x40:
        f = w & 0x3F
        if f < 0x3C:
            n = {0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"}.get(f)
            if n is None:
                return Lo("?lo40_%02X" % f, w)
            l = Lo(n, w)
            if n == "iaddi":
                imm = (w >> 6) & 31
                l.imm = imm - 32 if imm & 16 else imm
            return l
        op2 = ((w >> 6) & 31) << 2 | (w & 3)
        n = _LOWER_S2.get(op2)
        if n is None:
            return Lo("?lo2_%02X" % op2, w)
        return Lo(n, w)
    n = _LOWER.get(op)
    if n is None:
        return Lo("?lo%02X" % op, w)
    l = Lo(n, w)
    if n in ("lq", "sq", "ilw", "isw", "b", "bal", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"):
        l.imm = s11(w)
    elif n in ("iaddiu", "isubiu"):
        l.imm = ((w >> 10) & 0x7800) | (w & 0x7FF)
    elif n in ("fceq", "fcset", "fcand", "fcor"):
        l.imm = w & 0xFFFFFF
    elif n in ("fseq", "fsset", "fsand", "fsor"):
        l.imm = ((w >> 10) & 0x800) | (w & 0x7FF)
    return l


def decode_pair(lo, hi):
    """returns (upper, lower or None, flags dict, i_imm or None)"""
    up = decode_upper(hi)
    fl = {"I": (hi >> 31) & 1, "E": (hi >> 30) & 1, "M": (hi >> 29) & 1, "D": (hi >> 28) & 1, "T": (hi >> 27) & 1}
    if fl["I"]:
        return up, None, fl, lo
    return up, decode_lower(lo), fl, None
