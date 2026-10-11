# Architecture

SwitchRecomp has two halves. The recompiler, a Python package, translates a game's ARM64 code to C once, ahead of
time, and builds it into `<Game>_code.so`. The runtime, a C program, loads that module and stands in for everything
the game expects from the console: the kernel, the system services and the GPU. This page describes the parts, what
each one owns, and how a frame gets from the game to the screen. `CLAUDE.md` lists the files in each folder.

## The recompiler

`switchrecomp/` reads the extracted game (`work/<Game>/exefs` and `romfs`) and writes `games/<Game>`:

- `nxloader.py` loads the NSO executables (main, sdk, subsdks) and the RTLD, applies their relocations, and reads
  their symbols.
- `codegen.py` and `a64.py` find the functions and translate each ARM64 instruction to C. Every guest function becomes
  a C function that takes the CPU state, and the code is written in chunks the compiler can handle.
- `module.py` writes the module's tables: the module list, and each function's guest address and C function. The
  runtime builds the dispatch table from them at startup.
- `toolchain.py` compiles the chunks into `<Game>_code.so` (with an object cache, so a rerun only compiles what
  changed), and `data.py` copies the romfs and writes a default `<Game>.ini`.

`scripts/recomp.py` runs all of it, builds the runtime with CMake, and translates the game's shaders ahead of time.

## The contract between the module and the runtime

`runtime/abi/` is compiled into both sides. `cpu.h` defines the guest CPU state (`CPU`: registers, flags, vector
registers, the thread pointer registers), the dispatch lookup for indirect branches (`swr_lookup`), and `RtAPI`, the table of
runtime functions the module calls: supervisor calls, undefined instructions, BRK, the system counter, FPCR changes
and wait-for-event. `cpu_helpers.c`, with the topic files it includes from `helpers/`, holds the instructions too
large to generate inline (SIMD, crypto, float conversions). `SWR_ABI` in `cpu.h` is the version of this contract: the
runtime refuses a module built against another one, and changing any of these files means recompiling every game.

The module imports nothing from the runtime by name. Everything goes through `RtAPI`, which the runtime passes to the
module when it loads it.

## The runtime

Each folder under `runtime/` is a CMake object library, and each has headers that declare what other folders use
from it.

| Folder | Owns |
| --- | --- |
| `core/` | Startup (`main.c`): loading the module, the settings in `<Game>.ini` (`options.c`), the guest clock (`clock.c`), the `RtAPI` functions (`rtapi.c`), crash reports (`crash.c`) |
| `kernel/` | The Horizon kernel, emulated at a high level: handles and kernel objects, threads, synchronization and address arbitration, the SVC dispatch, the guest address space (`mem.c`) and IPC (`ipc.c`) |
| `hle/` | The system services the game talks to over IPC, one file per family (am, set, time, acc, audio, hid, fs, vi...), and nvdrv, the GPU driver's device files |
| `gpu/` | The GPU front end (GPFIFO, the 3D, compute, 2D, DMA and inline-to-memory engines), the shader translator (`shader/`), the renderer interface and the renderers (`gl/`, `vk/`, `null/`) |
| `video/` | host1x: the video decoder (NVDEC, with OpenH264) and the video image compositor (VIC) |
| `platform/` | The operating system layer (`plat_posix.c`, `plat_win32.c`) and the window backend (`backend/sdl3.c`, or `headless.c` for tests) |

### Threads and memory

Every guest thread is a host thread. The kernel creates it with `plat_thread`, sets up its CPU state and TLS, and calls
the module's function for the thread's entry point. Synchronization (mutexes, condition variables, events, address
arbitration) is implemented in the kernel with host locks and condition variables.

The guest's whole address space is one host reservation (`g_mem`, `kernel/mem.c`), so a guest address is an offset
into it (`GP(addr)`). The generated code reads and writes guest memory directly. The kernel tracks which ranges are
mapped, with what state and permissions, the way the console's memory manager does.

### System calls and services

A `svc` instruction calls `RtAPI.svc`, which lands in `kernel/svc.c` and is dispatched by number. IPC requests
(`svcSendSyncRequest`) go to `kernel/ipc.c`, which parses the message, finds the session's service object and calls
its dispatch function in `hle/`. The service reads the request's arguments and buffers, and writes the reply the way
the console would, since the game's SDK code checks the replies and asserts when one is impossible.

## How a frame gets to the screen

1. The game's own graphics driver, NVN, is part of the recompiled module. It builds Maxwell command buffers in guest
   memory, exactly as on the console.
2. It submits them through nvdrv: an ioctl on `/dev/nvhost-gpu` reaches `hle/nvdrv.c`, which hands the GPFIFO entries
   to `gpu_submit` (`gpu/gpu.c`).
3. The renderer decides where that runs. OpenGL queues it for its own GPU thread, and Vulkan runs it on the
   submitting thread. Either way, `gpu.c` reads the command stream and sends each method to its engine: the 3D engine
   (`engines.c`, `maxwell3d.c`), the macro engine (`mme.c`), compute (`compute.c`), the 2D blitter (`fermi2d.c`),
   DMA copies (`dma.c`).
4. When the 3D engine sees a draw, it calls the active renderer through `g_renderer` (`gpu/renderer.h`). The renderer
   reads the GPU's state registers, finds or translates the shaders (`gpu/shader/`, Maxwell SASS to GLSL, then SPIR-V
   for Vulkan), finds or uploads the textures and buffers from guest memory, and draws.
5. Syncpoint increments in the stream tell the game the GPU got past a point. They go through the renderer
   (`stream_incr`), so that results the game can observe, like compute output, are in guest memory first.
6. The game presents by queuing a buffer to vi (`hle/vi.c`), which calls the renderer's `present_buffer`. The
   renderer shows the render target that holds that buffer, or converts the buffer from guest memory when no render
   target does.

### The renderers

`Renderer` in `gpu/renderer.h` is the table of operations the front end calls: draws, clears, blits, compute
dispatches, render target flushes and reloads, syncpoints, presenting. Each renderer fills in all of them.

- **Vulkan** (`gpu/vk/`), the default: dynamic rendering, pipelines keyed by state, guest memory mirrored in buffers
  with write tracking, and compute on its own queue, with results landing in guest memory before anything the game
  can observe.
- **OpenGL** (`gpu/gl/`): the original renderer. It runs on its own GPU thread, with GL calls queued to that thread
  (`glq`). It also has the features Vulkan doesn't have yet: LSFG frame generation, the F7 recorder, the F5 trace and
  F8 screenshots.
- **null** (`gpu/null/`), for the headless build: draws nothing, and finishes each submission at once.

The window backend picks the renderer at startup (`renderer=` in the `.ini`), and falls back to OpenGL when Vulkan
can't start.
