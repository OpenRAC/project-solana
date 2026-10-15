# Ratchet & Clank (PAL v2.00, NTSC-U): build kit

A static recompilation of Ratchet & Clank (PAL `SCES_509.16`, and NTSC-U `SCUS_971.99` with `--region us`) for Windows and Linux.

`build.py` reads the game's executable and level programs from your disc image, generates C from them (the decomp's own C where it has it, everything else translated instruction by instruction by `tools/recomp.py`, the VU microcode by `tools/vu_recomp.py`), and compiles it with the host engine in `host/` into `build/windows/rac1_game.dll` and `rac1_wrapper.exe`.

```
python build.py --platform windows --iso "Ratchet & Clank (Europe).iso" --repo ../rac1-decomp --levels 0,1,...,18
```

`--region us` builds the NTSC-U version into `build/<platform>_us` from the same sources: `tools/region.py` first writes the decomp and `host/` in NTSC-U addresses (`build/<platform>_us/region`, with `region_report.txt` listing what it changed and what had no counterpart), using the address map `region/us.json` (made by `dev/region_map.py` from both executables and both sets of levels). Code only the NTSC-U game has goes in `host/region/us/`, already in its addresses.

Needs Python 3, `pip install rabbitizer ziglang`. The Project Solana launcher runs this for players with its own copies of both.

| Path | What |
|---|---|
| `tools/gen_host.py` | the decomp's C made host-ready (types, calls between C and translated code) |
| `tools/recomp.py` | MIPS (R5900) to C, the executable and each level |
| `tools/vu_recomp.py` | VU0/VU1 microprograms to C |
| `tools/level_dump.py` | a level's program from the disc image |
| `tools/region.py`, `region/us.json` | the NTSC-U build: the sources rewritten in its addresses |
| `host/src/gs/` | the Graphics Synthesizer: software and OpenGL 3.3 (upscaling: `UYA_SCALE`) |
| `host/src/gfx/` | VIF, GIF and the VU runtime |
| `host/src/sdk/`, `host/src/rac1/` | Sony library entry points, 989snd and the SPU2, levels, boot |
| `host/overrides/` | hand-ported assembly functions |
| `launcher/` | `rac1_wrapper.exe`: reserves PS2 RAM at its real addresses, joins split DLLs, runs the DLL |
| `docs/LEVELS.md` | how level programs are built in |

Environment variables for testing (`UYA_FPSLOG`, `UYA_GSREC`, `UYA_LEVEL`, `UYA_SCALE`, ...) are described where they are read.
