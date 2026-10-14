# Project Solana

Native PC versions of the PlayStation 2 Ratchet & Clank games, built on your own PC from your own disc.

Project Solana is a launcher and a set of static recompilations. Each game's code is translated ahead of time from the game's executable into C, with the decompiled source from OpenRAC where it exists, and compiled into a native Windows or Linux program that reads everything else (models, textures, sound, movies) from your disc image at run time.

**Nothing from the games is distributed here.** The launcher downloads only tools and source code; the game is built on your machine from a disc image you provide.

## Playing on Windows

1. Download `Project Solana.exe` from the [latest launcher release](../../releases) and put it in a folder of its own (it keeps its files next to itself).
2. Run it and add your disc image: **Add disc image**, **Scan a folder**, or drag an `.iso` onto the window.
3. Select the game and press **Build**. The first build downloads about 95 MB (Python and the zig compiler, then the game's build kit) and builds the game from your disc: about 15 minutes on a recent 6 to 8 core CPU, an hour or more on an older 4 core one. It only happens once per game version.
4. Press **Play**.

## Playing on Linux and Steam Deck

The game runs natively on Linux (no Wine or Proton). The Linux launcher is a single script, `launcher/linux/project-solana` (download it from the [latest launcher release](../../releases), then `chmod +x project-solana`):

```
./project-solana build ~/Games/ratchet.iso   # once: about 95 MB of downloads, then the same build as on Windows
./project-solana play                        # --upscaled / --native, --fullscreen / --windowed, --vsync (remembered)
./project-solana desktop                     # adds it to the applications menu; from there, Steam > Add a Non-Steam Game
```

It needs `python3` and the **32-bit** SDL2 and OpenGL libraries. Steam installs them, so SteamOS and any PC with Steam already has them; otherwise `./project-solana status` says what to install for your distribution. Everything is kept in `~/.local/share/project-solana`. On a Steam Deck, run the build once in Desktop Mode (Konsole), then add the game to Steam with `desktop`; it starts full screen there, with the Deck's controls as the controller.

### Supported games

| Game | Region | Disc executable | Status |
|---|---|---|---|
| Ratchet & Clank | PAL (v2.00) | `SCES_509.16`, MD5 `d86429e2eca4e1da7d7f1cc656af2574` | Playable start to finish |
| Ratchet & Clank | NTSC-U, NTSC-J | `SCUS_971.99`, `SCPS_150.37` | Recognised, not supported yet |
| Going Commando, Up Your Arsenal, Deadlocked | all | | Recognised, not supported yet |

### Requirements

- Windows 10 or 11, 64-bit, or Linux x86-64 (glibc 2.17 or newer, X11 or Wayland) with the 32-bit SDL2 and OpenGL libraries
- A CPU with SSE4.2 (Intel since 2008, AMD since 2011) and 8 GB of RAM
- A GPU with OpenGL 3.3 (tested: RTX 3060, Upscaled at 3x)
- A controller: Xbox, DualShock 4 or DualSense, over USB or Bluetooth (on Linux, anything SDL2 knows, including the Steam Deck's controls)
- About 2.5 GB free while building, 600 MB after (the game plus the build tools, kept for rebuilds)

### Settings

Resolution (Native, or Upscaled to a whole multiple of the PS2's 448 lines that covers your screen), VSync, the graphics threads, performance statistics in the log, the length of the F9 glitch capture, and a level select for testing. **Rebuild the game** fetches the latest build kit and builds again; saves are kept.

### Reporting problems

- **Open log** shows `rac1_log.txt` (or the build's `build_log.txt` when a build failed).
- On Linux, `project-solana report` writes one file with everything a report needs (system details, the 32-bit library check, the launcher, build and game logs, and the previous run's game log); attach that file.
- A crash writes the function it happened in and the call chain into the game log, and the previous run's log is kept as `rac1_log.prev.txt`.
- **F9** in the game records the next few seconds of graphics input to `gsrec_NN.bin` (about 260 MB per second of capture); attach it to a report together with what looked wrong.

## Repository layout

| Path | What |
|---|---|
| `launcher/` | `Project Solana.exe`: Win32 + GDI, no dependencies. `python launcher/build_launcher.py` builds it (needs zig: `pip install ziglang`) |
| `launcher/linux/project-solana` | the Linux launcher (Python 3, no dependencies) |
| `games/rac1/` | Ratchet & Clank's build kit: `build.py`, the recompilers (`tools/`), the host engine (`host/`: GS in OpenGL and software, DMA, VU, SPU2 and 989snd, disc, pad, memory card, movies; Win32 and Linux/SDL2 platform layers), the wrapper (`launcher/`) |
| `games/rac1/dev/` | developer tools (GS capture replay, VU reference check); not part of the kit |
| `release/make_release.py` | makes a game's release files |
| `THIRD_PARTY_NOTICES.md` | pl_mpeg (MIT), jak-project's 989snd tables (ISC) |

## How the Build button works

The launcher fetches from the release named after the game (`releases/download/rac1/`):

| File | What |
|---|---|
| `rac1.json` | version, file names, sizes and SHA-256s, the levels to build |
| `toolchain-win64.zip` | Python 3.12 (python-build-standalone), rabbitizer, zig 0.16; shared by all games, kept in `toolchain\` and fetched again only when its version changes |
| `toolchain-linux64.zip` | the same for Linux x86-64 (the Linux launcher's) |
| `rac1-build.zip` | `games/rac1` plus the decomp's source as `decomp/` (no `baserom/`) |

It checks both zips against their SHA-256s, unpacks them next to itself (`toolchain\`, `games\rac1\`), runs `build.py --platform windows --iso <your disc>`, and then deletes the intermediate files (about 1.3 GB) and the bytes it read from the disc, keeping `games\rac1\build\windows` (the game, its saves and logs).

`solana.ini` `[build]` `release_url=` (a URL or a local folder laid out like the release) and `levels=` override where it fetches from and which levels it builds, for testing.

## Making a release

```
python release/make_release.py rac1 --decomp ../rac1-decomp --version 2026.10.14.2 \
    --toolchain-zip dist/rac1/toolchain-win64.zip --toolchain-linux-zip dist/rac1/toolchain-linux64.zip   # (reuse, or omit to assemble them)
gh release create rac1 dist/rac1/rac1.json dist/rac1/rac1-build.zip dist/rac1/toolchain-win64.zip dist/rac1/toolchain-linux64.zip
```

To update an existing game release, upload the new files over the old ones (`gh release upload rac1 dist/rac1/* --clobber`). Players get them with **Rebuild the game**.

The launchers go in a release of their own (for example `launcher-v1`): `Project Solana.exe` and `project-solana` (Linux).