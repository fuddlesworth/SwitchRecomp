# Roadmap

This fork takes SwitchRecomp v142 (MZ Labs) and makes it a Linux-first project, with Catherine: Full Body
(USA release, TID 0100BF00112C0000 with its v2.0 update) as the game everything is checked against. Until 2026-10-11
that was the Asian release (TID 0100BC4010896000): game addresses quoted below before that date, like the
title-screen crash's, are in its code. The phases are in the order they should land. A phase can start before the
previous one is finished where it says so.

Status: `[x]` done, `[~]` in progress, `[ ]` not started.

Current order (2026-10-11): Phases 4, 5 and 6 are done, apart from trying a real controller (Phase 4), which needs
someone with a pad. Phase 8 (code layout) is done. A code audit of the whole project (2026-10-11) fixed what it found
in every part, and the larger follow-ups it left are open items in the phases below. Next: Phase 2's remaining items
and sound (Phase 3).

## Where things stand (2026-10-11)

Catherine (the USA release since 2026-10-11, with its v2.0 update) boots through the logos, the intro movie, the
terms screen, the main menu and the New Game cutscene at 60 FPS. It took eight fixes to the v142 runtime to get
there (see the history of `main`). There is no sound. The runtime draws with Vulkan: the OpenGL renderer, where the
title-screen crash from Phase 2 still happened in about half the runs, was removed on 2026-10-11.

How the pieces fit:

- **CPU**: the game's ARM64 code is translated to C once (the `scarletnx` package) and built as
  `<Game>_code.so`. This part is a real recompilation.
- **OS**: kernel, IPC and services are high-level emulation in `runtime/kernel/` and `runtime/hle/`.
- **GPU**: emulated at the hardware level. The game's own NVN driver is recompiled with the game and writes Maxwell
  command buffers. `runtime/gpu/` decodes them (`gpu.c`, the engines, `maxwell3d.c`), `gpu/shader/` translates
  Maxwell shader binaries to GLSL, and the Vulkan renderer (`gpu/vk/`) draws.
  The front end reaches it through `g_renderer`, the table of operations in `gpu/renderer.h`.
- **Video**: `runtime/video/` emulates NVDEC and VIC, with FFmpeg's libavcodec doing the H.264 decode. Each NVDEC
  or VIC device the game opens is a channel of its own, NVDEC writes each picture into the surface its job names,
  and VIC composes from those surfaces in guest memory.

## Phase 0: repository and build

- [x] Git history: an `upstream` branch holding each MZ Labs release as released and then formatted, and `main`
      with our work on top, one commit per fix. `scripts/import-upstream.sh` imports new releases.
- [x] `.gitignore` that keeps game data, keys, dumps and build output out of git.
- [x] No prebuilt binaries: the third-party libraries and hactool are CPM.cmake packages, each pinned to a release
      archive and its SHA-256 in `cmake/dependencies.cmake`, fetched at configure time and built with the runtime. They
      replaced the submodules and `scripts/bootstrap.py`, so `cmake -B build` works on a fresh clone.
- [x] CMake build with Clang only (23 in development), C23 and C++23. GCC was dropped: one compiler for the runtime,
      the game code and the libraries. Presets `default` and `windows-cross`.
- [x] FreeBSD builds (`plat_posix.c`'s mmap flags and crash handler), and CI builds it in a FreeBSD 15.1 VM.
- [x] A nightly AppImage of the runtime that runs on any Linux, made with pkgforge's quick-sharun in CI and released
      under the rolling `nightly` tag after every green push to `main`. The runtime takes its name and folder from
      `$APPIMAGE`, so a copy named after a game finds the game's code as the bare runtime does.
- [x] Scripts for the whole pipeline: extract a dump, recompile, deploy the runtime into `games/`.
- [x] Formatting: clang-format 23 and ruff through `scripts/format.sh`, applied to the whole history.
- [x] Windows: `scripts/bootstrap.ps1` downloads a pinned, checksummed toolchain (llvm-mingw, CMake, Ninja, NASM,
      Python, and w64devkit for FFmpeg's shell and make) so a Windows machine needs only Git. CMake builds the
      Windows runtime natively or cross-built with llvm-mingw.
- [x] CI on every push and pull request: formatting, Linux, FreeBSD, Windows from a bare runner, and the Linux to
      Windows cross build, all with Clang.
- [x] Debugging tools in `tools/`: symbol names, disassembly, unattended runs.
- [ ] Still to move from bring-up: the thread-dump reader and the Maxwell instruction scanner.
- [x] `README.md` for the Linux setup, crediting "SwitchRecomp by MZ Labs" as NOTICE requires. `NOTICE` and
      `LICENSE.apache` are the release's `NOTICE.txt` and `LICENSE.txt`, and `LICENSE` is the fork's GPL.
- [x] The fork is GPL-3.0-or-later since 2026-10-11, so code from yuzu and its forks can be ported. MZ Labs' files
      keep their Apache-2.0 headers and NOTICE, with `LICENSE.apache`.
- [x] FFmpeg's libavcodec decodes the NVDEC H.264 jobs instead of OpenH264: FFmpeg 9.0.2, built from source with only
      the H.264, VP8 and VP9 decoders, so VP8 and VP9 videos only need NVDEC's side now. The Opus decoder (with
      libswresample, which it needs) was added for `hwopus`.

## Phase 1: Claude Code setup

- [x] `CLAUDE.md` with the project's rules: what never gets committed, how upstream code is changed, how a change
      is verified, licensing of reference code.
- [x] Skills: `snx-build`, `snx-run`, `snx-crash`, `snx-service`, `import-upstream`, `record-lesson`, and the
      `code-audit` loop.
- [x] Six domain review agents for the audit loop (kernel and services, GPU, audio and video, platform,
      recompiler, build and tooling), and two read-only investigators (`crash-triage`, `sdk-scout`).
- [x] A hook that blocks staging keys or game files, broad `git add`, force pushes, `pkill` and `sudo`, and edits
      to generated or third-party trees.

## Phase 2: stability

This came before sound: the title-screen crash ended most runs, so nothing past it could be tested reliably, and a
renderer rewrite on top of a memory-corruption bug would have made every regression ambiguous.

- [~] Smoke test: a scripted run from boot to the New Game cutscene that checks for crashes, FPS and screenshots,
      runnable unattended (Xvfb or a headless Wayland compositor). `tools/run_game.py` runs the New Game script any
      number of times and reports each run's exit, crash site, frame count and average FPS, with the last frame as a
      picture. Still missing: a check of the pictures against known-good ones, and a run without a display.
- [x] VIC jobs wrote past NvRmStream's 128-entry syncpoint table into the game's heap (host1x syncpoints now come
      from the GPU's allocator).
- [x] Compute write-back wrote the whole SSBO range back late, reverting game writes made in the meantime. Only
      bytes the shader changed land now, and only where guest memory still holds what the shader started from.
- [x] The title-screen crash. A vertex-skinning compute shader writes position + normal
      records over the game's deferred-release list nodes (`{obj, arg1, arg2, fn1, fn2, next}`, walked at
      `0x82df5c0`), and the game later calls a float as a function pointer. Established by measurement:
      - The nodes are already inside the SSBO range when the GPU thread runs the dispatch, 1 to 8 ms after its
        submit. The shader really writes that whole range, and its stores land at their true addresses.
      - Not the cause: game-thread races (crashes pinned to one core), GPU VA remapping (all maps are made once),
        pushbuffers changing after submit, out-of-bounds shader writes, stream syncpoint wait timeouts, GPU-thread
        lag. Throttling DequeueBuffer on the frame fence made it worse.
      - What stopped it (0 crashes in 17 runs, against about half before): holding semaphore and report releases
        until compute results are in guest memory. Under OpenGL that waits for the whole rendering backlog
        (40 to 80 ms per release, stretches at 1 FPS), so it was not kept. A Vulkan backend with its own compute
        queue and explicit synchronization can order these without the stall.
      - Open question: what the game waited on before allocating those nodes (`sdk-scout` on the allocator).
      - The Vulkan renderer orders it that way (Phase 6, step 4): 0 crashes in 7 runs, then 13. OpenGL, where it
        still happened, was removed on 2026-10-11.
- [~] Turn compiler warnings back on (upstream builds with `-w`) and fix them file by file, starting with the
      files we change. The default warnings are on since Phase 8 removed `-w`, and the build is clean.
      They found one real bug, still open: in the shader translator, `R(c, r + 2)` for a vector operand that
      starts near register 255 writes past `used_reg[256]` (GCC's `-Wstringop-overflow` saw it while `R` was
      inlined into the same file). Next: what the hardware does there, then `-Wall`.

## Phase 3: sound

Catherine uses two audio paths, and both are silent today.

- [ ] **Audio output device.** SDL3 audio streams, through the SDL3 backend the runtime already has on Linux and
      Windows. Nothing reaches an audio device today.
- [ ] **`audout` (movie audio).** Buffers are already released on a 48 kHz clock. Send their PCM to the device and
      let the device's consumption drive the release instead of the wall clock, so movies stay in sync.
- [ ] **`audren` (music and sound effects).** The runtime answers `RequestUpdate` in the right format but mixes
      nothing. Needed: memory pools, voices (PCM16 and DSP-ADPCM, with pitch and resampling), volume and mix
      routing through submixes to the final mix, biquad filters, then effects (delay, reverb, aux) and the device and
      circular-buffer sinks. Ryujinx's `Ryujinx.Audio` renderer (MIT) is the reference. yuzu's `audio_core`
      (GPL-2.0-or-later) can be ported too, keeping its copyright lines.
- [~] `hwopus` decodes the game's Opus packets with FFmpeg's Opus decoder, with the commands the SDK's code sends
      (read from its disassembly, and from Ryujinx for the ones Catherine doesn't use). Still to check by ear once
      `audren` produces sound, with the other audio services the game opens.

## Phase 4: Wayland and the platform layer

- [x] An SDL3 backend (`platform/backend/sdl3.c`), the Linux default. It runs natively on Wayland and on X11, with
      gamepads (SDL's controller database, hotplug, buttons by position), fullscreen, and the Windows backend's keys
      and hotkeys.
- [x] The GL context: SDL creates it (EGL under Wayland) on the GPU thread. The GL worker's shared context is current
      on a hidden helper window, since EGL won't make one surface current on two threads. (Gone with the OpenGL
      renderer on 2026-10-11.)
- [x] The SDL3 backend on Windows too, for controllers other than XInput ones and for the Vulkan renderer. The
      CMake build links SDL3 statically from source. It runs Catherine on Vulkan under Wine.
- [x] The x11, x11es and win32 backends were removed on 2026-10-11: SDL3 is the window backend on Linux and Windows,
      with headless for tests. The android backend went with OpenGL the same day, since it drew with GLES. SDL3
      still hasn't run on real Windows, only under Wine.
- [ ] Try a real controller: the mapping follows SDL's positional buttons, but no pad was at hand to test it.
- [ ] On Windows the game freezes while the window is dragged. Windows runs its own loop inside the event pump for
      as long as the drag lasts, and vi's vsync tick runs in the same 60 Hz loop as the pump (`core/main.c`), so no
      vsync reaches the game until the drag ends. It goes with the frame loop's part of the GPU rewrite (Phase 6).

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
      other. `snx-shaderc --nvn` translates them as the renderer would on first use (VertexA and VertexB merged) and
      fills the SPIR-V cache, and `scripts/recomp.py` runs it after recompiling. Of the shaders a first run to the
      cutscene uses, 134 of 149 fragment and 56 of 60 vertex shaders are already there. The rest depend on draw state
      (integer attributes, textures or outputs) or read constant buffers while translating. Pipelines still compile
      on first use, then come from the driver's cache.
- [x] SPIR-V output: the generated GLSL compiled with glslang (Phase 6, step 2). Emitting SPIR-V straight from the
      Maxwell decoder would save little now that glslang's output is cached and mostly made ahead of time.

## Phase 6: GPU

Some GPU emulation can't be avoided: the shaders are compiled Maxwell code and have to be translated whatever sits
underneath. What can change is how much of the hardware is emulated and how well it maps to the host.

- [x] A Vulkan backend implementing the same interface as `gpu_gl.c` did, in the same runtime as OpenGL at first.
      Vulkan 1.3 (timeline semaphores, synchronization2, dynamic rendering), loaded through SDL's loader with volk so
      nothing links against a Vulkan library.
  - [x] Step 1, presenting: instance, device with a graphics and a separate compute queue, swapchain through SDL3
        (Wayland and X11), and the CPU path's frames shown on it. Nothing is drawn yet.
  - [x] Step 2, shaders: the Maxwell translator has a Vulkan mode (`#version 450`, one descriptor set per stage with
        the bindings in `shader.h`), compiled to SPIR-V with glslang 16.6 (built with the runtime), through
        an on-disk SPIR-V cache. `SNX_SHADER_CORPUS` records the shaders a run translates, and `snx-shaderc` checks a
        corpus offline: 116 of Catherine's 117 shaders compile and pass `spirv-val` (the other uses a global load the
        translator can't place, in GL too), and the GL output is byte-identical to before.
  - [x] Step 3, drawing: render targets, textures, samplers, pipelines and draws, ported from `gpu_gl.c`, in slices.
    - [x] Render targets and clears: targets by guest address (parked and reused when a size or format comes
          back), dynamic rendering passes kept open across clears and draws to the same set, write-back to guest
          memory and reload from it around CPU-side copies, frames in flight with a staging ring and deferred
          destruction, memory through VMA. The game's frame is presented from its render target.
    - [x] Draws: shaders as SPIR-V modules, pipelines by state (shaders, vertex layout, attachment formats, blend)
          with the rest dynamic, a descriptor set per stage from per-frame pools, textures from texture headers
          (deswizzled on the CPU, BCn native, ASTC decoded, change checks backing off like GL's), samplers, and
          vertex, index, constant and storage buffers (stable data in buffers of its own, changing data read from
          the staging ring). Catherine's logos, menus and the opening movie match GL, and the Atlus logo is
          pixel-identical. The viewport maps the guest's transform directly (y down), so shaders don't flip y.
    - [x] 3D scenes: the New Game cutscene matches GL, with its skinned models (step 4). Draws that read the depth
          target they test against (soft particles) bind it read-only. Clears with a partial color mask are a
          full-screen draw with that mask, the 2D engine's blits between render targets run on the GPU, and the
          driver's pipeline cache is kept in `shader_cache/vk_pipelines.bin`. A draw that samples a color target
          it renders to reads it as empty (none in Catherine so far).
  - [x] Step 4, compute on its own queue (`gpu/vk/compute.c`). Dispatches are batched on the compute queue, and their
        results land in guest memory, with the rule from Phase 2, before anything the game can observe: a read
        or GPU write of that memory, a semaphore release or report, a syncpoint. That is the ordering that
        stopped the title-screen crash, and it costs little on the compute queue. Draws read results that haven't
        landed straight from the arena (the batch signals a timeline semaphore the next graphics submit waits on),
        a dispatch reads what an earlier one in its batch wrote in place (multi-pass skinning), and the arena grows
        and is reused rather than filling up: about 200 000 dispatches in 4 800 batches over 150 s, with 1.3 s
        spent waiting. 0 crashes in 13 runs past the title screen (GL: 2 of 5 the same day), slightly faster than
        GL (8 700 frames in 150 s against 8 600).
  - [x] Step 5, the default. The sdl3 backend built both renderers into one runtime (`renderer.c` called the
        active one, and `gl_names.h` renamed `gpu_gl.c`'s side without editing it), and `renderer=auto` in the .ini
        picked Vulkan, falling back to OpenGL when Vulkan couldn't start or when LSFG frame generation was set up.
        Vulkan got the resolution scale (render targets at the scaled size, written back and reloaded through
        unscaled images) and `shadows=0`. Catherine matched OpenGL from boot to the New Game cutscene: the logo frame
        was pixel-identical, and later screens matched by eye. Only OpenGL had LSFG, dynamic resolution, the F7
        recorder and F8 screenshots. On Windows the SDL3 backend is the same as on Linux, and runs Catherine on
        Vulkan under Wine. OpenGL was removed on 2026-10-11 (below).
- [x] Texture and buffer cache tracking. On Linux the guest's memory is registered with a userfaultfd in
      asynchronous write-protect mode, and `PAGEMAP_SCAN` lists the pages written since a range was last scanned
      (`plat_ww_scan`, and `GetWriteWatch` with `SNX_WW` on Windows). The Vulkan caches scan a texture or buffer
      when they use it and re-read it only when it was written, instead of checking contents on a back-off that could
      miss a change for several frames: a movie's frames now reach their texture every frame. Results land in guest
      memory with the rule from Phase 2, and render targets are written back before any CPU-side copy reads them.
- [x] Research: NVN-level emulation, for Catherine. Findings (from the disassembly, nothing run):
      - The NVN driver is linked statically into the `sdk` module. `nvnBootstrapLoader` (0xcf8fd90) is exported
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
        register shadowing in `gpu/engines.c` and `gpu/maxwell3d.c` and the cache guessing, since NVN states what
        changes. A partial version that keeps the real driver for most calls is hard, because both would have to
        agree on object state.
        With the register-level Vulkan renderer now matching OpenGL, this stays a possible later rewrite rather
        than a next step: the cost is large and it fixes nothing that is broken today.
- [ ] A GPU architecture rewrite. The command stream runs on the thread that submitted it, under one lock for every
      channel, so a semaphore acquire or syncpoint wait in the stream holds up every other channel's submissions, and
      a wait for another channel's work can't end. The waits give up after 2 seconds and log it rather than hang. A
      GPU thread (or a lock per channel) with the waits outside the submit lock is the fix, and it touches most of the
      front end.
- [x] The OpenGL renderer removed (2026-10-11). Vulkan had drawn by default since step 4, matched OpenGL from boot to
      the New Game cutscene, and doesn't have the title-screen crash. A GPU or driver without Vulkan 1.3 now stops
      the runtime at startup with a message. Gone with OpenGL, and not on Vulkan yet: LSFG frame generation (and
      SPIRV-Cross, which only it needed), the F7 recorder with its smooth recording clock, the F5 frame trace, F8
      full-resolution screenshots, dynamic resolution, applying vi's crop and transform to a presented buffer (now
      logged as "not applied"), and BCn decoding on the CPU for GPUs without BCn.

## Phase 7: polish and games

- [ ] Performance: the CPU side was the limit in 3D scenes when this was measured under OpenGL. Measure again under
      Vulkan, try LTO and `-march=native` for the game module, then profile the hot runtime paths.
- [x] The Western release with its update (TID 0100BF00112C0000), extracted with its BKTR update. It replaced the
      Asian release as the game everything is checked against on 2026-10-11.
- [ ] Other games, to keep the fixes general rather than tuned to Catherine.
- [ ] NaNs in f64 fused multiply-add and in the arithmetic the recompiler emits inline (`+ - * /`) are the host's.
      x86 makes a negative default NaN where ARM's is positive, and of two NaN operands it returns the first rather
      than the first signalling one. f32 fused multiply-add follows ARM's rules now. Only NaN signs and payloads
      differ.
- [ ] `svcSetThreadActivity` stops a thread only on its way into or out of an SVC. A thread spinning on WFE without
      making SVCs runs on until its next one, since WFE is not a pause point (the generated code doesn't store `pc`
      before it). Catherine never pauses a thread.

## Phase 8: code layout

The release is a flat `runtime/` of 70 files, wired together with `extern` declarations inside functions (167 of
them), a catch-all `rt.h`, files of up to 7900 lines, and the recompiler's modules loose at the top. This fork is a
hard fork from here (decided 2026-10-11): upstream releases are ported by hand rather than merged (see the
`import-upstream` skill), so files can be moved, split and renamed freely. Each step was its own PR, built in
every variant and run on Catherine with both renderers while OpenGL was still there.

- [x] The recompiler as a package (`scarletnx/`, run as `python3 -m scarletnx`), split by job. The runtime is
      built only by CMake, so its source list lives in one place, and `SNX_ABI` in `cpu.h` is the one ABI version
      both sides check.
- [x] `runtime/` as a tree of components (`abi`, `core`, `kernel`, `hle`, `gpu` with `vk` and, until OpenGL went,
      `gl`, `video`, `platform`), with includes by path. Each is a CMake object library, apart from `abi`, which is
      compiled into the game module, and `gpu`, which is several (`gpu`, `shader`, `spirv`, `vk`, `null`).
- [x] Headers per component: `rt.h` split into a header per module (`platform/plat.h`, `platform/log.h`,
      `kernel/mem.h`, `kernel/kernel.h`, `kernel/ipc.h`, `core/loader.h`, `core/main.h`, `core/options.h`, one per
      service and device in `hle/`, `video/host1x.h`, `platform/backend.h`...). Every function and global used across
      files is declared once, in the header of the file that defines it, and the rest are static. The 237 `extern`
      lines in `.c` files are gone, `-w` with them, and `-Werror=missing-prototypes` and
      `missing-variable-declarations` keep it that way. It turned up three copies of the 2D
      blit surface struct passed as `void*`, renderer functions whose definitions didn't match the interface's types,
      a backend static shadowing an LSFG global, and declarations of functions that no longer exist.
- [x] The renderer interface as a table of operations (`Renderer` in `gpu/renderer.h`) that each renderer fills in
      completely (OpenGL, Vulkan and null at the time, Vulkan and null now), and `g_renderer` pointing at the active
      one. It replaces the
      `gpu_gl_*` names, `gpu/renderer.c`'s forwarding macros and `gpu/gl/names.h`, and each renderer's functions are
      now static to its own files.
- [x] The big files split along their seams: `gpu/gl/renderer.c`, `shader.c`, `hle/services.c` (one file per service
      family), `main.c`, `kernel.c`, `cpu_helpers.c`, `plat.c`, `host1x.c`, `gpu/engines.c`. Done: the OpenGL
      renderer (7900 lines) became ten files by job, sharing `gpu/gl/gl.h`, before it was removed, and the shader
      translator is four (`shader.c`, `decode.c`, `translate.c`, `corpus.c`), sharing `gpu/shader/translator.h`.
      The services are one file per family in `hle/`, sharing `hle/services_int.h`. `core/main.c` is split
      by job: options, the guest clock, RtAPI, the crash report and the CPU-side present stay in `core/`, and
      the audio renderer update and the shared fonts moved to `hle/audren.c` and `hle/pl.c`. The kernel
      is six files (`kernel.c`, `thread.c`, `sync.c`, `svc.c`, `svc_mem.c`, `debug.c`), sharing
      `kernel/kernel_int.h`. The platform layer is a file per OS (`plat_posix.c`, `plat_win32.c` with
      `profiler_win32.c`), and `plat.c` keeps logging. The GPU engines are a file each (`mme.c`, `compute.c`,
      `fermi2d.c`, `dma.c`, with the 3D engine's methods in `engines.c`), and host1x is `host1x.c`, `nvdec.c`
      and `vic.c`. `abi/cpu_helpers.c` includes its helpers by topic from `abi/helpers/`, so the game module
      still compiles them as one file.
- [x] `docs/ARCHITECTURE.md`: the components, what each owns, and how a frame flows through them.

