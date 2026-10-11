# SwitchRecomp (Linux fork)

A Linux-first fork of SwitchRecomp by MZ Labs, a static recompiler for Switch games: ARM64 code is translated to C
and compiled into `<Game>_code.so`, and the runtime in `runtime/` emulates the OS (kernel, IPC, services) and the GPU
(Maxwell command buffers and shaders, drawn with OpenGL). Catherine: Full Body (Asian release, 0100BC4010896000) is
the game every change is checked against. `README.md` has setup and commands, `ROADMAP.md` the plan and current
state. This file holds the rules sessions get wrong and points to the skills that carry the procedures.

## Rules

- **Nothing of the game goes into git or anywhere outside this machine.** Keys, dumps (XCI, NSP, NCA), `work/`,
  `games/` (code, data, saves, logs) and shader or texture dumps stay local. Commit messages and docs may quote
  addresses, symbol names and short disassembly, never extracted files. `.claude/hooks/guard.py` blocks the
  obvious ways.
- **A build is not a test.** A runtime change is verified by running the game (`swr-run` skill) through the scene
  it affects, and the commit says what was run. A change meant to fix a crash is run enough times to show the crash
  is gone: the title-screen crash in ROADMAP.md Phase 2 is intermittent.
- **Find the mechanism.** A fix explains why the game does what it does: what the game or SDK code expects
  (`tools/disasm.py`), and what the runtime gave it instead. No timing tweaks, retries or special cases for a
  title ID. Syncpoints above 127 "worked" for a whole session while overwriting the game's heap.
- **Upstream code is ours to change, carefully.** `main` is MZ Labs' code plus our commits. Keep changes small and
  in the file's style, with a comment saying why. The `upstream` branch is only ever written by
  `scripts/import-upstream.sh`.
- **Licensing.** The project is Apache 2.0 and every copy keeps NOTICE. Code under MIT, BSD, ISC, zlib or
  Apache 2.0 can be ported with its notice (Ryujinx is MIT). GPL code (yuzu, suyu, sudachi, Citron, Eden) may be
  read to understand behavior but never copied or closely paraphrased. Switchbrew and the SDK's own behavior, read
  from the disassembly, are the primary references.
- **Formatting:** `scripts/format.sh` before committing (clang-format 23, ruff). It only changes whitespace, and
  the whole history is formatted, so an unformatted file shows up as noise in every diff. Third-party code
  (`third_party/bcn`, `third_party/`) is never formatted.
- Ask only real decisions. When the next step is obvious, take it.

## Environment

- The Bash tool runs **zsh**: `$t:s` is a zsh modifier, so write `"${t}:s"`. Scripts in the repo use bash shebangs.
- Temporary files go in the session scratchpad, never in the repo root. Long runs use `run_in_background`.
- Never `pkill`/`killall` (they match the shell running them): `pgrep -a Catherine`, then `kill PID`.
- Don't build while a timing-sensitive test runs: the build deploys a new runtime into `games/` and loads the CPU.
- No `sudo` and no system packages: the user installs those. `uvx` fetches ruff on its own.
- Context is scarce. Never print a whole log (`games/Catherine.log` grows to megabytes), a whole disassembly or a
  ninja log. Use `grep -n`, `tail`, `tools/disasm.py` on one function.

## Build and run

| Task | Command |
| --- | --- |
| Third-party libraries and hactool (once, and after a submodule changes) | `scripts/bootstrap.py` (Windows: `scripts\bootstrap.ps1`, then `. scripts\env.ps1` per window) |
| Build the runtime (deploys to every game in `games/`) | `cmake --preset gcc && cmake --build --preset gcc` (or `clang`) |
| Extract a dump | `scripts/extract.py "Game.xci" --name Game [--update u.nsp]` |
| Recompile a game | `scripts/recomp.py work/Game Game` |
| Run with scripted input and a screenshot | `cd games && ./Catherine --seconds=60 --shot=/path/out --script=ms:hexbuttons,...` |
| Name addresses / disassemble game code | `tools/symbols.py work/Catherine ADDR...` / `tools/disasm.py work/Catherine FUNC` |
| Format / check | `scripts/format.sh` / `scripts/format.sh --check` |

The runtime finds its game module by its own file name, so each game has a copy of the runtime named after it, made
by the build's deploy step. Game code (`_code.so`) only needs recompiling when `switchrecomp/codegen.py`, `a64.py`, `abi/cpu.h` or
`abi/cpu_helpers.c` change (the ABI between generated code and runtime).

## Where things are

| Area | Files |
| --- | --- |
| Recompiler (`switchrecomp/`, run as `python3 -m switchrecomp`) | `cli.py` (command line), `nxloader.py` (NSO/NRO, symbols), `codegen.py` + `a64.py` (ARM64 to C), `module.py` (the module's C files), `toolchain.py` (compiling the module, the runtime through CMake), `data.py` |
| Generated-code ABI | `runtime/abi/`: `cpu.h` (with `SWR_ABI`), `cpu_helpers.c` |
| Startup | `runtime/core/`: `main.c` (startup, loading the game module), `options.c` (the .ini), `clock.c` (guest time), `rtapi.c` (what the game module calls), `crash.c`, `present.c` (vi's CPU-side present), `loader.c`, `entry.c` |
| Kernel and memory | `runtime/kernel/`: `kernel.c` (threads, sync, SVCs), `mem.c`, `ipc.c` (helpers `in32`, `out32`, `buf_in`, `buf_out`, `out_obj`...) |
| Services | `runtime/hle/`: one file per service family (`sm.c`, `am.c`, `set.c`, `time.c`, `acc.c`, `audio.c`, `misc.c` for the small ones, `services.c` registers them), `hid.c`, `fs.c`, `vi.c`, `nvdrv.c` (nvhost devices, syncpoints) |
| GPU | `runtime/gpu/`: `gpu.c` (GPFIFO), `engines.c`, `maxwell3d.c`, `renderer.h` (the `Renderer` table every renderer fills in, `g_renderer`), `renderer.c` (the `renderer=` setting), `shader/` (Maxwell to GLSL: `shader.c` walks a program, `decode.c` the instruction table, `translate.c` each instruction, `corpus.c`), `texture/astc.c`, `null/` (headless); `third_party/bcn/` |
| OpenGL renderer | `runtime/gpu/gl/`: `renderer.c` (the GPU thread, presenting, `gl_renderer`), `gl.h` (what its files share), `formats.c`, `texture.c` (texture cache, samplers), `programs.c` (shaders, programs, the shader pack), `framebuffer.c` (render targets, blits), `draw.c` (state, draws), `compute.c`, `trace.c` (F5), `capture.c` (F7 recorder, screenshots), `framegen.c` (LSFG), `glq*.c` (the GL thread), `load.c`, `lsfg*.c`, `overlay.c` |
| Vulkan renderer | `runtime/gpu/vk/`: `vk.h` (lists the parts), `device.c` (instance, device, queues, swapchain, VMA), `cmd.c` (frames in flight, staging), `surface.c` and `format.c` (render targets, formats), `texture.c` (textures, samplers), `buffer.c` (guest memory as buffers), `pipeline.c` (shader modules, pipelines), `render.c` (rendering passes, clears, draws), `compute.c` (compute on the compute queue, results landing in guest memory), `renderer.c` (its side of the renderer interface, presenting), `spirv.c` (GLSL to SPIR-V with glslang, SPIR-V cache); `third_party/Vulkan-Headers`, `third_party/volk`, `third_party/glslang`, `third_party/VulkanMemoryAllocator` |
| Shader corpus | `SWR_SHADER_CORPUS=dir` records each translated shader; `build/<dir>/swr-shaderc dir --validate` translates it for Vulkan, compiles and validates it (`--gl` for GL output, to diff two builds) |
| Video | `runtime/video/host1x.c` (NVDEC with OpenH264, VIC) |
| Platform | `runtime/platform/`: `plat.c` (threads, memory, files per OS), `backend/` (sdl3 is the Linux default, x11 the fallback, win32 Windows), `recorder_win32.c` |

Each folder under `runtime/` is one CMake object library (`runtime/CMakeLists.txt`), and includes are written from `runtime/`
(`#include "gpu/vk/vk.h"`). A function or global that another file uses is declared once, in the header next to the file
that defines it (`kernel/kernel.c` and `kernel/kernel.h`), and everything else is static. The build enforces it:
`-Werror=missing-prototypes`, `missing-variable-declarations` and, with GCC, `nested-externs`.

## Skills and agents

Skills in `.claude/skills/`:
- `swr-build`: building the runtime and games, the deploy step, the two compilers, and what fails silently.
- `swr-run`: running the game unattended: scripted input, screenshots, logs, repeat runs for intermittent bugs.
- `swr-crash`: from a crash report or `svcBreak` to the mechanism, with the symbol and disassembly tools.
- `swr-service`: implementing or fixing an HLE service against what the game's SDK code expects.
- `import-upstream`: bringing a new MZ Labs release into `main`.
- `record-lesson`: where a lesson goes so the next session finds it.
- `code-audit`: the multi-pass audit-and-fix loop, with the `swr-*-reviewer` agents in `.claude/agents/review/`.

Agents in `.claude/agents/`, to keep disassembly and logs out of the main context:
- `crash-triage` (read-only): symbolizes and disassembles a crash, and reports the mechanism with its evidence.
- `sdk-scout` (read-only): works out what a game or SDK function does and expects from the runtime.

## Git

- `origin` is `git@github.com:fuddlesworth/SwitchRecomp.git` (public). Push only when asked. Never force-push a
  pushed branch.
- Commit subjects are a plain imperative sentence (no prefixes). The body says why, and how it was verified. Check
  every claim in the message against the staged diff.
- Stage files by name, and check `git diff --cached --stat` right before committing: a commit takes everything
  staged, including a `git mv` or `git rm` from earlier in the session. Commits end with the `Co-Authored-By` line
  the session's instructions give.

## User-facing text

README, ROADMAP, commit messages, and messages the runtime prints for players read as plain prose: no em-dash
splicing clauses, no semicolons joining sentences, no spaced hyphen as a dash, no rule-of-three flourishes. Code
comments and developer docs under `.claude/` are out of scope.

## Lessons that cost a session

- GL calls go through the glq thread (`gpu/gl/glq_gen.c`). A function missing there is called without a context and
  crashes in libGLdispatch.
- NVIDIA's stream library sizes its per-syncpoint tables at 128 (`NvRmChannelNumSyncPoints` is a stub that fails).
  Every syncpoint comes from `syncpt_alloc()` in `hle/nvdrv.c`.
- The SDK asserts (`svcBreak`) when a service answers in a way real hardware never does, like an npad reporting two
  styles. Look for the assert's condition in the caller before changing anything else.
- Services that "do nothing" can still stall a game: CRI's movie player waits for released audio buffers.
- NVN dual vertex programs: VertexA (slot 0) writes the position, VertexB the varyings. Missing A means a black
  screen with every draw call succeeding.
- The recompiler's old runtime build dropped OpenH264 silently when the library was missing, and movies stayed black.
  The runtime is only built by CMake now, which makes it an error.
- `git submodule add` records the default branch's tip. Checking out a tag inside the submodule changes nothing until
  `git add third_party/NAME`, and `scripts/bootstrap.py` (`git submodule update`) puts the submodule back on the
  recorded commit. Stage the tag first, then check `git ls-files -s third_party/NAME` against
  `git -C third_party/NAME rev-parse TAG^{commit}`. Three pins were wrong for a week of commits this way.
