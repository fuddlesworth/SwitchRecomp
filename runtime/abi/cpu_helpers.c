/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* Helpers that get compiled into the game code module: FP with exact arm semantics (saturation, NaN
   handling, min/max, recip estimates), the AdvSIMD ops we don't emit inline, crypto (AES/SHA/CRC/PMULL)
   and sysregs. No libm on purpose, this has to build the same with gcc, clang and tcc. */
#include "cpu.h"

/* The helpers by topic, in the order they depend on each other (the module compiles this as one file) */
#include "helpers/fp.c"
#include "helpers/integer.c"
#include "helpers/simd.c"
#include "helpers/crypto.c"
#include "helpers/simd_mem.c"

/* ---- dispatcher for anything AdvSIMD / crypto ---- */
int swr_simd(CPU* c, u8* M, u32 w) {
    if ((w & 0xBF800000u) == 0x0C000000u || (w & 0xBF800000u) == 0x0C800000u || (w & 0xBF000000u) == 0x0D000000u)
        return simd_ldst(c, M, w);
    if ((w & 0x9F200400u) == 0x0E200400u) return simd_3same(c, w, 0);
    if ((w & 0xDF200400u) == 0x5E200400u) return simd_3same(c, w, 1);
    if ((w & 0x9F200C00u) == 0x0E200000u) return simd_3diff(c, w, 0);
    if ((w & 0xDF200C00u) == 0x5E200000u) return simd_3diff(c, w, 1);
    if ((w & 0x9F3E0C00u) == 0x0E200800u) return simd_2misc(c, w, 0);
    if ((w & 0xDF3E0C00u) == 0x5E200800u) return simd_2misc(c, w, 1);
    if ((w & 0x9F3E0C00u) == 0x0E300800u) return simd_across(c, w);
    if ((w & 0xDF3E0C00u) == 0x5E300800u) { /* scalar pairwise */
        int U = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31;
        V128 a = *VR(rn), r;
        memset(&r, 0, 16);
        if (!U && op == 0x1B) {
            if (size != 3) return 0;
            r.d[0] = a.d[0] + a.d[1];
            *VR(rd) = r;
            return 1;
        }
        if (!U) return 0;
        int sz = size & 1, hi = size >> 1;
        f64 x = rdf(&a, sz, 0), y = rdf(&a, sz, 1), v;
        switch (op) {
            case 0x0D:
                if (hi) return 0;
                v = x + y;
                break;
            case 0x0C: v = hi ? fminnm_(x, y, sz) : fmaxnm_(x, y, sz); break;
            case 0x0F: v = hi ? fmin_(x, y, sz) : fmax_(x, y, sz); break;
            default: return 0;
        }
        wrf(&r, sz, 0, v);
        *VR(rd) = r;
        return 1;
    }
    if ((w & 0x9FE08400u) == 0x0E000400u) return simd_copy(c, w);
    if ((w & 0xFFE0FC00u) == 0x5E000400u) { /* scalar DUP */
        int imm5 = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31, size = 0;
        while (size < 4 && !((imm5 >> size) & 1)) size++;
        if (size > 3) return 0;
        V128 r;
        memset(&r, 0, 16);
        eset(&r, size, 0, eget(VR(rn), size, imm5 >> (size + 1)));
        *VR(rd) = r;
        return 1;
    }
    if ((w & 0x9FF80400u) == 0x0F000400u) return simd_modimm(c, w);
    if ((w & 0x9F800400u) == 0x0F000400u) return simd_shimm(c, w, 0);
    if ((w & 0xDF800400u) == 0x5F000400u) return simd_shimm(c, w, 1);
    if ((w & 0x9F000400u) == 0x0F000000u) return simd_elem(c, w, 0);
    if ((w & 0xDF000400u) == 0x5F000000u) return simd_elem(c, w, 1);
    if ((w & 0xBF208C00u) == 0x0E000800u) return simd_perm(c, w);
    if ((w & 0xBFE08400u) == 0x2E000000u) {
        int Q = (w >> 30) & 1, rm = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31, rd = w & 31;
        if (!Q && (imm4 & 8)) return 0;
        u8 t[32];
        memcpy(t, VR(rn), 16);
        memcpy(t + (Q ? 16 : 8), VR(rm), 16);
        V128 r;
        memset(&r, 0, 16);
        memcpy(&r, t + imm4, Q ? 16 : 8);
        *VR(rd) = r;
        return 1;
    }
    if ((w & 0xBFE08C00u) == 0x0E000000u) {
        int Q = (w >> 30) & 1;
        v_tbl(c, w & 31, (w >> 5) & 31, (w >> 13) & 3, (w >> 16) & 31, Q, (w >> 12) & 1);
        return 1;
    }
    if ((w & 0xFFFF0C00u) == 0x4E280800u) {
        int op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31;
        switch (op) {
            case 4: v_aes(VR(rd), VR(rn), 0); return 1;
            case 5: v_aes(VR(rd), VR(rn), 1); return 1;
            case 6: v_aesmc(VR(rd), VR(rn), 0); return 1;
            case 7: v_aesmc(VR(rd), VR(rn), 1); return 1;
        }
        return 0;
    }
    if ((w & 0xFFE08C00u) == 0x5E000000u) {
        int op = (w >> 12) & 7, rm = (w >> 16) & 31, rn = (w >> 5) & 31, rd = w & 31;
        if (op <= 3) {
            v_sha1(c, op, rd, rn, rm);
            return 1;
        }
        if (op <= 6) {
            v_sha256(c, op, rd, rn, rm);
            return 1;
        }
        return 0;
    }
    if ((w & 0xFFFF0C00u) == 0x5E280800u) {
        int op = (w >> 12) & 31, rn = (w >> 5) & 31, rd = w & 31;
        if (op == 0) {
            v_sha1(c, 0x10, rd, rn, 0);
            return 1;
        }
        if (op == 1) {
            v_sha1(c, 0x11, rd, rn, 0);
            return 1;
        }
        if (op == 2) {
            v_sha256(c, 0x12, rd, rn, 0);
            return 1;
        }
        return 0;
    }
    return 0;
}
