---
name: swr-build
description: "Build the SwitchRecomp runtime and recompile games correctly, avoiding the setups that build \"successfully\" into something that doesn't work. Covers the bootstrap, the gcc and clang presets, the deploy into games/, when a game needs recompiling, and warnings hidden by -w. Use when building, rebuilding, recompiling a game, after pulling or changing a submodule, or when a build result looks suspicious."
---

# Build

## Order

1. `scripts/bootstrap.sh`: once, and again whenever `third_party/` moved (it compares each submodule's commit with
   a stamp in `deps/`). Without it CMake stops with the missing file and the flag that would skip it.
2. `cmake --preset gcc && cmake --build --preset gcc`. The clang preset builds the same into `build/clang`.
3. The build's last step copies the runtime to `games/<Game>` for every `games/<Game>_code.so`. That is the binary
   that runs. The build prints `Runtime deployed to ...` for each: no line means no game got the new runtime.

## Silent failures

| What happens | Why | What to do |
| --- | --- | --- |
| Movies stay black, the log says no H.264 decoder | `switchrecomp.py`'s own runtime build drops OpenH264 without a word when `deps/linux/lib/libdecoder.a` is missing | Build the runtime with CMake (`scripts/recomp.sh` does). It refuses to configure without the library. |
| A fix has no effect | The game ran an old runtime: the build didn't deploy (no `_code.so` in `games/` yet), or the game was already running | Check the deploy lines. A running game keeps its old copy (the deploy renames over it). |
| The two presets disagree on what is deployed | Both deploy to the same `games/` | The last build wins. Rebuild the one you mean to test. |
| A test binary next to the game was overwritten | The deploy targets every `*_code.so` in `games/`, symlinks included | Keep A/B copies outside `games/`, or don't build while they exist. |
| No warnings | Upstream builds with `-w` | Check a changed file with warnings on: `gcc -std=gnu23 -O2 -Wall -Wextra -fsyntax-only -Iruntime -Iruntime/bcn -Ideps/linux/include runtime/FILE.c`. Fix what comes from the change. |
| Game crashes at startup with "recompiled code is from another SwitchRecomp version" | `ABI` in `switchrecomp.py` and the runtime's check disagree | Recompile the game. |

## When the game module must be recompiled

Only when the generated code or its contract changed: `codegen.py`, `a64.py`, `nxloader.py`, `runtime/cpu.h`,
`runtime/cpu_helpers.c`. Everything else in `runtime/` is the runtime alone. A recompile of Catherine takes several
minutes with GCC; the objects in `games/<Game>_build/` are reused when their C didn't change.

```bash
scripts/recomp.sh work/Catherine Catherine            # SWR_PRESET=clang for the clang runtime
scripts/recomp.sh work/Catherine Catherine --cc clang  # the game module with clang too
```

## Formatting

`scripts/format.sh` before committing, `--check` to see what is off. It needs clang-format 23 exactly (another
version formats differently and would reformat every file) and fetches ruff through `uvx`.

## Never delegate builds to subagents

Reviewer and investigator agents read and grep. The main session builds and runs, once, serially: concurrent builds
fight over `build/` and the deploy, and a build during a test run changes its timing.
