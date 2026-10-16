<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Nintendo 3DS port

## Status: stage 1 + stage 2a + initial stage 2b

`scarletnx-3ds` can extract **decrypted application CIAs**. It is not yet a
CIA-to-native compiler or a working 3DS runtime. Stage 2 is split into extraction/
memory (2a, implemented) and ARM11 AOT integration (2b, in progress). The first
2b increment adds an opt-in ISA profile and checked-memory ABI with static AOT
witnesses, not a production extract/compile/run pipeline. Stages 3–6 remain pending. No firmware, keys, game assets, Azahar CPU JIT/interpreter, DSP or
PICA renderer are included.

Implemented:

- Stage 1: `SNX_BUILD_3DS`, independent driver, Old/New 3DS board profiles,
  physical-range bounds checks, lifecycle/base ticks, GUI CIA drops/browser/
  recents and strict routing to the 3DS driver without Switch-only flags.
- `src/3ds/loader/`: streaming CIA sections; signature-dependent ticket/TMD
  offsets; title, boot-index, content bitmap, unique ID/index, content-info and
  SHA-256 validation. Boot content is selected by its TMD index, not table order.
  All present contents and container metadata are materialized on disk.
- Decrypted application NCCH/ExHeader, checked process segment/BSS metadata,
  ExeFS files and hashes, bounded backward LZSS/BLZ `.code` expansion, and IVFC
  level-3 RomFS extraction into a host tree. Traversal/invalid UTF-16, metadata
  directory/file traversal cycles, overlaps, duplicate paths and excessive expansion are rejected.
- Shared NX host utilities (`common` filesystem paths, file IO, private staging,
  cleanup, publication) are reused; the NX kernel/NSO loader/FS service are not
  linked. Shared SHA-256 now also supports incremental hashing.
- `src/3ds/memory/ctr_memory.h`: independent page-based address spaces, shared
  physical RAM/VRAM/DSP backings, aliases, R/W/X checks, checked cross-page RAM
  transfers and explicit aligned MMIO callbacks. Failed map/protect/unmap and
  RAM range-validation operations do not partially change guest memory/mappings.
  No flat host pointer is exposed.
- Initial 2b: shared A32 codegen has independent, explicitly selected ARM11 ISA
  and checked-memory options. Existing A57 APIs/defaults and `SnxArmCPU` ABI 2
  remain unchanged. No 3DS global compiler define affects other boards.
- `src/3ds/cpu/ctr_cpu.h`: versioned checked CPU state, USR initialization and
  callbacks connecting statically generated integer instructions to the page
  controller, including aliases, R/W/X, whole-range multiword RAM validation and
  ordinary MMIO. Every instruction checks execute permission and code identity.
  Static O0/O2 native witnesses execute against this adapter without LLVM/JIT
  linked into their executables. **Automatic process setup and CIA execution
  are not yet connected.**

```sh
build/3ds/scarletnx-3ds -x --output work/my-title game.cia
```

The output is a **new** directory containing `contents/*.app`, `exefs/` (with
expanded/page-padded `.code`), `romfs/`, raw container/NCCH/ExHeader metadata and
`manifest.ini` with source/code hashes and segment layout. The default directory
is `work/3ds/<title-id>-<full-input-sha256>` of the data folder the Switch driver uses
(the checkout, a portable copy's folder, else the user's data folder; `--repo DIR` names
another), which an installed copy on Linux keeps in `~/.cache/scarletnx/work/3ds/`. No source CIA needs to be mounted by
the future guest FS. Extraction is privately staged, then published only after
all checks succeed; failures remove staging files. Existing destinations are
reused only when their format-1 manifest has all required fields and records a completed extraction with the
same input SHA-256, title ID and version, the extracted code matches its recorded
SHA-256, and the extraction directories are present without symlinks. Incomplete
or mismatched outputs are refused, never overwritten or repaired. This reuses
completed extraction only; interrupted extraction resume and native-module cache
validation are still stage-2b work. Keep the input and output parent trusted and
not concurrently modified; extraction is not a sandbox against hostile host
processes racing parent directory changes.

`-x`/`--extract` stops after extraction. Without it the driver extracts, then
returns an explicit unsupported error **before** compilation or guest execution.
The GUI follows that default path; it cannot run a title yet. `--model old|new`
selects the board profile (default Old); it does not change the extracted code.
`--board-info` prints physical regions without allocating RAM. Exit codes:

- 0: help, board information, or successful extract-only operation.
- 1: malformed/inconsistent data, IO/limit failure, or incomplete/mismatched existing destination.
- 2: invalid CLI arguments or missing/non-CIA file path.
- 3: unsupported encrypted/type variant, or missing ARM11 AOT/execution stage.

### Current limits and trust

Only decrypted application CIA/NCCH content is supported, not encrypted layers,
updates, DLC, system titles or nonstandard NCCH media units/platforms. Input and
total materialized output are each capped at 1 GiB; metadata tables at 16 MiB,
code at 64 MiB, entries at 100000, and RomFS nesting at 64. Host filenames must
be portable safe components (maximum 255 UTF-8 bytes); unsafe/reserved names
are rejected rather than silently renamed. Host paths with control characters
are refused to prevent log/banner injection. Larger/unsupported inputs fail closed.

**Hashes are integrity checks, not signature authentication.** RSA/ECDSA
signatures, certificate trust, ticket rights and ExHeader access-descriptor
signatures are not authenticated. RomFS superblock and entire CIA-content
hashes are checked, but the internal IVFC hash tree is not separately verified.
No untrusted extracted code is executed. Future AOT/HLE must not mistake these
checks or an editable extraction manifest for authenticated console permissions.

### Build and test

With the normal Clang/CMake prerequisites (no LLVM SDK, SDL or media dependencies
needed for the extraction/memory-only configuration):

```sh
cmake -S . -B build/3ds -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF
cmake --build build/3ds
ctest --test-dir build/3ds --output-on-failure
build/3ds/scarletnx-3ds --model new --board-info
```

Use `-DSNX_BUILD_GUI=ON` to include the launcher and its regression tests; SDL3
is then required/fetched as usual. `-DSNX_BUILD_3DS=OFF` omits the driver (the
GUI diagnoses its absence). The profile decoder test needs no LLVM. Additional
static AOT tests are explicitly opt-in:

```sh
cmake -S . -B build/arm11 -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF \
  -DSNX_BUILD_GUI=OFF -DSNX_TEST_ARM_AOT=ON
cmake --build build/arm11
ctest --test-dir build/arm11 --output-on-failure
```

This requires a native LLVM SDK (select with `SNX_LLVM_CONFIG` if needed).
`SNX_TEST_ARM_DIFF=ON` additionally requires Python `unicorn==2.1.4`, and enables
ARM11 MPCore plus shared A57 integer/VFP/NEON/Thumb regressions. See
`src/common/cpu/arm/README.md` for the codegen contract and exact limitations.
The CLI behavior is unchanged even when these test options are enabled.

## Source provenance and licensing

The initial shallow checkouts were:

- ScarletNX: `a9258f1eaa3a975f84ad2216950b3f62ddc92ba0`.
- Azahar: `68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e`.

Stage 1 uses physical-layout facts from Azahar `src/core/memory.h`, the tick
constants from `src/core/core_timing.h`, and the system-initialization design in
`src/core/core.cpp`. Stage 2a references `src/core/file_sys/{cia_container,
title_metadata,ticket,signature,ncch_container}.*`, `src/core/loader/ncch.cpp`
and `src/core/hle/romfs.cpp`. The bounded BLZ implementation credits the
Citra/Azahar algorithm; the extractor and page controller are new C code.
Initial 2b CPU/profile and bus code is new project C code using ARMv6K/legacy
Thumb architectural semantics; it does not copy an Azahar CPU engine. The
shared emitter retains its existing notices. LLVM's `mpcore` assembler model
and Unicorn 2.1.4's ARM11 MPCore model are test references, not runtime engines.
Azahar is not a build dependency.
Azahar's root license is GPLv3; individual files carry GPLv2-or-later or
GPLv3-or-later notices. Future adaptations must retain each source's notices
and record its origin/revision here; select the GPLv3-compatible terms where
applicable. Audit third-party/external components separately rather than assuming
that every dependency has the root license.

## Boundaries and prerequisites for stage 2b

The 3DS application CPU is ARM11/ARMv6K with VFP, **not** the Cortex-A57
ARMv8-A AArch32 default in `src/common/cpu/arm/`. That backend's existing
user-mode API still uses a single flat `mem`/`mem_size` range.
Mapping a sparse 3DS address space into that ABI would make holes accessible and
would bypass process permissions, aliases and device callbacks.

Before running guest code:

1. The initial ARM11 integer profile is implemented. Complete VFPv2 (including
   short-vector FPSCR behavior), missing ARMv6K instructions and alignment-mode
   validation. Current ordinary unaligned transfers model unaligned support
   enabled/alignment checking disabled, not every ARM11 CP15 configuration.
2. The separate checked-memory ABI and CTR adapter are implemented for integer
   transfers. Complete architectural reservation monitors/exclusives: these
   currently stop as unsupported, as do VFP/vector transfers and checked Thumb
   table branches. Do not pass an A64 `CPU*` or an NX module to an A32 block.
3. Define/version the CTR native-module ABI and AOT block discovery/dispatch,
   including ARM/Thumb state, indirect branches, execute checks and scheduling.
   No silent interpreter or runtime-JIT substitution for static recompilation.
4. Keep all CTR code under `src/3ds/`, with only `common` dependencies. Extract
   reusable host FS/tree operations from NX into common rather than linking the
   NX kernel/services/Maxwell runtime into CTR. Keep 3DS archive IDs, permissions,
   title identity and saves in the CTR adapter. Architecture tests enforce the
   runtime boundary.

The page controller is now connected through an opt-in `SnxArmCheckedCPU`
callback ABI, while the original CPU block/state ABI is deliberately unchanged.
The adapter is not linked to the legacy execution runner. The A32 runner in `src/common/cpu/arm/exec.c` uses MCJIT and is **not** an acceptable
substitute for static recompilation. Stage 2b must emit/load native modules ahead
of execution, route every memory instruction through the checked ABI, and use an
ARM11 ISA profile.

The tick domain remains 268,111,856 Hz, with 4,481,136 base ticks per frame, as
in Azahar. New 3DS CPU scaling is a separate future scheduler concern. There is
still no CIA application execution, interrupt scheduling, ARM9 emulation or boot
ROM; only asset-free statically generated CPU witnesses execute so far.

## Staged roadmap (2a implemented; 2b partially implemented)

Each stage should be one independently reviewable commit/diff, with all earlier
stages as its base. Stubs must return explicit unsupported errors, not pretend
that an unimplemented service/device succeeded.

### 2. CIA extraction, memory controller and static execution

The extraction and independent memory-controller subset is implemented as 2a.
Initial 2b supplies ARM11 integer profile selection, checked-memory integration
and static native-object witnesses. There is no CTR module ABI/loader, production
ARM/Thumb block discovery or indirect dispatch, process/stack/TLS setup from
ExHeader, validated resume/cache, or end-to-end extract/AOT/run witness yet.
The complete stage is **not finished** until the ARM11/memory/module integration
and end-to-end extract/AOT/run gate below pass.

Reference Azahar `src/core/file_sys/{cia_container,ticket,title_metadata,
ncch_container,romfs_reader}.*`, `src/core/hle/service/am/am.cpp` (installation),
`src/core/loader/ncch.*`, `src/core/memory.*` and kernel process/VM code.

- CIA is an installable container, not directly executable code. Validate all
  section sizes/alignment, signature-dependent ticket/TMD body offsets, content
  bitmap/indices and hashes; select the application NCCH, not the first content
  unconditionally. Distinguish updates/DLC, absent contents and encrypted layers.
- Implement the ARM11/memory/module prerequisites above. Split NCCH ExHeader,
  ExeFS `.code` (including BLZ), RomFS, segment permissions, BSS and TLS. Preserve
  metadata needed for process setup. Fail closed for unsupported crypto/formats;
  accept user-supplied keys only, never distribute keys/firmware.
- Always materialize extracted content into `work/3ds/<title>/...`, using the
  shared NX-style host filesystem utilities. Extract RomFS safely into a tree;
  no mount-on-demand of the original CIA. Reject traversal, duplicates, malformed
  offsets, oversized/decompression-bomb inputs and symlink escapes.
- Pipeline: **extract -> recompile -> run**. Reuse stage supervision and shared
  FS/tree logic, not NX's NSO/A64 loader. Publish staged extraction/compiler
  outputs atomically. Cache identity includes input hashes, keys/configuration
  identity without storing secrets, ISA, module ABI and compiler version; never
  reuse an incomplete build. Launch only compatible native modules.
- Gate: synthetic CIA/NCCH/RomFS fixtures, malformed/truncated/overflow cases,
  memory permissions/alias/MMIO tests, ARM/Thumb native witness, resume/cache
  invalidation and an asset-free end-to-end extract/AOT/run test.

### 3. Basic HLE OS and a SwIPC-derived generator

Reference Azahar kernel/SVC/IPC code and `src/core/hle/service/`. Use
`tools/swipc/swipc.c` as the parser/schema/codegen basis, with a **separate CTR
wire adapter**: 3DS TLS command words, normal/translation counts, handles,
static/mapped buffers and response layout are not Switch HIPC/CMIF.

- Threads, handles, events, waits, scheduling, SVCs, TLS, shared memory and result
  codes; checked guest buffers and handle lifetimes at every IPC boundary.
- Start with `srv:`, `fs:USER`, `APT:U`, configuration/time and resource services;
  implement shared-memory/event plumbing needed by later DSP/GSP/HID stages.
- Gate: generated dispatch against hand-encoded requests, malformed descriptors,
  unknown commands, handle transfer/close, IPC-buffer bounds, blocking/wakeup,
  process exit and a small homebrew OS/filesystem witness.

### 4. Audio DSP

Reference `src/audio_core/{dsp_interface.*,hle/*}` and
`src/core/hle/service/{dsp,csnd}/`. Port HLE DSP shared memory, voices, decoding,
resampling, mixing/effects, pipes/semaphores and interrupt events; adapt output to
ScarletNX's host audio boundary. Do not substitute silence for implemented DSP.
LLE/firmware behavior needs a separately licensed/dependency-audited integration.
Gate: asset-free PCM/ADPCM decoding/mixing golden tests, pipe/event lifecycle,
ring-buffer wrap, sample-rate timing and deterministic offline rendering.

### 5. Video graphics

Reference Azahar GSP/GPU/LCD, `src/video_core/pica/`, shader and software renderer
first, then Vulkan as an acceleration path. PICA200 is **not Maxwell**.
Implement GSP shared queues/interrupts, GPU command lists, physical-memory access,
DMA/fills/display transfers, texture formats, shader behavior, framebuffer layout
and top/bottom screen presentation. Integrate renderer cache invalidation with
CPU/DSP memory writes rather than copying the NX GPU frontend.
Gate: deterministic software-rendered golden scenes, GPU command/memory bounds,
transfer/coherency tests, two-screen layouts, then Vulkan comparisons.

### 6. HID and remaining peripherals

Reference `src/core/hle/service/{hid,ir,cam,mic,nfc,ptm,mcu,qtm}/` and the Azahar
frontend/input interfaces. Wire host input to 3DS shared-memory sample rings,
pad/circle-pad/touch, accelerometer/gyro, IR/New 3DS inputs and events with correct
timestamps and screen-coordinate transforms. Camera/microphone/NFC and lid/power
policies require explicit support or documented unsupported results; never claim
that every peripheral is wired when only button input works.
Gate: injected input/touch tests, ring wrap/events, disconnect/reconnect, New/Old
model differences, camera/mic bounds and end-to-end homebrew interaction.
