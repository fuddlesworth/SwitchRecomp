/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "arm.h"

static unsigned thumb16(uint16_t h, SnxThumb* o) {
    unsigned g = h >> 11;
    o->len = TH_F16;
    o->flags = 0;
    o->op = 0;
    o->ra = 0;
    if (g <= 2) { /* LSL/LSR/ASR Rd, Rm, #imm5 */
        o->shape = TH_SHIFT_IMM;
        o->op = g == 0 ? ALU_LSL : g == 1 ? ALU_LSR : ALU_ASR;
        o->rd = h & 7;
        o->rm = (h >> 3) & 7;
        o->imm = (h >> 6) & 31;
        o->flags = TH_S;
        return TH_F16;
    }
    if (g == 3) { /* ADD/SUB Rd, Rn, Rm|#imm3 */
        o->shape = TH_ADDSUB;
        o->op = (h >> 9) & 1 ? ALU_SUB : ALU_ADD;
        o->rn = (h >> 3) & 7;
        o->rd = h & 7;
        if ((h >> 10) & 1)
            o->imm = (h >> 6) & 7;
        else {
            o->rm = (h >> 6) & 7;
            o->flags = TH_REG;
        }
        o->flags |= TH_S;
        return TH_F16;
    }
    if (g >= 4 && g <= 7) { /* MOV/CMP/ADD/SUB Rd, #imm8 */
        o->shape = TH_IMM8;
        o->op = g - 4;
        o->rd = (h >> 8) & 7;
        o->imm = h & 255;
        return TH_F16;
    }
    if ((h >> 10) == 16) { /* data-processing register */
        o->shape = TH_ALU;
        o->op = (h >> 6) & 15;
        o->rd = h & 7;
        o->rn = h & 7;
        o->rm = (h >> 3) & 7;
        o->flags = TH_S;
        return TH_F16;
    }
    if ((h >> 10) == 17) { /* special data / branch exchange */
        unsigned op = (h >> 8) & 3, hd = (h >> 7) & 1, hs = (h >> 6) & 1;
        if (op == 3) { /* BX / BLX register: Rm in bits[6:3], L in bit 7 */
            if (h & 7) return 0;
            o->shape = TH_BRANCH;
            o->op = (h >> 7) & 1 ? BR_BLX_REG : BR_BX;
            o->rm = (h >> 3) & 15;
            return TH_F16;
        }
        o->shape = TH_HI;
        o->op = op; /* 0=ADD 1=CMP 2=MOV */
        o->rd = (h & 7) | (hd << 3);
        o->rm = ((h >> 3) & 7) | (hs << 3); /* Rm */
        o->rn = o->rd;                      /* ADD reads Rd as its first source */
        return TH_F16;
    }
    if (g == 9) { /* load literal: PC-relative word load (Thumb format 9, needed by every Thumb ROM) */
        o->shape = TH_LOAD;
        o->rd = (h >> 8) & 7;
        o->rn = 15;
        o->flags = TH_PCB | TH_UP | TH_PRE;
        o->imm = (h & 255) * 4;
        return TH_F16;
    }
    if (g == 10 || g == 11) { /* load/store register offset */
        unsigned sub = (h >> 9) & 7;
        o->shape = (sub >= 3) ? TH_LOAD : TH_STORE;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        o->rm = (h >> 6) & 7;
        o->flags = TH_REG | TH_UP;
        switch (sub) {
            case 1: o->flags |= TH_H; break;
            case 2: o->flags |= TH_B; break;
            case 3: o->flags |= TH_B | TH_SIGN; break;
            case 5: o->flags |= TH_H; break;
            case 6: o->flags |= TH_B; break;
            case 7: o->flags |= TH_H | TH_SIGN; break;
        }
        return TH_F16;
    }
    if (g >= 12 && g <= 15) { /* load/store word/byte immediate */
        o->shape = (h >> 11) & 1 ? TH_LOAD : TH_STORE;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        o->flags = ((h >> 12) & 1 ? TH_B : 0) | TH_UP | TH_PRE;
        o->imm = ((h >> 6) & 31) * (o->flags & TH_B ? 1 : 4);
        return TH_F16;
    }
    if (g == 16 || g == 17) { /* load/store halfword immediate */
        o->shape = g == 17 ? TH_LOAD : TH_STORE;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        o->flags = TH_H | TH_UP | TH_PRE;
        o->imm = ((h >> 6) & 31) * 2;
        return TH_F16;
    }
    if (g == 18 || g == 19) { /* SP-relative load/store */
        o->shape = g == 19 ? TH_LOAD : TH_STORE;
        o->rd = (h >> 8) & 7;
        o->rn = 13;
        o->flags = TH_SPB | TH_UP | TH_PRE;
        o->imm = (h & 255) * 4;
        return TH_F16;
    }
    if (g == 20 || g == 21) { /* load address */
        o->shape = TH_ADDR;
        o->op = ALU_ADD;
        o->rd = (h >> 8) & 7;
        o->rn = g == 21 ? 13 : 15;
        o->imm = (h & 255) * 4;
        return TH_F16;
    }
    if (g == 22 || g == 23) { /* misc 0xB0-0xBF */
        unsigned top = h >> 8;
        if (top == 0xB0) { /* ADD/SUB SP, #imm7 */
            o->shape = TH_ADDR;
            o->op = (h >> 7) & 1 ? ALU_SUB : ALU_ADD;
            o->rd = 13;
            o->rn = 13;
            o->imm = (h & 127) * 4;
            return TH_F16;
        }
        if (top == 0xB2) { /* sign/zero extend */
            o->shape = TH_EXTEND;
            o->op = (h >> 6) & 3;
            o->rd = h & 7;
            o->rm = (h >> 3) & 7;
            return TH_F16;
        }
        if (top == 0xB4 || top == 0xB5 || top == 0xBC || top == 0xBD) { /* PUSH/POP */
            o->shape = TH_PUSHPOP;
            o->op = (top == 0xBC || top == 0xBD) ? 1 : 0;
            o->imm = h & 255;
            o->ra = (h >> 8) & 1;
            return TH_F16;
        }
        if (top == 0xB1 || top == 0xB3 || top == 0xB9 || top == 0xBB) { /* CBZ/CBNZ */
            o->shape = TH_BRANCH;
            o->op = (top == 0xB9 || top == 0xBB) ? BR_CBNZ : BR_CBZ;
            o->rn = h & 7;
            o->imm = (((h >> 9) & 1) << 5) | ((h >> 3) & 31);
            o->imm <<= 1;
            return TH_F16;
        }
        if (top == 0xBF) { /* hints and IT */
            unsigned firstcond = (h >> 4) & 15, mask = h & 15;
            if (!mask) {
                if (firstcond <= 4) {
                    o->shape = TH_HINT;
                    o->op = firstcond;
                    return TH_F16;
                }
                return 0;
            }
            o->shape = TH_IT;
            o->imm = (firstcond << 4) | mask;
            return TH_F16;
        }
        return 0;
    }
    if (g == 24 || g == 25) { /* STMIA/LDMIA */
        o->shape = TH_LDMSTM;
        o->op = g == 25 ? 1 : 0;
        o->rn = (h >> 8) & 7;
        o->imm = h & 255;
        o->flags = TH_WB;
        return TH_F16;
    }
    if (g == 26 || g == 27) { /* conditional branch / SVC */
        unsigned cond = (h >> 8) & 15;
        if (cond == 15) {
            o->shape = TH_BRANCH;
            o->op = BR_SVC;
            o->imm = h & 255;
            return TH_F16;
        }
        if (cond == 14) return 0;
        o->shape = TH_BRANCH;
        o->op = BR_BCOND;
        o->ra = cond;
        o->imm = h & 255;
        return TH_F16;
    }
    if (g == 28) { /* unconditional branch */
        o->shape = TH_BRANCH;
        o->op = BR_B;
        o->imm = h & 0x7FF;
        return TH_F16;
    }
    o->shape = TH_UNDEF;
    o->len = 0;
    return 0;
}

/* ThumbExpandImm: the 12-bit modified immediate shared by T32 data-processing. */
static uint32_t thumb_expand_imm(unsigned imm12) {
    unsigned imm8 = imm12 & 255;
    if ((imm12 & 0xC00) == 0) {
        switch ((imm12 >> 8) & 3) {
            case 0: return imm8;
            case 1: return imm8 | (imm8 << 16);
            case 2: return (imm8 << 8) | (imm8 << 24);
            default: return imm8 * 0x01010101u;
        }
    }
    {
        unsigned rot = (imm12 >> 7) & 31;
        uint32_t v = 0x80u | (imm12 & 127);
        return (v >> rot) | (v << ((32 - rot) & 31));
    }
}

static unsigned thumb32(uint16_t a, uint16_t b, SnxThumb* o) {
    o->len = TH_F32;
    o->flags = 0;
    o->op = 0;
    o->ra = 0;
    if ((a & 0xF800) == 0xF000 && (b & 0xC000) == 0xC000) { /* BL / BLX */
        o->shape = TH_BRANCH;
        o->op = (b & 0x1000) ? BR_BL : BR_BLX_IMM;
        o->rn = a;
        o->rm = b;
        return TH_F32;
    }
    if ((a & 0xF800) == 0xF000 && (b & 0xD000) == 0x9000) { /* B.W */
        o->shape = TH_BRANCH;
        o->op = BR_BW;
        o->rn = a;
        o->rm = b;
        return TH_F32;
    }
    if ((a >> 11) == 0x1E && ((a >> 9) & 1) == 0) { /* data-processing, modified immediate */
        o->shape = TH_WDATA;
        o->op = (a >> 5) & 15;
        o->flags = (a & 0x10) ? TH_S : 0;
        o->rn = a & 15;
        o->rd = (b >> 8) & 15;
        o->imm = thumb_expand_imm((((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255));
        o->flags |= TH_MODIMM;
        return TH_F32;
    }
    if ((a & 0xFBF0) == 0xF240 || (a & 0xFBF0) == 0xF2C0) { /* MOVW / MOVT */
        o->shape = TH_WIMM;
        o->op = (a & 0x80) ? 1 : 0;
        o->rd = (b >> 8) & 15;
        o->imm = ((a & 15) << 12) | (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255);
        return TH_F32;
    }
    if ((a & 0xFBF0) == 0xF200 || (a & 0xFBF0) == 0xF2A0) { /* ADDW / SUBW */
        o->shape = TH_WIMM;
        o->op = (a & 0x80) ? ALU_SUB : ALU_ADD;
        o->rn = a & 15;
        o->rd = (b >> 8) & 15;
        o->imm = (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 255);
        return TH_F32;
    }
    if ((a & 0xFB00) == 0xF300) { /* bitfield */
        unsigned lsb = (((b >> 12) & 7) << 2) | ((b >> 6) & 3);
        unsigned msb = b & 31;
        o->shape = TH_BITFIELD;
        o->rd = (b >> 8) & 15;
        o->rn = a & 15;
        if (!(a & 0x20)) {              /* extract: bit5=0, bit7 selects sign */
            o->op = (a & 0x80) ? 1 : 0; /* UBFX : SBFX */
        } else {                        /* insert: bit5=1, Rn=15 is BFC */
            o->op = (a & 15) == 15 ? 3 : 2;
        }
        o->imm = lsb;
        o->ra = msb;
        return TH_F32;
    }
    if ((a & 0xFFF0) == 0xE8D0 && (b & 0x10) == 0) { /* TBB / TBH */
        o->shape = TH_TABLE;
        o->op = (b >> 4) & 1;
        o->rn = a & 15;
        o->rm = b & 15;
        return TH_F32;
    }
    if ((a >> 9) == 0x7C) { /* wide load/store T3/T4 (byte/half/word) */
        unsigned type = (a >> 4) & 31, low = type & 7, size, load;
        switch (low) {
            case 0:
                size = 1;
                load = 0;
                break;
            case 1:
                size = 1;
                load = 1;
                break;
            case 2:
                size = 2;
                load = 0;
                break;
            case 3:
                size = 2;
                load = 1;
                break;
            case 4:
                size = 4;
                load = 0;
                break;
            case 5:
                size = 4;
                load = 1;
                break;
            default: return 0;
        }
        o->shape = load ? TH_LOAD : TH_STORE;
        o->rn = a & 15;
        o->rd = (b >> 12) & 15;
        o->flags = size == 1 ? TH_B : size == 2 ? TH_H : 0;
        if ((type & 0x10) && size < 4 && load) o->flags |= TH_SIGN;
        if ((b & 0x900) == 0x900) {            /* T3: 1 P U 1 imm8 */
            if (b & 0x400) o->flags |= TH_PRE; /* P */
            if (b & 0x200) o->flags |= TH_UP;  /* U */
            o->flags |= TH_WB;
            o->imm = b & 255;
        } else if ((b & 0xFC0) == 0) { /* T4 register */
            o->flags |= TH_REG | TH_UP;
            o->rm = b & 15;
            o->imm = (b >> 4) & 3;
        } else { /* T4 immediate, positive offset */
            o->flags |= TH_UP | TH_PRE;
            o->imm = b & 0xFFF;
        }
        return TH_F32;
    }
    if ((a >> 8) == 0xEA || (a >> 8) == 0xEB) { /* data-processing, immediate shift */
        o->shape = TH_WDATA;
        o->op = (a >> 5) & 15;
        o->flags = (a & 0x10) ? TH_S : 0;
        o->rn = a & 15;
        o->rd = (b >> 8) & 15;
        o->rm = b & 15;
        o->ra = (b >> 4) & 3;                             /* shift type */
        o->imm = (((b >> 12) & 7) << 2) | ((b >> 6) & 3); /* shift amount */
        return TH_F32;
    }
    if ((a & 0xFF8F) == 0xFA0F && (b & 0xF0) == 0x80) { /* wide sign/zero extend */
        o->shape = TH_EXTEND;
        o->op = (((a >> 6) & 1)) | (((a >> 4) & 1) << 1); /* bit0: byte, bit1: unsigned */
        o->rd = (b >> 8) & 15;
        o->rm = b & 15;
        return TH_F32;
    }
    if ((a >> 8) == 0xFA) { /* data-processing / shift, register shift */
        o->shape = TH_WDATA;
        o->flags = (a & 0x10) ? TH_S : 0;
        o->rd = (b >> 8) & 15;
        if (b & 0x8000) { /* shift by register: MOV Rd, Rm <type> Rs */
            o->op = 2;    /* ORR with Rn=15 => MOV */
            o->flags |= TH_RSHIFT;
            o->rn = 15;
            o->rm = a & 15;
            o->imm = b & 15; /* shift register */
            o->ra = (a >> 5) & 3;
        } else {
            o->op = (a >> 5) & 15;
            o->rn = a & 15;
            o->rm = b & 15;
            o->imm = (b >> 4) & 15; /* shift register */
            o->ra = (b >> 4) & 3;
            o->flags |= TH_RSHIFT;
        }
        return TH_F32;
    }
    if ((a >> 8) == 0xFB) { /* MUL / MLA / MLS */
        unsigned sub = (b >> 4) & 15;
        if (sub == 0 || sub == 1) {
            o->shape = TH_MUL;
            o->rn = a & 15;
            o->rd = (b >> 8) & 15;
            o->rm = b & 15;
            o->ra = (b >> 12) & 15;
            o->op = sub == 1 ? 2 : ((b >> 12) == 15 ? 0 : 1); /* MLS : MUL : MLA */
            return TH_F32;
        }
        return 0; /* long/other multiplies: not yet decoded */
    }
    o->shape = TH_UNDEF;
    o->len = 0;
    return 0;
}

unsigned snx_thumb_decode(uint16_t hw1, uint16_t hw2, SnxThumb* out) {
    out->shape = TH_UNDEF;
    out->len = 0;
    if ((hw1 >> 11) >= 0x1D) return thumb32(hw1, hw2, out);
    return thumb16(hw1, out);
}
