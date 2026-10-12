/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Part of cpu_helpers.c, which includes it: bitmask immediates, CRC32, compare-and-swap, fences, and the system registers (MRS, MSR). Not compiled on its own. */
/* ================================================================ integer */
u64 snx_decode_bitmask(u32 N, u32 imms, u32 immr, u32 ds) {
    u32 v = (N << 6) | (~imms & 0x3F);
    int len = -1;
    for (int i = 6; i >= 0; i--)
        if (v & (1u << i)) {
            len = i;
            break;
        }
    if (len < 1) return 0;
    u32 esize = 1u << len, levels = esize - 1;
    u32 S = imms & levels, R = immr & levels;
    if (S == levels) return 0;
    u64 welem = bmask(S + 1);
    if (R) welem = ((welem >> R) | (welem << (esize - R))) & bmask(esize);
    u64 r = 0;
    for (u32 i = 0; i < ds; i += esize) r |= welem << i;
    return ds == 32 ? (u32)r : r;
}

u32 snx_crc32(u32 acc, u64 val, int bytes, int castagnoli) {
    u32 poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (int i = 0; i < bytes; i++) {
        acc ^= (u8)(val >> (i * 8));
        for (int k = 0; k < 8; k++) acc = (acc >> 1) ^ ((acc & 1) ? poly : 0);
    }
    return acc;
}

/* the 16-byte builtin compiles to lock cmpxchg16b only where cx16 is enabled, and the game module isn't built with
   -mcx16, so STXP of a 64-bit pair used to take a fallback that swapped the two halves separately. That isn't atomic:
   Tears of the Kingdom's lock-free lists (102 such pairs, a pointer and a counter) got a pointer from one thread and a
   counter from another. Every x86-64 CPU that runs the game has cmpxchg16b, so this function enables it for itself.
   Clang still warns that 16 bytes is past its lock-free size, though it emits the one instruction */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Watomic-alignment"
#if defined(__x86_64__)
__attribute__((target("cx16")))
#endif
u32 snx_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
    unsigned __int128 o = ((unsigned __int128)o1 << 64) | o0, n = ((unsigned __int128)n1 << 64) | n0;
    return __atomic_compare_exchange_n((unsigned __int128*)p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#pragma clang diagnostic pop

/* ================================================================ system */
#define SYSREG(op0, op1, crn, crm, op2) ((((op0) & 1) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
u64 snx_mrs(CPU* c, u32 r) {
    switch (r) {
        case SYSREG(3, 3, 13, 0, 2): return c->tpidr;
        case SYSREG(3, 3, 13, 0, 3): return c->tpidrro;
        case SYSREG(3, 3, 4, 2, 0): return get_nzcv(c);
        case SYSREG(3, 3, 4, 4, 0): return c->fpcr;
        case SYSREG(3, 3, 4, 4, 1): return c->fpsr;
        case SYSREG(3, 3, 14, 0, 0): return 19200000;
        case SYSREG(3, 3, 14, 0, 1):
        case SYSREG(3, 3, 14, 0, 2): return c->rt->ticks();
        case SYSREG(3, 3, 0, 0, 7): return 4;           /* DCZID = 64 bytes */
        case SYSREG(3, 3, 0, 0, 1): return 0x8444C004u; /* CTR_EL0 (A57) */
        case SYSREG(3, 0, 0, 0, 0): return 0x411FD071u; /* MIDR */
        case SYSREG(3, 0, 0, 0, 5): return 0x80000000u; /* MPIDR */
        default: return 0;
    }
}
void snx_msr(CPU* c, u32 r, u64 v) {
    switch (r) {
        case SYSREG(3, 3, 13, 0, 2): c->tpidr = v; break;
        case SYSREG(3, 3, 4, 2, 0): set_nzcv(c, (u32)(v >> 28)); break;
        case SYSREG(3, 3, 4, 4, 0):
            c->fpcr = (u32)v;
            if (c->rt->fpcr_changed) c->rt->fpcr_changed(c);
            break;
        case SYSREG(3, 3, 4, 4, 1): c->fpsr = (u32)v; break;
        default: break;
    }
}
