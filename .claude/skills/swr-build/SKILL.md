---
name: swr-build
description: "Build the SwitchRecomp runtime and recompile games correctly, avoiding the setups that build \"successfully\" into something that doesn't work. Covers the bootstrap, the gcc and clang presets, the deploy into games/, when a game needs recompiling, and warnings hidden by -w. Use when building, rebuilding, recompiling a game, after pulling or changing a submodule, or when a build result looks suspicious."
---

# Build

## Order

1. `scripts/bootstrap.py`: once, and again whenever `third_party/` moved (it compares each submodule's commit and
   the compiler with a stamp in `deps/`). Without it CMake stops with the missing file and the flag that would skip
   it. On Windows, `scripts\bootstrap.ps1` downloads the toolchain into `deps\tools` first and then runs it;
   `. scripts\env.ps1` puts that toolchain on PATH in a new PowerShell window.
2. `cmake --preset gcc && cmake --build --preset gcc`. The clang preset builds the same into `build/clang`.
3. The build's last step copies the runtime to `games/<Game>` for every `games/<Game>_code.so`. That is the binary
   that runs. The build prints `Runtime deployed to ...` for each: no line means no game got the new runtime.

## Silent failures

| What happens | Why | What to do |
| --- | --- | --- |
| Movies stay black, the log says no H.264 decoder | A runtime built some other way than CMake (upstream's recompiler had its own build) drops OpenH264 without a word when `deps/linux/lib/libdecoder.a` is missing | Build the runtime with CMake (`scripts/recomp.py` does). It refuses to configure without the library. |
| A fix has no effect | The game ran an old runtime: the build didn't deploy (no `_code.so` in `games/` yet), or the game was already running | Check the deploy lines. A running game keeps its old copy (the deploy renames over it). |
| The two presets disagree on what is deployed | Both deploy to the same `games/` | The last build wins. Rebuild the one you mean to test. |
| A test binary next to the game was overwritten | The deploy targets every `*_code.so` in `games/`, symlinks included | Keep A/B copies outside `games/`, or don't build while they exist. |
| No warnings | Upstream builds with `-w` | Check a changed file with warnings on: `gcc -std=gnu23 -O2 -Wall -Wextra -fsyntax-only -Iruntime -Ithird_party/bcn -Ideps/linux/include runtime/FILE.c`. Fix what comes from the change. |
| Game crashes at startup with "recompiled code is from another SwitchRecomp version" | The game module was built against another `SWR_ABI` (`runtime/abi/cpu.h`) | Recompile the game. |

## The Vulkan renderer

The sdl3 backend builds both renderers into one runtime (`SWR_VULKAN`, on by default; x11, x11es, win32 and
headless have only OpenGL). `renderer=auto|vulkan|opengl` in the game's .ini, or `--renderer=opengl` on the command
line (`run_game.py Catherine -- --renderer=opengl`), picks one at startup, and the log says which ran
(`renderer: ...`). auto means Vulkan, falling back to OpenGL when Vulkan can't start (no driver, no Vulkan 1.3) or
when LSFG frame generation is set up. The interface is the `Renderer` table in `gpu/renderer.h`: the OpenGL, Vulkan
and null renderers each fill in every operation (`gl_renderer`, `vk_renderer`, `null_renderer`), the backend points
`g_renderer` at the one it started, and everything else calls `g_renderer->draw(...)` and so on.

The build also makes `swr-shaderc`. `swr-shaderc --nvn DIR --cache games/shader_cache/spirv` pre-translates a game's
NVN shader files into the SPIR-V cache (`scripts/recomp.py` runs it on any romfs folder holding them); to measure what
it covers, record a corpus of a run and replay it with `--cache` against a copy of the pre-filled folder, counting the
files it adds. A translator change is checked on a shader corpus before any rendering: record one
with `SWR_SHADER_CORPUS=dir` (any renderer), then `build/gcc/swr-shaderc dir --validate` for Vulkan, and `--gl --out` from
two builds, diffed, to show the GL output didn't change. `SWR_VK_VALIDATION=1`
turns on the Khronos validation layer when it is installed (Arch: `vulkan-validation-layers`, which the user
installs). A screenshot (`--shot`, or run_game.py's) reads back the frame Vulkan presented, so the hash in the log
compares directly with a GL run at the same frame: Catherine's Atlus logo (`--seconds 4 --script ""`) is
pixel-identical. Compare later screens by eye, since the two renderers reach them at slightly different frames. The
first run after a translator change compiles every shader to SPIR-V and starts slower, which can make the New Game
script's timed presses miss; run again with the cache warm before suspecting the renderer.

## Windows

CI (`.github/workflows/build.yml`) builds Windows natively with GCC and Clang from `scripts/bootstrap.ps1`, and
cross-builds it on Linux. To check Windows code before pushing, cross-build it here: with a MinGW-w64 toolchain on
PATH (Arch's `mingw-w64-gcc`, or the Linux build of llvm-mingw, which provides the `x86_64-w64-mingw32-*` names),
run `python3 scripts/bootstrap.py --target win64`, then `cmake --preset mingw-cross && cmake --build --preset
mingw-cross`. Code that only Windows compiles (`platform/backend/win32.c`, `platform/recorder_win32.c`, the `_WIN32` parts of
`platform/plat.c`) is checked only there: a missing header, or a global that should be static, shows up only in that build.

The runtime builds with the compilers' default warnings plus three errors: `-Werror=missing-prototypes`,
`-Werror=missing-variable-declarations` and (GCC) `-Werror=nested-externs`. A function or global another file uses
goes in the header next to the file that defines it, which that file includes too, so the compiler checks the two
agree. Anything else is static. An `extern` inside a function, or a prototype copied into another `.c` file, is the
pattern this replaced: upstream had 237 of them, and some disagreed with the definition.

The SDL3 backend, with both renderers, builds for Windows too: `cmake --preset mingw-cross -B build/mingw-sdl3
-DSWR_BACKEND=sdl3 -DSWR_DEPLOY=OFF` (the bootstrap builds a static SDL3 into `deps/win64/sdl3`).

To run a Windows build, use Wine with a game module built for Windows, kept out of `games/`:
- `python3 -m switchrecomp work/Catherine -o Catherine --outdir build/wingame --target win64 --cc x86_64-w64-mingw32-gcc
  --runtime build/mingw-sdl3/swr-runtime.exe`. Make `build/wingame/Catherine_data/romfs` a symlink to the Linux copy
  first, or it copies 13 GB.
- Use a prefix of its own, made without the Mono and Gecko installers, which otherwise wait for a click with nothing
  on screen: `WINEPREFIX=$PWD/build/wineprefix WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all wineboot -i`, and
  the same variables for `wine Catherine.exe --seconds=20 --shot=shot`. The log is `Catherine.log` next to it.
- What works under Wine 11.19 here: the win32 backend (OpenGL) and the SDL3 backend with Vulkan. SDL3 can't open an
  OpenGL window under that Wine at all ("No matching GL pixel format available", even in a ten-line SDL program), so
  the SDL3 backend with `renderer=opengl` is only checked by building it.

## When the game module must be recompiled

Only when the generated code or its contract changed: `switchrecomp/` (codegen, a64, nxloader), `runtime/abi/cpu.h`,
`runtime/abi/cpu_helpers.c`. Everything else in `runtime/` is the runtime alone. A recompile of Catherine takes several
minutes with GCC; the objects in `games/<Game>_build/` are reused when their C didn't change.

```bash
scripts/recomp.py work/Catherine Catherine                  # runtime and game module with GCC
scripts/recomp.py work/Catherine Catherine --preset clang   # both with Clang
```

## Adding or moving a third-party submodule

`git submodule add URL third_party/NAME`, `git -C third_party/NAME checkout TAG`, then `git add third_party/NAME`
before anything runs `git submodule update` (the bootstrap does). Check the pin:
`git ls-files -s third_party/NAME` must show `git -C third_party/NAME rev-parse TAG^{commit}`. Then a build step in
`scripts/bootstrap.py`, the license in README.md's table, and the tag in the commit message.

## Formatting

`scripts/format.sh` before committing, `--check` to see what is off. It needs clang-format 23 exactly (another
version formats differently and would reformat every file) and fetches ruff through `uvx`.

## Never delegate builds to subagents

Reviewer and investigator agents read and grep. The main session builds and runs, once, serially: concurrent builds
fight over `build/` and the deploy, and a build during a test run changes its timing.
