#!/usr/bin/env python3
"""crash_lookup.py: names the function at a crash offset in rac1_game.dll.

Builds from kit 2026.10.14 on name crashes in rac1_log.txt themselves (rac1_game.sym);
this is for older builds, or offsets from elsewhere.

usage: python crash_lookup.py rac1_game.dll rac1_game.pdb 0x0c4f6807 [more offsets]

The offset is the "Fault offset" of a Windows Application Error event (or a crash
address minus the DLL's base). The .pdb must come from the same build as the .dll:
the launcher deletes it after building, so rebuild by hand to keep it (see below).
A player's crash can be looked up with your own build as long as both were built
from the same kit version: the build is reproducible.

To keep the .pdb, run the build yourself from the game's folder (games\\rac1):
  ..\\..\\toolchain\\python\\python.exe -u build.py --platform windows --iso "C:\\path\\disc.iso" --repo decomp --levels 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18
with ..\\..\\toolchain\\zig on PATH. It reads only the PDB's public symbols."""
import bisect, struct, sys


def sections(dll):
    d = open(dll, "rb").read(4096)
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    n, opt = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    o = pe + 24 + opt
    return [struct.unpack_from("<8sII", d, o + i * 40) for i in range(n)]   # name, vsize, rva


def pdb_streams(path):
    f = open(path, "rb").read()
    if not f.startswith(b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"): sys.exit("not a PDB: " + path)
    bs, _, nblocks, dirbytes, _, dirmap = struct.unpack_from("<6I", f, 32)
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


def publics(pdb):
    st = pdb_streams(pdb)
    dbi = st[3]
    symrec = struct.unpack_from("<H", dbi, 20)[0]   # DBI header: SymRecordStream
    rec, p, syms = st[symrec], 0, []
    while p + 4 <= len(rec):
        ln, kind = struct.unpack_from("<HH", rec, p)
        if kind == 0x110E:   # S_PUB32: flags, offset, segment, name
            _, off, seg = struct.unpack_from("<IIH", rec, p + 4)
            name = rec[p + 14:rec.index(b"\0", p + 14)].decode("utf-8", "replace")
            syms.append((seg, off, name))
        p += 2 + ln
    return syms


def main():
    if len(sys.argv) < 4: sys.exit(__doc__)
    secs = sections(sys.argv[1])
    table = sorted((secs[seg - 1][2] + off, name) for seg, off, name in publics(sys.argv[2]) if 0 < seg <= len(secs))
    addrs = [a for a, _ in table]
    for arg in sys.argv[3:]:
        rva = int(arg, 16)
        i = bisect.bisect_right(addrs, rva) - 1
        sec = next((s[0].rstrip(b"\0").decode() for s in secs if s[2] <= rva < s[2] + s[1]), "?")
        if i < 0: print("%s: before every symbol (section %s)" % (arg, sec)); continue
        print("%s: %s+0x%X (section %s)" % (arg, table[i][1], rva - table[i][0], sec))


main()
