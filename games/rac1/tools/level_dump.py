#!/usr/bin/env python3
"""level_dump.py OUT_DIR LEVEL [LEVEL...] [--iso ISO] [--repo DECOMP_REPO]

Writes each level's program (the code overlay ParseBin loads over the
executable's main segment) to OUT_DIR/level_NN/: one file per record
(lit, bss, data, vtbl, camvtbl, sndvtbl, text) and manifest.json with the
load addresses and the entry point, the layout of OpenRAC's own dump
(tools/overlays.py dump, baserom/overlays/level_NN/).

The bytes come from the user's own disc image, read the way the game finds
them: the table of contents at LBA 1500, the level's header, the header's
"data" sector range, whose first word is the offset of the code overlay
(docs/ASSETS.md, "Code overlays"). If the decomp repo already has an OpenRAC
dump of the level (baserom/overlays/level_NN/manifest.json), that is used
instead. Nothing here is ever committed: it is game data.

recomp.py translates the level's assembly-only functions from text.bin.
"""
import json, os, shutil, struct, sys

SECTOR = 2048
TOC_LBA, TOC_SIZE = 1500, 0x2960
LEVEL_TABLE = 0x28C8          # D_0013A548: (header LBA, size) per level
LEVEL_HEADER_SIZE = 0x2434
RECORD_NAMES = ("lit", "bss", "data", "vtbl", "camvtbl", "sndvtbl", "text")


def read(f, lba, size):
    f.seek(lba * SECTOR)
    d = f.read(size)
    if len(d) != size:
        sys.exit("level_dump: short read from the disc image at LBA %d" % lba)
    return d


def wad(raw, repo):
    """the overlay is stored plain on PAL v2.00; a WAD-packed one is unpacked
    with OpenRAC's decoder (tools/extract/formats.py)"""
    sys.path.insert(0, os.path.join(repo, "tools", "extract"))
    from formats import wad as unwad
    return unwad(raw)


def overlay_records(raw):
    """(entry point, [(address, size, type, bytes)]), as ParseBin (func_0012DA38) walks them"""
    entry = struct.unpack_from("<I", raw, 12)[0]
    pos, recs = 0, []
    while pos + 16 <= len(raw):
        addr, size, kind, e = struct.unpack_from("<4I", raw, pos)
        if e != entry:
            break
        recs.append((addr, size, kind, raw[pos + 16:pos + 16 + size]))
        pos += 16 + size
    if len(recs) != len(RECORD_NAMES):
        sys.exit("level_dump: %d overlay records, expected %d" % (len(recs), len(RECORD_NAMES)))
    return entry, recs


def dump_from_iso(iso, level, out, repo):
    with open(iso, "rb") as f:
        toc = read(f, TOC_LBA, TOC_SIZE)
        if struct.unpack_from("<II", toc, 0) != (1, TOC_SIZE):
            sys.exit("level_dump: %s does not look like the PAL v2.00 disc (table of contents)" % iso)
        lba, _ = struct.unpack_from("<II", toc, LEVEL_TABLE + level * 8)
        hdr = read(f, lba, LEVEL_HEADER_SIZE)
        if struct.unpack_from("<II", hdr, 0) != (level, LEVEL_HEADER_SIZE):
            sys.exit("level_dump: unexpected header for level %d" % level)
        start, count = struct.unpack_from("<II", hdr, 8)        # the "data" range
        data = read(f, start, count * SECTOR)
    off, size = struct.unpack_from("<ii", data, 0)
    raw = data[off:off + size]
    if raw[:3] == b"WAD":
        raw = wad(raw, repo)
    entry, recs = overlay_records(raw)
    os.makedirs(out, exist_ok=True)
    man = {"level": level, "entry_point": entry, "records": [], "source": "disc image"}
    for name, (addr, sz, kind, b) in zip(RECORD_NAMES, recs):
        open(os.path.join(out, name + ".bin"), "wb").write(b)
        man["records"].append({"name": name, "address": addr, "bytes": sz, "type": kind})
    json.dump(man, open(os.path.join(out, "manifest.json"), "w"), indent=1)


def main():
    args = sys.argv[1:]
    iso = repo = None
    rest = []
    while args:
        a = args.pop(0)
        if a == "--iso": iso = args.pop(0)
        elif a == "--repo": repo = args.pop(0)
        else: rest.append(a)
    out_dir, levels = rest[0], [int(x) for x in rest[1:]]
    for lv in levels:
        out = os.path.join(out_dir, "level_%02d" % lv)
        if os.path.exists(os.path.join(out, "manifest.json")):
            continue
        ready = os.path.join(repo or "", "baserom", "overlays", "level_%02d" % lv)
        if repo and os.path.exists(os.path.join(ready, "manifest.json")):
            shutil.copytree(ready, out, dirs_exist_ok=True)
            print("level %02d: from OpenRAC's dump %s" % (lv, ready))
            continue
        if not iso or not os.path.exists(iso):
            sys.exit("level_dump: level %02d needs the disc image (build.py --iso PATH, RAC1_ISO, or iso= in "
                     "build/<platform>/rac1_wrapper.cfg)" % lv)
        dump_from_iso(iso, lv, out, repo or "")
        print("level %02d: read from %s" % (lv, iso))


if __name__ == "__main__":
    main()
