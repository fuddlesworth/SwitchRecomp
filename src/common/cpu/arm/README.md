# Cortex-A57 A32 LLVM backend

This is an initial ARMv8-A AArch32 **user-mode CPU backend**, not an AArch32 guest OS or a complete Cortex-A57 emulator. It emits native LLVM IR, not calls to an instruction interpreter. The `snx_arm_llvm` CMake target is built alongside, and linked by, the existing LLVM backend.

The NX and GNU ABI loaders, module ABI, syscall handlers and thread dispatch currently remain A64-specific. They do **not** automatically run 32-bit applications with this change. Connecting those components to the A32 block ABI is a separate integration step. In particular, never pass an existing `CPU*` to an A32 block.

## Files and API

- `arm.h`: separate, versioned `SnxArmCPU` state and exit reasons, plus the Thumb decode model.
- `a32.c`: validated A32 decoder and explicit libgccjit unsupported stub. No LLVM dependency.
- `thumb_decode.c`: validated Thumb16/Thumb32 decoder to an explicit operand model.
- `llvm.h`, `llvm.c`, `llvm_vector.inc`, `llvm_thumb.inc`: `snx_arm_llvm_block(module, name, words, count, pc, &consumed)` and `snx_thumb_llvm_block(module, name, halfwords, count, pc, &consumed)` emit `void name(SnxArmCPU*)` into a **host-target** LLVM module.
- `tests/arm_emit.c`, `tests/thumb_emit.c`: object generators demonstrating module creation, verification, optimization and native object emission.
- `tests/arm_test.py`, `tests/arm_vector_test.py`, `tests/thumb_test.py`: native execution witnesses and optional independent Unicorn comparisons.

Input words are host integers decoded from little-endian guest code (A32 words, or Thumb halfwords). A block consumes at most `count` instructions (or halfwords for Thumb) and stops at a control transfer, including a conditional transfer. `consumed` reports the compiled length. Bad input returns NULL before changing the module.

Initialize `r[15]` to the block's entry address, `cpsr` to user mode (`0x10`), and `mem`/`mem_size` to a flat guest mapping. Zero the exclusive monitor. The memory base must be at least eight-byte aligned. `mem_size` describes actual accessible storage, not a virtual reservation containing inaccessible holes. The mapping and CPU state must not overlap. Initialize the TLS fields from the guest thread. No A64 `SNX_ABI` bump is needed because this state is not part of that ABI.

On return:

- `exit == SNX_ARM_OK`: `r[15]` is the next instruction address. Look up its block and check `cpsr & SNX_ARM_T` before dispatching.
- SVC/BKPT: `syndrome` contains the immediate and `r[15]` points past the instruction. The caller handles the event.
- Undefined instruction: `syndrome` contains the instruction word. The PC stays at the faulting instruction.
- Data/alignment abort: `syndrome` is the address. The PC stays at the faulting instruction. Bounds checks precede memory accesses and register writeback.
- Bad entry or unsupported instruction state: execution stops explicitly.

`r[15]` stores the actual PC. Operand reads synthesize PC+8 in A32 (PC+4, word-aligned, in Thumb). Branches, ALU writes to PC and loads to PC implement AArch32 interworking and preserve the Thumb state bit. An A32 block rejects a Thumb entry state (and vice versa) as a bad entry: the dispatcher selects the block form from `cpsr & SNX_ARM_T`. Interworking branches set or clear `cpsr & SNX_ARM_T` before returning so the dispatcher can pick the next block's ISA.

Use bounded blocks for scheduling and polling. The caller owns instruction fetching, executable-page validation, block lookup, object loading, code-cache invalidation, syscall dispatch, and clearing monitors on context switches. For example, the existing `SnxIr` module/object utilities can host these functions, but its A64 function dispatcher cannot call them.

## Implemented instructions

- Word/byte LDR and STR, immediate and shifted-register offsets, positive/negative offsets, pre/post indexing, writeback and unprivileged aliases.
- LDRH, STRH, LDRSB, LDRSH, LDRD, STRD, including literal loads and applicable unprivileged aliases.
- LDM/STM IA, IB, DA, DB, PUSH/POP, and loads of PC. Register sources and writeback use the original base.
- LDREX/STREX byte, halfword, word and doubleword, CLREX, and ARMv8 LDA/STL and LDAEX/STLEX variants.
- DMB, DSB, ISB and PLD/PLDW/PLI prefetch hints.
- All sixteen ordinary data-processing opcodes, including ADC/SBC/RSC and tests/comparisons, with NZCV semantics and conditional execution.
- Immediate rotation, LSL/LSR/ASR/ROR/RRX, including register-shift amounts zero, 32, above 32 and multiples of 32.
- MOVW/MOVT, MUL/MLA/MLS, signed/unsigned long multiplies and accumulates, SDIV/UDIV.
- CLZ, REV/REV16/REVSH/RBIT, scalar byte/halfword extends and extend-adds, UBFX/SBFX/BFI/BFC.
- B, BL, BX, BLX immediate/register, SVC and BKPT.
- MRS CPSR, MSR APSR flag fields, and user TPIDRURW/TPIDRURO accesses through MRC/MCR.

## VFP and Advanced SIMD

Implemented for the Cortex-A57 A32 instruction set as ARMv8-A defines it, with exact raw-bit semantics rather
than host floating point:

- VFP scalar F32/F64: VADD, VSUB, VMUL, VDIV, VMLA, VMLS, VNMLA, VNMLS, VNMUL, VFMA, VFMS, VFNMA, VFNMS,
  VMOV immediate/register, VABS, VNEG, VSQRT, VCMP/VCMP with NaN behaviour, and FPSCR access through VMRS/VMSR
  and core register moves.
- VFP conversions and rounding: VCVT between F16/F32/F64 and signed/unsigned integers, fixed-point `#fbits`
  conversions, VRINTA/N/P/M/Z/R/X, VCVTA/N/P/M, and half-precision VCVTB/VCVTT.
- Advanced SIMD three-same, three-different, two-register, across-lane, element and structure groups: integer
  add/sub/saturating/halving, min/max/compare, multiply and long multiply, shifts (immediate and register),
  absolute/negate, permutes (VTBL/VTBX, VEXT, VTRN/VZIP/VUZP/VSWP), VDUP, and FP32 lanes with per-lane
  FPSCR semantics.
- FP approximation instructions VRSQRTE/VSQRTE/VRECPE/VRSQRTS/VRECPS and the integer urecpe/ursqrte forms,
  using ARM's estimate algorithms.
- VLDn/VSTn structure loads and stores (1 to 4 registers, single/multiple elements, all lanes, replicate,
  pre/post-indexed with writeback), VLD/VSTM/VLDR/VSTR, and core-to-lane and lane-to-core moves.
- AES (AESE/AESD/AESMC/AESIMC), SHA1 and SHA256 (SHA1H/P/M/C/SU0/SU1, SHA256H/H2/SU0/SU1), and polynomial
  multiply VMULL.P8/P64, all through portable reference implementations. No AES-NI or CLMUL is used here.

FP exception flags (IOC, DZC, OFC, UFC, IXC, IDC) and FPSCR cumulative state follow ARM's definitions, including
flush-to-zero, default-NaN, and alternate-half-precision modes. Rounding modes RNE, RP, RM and RZ are honoured
per lane. Reciprocal step instructions round the product before subtracting, as ARMv8-A specifies, which is
not the same as a fused step.

Thumb16 and Thumb32 are supported (see `thumb_decode.c`, `llvm_thumb.inc`): integer data processing, loads/stores (immediate, literal, register, pre/post-indexed, unprivileged), push/pop, LDM/STM, branches (B/Bcc/BL/BLX/BX, CBZ/CBNZ, TBB/TBH), bitfield (SBFX/UBFX/BFI/BFC), extends, MOVW/MOVT/ADDW/SUBW, MUL/MLA/MLS, wide data-processing with shifted and modified-immediate operands, and IT blocks (conditions applied per instruction against current flags, as ARMv8-A specifies). Thumb32 coprocessor (VFP/NEON) shares the A32 vector emitter.

## Limits and memory model

- ARMv8.1+ extensions that Cortex-A57 lacks (FP16
  arithmetic, dot-product, RDM) and privileged VFP state are not implemented. libgccjit is an explicit stub for
  both A32 and Thumb. Unsupported and rejected
  unpredictable encodings trap rather than using an A64 fallback.
- Little-endian Normal guest memory only. CPSR.E is rejected. Host-endian conversion is separate from the guest byte order.
- Effective addresses wrap at 32 bits before host pointer construction. Transfers outside the supplied flat mapping, or crossing its end, abort. Unaligned ordinary scalar accesses are supported. Multiple transfers, doubleword transfers, PC loads and atomics enforce their alignment constraints.
- LDM/STM and doubleword bounds are checked as a group before any transfer. This is a deterministic flat-memory fault policy, not emulation of partial transfers on an MMU abort.
- Acquire/release accesses and exclusives use LLVM sequentially consistent atomics. Barrier scopes are conservatively strengthened to a full fence. Hints may complete without waiting.
- The exclusive monitor is value-based CAS, like the existing A64 backend. It detects changed values but not ABA writes by another CPU. Local stores may clear it and STREX always clears it after an attempt. A globally invalidated architectural reservation monitor, device memory and concurrent self-modifying code need runtime integration. This is not a claim of complete multicore memory-model fidelity.
- Scalar loads/stores use LLVM integer accesses with explicit unaligned alignment, not typed C dereferences or byte-by-byte stores. LLVM arithmetic does not use signed-overflow promises. Dynamic shifts and signed division avoid LLVM poison on architectural edge cases.

## Tests

With Clang and LLVM development files installed:

```sh
cmake -S . -B build/a32 -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF
cmake --build build/a32 --target snx-test-arm-decode snx-test-arm-emit
ctest --test-dir build/a32 -R '^arm_' --output-on-failure
```

Optional differential suite, using a local Python environment:

```sh
python3 -m venv .venv
.venv/bin/pip install unicorn==2.1.4
cmake -S . -B build/a32 -DSNX_TOOLS_ONLY=ON -DSNX_BUILD_GNUABI=OFF \
  -DSNX_TEST_ARM_DIFF=ON -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
ctest --test-dir build/a32 -R 'arm_|thumb_' --output-on-failure
```

The suite verifies IR before and after optimization and executes generated objects at O0 and O2. The integer
suite covers 39 witness programs, including an inter-block BL/BX loop, bounds failures, address wrapping,
writeback, PC loads, failed predicates, monitor invalidation, TLS and deferred-backend exits, with 1,081
instruction/block encodings over 64 initial states each. The VFP/NEON suite compares all GPRs, D registers,
CPSR, FPSCR and guest memory for 1,490 encodings over 80 initial states each at O0 and O2 (119,200 exact
comparisons per level), including FP edge cases (NaNs, denormals, infinities, rounding-mode and FPSCR
combinations) and SIMD saturation/shift boundaries. The Thumb suite compares all GPRs, CPSR and guest memory
for 141 instruction/IT sequences at O0 and O2, covering Thumb16 and Thumb32 data processing, loads/stores,
branches, bitfield and IT blocks. Decode tests check both accepted and rejected encodings
against llvm-objdump on cortex-a57. Unicorn's ARMv8-capable AArch32 model is the reference, not Cortex-A57
hardware. Tests use no game assets.
