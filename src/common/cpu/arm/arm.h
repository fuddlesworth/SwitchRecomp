/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_H
#define SNX_ARM_H

#include <stddef.h>
#include <stdint.h>

/* Cortex-A57, ARMv8-A AArch32 user mode, little-endian guest memory.
   Separate from the A64 module ABI: never cast CPU* to SnxArmCPU*. */
#define SNX_ARM_ABI 3 /* shared S/D/Q register bank and FPSCR, plus the bus hook */
#define SNX_ARM_T   (1u << 5)
#define SNX_ARM_E   (1u << 9)

typedef enum {
    SNX_ARM_OK,
    SNX_ARM_UNDEFINED,
    SNX_ARM_THUMB16_UNSUPPORTED,
    SNX_ARM_THUMB32_UNSUPPORTED,
    SNX_ARM_SVC,
    SNX_ARM_BREAKPOINT,
    SNX_ARM_DATA_ABORT,
    SNX_ARM_ALIGNMENT,
    SNX_ARM_BAD_ENTRY,
    SNX_ARM_ENDIAN_UNSUPPORTED,
    SNX_ARM_BACKEND_UNSUPPORTED
} SnxArmExit;

typedef struct SnxArmCPU {
    uint32_t r[16];          /* r15 is the current instruction address, not the pipelined PC */
    uint32_t cpsr;           /* initialize to USR (0x10); NZCV, Q, GE and T are preserved */
    uint32_t exit, syndrome; /* SVC/BKPT immediate, fault address, or unsupported word */
    uint8_t* mem;
    uint64_t mem_size; /* flat mapping, at most 2^32 bytes; no MMIO */
    /* An optional bus hook for the regions whose bus rules a plain access cannot follow: mirrors
       finer than a page, byte write widths. Reads return the value with size_write = the width;
       writes pass size_write = 0x100 | width and value. Like the fetch callback, none is needed by
       default: without one every access goes straight to mem. */
    uint32_t (*io)(struct SnxArmCPU* cpu, uint32_t address, uint32_t size_write, uint32_t value);
    uint64_t exclusive_value;
    uint32_t exclusive_address, exclusive_size, exclusive_valid;
    uint32_t tpidrurw, tpidruro; /* AArch32 user TLS registers, initialized by the runtime */
    uint32_t fpscr;              /* NZCV, QC, AHP, DN, FZ, RMode and cumulative exceptions */
    uint64_t d[32];              /* S(2n) = Dn[31:0], S(2n+1) = Dn[63:32], Qn = D(2n+1):D(2n) */
} SnxArmCPU;

typedef void (*SnxArmBlock)(SnxArmCPU* cpu);

typedef enum {
    SNX_A32_UNDEFINED,
    SNX_A32_DATA,
    SNX_A32_SINGLE,
    SNX_A32_EXTRA,
    SNX_A32_MULTIPLE,
    SNX_A32_BRANCH,
    SNX_A32_BX,
    SNX_A32_BLX_IMM,
    SNX_A32_MOVW,
    SNX_A32_MOVT,
    SNX_A32_MUL,
    SNX_A32_MULL,
    SNX_A32_MLS,
    SNX_A32_DIV,
    SNX_A32_CLZ,
    SNX_A32_REV,
    SNX_A32_EXTEND,
    SNX_A32_BITFIELD,
    SNX_A32_MRS,
    SNX_A32_MSR,
    SNX_A32_EXCLUSIVE,
    SNX_A32_ACQUIRE_RELEASE,
    SNX_A32_CLREX,
    SNX_A32_BARRIER,
    SNX_A32_HINT,
    SNX_A32_SVC,
    SNX_A32_BKPT,
    SNX_A32_TLS,
    SNX_A32_VECTOR
} SnxA32Kind;

/* Reserved, privileged and unpredictable encodings are rejected.
   Decode is independent of LLVM and does not execute an instruction. */
SnxA32Kind snx_a32_decode(uint32_t word);

/* ---- Thumb16/Thumb32 ----
   Decoded to an explicit operand model (unlike A32, whose emitter re-decodes
   its word): Thumb16 uses 3-bit register fields and Thumb32 spreads operands
   over two halfwords, so normalising up front keeps the emitter simple. */
typedef enum {
    TH_UNDEF,
    TH_SHIFT_IMM, /* LSL/LSR/ASR Rd, Rm, #imm */
    TH_ADDSUB,    /* ADD/SUB Rd, Rn, Rm|#imm3; 3-register and immediate */
    TH_IMM8,      /* MOV/CMP/ADD/SUB Rd, #imm8 */
    TH_ALU,       /* AND..MVN and RSB; op in .op */
    TH_HI,        /* ADD/CMP/MOV high registers; BX/BLX register */
    TH_LOAD,      /* all load forms: .flags carries size/sign/addressing */
    TH_STORE,     /* all store forms */
    TH_ADDR,      /* ADR, ADD Rd, SP, ADD/SUB SP */
    TH_EXTEND,    /* SXTB/SXTH/UXTB/UXTH */
    TH_PUSHPOP,   /* PUSH/POP */
    TH_LDMSTM,    /* LDMIA/STMIA */
    TH_BRANCH,    /* B/Bcc/BL/BLX/BX/CBZ/CBNZ; .op selects */
    TH_MUL,       /* MUL/MLA/MLS */
    TH_BITFIELD,  /* SBFX/UBFX/BFI/BFC */
    TH_WIMM,      /* MOVW/MOVT/ADDW/SUBW */
    TH_WDATA,     /* wide data-processing (shifted register) */
    TH_TABLE,     /* TBB/TBH */
    TH_IT,        /* If-Then */
    TH_HINT       /* NOP/YIELD/WFE/WFI/SEV */
} SnxThumbShape;

/* ALU sub-operations for TH_ALU / TH_WDATA / TH_ADDSUB. */
enum {
    ALU_AND,
    ALU_EOR,
    ALU_LSL,
    ALU_LSR,
    ALU_ASR,
    ALU_ADC,
    ALU_SBC,
    ALU_ROR,
    ALU_TST,
    ALU_NEG,
    ALU_CMP,
    ALU_CMN,
    ALU_ORR,
    ALU_MUL,
    ALU_BIC,
    ALU_MVN,
    ALU_ADD,
    ALU_SUB,
    ALU_RSB,
    ALU_ORN
};
/* Branch sub-operations. */
enum { BR_B, BR_BCOND, BR_BL, BR_BLX_IMM, BR_BX, BR_BLX_REG, BR_CBZ, BR_CBNZ, BR_BW, BR_SVC };
/* TH_IMM8 sub-operations. */
enum { I8_MOV, I8_CMP, I8_ADD, I8_SUB };

/* Addressing/store flags for TH_LOAD/TH_STORE. */
#define TH_B      (1u << 0)  /* byte */
#define TH_H      (1u << 1)  /* halfword */
#define TH_SIGN   (1u << 2)  /* sign-extended load */
#define TH_SPB    (1u << 3)  /* base is SP */
#define TH_PCB    (1u << 4)  /* base is PC (literal) */
#define TH_PRE    (1u << 5)  /* pre-indexed (else post-indexed) */
#define TH_UP     (1u << 6)  /* add offset (else subtract) */
#define TH_WB     (1u << 7)  /* write back */
#define TH_S      (1u << 8)  /* set flags */
#define TH_REG    (1u << 9)  /* register offset (else immediate) */
#define TH_RSHIFT (1u << 10) /* TH_WDATA operand shifted by a register */
#define TH_MODIMM (1u << 11) /* TH_WDATA operand is a modified immediate in .imm */

#define TH_F16 1u /* instruction length in halfwords */
#define TH_F32 2u
typedef struct {
    SnxThumbShape shape;
    unsigned op; /* operation within the shape */
    unsigned rd, rn, rm, ra;
    uint32_t imm;
    unsigned flags;
    unsigned len; /* TH_F16 or TH_F32 */
} SnxThumb;

/* Strictly decode one Thumb16 or Thumb32 instruction. hw2 is the second
   halfword (ignored for 16-bit forms). Returns the length in halfwords
   (1 or 2), or 0 for a reserved/unpredictable encoding. */
unsigned snx_thumb_decode(uint16_t hw1, uint16_t hw2, SnxThumb* out);
/* Explicitly unsupported backend, never falls back to the A64 decoder. */
SnxArmExit snx_arm_gccjit_stub(void);

#endif
