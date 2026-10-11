# Roadmap

This fork takes SwitchRecomp v142 (MZ Labs) and makes it a Linux-first project, with Catherine: Full Body
(Asian release, TID 0100BC4010896000) as the game everything is checked against. The phases are in the order they
should land. A phase can start before the previous one is finished where it says so.

Status: `[x]` done, `[~]` in progress, `[ ]` not started.

## Where things stand (2026-10-10)

Catherine boots through the logos, the intro movie, the terms screen, the main menu and the New Game cutscene at
about 58 to 60 FPS, and around 40 FPS in heavy 3D scenes. It took eight fixes to the v142 runtime to get there (see
the history of `main`). There is no sound, and a memory-corruption crash shows up in roughly one run in seven.

How the pieces fit:

- **CPU**: the game's ARM64 code is translated to C once (`switchrecomp.py`, `codegen.py`, `a64.py`) and built as
  `<Game>_code.so`. This part is a real recompilation.
- **OS**: kernel, IPC and services are high-level emulation in `runtime/` (`kernel.c`, `ipc.c`, `svc_*.c`,
  `nvdrv.c`).
- **GPU**: emulated at the hardware level. The game's own NVN driver is recompiled with the game and writes Maxwell
  command buffers. `gpu.c`, `gpu_engines.c` and `gpu_3d.c` decode them, `shader.c` translates Maxwell shader
  binaries to GLSL, and `gpu_gl.c` draws with OpenGL. The backend interface is small (see `gpu_nogl.c`, about 20
  functions), which is what makes a second backend realistic.
- **Video**: `host1x.c` emulates NVDEC and VIC, with OpenH264 doing the H.264 decode.

## Phase 0: repository and build

- [ ] Git history: an `upstream` branch holding each MZ Labs release as imported, and `main` with our work on top,
      one commit per fix. New upstream releases are imported with `scripts/import-upstream.sh` and merged.
- [ ] `.gitignore` that keeps game data, keys, dumps and build output out of git.
- [ ] CMake build for the Linux runtime, with OpenH264 and hactool as submodules built from source.
- [ ] Scripts for the whole pipeline: extract a dump with hactool, recompile, link the runtime into `games/`.
- [ ] Move the debugging tools written during bring-up (symbolizer, thread-dump reader, Maxwell scanner) into
      `tools/`.
- [ ] `README.md` for the Linux setup, crediting "SwitchRecomp by MZ Labs" as NOTICE.txt requires.

## Phase 1: Claude Code setup

- [ ] `CLAUDE.md` with the project's rules: what never gets committed, how upstream code is changed, how a change
      is verified, licensing of reference code.
- [ ] Skills for the procedures that sessions get wrong: building, running the game with scripted input and
      screenshots, triaging a crash report, importing an upstream release, recording a lesson.
- [ ] Domain review agents for the audit loop (kernel and services, GPU and shaders, audio and video, recompiler,
      build) and read-only investigators for crash triage and shader inspection.
- [ ] A hook that blocks committing keys or game files and editing generated output.

## Phase 2: sound

Catherine uses two audio paths, and both are silent today.

- [ ] **Audio output device.** SDL3 audio streams (SDL3 is already a dependency of Phase 4). The runtime only uses
      SDL's audio subsystem until the SDL3 window backend exists.
- [ ] **`audout` (movie audio).** Buffers are already released on a 48 kHz clock. Send their PCM to the device and
      let the device's consumption drive the release instead of the wall clock, so movies stay in sync.
- [ ] **`audren` (music and sound effects).** The runtime answers `RequestUpdate` in the right format but mixes
      nothing. Needed: memory pools, voices (PCM16 and DSP-ADPCM, with pitch and resampling), volume and mix
      routing through submixes to the final mix, biquad filters, then effects (delay, reverb, aux) and the device and
      circular-buffer sinks. Ryujinx's `Ryujinx.Audio` renderer (MIT) is the reference. yuzu's `audio_core` is GPLv3
      and can be read for behavior but never copied, because this project is Apache 2.0.
- [ ] Check `hwopus` and other audio services the game opens once `audren` produces sound.

## Phase 3: stability

This can run in parallel with Phase 2, and it should be finished before Phase 5 starts, since a renderer rewrite on
top of a memory-corruption bug makes every regression ambiguous.

- [ ] Smoke test: a scripted run from boot to the New Game cutscene that checks for crashes, FPS and screenshots,
      runnable unattended (Xvfb or a headless Wayland compositor).
- [ ] The intermittent crash. Worker threads read job records whose pointers have been overwritten with float data
      that looks like vertex positions (`x10=3f800000438e91de`, a position with w = 1.0). The leading suspect is the
      compute write-back copying shader output into guest memory that the game has since reused. The fix is to
      check every write-back target against the current memory map and the GPU fence the game waited on.
- [ ] Turn compiler warnings back on (upstream builds with `-w`) and fix them file by file, starting with the
      files we change.

## Phase 4: Wayland and the platform layer

- [ ] An SDL3 backend (`backend_sdl3.c`) next to `backend_x11.c`. It runs natively on Wayland and on X11, and adds
      gamepads (with SDL's controller database), fullscreen, scaling and hotplug.
- [ ] The GL context: SDL creates it on EGL under Wayland. The glq worker thread owns the context, so it needs
      `SDL_GL_MakeCurrent` on that thread, with events still pumped on the main thread.
- [ ] Keep `backend_x11.c` as a fallback until the SDL3 backend has run the smoke test on both Wayland and X11.

## Phase 5: shaders

The game stalls the first time it meets a shader (`waiting for shader=414` in a short session).

- [ ] A persistent shader cache: store translated GLSL and GL program binaries on disk, keyed by the Maxwell code
      hash and the draw state that affects translation (vertex attribute types, the VertexA/VertexB pair, texture
      types).
- [ ] Pre-translation at recomp time. Catherine ships its shaders as files (`romfs/data/shaders_binary/*.vs`,
      `*.ps`), so most can be translated before the game ever runs. Variants that depend on draw state are recorded
      in the cache on the first run and built at startup afterwards, the way Ryujinx's shader cache works.
- [ ] SPIR-V output. Emit SPIR-V directly from the Maxwell decoder, or compile the generated GLSL with glslang at
      cache-build time. SPIR-V pays off with the Vulkan backend (Phase 6). OpenGL's `GL_ARB_gl_spirv` gains little.

## Phase 6: GPU

Some GPU emulation can't be avoided: the shaders are compiled Maxwell code and have to be translated whatever sits
underneath. What can change is how much of the hardware is emulated and how well it maps to the host.

- [ ] A Vulkan backend implementing the same interface as `gpu_gl.c`. It consumes SPIR-V from Phase 5, gets a real
      pipeline cache, runs compute asynchronously with explicit synchronization, and replaces the glq thread.
- [ ] Texture and buffer cache tracking: invalidation on CPU writes and correct write-back, which is the same area
      as the Phase 3 crash.
- [ ] Research: NVN-level emulation. The game reaches NVN through `nvnBootstrapLoader` and
      `nvnDeviceGetProcAddress`, so the recompiled code could be pointed at our own NVN implementation on Vulkan.
      That skips command-buffer decoding and most of the register-level emulation. NVN has around 800 entry points,
      so this only makes sense once the Vulkan backend works.

## Phase 7: polish and games

- [ ] Performance: the CPU side is the limit in 3D scenes (GPU thread about 45% busy at 40 FPS). Try LTO and
      `-march=native` for the game module, then profile the hot runtime paths.
- [ ] The Western release with its update (TID 0100BF00112C0000). BKTR updates are supported by the extraction
      script. It needs a matching base game.
- [ ] Other games, to keep the fixes general rather than tuned to Catherine.
