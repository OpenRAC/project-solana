"""pdb_syms.py: the public symbols of a Windows DLL, from its .pdb (no other tools needed).

build.py writes rac1_game.sym with it after linking: one line per function, "RVA name"
in hex, sorted. The launcher deletes the .pdb (hundreds of MB) but keeps the .sym, and
the crash handler names the function a crash is in from it (in rac1_log.txt)."""
import struct


def sections(dll):
    """(name, virtual size, RVA) of each section of a PE file"""
    with open(dll, "rb") as fh: d = fh.read(4096)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    o = pe + 24 + opt
    return [struct.unpack_from("<8sII", d, o + i * 40) for i in range(n)]


def _streams(path):
    with open(path, "rb") as fh: f = fh.read()
    if not f.startswith(b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"): raise ValueError("not a PDB: " + path)
    bs, _, _, dirbytes, _, dirmap = struct.unpack_from("<6I", f, 32)
    nb = lambda n: (n + bs - 1) // bs
    blk = lambda i: f[i * bs:(i + 1) * bs]
    mapblocks = struct.unpack_from("<%dI" % nb(dirbytes), blk(dirmap))
    dirdata = b"".join(blk(b) for b in mapblocks)[:dirbytes]
    ns = struct.unpack_from("<I", dirdata)[0]
    sizes = struct.unpack_from("<%dI" % ns, dirdata, 4)
    pos, out = 4 + 4 * ns, []
    for s in sizes:
        s = 0 if s == 0xFFFFFFFF else s
        bl = struct.unpack_from("<%dI" % nb(s), dirdata, pos); pos += 4 * nb(s)
        out.append(b"".join(blk(b) for b in bl)[:s])
    return out


def symbols(dll, pdb):
    """sorted (RVA, name) of the public symbols of dll"""
    st = _streams(pdb)
    rec = st[struct.unpack_from("<H", st[3], 20)[0]]   # DBI header: the symbol record stream
    secs = sections(dll)
    p, out = 0, []
    while p + 4 <= len(rec):
        ln, kind = struct.unpack_from("<HH", rec, p)
        if kind == 0x110E:   # S_PUB32: flags, offset, segment, name
            _, off, seg = struct.unpack_from("<IIH", rec, p + 4)
            if 0 < seg <= len(secs):
                name = rec[p + 14:rec.index(b"\0", p + 14)].decode("utf-8", "replace")
                out.append((secs[seg - 1][2] + off, name[1:] if name.startswith("_") else name))
        p += 2 + ln
    return sorted(out)


def write_sym(dll, pdb, path):
    """rac1_game.sym: the code symbols, "RVA name" per line"""
    text = [s for s in sections(dll) if s[0].rstrip(b"\0") == b".text"]
    lo, hi = (text[0][2], text[0][2] + text[0][1]) if text else (0, 1 << 32)
    syms = [(a, n) for a, n in symbols(dll, pdb) if lo <= a < hi]
    with open(path, "w", newline="\n") as f:
        for a, n in syms: f.write("%08x %s\n" % (a, n))
    return len(syms)
