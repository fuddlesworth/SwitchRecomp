# SwitchRecomp for Linux

A Linux-first fork of **SwitchRecomp by MZ Labs**, a static recompiler for Nintendo Switch games. The game's ARM64
code is translated to C once and compiled into a native program. The runtime stands in for the Switch's operating
system, and its GPU emulation draws with OpenGL. The game's data is read as it is from the extracted dump.

This fork adds a CMake build, source builds of every third-party library, scripts for the whole pipeline, and fixes
to the runtime. Catherine: Full Body is the game it is tested with. [ROADMAP.md](ROADMAP.md) has the plan and the
current state, and the history of `main` has every fix with the reason for it. The MZ Labs release this fork started
from (v142) is the tag `upstream-v142`.

Use only games and keys dumped from your own console. None are included, and none may be committed here.

## Requirements

- GCC 16 or Clang 23, CMake 3.20 or newer, Ninja, Make, NASM, Python 3.10 or newer
- X11 and OpenGL 4.3 development files
- `uv`, only to run the formatter

## Setup

```bash
git clone --recurse-submodules git@github.com:fuddlesworth/SwitchRecomp.git
cd SwitchRecomp
scripts/bootstrap.sh
```

`scripts/bootstrap.sh` builds OpenH264, SPIRV-Cross and hactool from the pinned submodules in `third_party/` into
`deps/`. Run it again after a submodule changes. `--target win64` cross-builds the Windows libraries with
MinGW-w64.

## Recompiling a game

Put `prod.keys` (and `title.keys`, for NSPs) in the repository root, or pass them with `--keys` and `--title-keys`.

```bash
scripts/extract.py "Catherine Full Body.xci" --name Catherine   # -> work/Catherine/{exefs,romfs}
scripts/recomp.sh work/Catherine Catherine                      # -> games/Catherine
cd games && ./Catherine
```

An update is applied with `--update update.nsp`. It must be for the same title as the base game.

The first recompile of a big game takes several minutes. After that, a runtime change only needs a runtime build,
which copies the new runtime next to every game in `games/`:

```bash
cmake --preset gcc && cmake --build --preset gcc      # or the clang preset
```

Game settings are in `games/<Game>.ini`, which the runtime rewrites with every option explained each time it
starts. The ones you are most likely to change:

| Option | Effect |
| --- | --- |
| `docked=1` | TV mode (1080p) instead of handheld (720p) |
| `resolution=1` | Internal resolution scale, 0.25 to 4 |
| `fps_limit=1` | 1 caps at 60 FPS like the console, 0 is unlimited |
| `shadows=1` | 0 skips shadow maps, which is faster on weak GPUs |
| `lsfg=1` | Frame generation with your own copy of Lossless Scaling, whose `Lossless.dll` is read at run time and never copied |
| `language=1` | The system language the game sees |
| `log=2` | Log everything, for bug reports |

When something goes wrong, `games/<Game>.log` has the details, `games/threads.txt` the state of every game thread
(rewritten every 15 seconds), and `games/performance.txt` a summary of slow frames.

Controls on the keyboard (X11): arrows for the D-pad, X or Space for A, Z for B, S for X, A for Y, Q and E for L and
R, 1 and 3 for ZL and ZR, Enter for Plus, Backspace for Minus and Escape to quit. The X11 backend has no sticks and no
gamepad support yet (see ROADMAP.md, Phase 4).

## Development

| Task | Command |
| --- | --- |
| Format C, C++ and Python | `scripts/format.sh` (`--check` to only report) |
| Name game addresses from a crash report | `tools/symbols.py work/Catherine cf34380 ...` |
| Disassemble game code | `tools/disasm.py work/Catherine NvRmStreamFlushSync` |
| Run the game for a set time with scripted input and a screenshot | `./Catherine --seconds=60 --script=15000:400,16000:1 --shot=out` |
| Import a new MZ Labs release | `scripts/import-upstream.sh SwitchRecomp_v143.zip v143`, then `git merge upstream` |

The `upstream` branch holds the MZ Labs releases: each one as released (tagged `upstream-vN`) and then formatted like
`main`, so merging a release into `main` only shows what it changed.

| Folder | What it holds |
| --- | --- |
| `switchrecomp.py`, `codegen.py`, `a64.py`, `nxloader.py` | The recompiler: loading, analysis, ARM64 to C |
| `runtime/` | The runtime: kernel, services, GPU, video, platform backends |
| `scripts/` | Bootstrap, extraction, recompiling, formatting, upstream imports |
| `tools/` | Debugging tools |
| `third_party/` | Pinned submodules, built by `scripts/bootstrap.sh` |
| `deps/`, `build/`, `work/`, `games/` | Built libraries, build trees, extracted dumps and recompiled games, none of them in git |

## License and credits

SwitchRecomp is © 2026 MZ Labs, under the Apache License 2.0. [LICENSE](LICENSE) has the license and
[NOTICE](NOTICE) the credit that every copy and derived work keeps. This fork is under the same terms. The source
files' headers still name the release's `LICENSE.txt` and `NOTICE.txt`, which are `LICENSE` and `NOTICE` here.

The Apache license covers only the SwitchRecomp code. Third-party components keep their own licenses:

| Component | Used for | License |
| --- | --- | --- |
| OpenH264 (Cisco) | Game videos | BSD 2-Clause, `third_party/openh264/LICENSE` |
| SPIRV-Cross (Khronos) | LSFG shaders | Apache 2.0, `third_party/SPIRV-Cross/LICENSE` |
| hactool (SciresM) | Extracting dumps | ISC, `third_party/hactool/LICENSE` |
| texture2ddecoder | BCn textures on GPUs without them | MIT, `runtime/bcn/LICENSE_texture2ddecoder.txt` |
| lsfg-vk | LSFG pass layout, ported to GL in `runtime/lsfg.c` | MIT |
| DejaVu Sans Mono | The on-screen overlay font (`runtime/overlay_font.h`) | Bitstream Vera / DejaVu license |
