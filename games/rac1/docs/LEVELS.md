# Level programs

Each RAC1 level carries its own build of the game program. When a level
loads, ParseBin (`func_0012DA38`) copies it over the executable's `main`
segment (`.lit`, `.bss`, `.data`, the three level vtables and `.text`,
0x0015F000 up) and returns its entry point; main calls it (OpenRAC
docs/OVERLAYS.md, docs/ASSETS.md "Code overlays"). The wrapper compiles
level programs natively, like the executable: C from OpenRAC's
`src/overlays` where it exists, everything else translated ahead of time
from the level's own bytes by `tools/recomp.py`. Nothing is interpreted at
run time. `build.py --levels` picks the levels (default `00`, Veldin 1,
where the main menu is).

## Where the program comes from

- **At run time** the game itself reads the level's data section from the
  disc into the buffer `D_0015EF4C` points at (exe code, before ParseBin).
  The host's ParseBin (`host/src/rac1/levels.c`) walks the records exactly as
  `src/core/boot.c` does and copies each to its address, so `lit`, `bss`,
  `data` and the vtables land where the PS2 puts them. It then recognises
  the program (entry point and text record against `gen/level_table.c`),
  fills the executable's old text and the level's text with int3, installs
  the level's trampolines and makes the level's table the one `rc_dispatch`
  uses. A program that is not built in stops the run with a log line.
- **At build time** recomp.py needs the level's code bytes.
  `tools/level_dump.py` extracts the seven records and a `manifest.json`
  (OpenRAC's dump layout) to `build/levels/level_NN/` from the disc image
  (`--iso`, `RAC1_ISO`, or `iso=` in `build/<platform>/rac1_wrapper.cfg`):
  table of contents at LBA 1500, the level's header, its data range, whose
  first word is the overlay's offset. An OpenRAC dump in the decomp repo
  (`baserom/overlays/level_NN/`) is used instead when present. These are
  game data; they stay in `build/`.

## Which code is which

`config/overlays/functions.tsv` lists every function of every level with
its places (`NN:ADDR`). For level NN the build uses those with a place in
NN, at that place:

| Function | Built from |
|---|---|
| shared / level (`func_LNN_X`) with C in `src/overlays/shared` or `src/overlays/lNN_*` | that C (gen_host.py `--level`) |
| the same, `INCLUDE_ASM`, or C with inline asm | the level's bytes (recomp.py `--level`) |
| shared, canonical level another one (`func_L00_X` in level 01) | the level's bytes (its C names level 00's data) |
| exe kind (`func_X`, a copy of an executable game function) | the level's bytes: the copy's relocations are the level's own (data at the level's addresses, calls to the level's functions), which the executable's C cannot know |
| exe kind with a host version (`host/overrides/func_X.c`: memcpy-likes, WAD unpack, movie player, sound init) | that host C, compiled again for the level |
| exe kind the host provides in `host/src` (`func_001F9988`) | the same host function |
| a table entry inside a function (InitCamera_7 at 0x2EDB68 in level 00), a `jal` target no catalogued function starts at | the level's bytes, from that address (`func_lNN_entry_ADDR`) |
| a host function the catalogue does not place (level 00's busy wait at 0x1FEFC8, catalogued as nops plus `func_L00_001FEFE0`) | found by the executable function's bytes; the host function |
| a function at several places | each place translated on its own (`func_X__lNNatADDR`); C is shared |

The level's C is only right for the level it was written for: a shared
function's C names its data and callees by its canonical level
(`D_L00_...`). Level 00 is the canonical level of everything it has, so all
its C applies. In another level, functions whose canonical level is not
that level come from its bytes (until C with relocations mapped from the
bytes, see Next).

Assembly-only functions have no definition to take a signature from, so
the generator makes one of their declarations the reference (the longest
plain `extern` prototype), puts every other prototype in its float/int
order and gives recomp.py's C entry the same signature: the EE passes
floats and integers in separate registers, so `f(float, void *)` in one
file and `f(void *, float)` in another are the same call there, not on
x86.

## Names: one header per level

The executable and every level link into one program, and their names
overlap (the same `func_X` is the executable's function and a different
copy in each level; a shared `func_L00_X` exists in 19 levels). So the
level pass gives every name it defines or places the suffix `_lNN`:
`gen/lNN/lNN_names.h` (`#define func_L00_X func_L00_X_l00`, the same for
each main-segment `func_X`, each level data name `D_L00_X`, each alias) is
forced into every C file of the level (`-include`), and asm labels
(`__asm__("D_L00_X")`, `SDATA(...)`) are rewritten to match. Names the
executable pass provides with the same value (the core segment, core data
below 0x15F000) are left alone and resolve to the executable's. The
generator's own passes work on the original names; only the compiler sees
the suffixes. Aliases from the generator's passes (`__h1`, `__imp`) carry
the level too (`__l00h1`).

Level outputs in `build/<platform>/gen/lNN/`:

| File | What |
|---|---|
| `overlays_*.c` | the level's C (other levels' functions left out) |
| `asm_stubs.c`, `missing.c` | stubs: the first are replaced by recomp.py; the second log calls to names the level's C uses that this level's program does not have |
| `rc_funcs_*.c`, `rc_wrappers.c`, `rc_thunks.c`, `rc_table.c` | translated functions (`rcNN_` prefix), their C entries, thunks into C, the dispatch table `rcNN_table` (with the core's translated functions and thunks) |
| `tramp_table.c` | `uya_tramp_table_lNN`: every function of the level at every place |
| `abs_syms.s` | the level's data and other by-address names (with the suffix) |
| `sigs.json`, `asm_entries.json`, `overrides.txt`, `funcs.tsv` | for recomp.py and build.py |

## Fixes made along the way (both passes)

- **Function values**: a function named as a value (callback argument,
  pointer stored in a moby, `m->f74 = func_L00_0028F458`) is its PS2
  address on the PS2. In level C it now is too (`((__typeof__(f) *)0xADDR)`),
  so translated code and tables get an address `rc_dispatch` knows, and C
  calls through it still work (trampoline). Level pass only.
- **Labeled prototypes** (`extern void f_x(...) __asm__("f")`) now get the
  float/int order fix as well (only the order: they are deliberate views).
- **Thunks** pass the extra `uya_aN` arguments of extended definitions.
- **Quadword accesses** (`lq`/`sq`/`lqc2`/`sqc2`) of translated code keep the
  EE's masking of the low 4 bits, except on the x86 stack, where C locals
  are not 16-byte aligned (libvu0's matrix functions on C locals read the
  wrong data before). A side effect: the executable's attract loop (a run
  without `UYA_LEVEL`) now gets to its moby drawing (`func_001EE9F8`, a
  hand-written loop over scratchpad data that toSPR DMA brings in) and
  stays there after about 16 frames (not investigated yet); with
  `UYA_LEVEL=0@1` the level is asked for before.
- **recomp.py**: a function whose last instruction has a delay slot reads it
  from the next word; in level mode a function that runs off its end
  continues into the next one; libgcc (0x11DF10-0x1207B8) is translated
  (level code calls helpers the executable's C does not).

## Testing

`UYA_LEVEL="0@1"` asks for level 0 at vsync 1, as the title menu's "go to
level" (`func_0022F4A0`) does, so a run reaches ParseBin without the menu.
`UYA_VSYNCS=N` ends a run at vsync N with the list of missing functions.

```
RAC1_ISO=disc.iso UYA_SKIPMOVIES=20 UYA_LEVEL=0@1 UYA_VSYNCS=2500 ./rac1_wrapper SCES_509.16
```

State on 2026-10-10: ParseBin loads level 00 at vsync 769 (after the
level's three opening movies), the entry point (`func_L00_002465F8`,
translated) runs the level start-up (loaders, cameras, mobys: C and
translated code mixed) and enters the level's frame loop; up to vsync 2500
no level function traps. The first frame's moby updates then wait: the update
of moby class 834 (`func_L00_002DB278`) starts the scene's dialog stream
(`func_L00_00299B68`) and calls `sound_update` and vsync until the stream
state (the halfword at 0x0015182A) reaches 3. Since 2026-10-11 the host's 989snd
(host/src/rac1/snd989.c) plays the stream and answers "started" then "buffered", so the
state goes 1, 2, 3, the scene continues it (4) and the dialog plays to its end. Nothing is drawn (as in the executable: the
display set-up and VU1 are not there yet).

## Next

- **Exe-kind functions from C.** About 940 of level 00's functions are
  copies of executable functions that have C in `src/game`; today they are
  translated from bytes. Comparing each copy with the executable's bytes
  instruction by instruction gives its relocation map (each `%hi/%lo` and
  `$gp` reference, each `jal`): the executable's C can then be compiled for
  the level with those names mapped (a per-function rename, as the header
  does per level), and checked: a copy whose map is not consistent stays
  translated. The same map makes shared C usable in levels other than its
  canonical one.
- **Other levels**: `--levels 00,01,...` builds them (shared functions of
  another canonical level from bytes); only level 00 has been run.
- The level's assembly-only functions shrink as OpenRAC matches them; the
  build picks the C up by itself.
