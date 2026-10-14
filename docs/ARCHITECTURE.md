# Architecture

ScarletNX AOT: AArch64 -> C -> native module, shared by Horizon HLE/Maxwell
and CPU-only Linux AArch64 ABI runtimes.
Source map: [CLAUDE.md](../CLAUDE.md).

## Runtime boundaries

| Source / CMake target | Ownership |
| --- | --- |
| `src/common/` / `scarletnx-common` | AArch64 CPU state, instruction semantics, code discovery/emission, native module execution, runtime callback interface, host utilities |
| `src/nx/` / `scarletnx-nx` | Horizon SVC/kernel/services/filesystems, NSO/NRO loading, Switch memory, Maxwell and graphics/media backends, Switch frontend/tooling |
| `src/gnuabi/` / `scarletnx-gnuabi` | Linux syscall/errno/structure translation, ELF loading, isolated VFS, Linux process/memory/signal/futex compatibility |

The common target has no runtime-specific imports. NX and GNU ABI each link
common and have no dependency on each other. The GNU executable target is
`scarletnx-gnuabi-run`, whose output is `scarletnx-gnuabi`; its library output is
`scarletnx-gnuabi-runtime`. `SNX_BUILD_NX=OFF` builds GNU ABI without fetching or
linking NX graphics/media packages. CTest checks include ownership and the actual
transitive CMake dependency graph and target source ownership.

`src/common/runtime/interface.h` defines `RtAPI`; the active runtime supplies
the callbacks on `CPU.rt`. NX routes SVC to Horizon. GNU ABI accepts `SVC #0`,
reads Linux syscall number `x8` and arguments `x0` through `x5`, and writes result
`x0`. CPU helpers and instruction emission are shared; Horizon handlers are never
used as Linux syscalls.

`scarletnx-erista`, `scarletnx-mariko`, and `scarletnx-aula` share the existing
`scarletnx` NX implementation through Unix symlinks or Windows executable copies.
The original game-name entry points and CLI remain available. GNU ABI has an
independent CPU-only entry point; see [GNUABI.md](GNUABI.md) for implemented
syscalls, ELF execution, filesystem scope, and differential testing.

## Recompiler + module ABI

NX tooling under `src/nx/runtime/driver/` and `src/nx/loader/`:
`work/<Game>/{exefs,romfs}` -> `games/<Game>[.exe]`, `<Game>_code.{so,dll}`, `<Game>_data/`:

- `driver.c`: CLI dispatch only; `extract.c` and `recompile.c` own their workflows.
- `src/nx/loader/loader.c`: NSO/NRO, LZ4, SHA-256, NPDM, MOD0, symbols/relocs/FDEs.
- `src/common/cpu/aarch64/a64.c` + `a64_templates.inc`: shared instruction semantics for AOT and interpreter.
- `src/common/cpu/recompiler/codegen.c`: function/CFG discovery over platform-neutral image views.
  `src/nx/loader/codegen_view.c` adapts NX images; `module.c` holds what the backends share (the dispatch entry
  table, chunk labels, progress lines); chunks batch whole functions up to `SNX_CHUNK_INSTRUCTIONS`, 90000.
- `toolchain.c`: the runtime build for deployment.
- Code generators (`--backend`, `src/common/cpu/recompiler/backend.h`): `llvm` (the default and the recommended
  one; `src/common/cpu/recompiler/llvm/`, LLVM a required dependency) builds LLVM IR with the LLVM-C API: `ir_a64.c`, `ir_simd.c` and `ir_fp.c` emit each
  `a64_templates.inc` case as the IR of the C it holds, `ir_codegen.c` the functions of `codegen.c`, and
  `src/nx/loader/module_llvm.c` the chunks and `SnxModule` of `module.c`. It links the objects itself (`link_elf.c`,
  `link_coff.c`) with `rtlib.c`, the helpers and interpreter compiled when the recompiler is built, into a module
  with no imports, for the host it runs on: x86-64 (ELF or COFF) or AArch64 (ELF, with a veneer for a branch past
  128 MiB). `tests/llvm_differential.c` runs both backends' step functions on random
  states (every pattern, failing if any instruction falls back to the interpreter's `snx_step`), `tests/llvm_module.c`
  a whole program built by both, `tests/llvm_bench.c` times both on assembly kernels. `gcc` (libgccjit,
  optional, built when libgccjit is found: `cmake/gccjit.cmake`, `SNX_GCCJIT_ROOT`) is for DEC Alpha EV67 hosts,
  which LLVM doesn't generate code for; everywhere else `llvm` is the backend to use. Ported on x86-64 for now:
  `src/common/cpu/recompiler/gccjit/jit.c` emits `ir_codegen.c`'s control flow, and `jit_a64.c`, `jit_fp.c` and
  `jit_simd.c` each instruction as the LLVM emitters do (cpu.h's helpers written out for the word's constant
  fields, SIMD a lane at a time, no host intrinsics: GCC picks the instructions with `-march=native`). Guest
  accesses are relaxed atomics (libgccjit 14 drops volatile on dereferences); `src/nx/loader/module_gccjit.c`
  compiles each chunk to an object and lets GCC's driver link the module with the chunks and the runtime library.
  `llvm_differential --gccjit` (also failing on any `snx_step` fallback) and `llvm_module` check it against the
  other backends. On `llvm_bench` it matches LLVM on integer, FP and calls; SIMD is several times slower (scalar
  lanes, GCC's SLP vectorizer off: it packs scalar code's CPU fields into vectors).
- Both backends poll `CPU.poll` on backward branches inside a function and at the `D_` dispatch, as `codegen.c`'s C
  does, so a loop without SVCs gives its core to a waiting thread of higher priority.
- The LLVM backend's speed: guest memory and the CPU structure are separate alias scopes (registers stay in host
  registers) and guest accesses are volatile (each happens, as on the hardware); logical immediates and SIMD
  immediates are folded; FP arithmetic, compares and conversions run inline on the host's IEEE unit when the result
  is provably the soft-float helper's, flags included (`ir_fp_fast`); common SIMD (loads/stores, permutes, shifts,
  compares, byte ops, saturating and halving adds) is inline instead of `snx_simd`. Build == host == target: the
  generated code uses the host's extensions (`snx_ir_host`, from LLVM's host detection) and the runtime library is
  built with `-march=native`, its helpers choosing paths from the compiler's feature macros; nothing is detected at
  run time. x86 paths sit under `__x86_64__` or the target triple: AES-NI, PCLMULQDQ (PMULL, CRC32 by Barrett
  reduction), SSE4.2 CRC32C, VPTERNLOG (BSL/BIT/BIF, signed saturation), PAVGB/PAVGW (rounding halving adds, signed
  ones biased by 0x80), GF2P8AFFINEQB (8-bit shifts, bit reversal); the tricks are wunkolo's (wunkolo.github.io).
- `src/common/utilities/common.c` and `process.c`: host files/processes; argv-based subprocesses.
  NX extraction retains its isolated key environment.

Host tooling uses C90 syntax with SDK fixed-width types, formatting and OS APIs.
The runtime and generated guest code retain their existing Clang/ABI requirements.
Normal builds link the driver into the runtime; `SNX_TOOLS_ONLY` builds the same CLI independently.

Shared CPU ABI (`src/common/cpu/aarch64/`): guest functions take `CPU*`; `snx_lookup` dispatches indirect branches, with interpreter fallback.
`CPU.rt` supplies `RtAPI` callbacks: SVC, UNDEF, BRK, hints, CNTVCT, FPCR, SEV/WFE; no named runtime imports.
`cpu_helpers.c` + `helpers/`: SIMD, crypto, FP.

GNU supplies an optional fetch callback that checks execute permissions and
compares the live instruction against the compiled word. Modified code falls
back to shared interpreter semantics. NX leaves this callback unset and keeps
its existing execution path.

NX module ABI is in `src/nx/loader/module_abi.h`; the shared CPU layout is in
`src/common/cpu/aarch64/cpu.h`. Rebuild game modules after shared ABI changes;
bump `SNX_ABI` for ABI changes. Version mismatches reject loading.

## Runtime ownership

| `src/nx/` directory | Responsibility |
| --- | --- |
| `runtime/` | Module loading, INI, clock, `RtAPI`, crashes |
| `hle/kernel/`, `hle/svc/`, `memory/` | Objects, threads, sync, SVC, Switch memory, IPC |
| `hle/services/`, `hle/filesystem/` | Services, nvdrv, guest filesystem services |
| `gpu/` and `gpu/maxwell/` | GPFIFO, Maxwell engine implementation, shaders, Vulkan renderer |
| `gpu/video/` | host1x, NVDEC/FFmpeg, VIC |
| `runtime/platform/` | Host APIs, SDL3/headless, input, screenshots, timed exit |

### Guest exec

Guest thread = host thread + `CPU` + TLS. `hle/kernel/thread.c`: one owner/core, four cores; release on kernel block,
sleep/yield, IPC, WFE. Waiters: priority then FIFO; a waiter of higher priority takes the core at the holder's next SVC
or backward branch, any taken branch in the interpreter (`CPU.poll`, `RtAPI.poll`). A ready thread that would wait
behind its core's holder runs on an idle core of its mask, and an idle core takes such a waiter (only
`svcSleepThread(0)` keeps a yielding thread on its core). A mutex owner runs at its best waiter's priority
(`hle/kernel/sync.c`).

`memory/mem.c`: one guest VA reservation; `GP(addr) = g_mem + addr`, accessed directly by generated code.

SVC -> `hle/svc/svc.c`; `svcSendSyncRequest` -> `hle/kernel/ipc.c` -> session's HLE service object.

[Filesystem](FILESYSTEM.md): startup scan; indexed lookup, metadata, enumeration, mutations.

### GPU submit/present

1. NVN command buffers -> `/dev/nvhost-gpu` GPFIFO ioctl -> `hle/services/nvdrv.c` -> `gpu_submit` (`gpu/gpu.c`).
2. Copy entries to GPU queue; `nv.submit_lk` orders fence promises and cross-channel submits.
   Promised GPU-channel fences wait on the GPU thread; host1x/CPU/unpromised fences wait on the submitter.
3. GPU-thread dispatch: 3D, MME, compute, 2D, DMA, inline-to-memory. Draws -> `g_renderer` (`gpu/renderer.h`).
   Shaders: Maxwell SASS -> SPIR-V (`gpu/shader/`).
4. Vk `stream_incr` lands compute writes in guest memory before incrementing the syncpoint.
5. vi waits on unpromised fences, then queues present after preceding submits. GPU thread checks remaining fences;
   `present_buffer` uses matching RT + vi crop, or converts guest memory.

### Renderer coherence + lifetime

- **Vk**: Vulkan 1.3 in windowed or headless mode, with guest buffer mirrors; separate compute queue when available,
  graphics queue otherwise.

Invalidation: host write tracking + `mem_unmap` -> `gpu_mem_changed`. Vk treats zeroed unmap/heap-return ranges
as written at the next cache check.

`Renderer.save`: optional pipeline-cache persistence every 300 frontend ticks (~5s) + exit, concurrent with execution.
Exit skips renderer teardown and guest-thread joins.

### Videodec/VIC channels

Each NVDEC/VIC fd owns a `host1x_open` channel, registers and syncpoints; NVDEC also owns a decoder.
NVDEC writes guest surfaces; VIC composites output.
