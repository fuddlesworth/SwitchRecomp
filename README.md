# ScarletNX

<h1 align="center">
  <br>
  <img src="./wimsy.svg" alt="Eden" width="200">
  <br>
  <b>Wimsy, the ScarletNX mascot</b>
  <br>
</h1>

A fork of **SwitchRecomp by MZ Labs**, a static recompiler for Nintendo Switch games, for Linux and Windows. The
game's ARM64 code is translated to C once and compiled into a native program. The runtime stands in for the Switch's
operating system, and its GPU emulation draws with Vulkan. The game's data is read as it is from the extracted dump.

This fork adds a CMake build, source builds of every third-party library, scripts for the whole pipeline, CI on
both systems, and fixes to the runtime. Catherine: Full Body is the game it is tested with. [ROADMAP.md](ROADMAP.md) has the plan and the
current state, and the history of `main` has every fix with the reason for it. The MZ Labs release this fork started
from (v142) is the tag `upstream-v142`.

Use only games and keys dumped from your own console. None are included, and none may be committed here.

## Linux and FreeBSD

### Requirements

- Clang 23 (FreeBSD's own Clang works too), CMake 3.25 or newer, Ninja, GNU make (`gmake` on FreeBSD), NASM,
  Python 3.12 or newer, Git
- SDL3 development files, and a GPU and driver with Vulkan 1.3
- `uv`, only to run the formatter

### Setup

```bash
git clone https://github.com/fuddlesworth/SwitchRecomp.git
cd SwitchRecomp
cmake --preset default && cmake --build --preset default
```

The first configure downloads the third-party libraries (FFmpeg, glslang, the Vulkan headers, volk, VMA and
hactool) with [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake). Each is pinned to a release and its SHA-256 in
`cmake/dependencies.cmake`, and kept in `.cache/cpm` for every build folder after that. They are built with the
runtime. Only Clang is supported: a plain `cmake -B build` picks it too. `cmake --preset windows-cross` builds the
Windows runtime with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw).

### The nightly AppImage

Every commit on `main` that builds everywhere is released as `SwitchRecomp-x86_64.AppImage` under the
[nightly](https://github.com/fuddlesworth/SwitchRecomp/releases/tag/nightly) prerelease. It is the runtime with every
library it needs, made with [pkgforge's tools](https://github.com/pkgforge-dev/Anylinux-AppImages), and runs on any
Linux. Point it at a game with `--game`, as in `./SwitchRecomp-x86_64.AppImage --game=games/Catherine`, which runs
`games/Catherine_code.so` with `games/Catherine_data/`. Or copy it next to the game's code as
`games/Catherine.AppImage` (or just `games/Catherine`): like the runtime the build makes, it then runs the game it is
named after. Recompiling the game still takes a clone of this repository and Clang. `scripts/make-appimage.sh` is what
makes it.

### Recompiling a game

Put `prod.keys` (and `title.keys`, for NSPs) in the repository root or in `~/.switch`, or pass them with `--keys` and
`--title-keys`.

```bash
python3 scripts/extract.py "Catherine Full Body.xci" --name Catherine   # -> work/Catherine/{exefs,romfs}
python3 scripts/recomp.py work/Catherine Catherine                      # -> games/Catherine
cd games && ./Catherine
```

The runtime draws with Vulkan, so the GPU needs Vulkan 1.3. The first recompile of a big game takes several minutes. After that, a runtime
change only needs a runtime build, which copies the new runtime next to every game in `games/`:

```bash
cmake --build --preset default
```

The window runs natively on Wayland, and on X11 (`SDL_VIDEO_DRIVER=x11` picks X11 on a Wayland desktop). Controls
are the same as on Windows (below).

## Windows

### Requirements

- Windows 10 or 11, 64-bit, and a GPU with Vulkan 1.3 (keep the driver up to date)
- [Git for Windows](https://git-scm.com). Everything else is downloaded by the bootstrap.

### Setup

In PowerShell:

```powershell
git clone https://github.com/fuddlesworth/SwitchRecomp.git
cd SwitchRecomp
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
. scripts\env.ps1
cmake --preset default; cmake --build --preset default
```

`scripts\bootstrap.ps1` downloads llvm-mingw (Clang), CMake, Ninja, NASM, Python and w64devkit (for the shell and
make that FFmpeg's build uses) into `deps\tools` and checks each
download against its pinned SHA-256. Nothing is installed on the system. The CMake build fetches and builds the
libraries, as on Linux. In every new PowerShell window, run `. scripts\env.ps1` first to put those tools on PATH.

### Recompiling a game

Put `prod.keys` (and `title.keys`, for NSPs) in the repository folder or in `%USERPROFILE%\.switch`, or pass them
with `--keys` and `--title-keys`.

```powershell
python scripts\extract.py "Catherine Full Body.xci" --name Catherine   # -> work\Catherine
python scripts\recomp.py work\Catherine Catherine                      # -> games\Catherine.exe
cd games; .\Catherine.exe
```

A runtime change only needs `cmake --build --preset default`.

Controls: any controller SDL knows works like a Switch controller, by position. Keyboard: arrows for the D-pad,
WASD for the left stick, IJKL for the right stick, X or Space for A, Z for B, C for X, V for Y, Q and E for L and R,
1 and 3 for ZL and ZR, Enter for Plus and Backspace for Minus. F1 shows help, F3 takes a screenshot, F4 switches the
FPS limit, F5 writes the state of every game thread to the log, F11 or Alt+Enter toggles fullscreen, and Escape
quits.

The window and input go through SDL3 on Windows too (the CMake build fetches SDL3 and links it statically), with the
same two renderers as on Linux.

## Settings and logs

An update is applied when extracting, with `--update update.nsp`. It must be for the same title as the base game.

Game settings are in `games/<Game>.ini`, which the runtime rewrites with every option explained each time it
starts. The ones you are most likely to change:

| Option | Effect |
| --- | --- |
| `docked=1` | TV mode (1080p) instead of handheld (720p) |
| `resolution=1` | Internal resolution scale, 0.25 to 4 |
| `fps_limit=1` | 1 caps at 60 FPS like the console, 0 is unlimited |
| `shadows=1` | 0 skips shadow maps, which is faster on weak GPUs |
| `language=1` | The system language the game sees |
| `log=2` | Log everything, for bug reports |

When something goes wrong, `games/<Game>.log` has the details, `games/threads.txt` the state of every game thread
(rewritten every 15 seconds), and `games/performance.txt` a summary of slow frames.

## Development

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) describes how the recompiler and the runtime fit together, and how a
frame gets from the game to the screen.

| Task | Command |
| --- | --- |
| Format C, C++ and Python | `scripts/format.sh` (`--check` to only report) |
| Name game addresses from a crash report | `tools/symbols.py work/Catherine cf34380 ...` |
| Disassemble game code | `tools/disasm.py work/Catherine NvRmStreamFlushSync` |
| Run the game for a set time with scripted input and a screenshot | `./Catherine --seconds=60 --script=15000:400,16000:1 --shot=out` |
| Import a new MZ Labs release | `scripts/import-upstream.sh SwitchRecomp_v143.zip v143`, then `git merge upstream` |
| Record the shaders a run translates, then check them for Vulkan | `SWR_SHADER_CORPUS=dir ./Catherine`, then `build/default/swr-shaderc dir --validate` |

The `upstream` branch holds the MZ Labs releases: each one as released (tagged `upstream-vN`) and then formatted like
`main`, so merging a release into `main` only shows what it changed.

| Folder | What it holds |
| --- | --- |
| `switchrecomp/` | The recompiler (`python3 -m switchrecomp`): loading, analysis, ARM64 to C, building the game module |
| `runtime/` | The runtime: kernel, services, GPU, video, platform backends |
| `scripts/` | The Windows toolchain download, extraction, recompiling, formatting, upstream imports |
| `docs/` | How the parts fit together |
| `tools/` | Debugging tools |
| `cmake/` | The third-party libraries (`dependencies.cmake`, fetched with CPM.cmake) and the deploy step |
| `.github/workflows/` | CI: formatting, and Clang builds on Linux, FreeBSD and Windows, natively and cross-built from Linux |
| `.cache/`, `deps/`, `build/`, `work/`, `games/` | Downloaded libraries, the Windows toolchain, build trees, extracted dumps and recompiled games, none of them in git |

## License and credits

This fork is licensed under the GNU General Public License, version 3 or (at your option) any later version
([LICENSE](LICENSE)). It is a derived work of SwitchRecomp, © 2026 MZ Labs, which is under the Apache License 2.0
([LICENSE.apache](LICENSE.apache)). Apache-licensed code can be part of a GPLv3 work: MZ Labs' files keep their
copyright headers and are distributed here as part of the GPLv3 whole, with a line saying the fork's changes are
GPL-3.0-or-later. [NOTICE](NOTICE) keeps the credit to "SwitchRecomp by MZ Labs" that every copy and derived work
carries. Those headers name the release's `LICENSE.txt` and `NOTICE.txt`, which are `LICENSE.apache` and `NOTICE`
here. Files the fork wrote start with `SPDX-License-Identifier: GPL-3.0-or-later`.

The game modules the recompiler builds include the runtime's `runtime/abi/` code, which is under the GPL like the rest.
Games, keys and firmware are not part of this project.

Third-party components keep their own licenses, found in each package CPM.cmake downloads (`.cache/cpm/<name>/`, from the URLs in `cmake/dependencies.cmake`):

| Component | Used for | License |
| --- | --- | --- |
| FFmpeg (libavcodec, libavutil) | Game videos (H.264, with VP8 and VP9 built in) | LGPL 2.1 or later |
| hactool (SciresM) | Extracting dumps | ISC |
| glslang (Khronos) | Vulkan shaders to SPIR-V | BSD 3-Clause, MIT and Apache 2.0, and a parser under GPL 3 with the Bison exception |
| Vulkan-Headers (Khronos) | Vulkan definitions | Apache 2.0 |
| volk (Arseny Kapoulkine) | Loading Vulkan | MIT |
| Vulkan Memory Allocator (AMD) | GPU memory for the Vulkan renderer | MIT |
| SDL3 | Window and input (the sdl3 backend): the system's on Linux and FreeBSD, built with the runtime on Windows | zlib |

This project builds on the work of the Switch emulation and homebrew community, whose projects showed how the
console and its operating system behave:

- yuzu, Ryujinx, Ryubing (Ryujinx's continuation), Citron and Eden, the Switch emulators
- Atmosphère, the reimplementation of Horizon OS
- LibHac, the library for the Switch's file systems and content formats

Wimsy, the mascot in `wimsy.svg`, was drawn by Madeline_Dev, © 2025 Eden Emulator Project, under the GPL 3.
