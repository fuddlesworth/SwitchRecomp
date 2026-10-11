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
| Movies stay black, the log says no H.264 decoder | `switchrecomp.py`'s own runtime build drops OpenH264 without a word when `deps/linux/lib/libdecoder.a` is missing | Build the runtime with CMake (`scripts/recomp.py` does). It refuses to configure without the library. |
| A fix has no effect | The game ran an old runtime: the build didn't deploy (no `_code.so` in `games/` yet), or the game was already running | Check the deploy lines. A running game keeps its old copy (the deploy renames over it). |
| The two presets disagree on what is deployed | Both deploy to the same `games/` | The last build wins. Rebuild the one you mean to test. |
| A test binary next to the game was overwritten | The deploy targets every `*_code.so` in `games/`, symlinks included | Keep A/B copies outside `games/`, or don't build while they exist. |
| No warnings | Upstream builds with `-w` | Check a changed file with warnings on: `gcc -std=gnu23 -O2 -Wall -Wextra -fsyntax-only -Iruntime -Iruntime/bcn -Ideps/linux/include runtime/FILE.c`. Fix what comes from the change. |
| Game crashes at startup with "recompiled code is from another SwitchRecomp version" | `ABI` in `switchrecomp.py` and the runtime's check disagree | Recompile the game. |

## The Vulkan renderer

The sdl3 backend builds both renderers into one runtime (`SWR_VULKAN`, on by default; x11, x11es, win32 and
headless have only OpenGL). `renderer=auto|vulkan|opengl` in the game's .ini, or `--renderer=opengl` on the command
line (`run_game.py Catherine -- --renderer=opengl`), picks one at startup, and the log says which ran
(`renderer: ...`). auto means Vulkan, falling back to OpenGL when Vulkan can't start (no driver, no Vulkan 1.3) or
when LSFG frame generation is set up. `renderer.c` holds the interface names and calls the active renderer:
`gpu_gl.c`'s side gets `glr_` names from `gl_names.h` (force-included by CMake, so upstream's file is unchanged),
`gpu_vk.c`'s side is `vkr_`.

The build also makes `swr-shaderc`. A translator change is checked on a shader corpus before any rendering: record one
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
mingw-cross`. Upstream code that only Windows compiles (`backend_win32.c`, `rec_win32.c`, the `_WIN32` parts of
`plat.c`) has relied on implicit declarations that `-w` hid and that GCC 14+ and Clang reject: a missing header shows
up only there.

The SDL3 backend, with both renderers, builds for Windows too: `cmake --preset mingw-cross -B build/mingw-sdl3
-DSWR_BACKEND=sdl3 -DSWR_DEPLOY=OFF` (the bootstrap builds a static SDL3 into `deps/win64/sdl3`).

To run a Windows build, use Wine with a game module built for Windows, kept out of `games/`:
- `switchrecomp.py work/Catherine -o Catherine --outdir build/wingame --target win64 --cc x86_64-w64-mingw32-gcc
  --runtime build/mingw-sdl3/swr-runtime.exe`. Make `build/wingame/Catherine_data/romfs` a symlink to the Linux copy
  first, or it copies 13 GB.
- Use a prefix of its own, made without the Mono and Gecko installers, which otherwise wait for a click with nothing
  on screen: `WINEPREFIX=$PWD/build/wineprefix WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all wineboot -i`, and
  the same variables for `wine Catherine.exe --seconds=20 --shot=shot`. The log is `Catherine.log` next to it.
- What works under Wine 11.19 here: the win32 backend (OpenGL) and the SDL3 backend with Vulkan. SDL3 can't open an
  OpenGL window under that Wine at all ("No matching GL pixel format available", even in a ten-line SDL program), so
  the SDL3 backend with `renderer=opengl` is only checked by building it.

## When the game module must be recompiled

Only when the generated code or its contract changed: `codegen.py`, `a64.py`, `nxloader.py`, `runtime/cpu.h`,
`runtime/cpu_helpers.c`. Everything else in `runtime/` is the runtime alone. A recompile of Catherine takes several
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
