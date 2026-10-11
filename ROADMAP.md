# Roadmap

This fork takes SwitchRecomp v142 (MZ Labs) and makes it a Linux-first project, with Catherine: Full Body
(Asian release, TID 0100BC4010896000) as the game everything is checked against. The phases are in the order they
should land. A phase can start before the previous one is finished where it says so.

Status: `[x]` done, `[~]` in progress, `[ ]` not started.

Current order (2026-10-11): Phases 4, 5 and 6 are done, apart from trying a real controller (Phase 4), which needs
someone with a pad. Next is Phase 2's remaining items, then sound (Phase 3).

## Where things stand (2026-10-11)

Catherine boots through the logos, the intro movie, the terms screen, the main menu and the New Game cutscene at
60 FPS. It took eight fixes to the v142 runtime to get there (see the history of `main`). There is no sound. The
runtime has two renderers and uses Vulkan by default, with OpenGL as the fallback. On Vulkan the title-screen crash
from Phase 2 hasn't happened in 13 runs. It still happens under OpenGL, in about half the runs past the title
screen.

How the pieces fit:

- **CPU**: the game's ARM64 code is translated to C once (`switchrecomp.py`, `codegen.py`, `a64.py`) and built as
  `<Game>_code.so`. This part is a real recompilation.
- **OS**: kernel, IPC and services are high-level emulation in `runtime/` (`kernel.c`, `ipc.c`, `svc_*.c`,
  `nvdrv.c`).
- **GPU**: emulated at the hardware level. The game's own NVN driver is recompiled with the game and writes Maxwell
  command buffers. `gpu.c`, `gpu_engines.c` and `gpu_3d.c` decode them, `shader.c` translates Maxwell shader
  binaries to GLSL, and a renderer draws: `gpu_vk.c` and the `vk_*.c` files with Vulkan, or `gpu_gl.c` with
  OpenGL. `renderer.c` calls the active one through the interface `gpu_nogl.c` shows (about 20 functions).
- **Video**: `host1x.c` emulates NVDEC and VIC, with OpenH264 doing the H.264 decode.

## Phase 0: repository and build

- [x] Git history: an `upstream` branch holding each MZ Labs release as released and then formatted, and `main`
      with our work on top, one commit per fix. `scripts/import-upstream.sh` imports new releases.
- [x] `.gitignore` that keeps game data, keys, dumps and build output out of git.
- [x] No prebuilt binaries: OpenH264, SPIRV-Cross and hactool are submodules that `scripts/bootstrap.py` builds,
      on Linux, on Windows, or cross-built for Windows.
- [x] CMake build for the Linux runtime, with presets for GCC 16 and Clang 23, C23 and C++23.
- [x] Scripts for the whole pipeline: extract a dump, recompile, deploy the runtime into `games/`.
- [x] Formatting: clang-format 23 and ruff through `scripts/format.sh`, applied to the whole history.
- [x] Windows: `scripts/bootstrap.ps1` downloads a pinned, checksummed toolchain (w64devkit, NASM, Python, and
      llvm-mingw for Clang) so a Windows machine needs only Git. CMake builds the Windows runtime natively or
      cross-built.
- [x] CI on every push and pull request: formatting, Linux with GCC and Clang, Windows with GCC and Clang from a bare
      runner, and the Linux to Windows cross build.
- [x] Debugging tools in `tools/`: symbol names, disassembly, unattended runs.
- [ ] Still to move from bring-up: the thread-dump reader and the Maxwell instruction scanner.
- [x] `README.md` for the Linux setup, crediting "SwitchRecomp by MZ Labs" as NOTICE requires. `LICENSE` and
      `NOTICE` replace the release's `.txt` files.

## Phase 1: Claude Code setup

- [x] `CLAUDE.md` with the project's rules: what never gets committed, how upstream code is changed, how a change
      is verified, licensing of reference code.
- [x] Skills: `swr-build`, `swr-run`, `swr-crash`, `swr-service`, `import-upstream`, `record-lesson`, and the
      `code-audit` loop.
- [x] Six domain review agents for the audit loop (kernel and services, GPU, audio and video, platform,
      recompiler, build and tooling), and two read-only investigators (`crash-triage`, `sdk-scout`).
- [x] A hook that blocks staging keys or game files, broad `git add`, force pushes, `pkill` and `sudo`, and edits
      to generated or third-party trees.

## Phase 2: stability

This comes before sound: the title-screen crash now ends most runs, so nothing past it can be tested reliably, and
a renderer rewrite on top of a memory-corruption bug would make every regression ambiguous.

- [ ] Smoke test: a scripted run from boot to the New Game cutscene that checks for crashes, FPS and screenshots,
      runnable unattended (Xvfb or a headless Wayland compositor).
- [x] VIC jobs wrote past NvRmStream's 128-entry syncpoint table into the game's heap (host1x syncpoints now come
      from the GPU's allocator).
- [x] Compute write-back wrote the whole SSBO range back late, reverting game writes made in the meantime. Only
      bytes the shader changed land now, and only where guest memory still holds what the shader started from.
- [~] The title-screen crash (paused until Phase 6). A vertex-skinning compute shader writes position + normal
      records over the game's deferred-release list nodes (`{obj, arg1, arg2, fn1, fn2, next}`, walked at
      `0x82df5c0`), and the game later calls a float as a function pointer. Established by measurement:
      - The nodes are already inside the SSBO range when the GPU thread runs the dispatch, 1 to 8 ms after its
        submit. The shader really writes that whole range; stores land at their true addresses.
      - Not the cause: game-thread races (crashes pinned to one core), GPU VA remapping (all maps are made once),
        pushbuffers changing after submit, out-of-bounds shader writes, stream syncpoint wait timeouts, GPU-thread
        lag. Throttling DequeueBuffer on the frame fence made it worse.
      - What stopped it (0 crashes in 17 runs, against about half before): holding semaphore and report releases
        until compute results are in guest memory. Under OpenGL that waits for the whole rendering backlog
        (40 to 80 ms per release, stretches at 1 FPS), so it was not kept. A Vulkan backend with its own compute
        queue and explicit synchronization can order these without the stall.
      - Open question: what the game waited on before allocating those nodes (`sdk-scout` on the allocator).
      - The Vulkan renderer orders it that way (Phase 6, step 4): 0 crashes in 7 runs. What remains here is
        the same ordering for OpenGL, or Vulkan becoming the default.
- [ ] Turn compiler warnings back on (upstream builds with `-w`) and fix them file by file, starting with the
      files we change.

## Phase 3: sound

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

## Phase 4: Wayland and the platform layer

- [x] An SDL3 backend (`backend_sdl3.c`), the Linux default. It runs natively on Wayland and on X11, with gamepads
      (SDL's controller database, hotplug, buttons by position), fullscreen, and the Windows backend's keys and
      hotkeys.
- [x] The GL context: SDL creates it (EGL under Wayland) on the GPU thread. The GL worker's shared context is current
      on a hidden helper window, since EGL won't make one surface current on two threads.
- [x] `backend_x11.c` stays as a fallback (`-DSWR_BACKEND=x11`), and CI keeps building it.
- [x] The SDL3 backend on Windows too (`-DSWR_BACKEND=sdl3`), for controllers other than XInput ones and for the
      Vulkan renderer. The bootstrap builds SDL3 statically from `third_party/SDL`, and CI builds both variants. The
      Vulkan build runs Catherine under Wine. The win32 backend stays the default until the SDL3 one has run on real
      Windows: under Wine, SDL3 can't open an OpenGL window at all, so that combination is only built.
- [ ] Try a real controller: the mapping follows SDL's positional buttons, but no pad was at hand to test it.

## Phase 5: shaders

The game stalled the first time it met a shader (`waiting for shader=414` in a short session).

- [x] A persistent shader cache. OpenGL already had one from upstream: the translated GLSL in
      `shader_cache/shader_pack_v3.bin`, program binaries next to it, compiled in the background at startup. It still
      loaded a known program's binary on the compile thread, so every shader the session met skipped its draws for a
      frame (1758 draws in a warm-cache run to the cutscene). Outside Windows that binary now loads on the GPU thread,
      and those skips are gone. Vulkan keeps SPIR-V (`shader_cache/spirv/`, keyed by the GLSL) and the driver's
      pipeline cache (`shader_cache/vk_pipelines.bin`).
- [x] Pre-translation at recomp time. Catherine ships its shaders as NVN files (`romfs/data/shaders_binary/*.vs`,
      `*.ps`): a control section, then the program at 2896 bytes, a vertex shader's VertexA and VertexB one after the
      other. `swr-shaderc --nvn` translates them as the renderer would on first use (VertexA and VertexB merged) and
      fills the SPIR-V cache, and `scripts/recomp.py` runs it after recompiling. Of the shaders a first run to the
      cutscene uses, 134 of 149 fragment and 56 of 60 vertex shaders are already there. The rest depend on draw state
      (integer attributes, textures or outputs) or read constant buffers while translating. Pipelines still compile
      on first use, then come from the driver's cache.
- [x] SPIR-V output: the generated GLSL compiled with glslang (Phase 6, step 2). Emitting SPIR-V straight from the
      Maxwell decoder would save little now that glslang's output is cached and mostly made ahead of time.

## Phase 6: GPU

Some GPU emulation can't be avoided: the shaders are compiled Maxwell code and have to be translated whatever sits
underneath. What can change is how much of the hardware is emulated and how well it maps to the host.

- [x] A Vulkan backend implementing the same interface as `gpu_gl.c`, in the same runtime as OpenGL. Vulkan 1.3 (timeline semaphores, synchronization2, dynamic rendering), loaded through
      SDL's loader with volk so nothing links against a Vulkan library.
  - [x] Step 1, presenting: instance, device with a graphics and a separate compute queue, swapchain through SDL3
        (Wayland and X11), and the CPU path's frames shown on it. Nothing is drawn yet.
  - [x] Step 2, shaders: the Maxwell translator has a Vulkan mode (`#version 450`, one descriptor set per stage with
        the bindings in `shader.h`), compiled to SPIR-V with glslang 16.6 (a submodule the bootstrap builds), through
        an on-disk SPIR-V cache. `SWR_SHADER_CORPUS` records the shaders a run translates, and `swr-shaderc` checks a
        corpus offline: 116 of Catherine's 117 shaders compile and pass `spirv-val` (the other uses a global load the
        translator can't place, in GL too), and the GL output is byte-identical to before.
  - [~] Step 3, drawing: render targets, textures, samplers, pipelines and draws, ported from `gpu_gl.c`, in slices.
    - [x] Render targets and clears: targets by guest address (parked and reused when a size or format comes
          back), dynamic rendering passes kept open across clears and draws to the same set, write-back to guest
          memory and reload from it around CPU-side copies, frames in flight with a staging ring and deferred
          destruction, memory through VMA. The game's frame is presented from its render target.
    - [x] Draws: shaders as SPIR-V modules, pipelines by state (shaders, vertex layout, attachment formats, blend)
          with the rest dynamic, a descriptor set per stage from per-frame pools, textures from texture headers
          (deswizzled on the CPU, BCn native, ASTC decoded, change checks backing off like GL's), samplers, and
          vertex, index, constant and storage buffers (stable data in buffers of its own, changing data read from
          the staging ring). Catherine's logos, menus and the opening movie match GL; the Atlus logo is
          pixel-identical. The viewport maps the guest's transform directly (y down), so shaders don't flip y.
    - [x] 3D scenes: the New Game cutscene matches GL, with its skinned models (step 4). Draws that read the depth
          target they test against (soft particles) bind it read-only. Clears with a partial color mask are a
          full-screen draw with that mask, the 2D engine's blits between render targets run on the GPU, and the
          driver's pipeline cache is kept in `shader_cache/vk_pipelines.bin`. A draw that samples a color target
          it renders to reads it as empty (none in Catherine so far).
  - [x] Step 4, compute on its own queue (`vk_compute.c`). Dispatches are batched on the compute queue, and their
        results land in guest memory, with the rule from Phase 2, before anything the game can observe: a read
        or GPU write of that memory, a semaphore release or report, a syncpoint. That is the ordering that
        stopped the title-screen crash, and it costs little on the compute queue. Draws read results that haven't
        landed straight from the arena (the batch signals a timeline semaphore the next graphics submit waits on),
        a dispatch reads what an earlier one in its batch wrote in place (multi-pass skinning), and the arena grows
        and is reused rather than filling up: about 200 000 dispatches in 4 800 batches over 150 s, with 1.3 s
        spent waiting. 0 crashes in 13 runs past the title screen (GL: 2 of 5 the same day), slightly faster than
        GL (8 700 frames in 150 s against 8 600).
  - [x] Step 5, the default. The sdl3 backend builds both renderers into one runtime (`renderer.c` calls the active
        one; `gl_names.h` renames `gpu_gl.c`'s side without editing it), and `renderer=auto` in the .ini picks Vulkan,
        falling back to OpenGL when Vulkan can't start or when LSFG frame generation is set up. Vulkan has the
        resolution scale (render targets at the scaled size, written back and reloaded through unscaled images) and
        `shadows=0`. Catherine matches OpenGL from boot to the New Game cutscene: the logo frame is pixel-identical,
        later screens match by eye. Only OpenGL has LSFG, dynamic resolution, the F7 recorder and F8 screenshots.
        On Windows the default backend is still win32 (OpenGL), until the SDL3 backend has run on real Windows
        (Phase 4); with `-DSWR_BACKEND=sdl3` it is the same as on Linux, and runs Catherine on Vulkan under Wine.
- [x] Texture and buffer cache tracking. On Linux the guest's memory is registered with a userfaultfd in
      asynchronous write-protect mode, and `PAGEMAP_SCAN` lists the pages written since a range was last scanned
      (`plat_ww_scan`; Windows has `GetWriteWatch` with `SWR_WW`). The Vulkan caches scan a texture or buffer when they
      use it and re-read it only when it was written, instead of checking contents on a back-off that could miss a
      change for several frames: a movie's frames now reach their texture every frame. Results land in guest memory
      with the rule from Phase 2, and render targets are written back before any CPU-side copy reads them. OpenGL
      keeps its content checks.
- [x] Research: NVN-level emulation, for Catherine. Findings (from the disassembly, nothing run):
      - The NVN driver is linked statically into the `sdk` module; `nvnBootstrapLoader` (0xcf8fd90) is exported
        there and imported by `main` through a PLT stub (GOT slot 0x8ae3d20), so the runtime could interpose it at
        load time.
      - `main`'s loader (0x8320b70) asks `nvnDeviceGetProcAddress` for 468 entry points into a table of function
        pointers at 0x8ae8000. Game code uses about 178 of them, nearly all from one wrapper layer, the engine's
        `cvDevice` (0x831e000 to 0x8327000): command buffers (138 requested), textures and samplers, state objects,
        memory pools, queues, syncs and the window. It never writes command memory itself, and reads nothing back
        from the GPU but fences: no report counters or queries.
      - The movie player (`android::SfNvnUtil` in `subsdk1`) uses NVX entry points that reach NvRm surfaces and
        syncpoints, so the host1x path stays whatever sits above it.
      - Assessment: an NVN implementation on Vulkan would need those ~178 entry points (stubs for the rest), the
        texture and sampler pools as packed Maxwell descriptors (shaders index them by handle), memory pools as
        CPU-visible memory with aliasing, and the same shader translation (the programs are still Maxwell code, and
        shaders read raw GPU addresses from constant buffers). It would replace command-buffer decoding, the
        register shadowing in `gpu_3d.c` and the cache guessing, since NVN states what changes. A partial version
        that keeps the real driver for most calls is hard, because both would have to agree on object state.
        With the register-level Vulkan renderer now matching OpenGL, this stays a possible later rewrite rather
        than a next step: the cost is large and it fixes nothing that is broken today.

## Phase 7: polish and games

- [ ] Performance: the CPU side is the limit in 3D scenes (GPU thread about 45% busy at 40 FPS). Try LTO and
      `-march=native` for the game module, then profile the hot runtime paths.
- [ ] The Western release with its update (TID 0100BF00112C0000). BKTR updates are supported by the extraction
      script. It needs a matching base game.
- [ ] Other games, to keep the fixes general rather than tuned to Catherine.
