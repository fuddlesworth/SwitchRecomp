---
name: swr-build
description: "Build the SwitchRecomp runtime and recompile games correctly, avoiding the setups that build \"successfully\" into something that doesn't work. Covers the CPM.cmake dependencies, the Clang-only presets, the deploy into games/, when a game needs recompiling, and warnings hidden by -w. Use when building, rebuilding, recompiling a game, after pulling or bumping a dependency, or when a build result looks suspicious."
---

# Build

## Order

1. `cmake --preset default && cmake --build --preset default`, into `build/default`. The configure fetches the
   third-party packages (`cmake/dependencies.cmake`) into `.cache/cpm` the first time, so a fresh clone needs the
   network once. They build in the same tree as the runtime: OpenH264 and hactool from our own CMake scripts
   (`cmake/openh264.cmake`, `cmake/hactool.cmake`, since theirs are Makefiles), the others through their CMake.
   Clang only: any other compiler stops the configure. On Windows, `scripts\bootstrap.ps1` downloads the toolchain
   (llvm-mingw, CMake, Ninja, NASM, Python) into `deps\tools` once, and `. scripts\env.ps1` puts it on PATH in a new
   PowerShell window.
2. `build/default/hactool` is what `scripts/extract.py` runs (it builds that target when it's missing).
3. The build's last step copies the runtime to `games/<Game>` for every `games/<Game>_code.so`. That is the binary
   that runs. The build prints `Runtime deployed to ...` for each: no line means no game got the new runtime.

## Silent failures

| What happens | Why | What to do |
| --- | --- | --- |
| Movies stay black, the log says no H.264 decoder | A runtime built some other way than CMake (upstream's recompiler had its own build) drops OpenH264 without a word | Build the runtime with CMake (`scripts/recomp.py` does), which always builds OpenH264 in unless `-DSWR_H264=OFF`. |
| Movies decode slowly, the configure warned about NASM | OpenH264 falls back to its C code without NASM | Install `nasm` and configure a new build folder. |
| A fix has no effect | The game ran an old runtime: the build didn't deploy (no `_code.so` in `games/` yet), or the game was already running | Check the deploy lines. A running game keeps its old copy (the deploy renames over it). |
| Two build folders disagree on what is deployed | Every build folder with `SWR_DEPLOY` on deploys to the same `games/` | The last build wins. Configure extra folders with `-DSWR_DEPLOY=OFF`. |
| A test binary next to the game was overwritten | The deploy targets every `*_code.so` in `games/`, symlinks included | Keep A/B copies outside `games/`, or don't build while they exist. |
| No warnings | Upstream builds with `-w` | Check a changed file with warnings on: take the file's line from `build/default/compile_commands.json` and run it with `-Wall -Wextra -fsyntax-only`. Fix what comes from the change. |
| Game crashes at startup with "recompiled code is from another SwitchRecomp version" | The game module was built against another `SWR_ABI` (`runtime/abi/cpu.h`) | Recompile the game. |

## The Vulkan renderer

The sdl3 backend, the only window backend on Linux and Windows since the x11, x11es and win32 ones were removed,
builds both renderers into one runtime (`SWR_VULKAN`, on by default; headless has the null renderer). `renderer=auto|vulkan|opengl` in the game's .ini, or `--renderer=opengl` on the command
line (`run_game.py Catherine -- --renderer=opengl`), picks one at startup, and the log says which ran
(`renderer: ...`). auto means Vulkan, falling back to OpenGL when Vulkan can't start (no driver, no Vulkan 1.3) or
when LSFG frame generation is set up. The interface is the `Renderer` table in `gpu/renderer.h`: the OpenGL, Vulkan
and null renderers each fill in every operation (`gl_renderer`, `vk_renderer`, `null_renderer`), the backend points
`g_renderer` at the one it started, and everything else calls `g_renderer->draw(...)` and so on.

The build also makes `swr-shaderc`. `swr-shaderc --nvn DIR --cache games/shader_cache/spirv` pre-translates a game's
NVN shader files into the SPIR-V cache (`scripts/recomp.py` runs it on any romfs folder holding them); to measure what
it covers, record a corpus of a run and replay it with `--cache` against a copy of the pre-filled folder, counting the
files it adds. A translator change is checked on a shader corpus before any rendering: record one
with `SWR_SHADER_CORPUS=dir` (any renderer), then `build/default/swr-shaderc dir --validate` for Vulkan, and `--gl --out` from
two builds, diffed, to show the GL output didn't change. `SWR_VK_VALIDATION=1`
turns on the Khronos validation layer when it is installed (Arch: `vulkan-validation-layers`, which the user
installs). A screenshot (`--shot`, or run_game.py's) reads back the frame Vulkan presented, so the hash in the log
compares directly with a GL run at the same frame: Catherine's Atlus logo (`--seconds 4 --script ""`) is
pixel-identical. Compare later screens by eye, since the two renderers reach them at slightly different frames. The
first run after a translator change compiles every shader to SPIR-V and starts slower, which can make the New Game
script's timed presses miss; run again with the cache warm before suspecting the renderer.

## Windows

CI (`.github/workflows/build.yml`) builds Windows natively from `scripts/bootstrap.ps1`, and cross-builds it on
Linux. To check Windows code before pushing, cross-build it here: with the Linux build of llvm-mingw on PATH (its
release `llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64.tar.xz`, which provides the `x86_64-w64-mingw32-*` names), run
`cmake --preset windows-cross && cmake --build --preset windows-cross`. Code that only Windows compiles (`platform/backend/win32.c`, `platform/recorder_win32.c`, the `_WIN32` parts of
`platform/plat.c`) is checked only there: a missing header, or a global that should be static, shows up only in that build.

The runtime builds with Clang's default warnings plus two errors: `-Werror=missing-prototypes` and
`-Werror=missing-variable-declarations`. A function or global another file uses
goes in the header next to the file that defines it, which that file includes too, so the compiler checks the two
agree. Anything else is static. An `extern` inside a function, or a prototype copied into another `.c` file, is the
pattern this replaced: upstream had 237 of them, and some disagreed with the definition.

The Windows runtime is the SDL3 backend with both renderers: `cmake --preset windows-cross && cmake --build --preset
windows-cross` (SDL3 is then a CPM package, linked statically).

To run a Windows build, use Wine with a game module built for Windows, kept out of `games/`:
- `python3 -m switchrecomp work/Catherine -o Catherine --outdir build/wingame --target win64 --cc x86_64-w64-mingw32-clang
  --runtime build/windows-cross/swr-runtime.exe`. Make `build/wingame/Catherine_data/romfs` a symlink to the Linux copy
  first, or it copies 13 GB.
- Use a prefix of its own, made without the Mono and Gecko installers, which otherwise wait for a click with nothing
  on screen: `WINEPREFIX=$PWD/build/wineprefix WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all wineboot -i`, and
  the same variables for `wine Catherine.exe --seconds=20 --shot=shot`. The log is `Catherine.log` next to it.
- What works under Wine 11.19 here: the SDL3 backend with Vulkan. SDL3 can't open an OpenGL window under that Wine at
  all ("No matching GL pixel format available", even in a ten-line SDL program), so `renderer=opengl` on Windows is
  only checked by building it.

## When the game module must be recompiled

Only when the generated code or its contract changed: `switchrecomp/` (codegen, a64, nxloader), `runtime/abi/cpu.h`,
`runtime/abi/cpu_helpers.c`. Everything else in `runtime/` is the runtime alone. A recompile of Catherine takes several
minutes; the objects in `games/<Game>_build/` are reused when their C didn't change (they are keyed on the compiler
and flags, so the first Clang build after a GCC one compiles everything).

```bash
scripts/recomp.py work/Catherine Catherine                  # runtime and game module, both with Clang
```

## Adding or bumping a third-party package

A `CPMAddPackage` in `cmake/dependencies.cmake` with the release archive's URL (a tag, or a commit for projects
without one) and its `URL_HASH SHA256=`. Get the hash from the release asset's digest (`gh api
repos/OWNER/REPO/releases/tags/TAG --jq '.assets[].digest'`) or by downloading the archive once, never by trusting
the next configure. Prefer `DOWNLOAD_ONLY` plus our own targets for projects without CMake (OpenH264, hactool) or
with a CMake too old for CMake 4, `EXCLUDE_FROM_ALL YES SYSTEM YES` for the rest, and `swr_quiet(target...)` on what it
builds. `OPTIONS` are cache variables of the whole build, not just that package. Then the license in README.md's
table, and the version in the commit message. CI caches `.cache/cpm` on the file's hash.

## Formatting

`scripts/format.sh` before committing, `--check` to see what is off. It needs clang-format 23 exactly (another
version formats differently and would reformat every file) and fetches ruff through `uvx`.

## Never delegate builds to subagents

Reviewer and investigator agents read and grep. The main session builds and runs, once, serially: concurrent builds
fight over `build/` and the deploy, and a build during a test run changes its timing.
