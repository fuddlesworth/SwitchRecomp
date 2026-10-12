---
name: snx-build
description: "Build the ScarletNX runtime and recompile games correctly, avoiding the setups that build \"successfully\" into something that doesn't work. Covers the CPM.cmake dependencies, the Clang-only presets, the deploy into games/, when a game needs recompiling, and the warnings a plain build doesn't show. Use when building, rebuilding, recompiling a game, after pulling or bumping a dependency, or when a build result looks suspicious."
---

# Build

## Order

1. `cmake --preset default && cmake --build --preset default`, into `build/default`. The configure fetches the
   third-party packages (`cmake/dependencies.cmake`) into `.cache/cpm` the first time, so a fresh clone needs the
   network once. They build in the same tree as the runtime: hactool from our own CMake script (`cmake/hactool.cmake`, since
   its own is a Makefile), FFmpeg with its own configure and make run by `cmake/ffmpeg.cmake` (logs in
   `build/default/ffmpeg_build-prefix/src/ffmpeg_build-stamp/`), the others through their CMake.
   Clang only: any other compiler stops the configure. With `SNX_H264` on (the default) the configure also stops when
   the FFmpeg source path (the checkout, or `CPM_SOURCE_CACHE`) has a space in it, since FFmpeg's configure can't
   build from one, and on x86-64 when NASM is missing (below). On Windows, `scripts\bootstrap.ps1` downloads the
   toolchain (llvm-mingw, CMake, Ninja, NASM, Python, and w64devkit for the sh and GNU make FFmpeg needs) into
   `deps\tools` once, and `. scripts\env.ps1` puts it on PATH in a new PowerShell window. Windows' default execution
   policy runs no scripts, so run `Set-ExecutionPolicy -Scope Process Bypass` in that window first.
2. `build/default/hactool` is what `scripts/extract.py` runs (it builds that target when it's missing).
3. The build's last step copies the runtime to `games/<Game>` for every `games/<Game>_code.so`. That is the binary
   that runs. The build prints `Runtime deployed to ...` for each: no line means no game got the new runtime.

## Silent failures

| What happens | Why | What to do |
| --- | --- | --- |
| Movies stay black, the log says no H.264 decoder | A runtime built some other way than CMake (upstream's recompiler had its own build) drops the H.264 decoder without a word | Build the runtime with CMake (`scripts/recomp.py` does), which always builds FFmpeg in unless `-DSNX_H264=OFF`. |
| The configure stops with "NASM was not found" | FFmpeg's x86-64 assembly needs NASM, and a build without it decodes movies several times slower, so it is never dropped silently | Install `nasm`. `-DSNX_FFMPEG_ASM=OFF` builds FFmpeg without its assembly on purpose (slower movies). |
| A fix has no effect | The game ran an old runtime: the build didn't deploy (no `_code.so` in `games/` yet), or the game was already running | Check the deploy lines. On Linux a running game keeps its old copy (the deploy renames over it). On Windows a running `.exe` can't be replaced, so stop the game before building. |
| Two build folders disagree on what is deployed | Every build folder with `SNX_DEPLOY` on deploys to the same `games/` | The last build wins. Configure extra folders with `-DSNX_DEPLOY=OFF`. The recompiler builds its runtime in `build/<preset>-recomp` with `SNX_DEPLOY=OFF` (`scarletnx/toolchain.py`), so the preset's own folder keeps deploying. |
| A test binary next to the game was overwritten | The deploy targets every `*_code.so` in `games/`, symlinks included | Keep A/B copies outside `games/`, or don't build while they exist. |
| A new warning goes unnoticed | The runtime builds with Clang's default warnings only (the third-party packages and the generated game module build with `-w`), and a warning doesn't stop the build | Check a changed file with more warnings on: take the file's line from `build/default/compile_commands.json` and run it with `-Wall -Wextra -fsyntax-only`. Fix what comes from the change. |
| Game crashes at startup with "recompiled code is from another ScarletNX version" | The game module was built against another `SNX_ABI` (`runtime/abi/cpu.h`) | Recompile the game. |

## The Vulkan renderer

The renderer is Vulkan, and the sdl3 backend is the only window backend on Linux and Windows (the OpenGL renderer
and the x11, x11es, win32 and android backends were removed on 2026-10-11). A GPU or driver without Vulkan 1.3 stops
the runtime at startup with a message. The headless backend has the null renderer. The interface is the `Renderer`
table in `gpu/renderer.h`, which `vk_renderer` and `null_renderer` fill in completely: everything else calls
`g_renderer->draw(...)` and so on.

The build also makes `snx-shaderc`. `snx-shaderc --nvn DIR --cache games/shader_cache/spirv` pre-translates a game's
NVN shader files into the SPIR-V cache (`scripts/recomp.py` runs it on any romfs folder holding them); to measure what
it covers, record a corpus of a run and replay it with `--cache` against a copy of the pre-filled folder, counting the
files it adds. A translator change is checked on a shader corpus before any rendering: record one
with `SNX_SHADER_CORPUS=dir`, then `build/default/snx-shaderc dir --validate` (`spirv-val` on PATH), and `--out` from
two builds, diffed, to show what the change did to the GLSL and SPIR-V. A corpus is the game's own shaders: keep it
under `games/` or in the scratchpad, never in a tracked folder of the repo. `SNX_VK_VALIDATION=1`
turns on the Khronos validation layer when it is installed (Arch: `vulkan-validation-layers`, which the user
installs). A screenshot (`--shot`, or run_game.py's) reads back the frame Vulkan presented, and its hash is in the
log: compare a change against the build before it at the same frame (Catherine's Atlus logo, `--seconds 4 --script
""`), on a quiet machine, since the logo animates per frame. The first run after a translator change compiles every shader to SPIR-V and starts slower, which can make the New Game
script's timed presses miss; run again with the cache warm before suspecting the renderer.

## Windows

CI (`.github/workflows/build.yml`) builds Windows natively from `scripts/bootstrap.ps1`, and cross-builds it on
Linux. To check Windows code before pushing, cross-build it here: with the Linux build of llvm-mingw on PATH (its
release `llvm-mingw-<date>-ucrt-ubuntu-22.04-x86_64.tar.xz`, which provides the `x86_64-w64-mingw32-*` names), run
`cmake --preset windows-cross && cmake --build --preset windows-cross`. Code that only Windows compiles
(`platform/plat_win32.c`, `platform/profiler_win32.c`, the `_WIN32` parts of the SDL3 backend) is checked only there:
a missing header, or a global that should be static, shows up only in that build.

The runtime builds with Clang's default warnings plus two errors: `-Werror=missing-prototypes` and
`-Werror=missing-variable-declarations`. A function or global another file uses
goes in the header next to the file that defines it, which that file includes too, so the compiler checks the two
agree. Anything else is static. An `extern` inside a function, or a prototype copied into another `.c` file, is the
pattern this replaced: upstream had 237 of them, and some disagreed with the definition.

The Windows runtime is the SDL3 backend with the Vulkan renderer, as on Linux: `cmake --preset windows-cross &&
cmake --build --preset windows-cross` (SDL3 is then a CPM package, linked statically).

To run a Windows build, use Wine with a game module built for Windows, kept out of `games/`:
- `python3 -m scarletnx work/Catherine -o Catherine --outdir build/wingame --target win64 --cc x86_64-w64-mingw32-clang
  --runtime build/windows-cross/scarletnx.exe`. Make `build/wingame/Catherine_data/romfs` a symlink to the Linux copy
  first, or it copies 13 GB.
- Use a prefix of its own, made without the Mono and Gecko installers, which otherwise wait for a click with nothing
  on screen: `WINEPREFIX=$PWD/build/wineprefix WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all wineboot -i`, and
  the same variables for `wine Catherine.exe --seconds=20 --shot=shot`. The log is `Catherine.log` next to it.
- What works under Wine 11.19 here: the SDL3 backend with Vulkan.

## When the game module must be recompiled

Only when the generated code or its contract changed: `scarletnx/` (`codegen.py`, `a64.py`, `nxloader.py`,
`module.py`, and the module's compile flags in `toolchain.py`), `runtime/abi/cpu.h`, `runtime/abi/cpu_helpers.c` and
the files it includes from `runtime/abi/helpers/`. Everything else in `runtime/` is the runtime alone. The runtime
only checks `SNX_ABI`: a module built before an ABI change that kept it still loads and runs the old code, and a
bump makes the runtime refuse every module built before it. A recompile of
Catherine takes several minutes. The objects in `games/<Game>_build/` are reused when their C didn't change (they are
keyed on the compiler and flags, so changing either compiles everything again).

```bash
scripts/recomp.py work/Catherine Catherine                  # runtime and game module, both with Clang
```

## Adding or bumping a third-party package

A `CPMAddPackage` in `cmake/dependencies.cmake` with the release archive's URL (a tag, or a commit for projects
without one) and its `URL_HASH SHA256=`. Get the hash from the release asset's digest (`gh api
repos/OWNER/REPO/releases/tags/TAG --jq '.assets[].digest'`) or by downloading the archive once, never by trusting
the next configure. Prefer `DOWNLOAD_ONLY` plus our own targets for projects without CMake (FFmpeg, hactool) or
with a CMake too old for CMake 4, `EXCLUDE_FROM_ALL YES` for the rest (not `SYSTEM`: a system copy of the headers
found through an `-I` in CFLAGS would win over `-isystem` ones; SDL3 on Windows is the one exception, since there is
no system copy to collide with), and `snx_quiet(target...)` on what it
builds. `OPTIONS` are cache variables of the whole build, not just that package. Then the license in README.md's
table and in the install (below), and the version in the commit message. CI caches `.cache/cpm` on the file's hash.

## The AppImage

CI's `appimage` job installs the build into `/usr` in pkgforge's Arch container and runs `scripts/make-appimage.sh`,
which bundles `scarletnx` with quick-sharun and checks that a copy named `Probe.AppImage` looks for
`Probe_code.so` next to itself. The `release` job replaces the `nightly` prerelease with it on every push to `main`
that passed every other job. Locally, quick-sharun works from the host's libraries (it wants patchelf on PATH) and
bundles the full Mesa, so the AppImage comes out several times bigger than CI's, which uses pkgforge's debloated
packages. Inside an AppImage argv[0] points into its mount: anything that finds files next to the runtime goes
through `g_exedir`, which `main.c` takes from `$APPIMAGE`.

`cmake --install` puts the license files in `share/licenses/scarletnx`: `LICENSE`, `LICENSE.apache` and `NOTICE`,
then a folder per library built into the runtime or hactool. The AppImage copies that folder and adds the licenses of
the Arch packages it bundles, with their list in `share/licenses/bundled-packages.txt`, and the CI artifacts are the
install tree. A new package means a new `install(...)` line for its license in `CMakeLists.txt`.

## Formatting

`scripts/format.sh` before committing, `--check` to see what is off. It covers the C and C++ under `runtime/` and
`tools/`, the Python up to two folders deep and `.claude/hooks/`. It needs clang-format 23 exactly (another version
formats differently and would reformat every file) and fetches ruff through `uvx`.

## Never delegate builds to subagents

Reviewer and investigator agents read and grep. The main session builds and runs, once, serially: concurrent builds
fight over `build/` and the deploy, and a build during a test run changes its timing.
