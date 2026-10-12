/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Part of cpu_helpers.c, which includes it: bitmask immediates, CRC32, compare-and-swap, fences, and the system registers (MRS, MSR). Not compiled on its own. */
/* ================================================================ integer */
u64 swr_decode_bitmask(u32 N, u32 imms, u32 immr, u32 ds) {
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

u32 swr_crc32(u32 acc, u64 val, int bytes, int castagnoli) {
    u32 poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (int i = 0; i < bytes; i++) {
        acc ^= (u8)(val >> (i * 8));
        for (int k = 0; k < 8; k++) acc = (acc >> 1) ^ ((acc & 1) ? poly : 0);
    }
    return acc;
}

#if defined(__TINYC__)
/* tcc has no __atomic but does have inline asm. Exclusives need a real lock cmpxchg here since the
   kernel side touches the same words (mutex/condvar) with real atomics; a spinlock-emulated CAS isn't
   atomic against that. */
u32 swr_cas64(u64* p, u64 o, u64 n) {
    u64 prev = o;
    __asm__ __volatile__("lock; cmpxchgq %2,%1" : "+a"(prev), "+m"(*p) : "r"(n) : "memory", "cc");
    return prev == o;
}
u32 swr_cas32(u32* p, u32 o, u32 n) {
    u32 prev = o;
    __asm__ __volatile__("lock; cmpxchgl %2,%1" : "+a"(prev), "+m"(*p) : "r"(n) : "memory", "cc");
    return prev == o;
}
u32 swr_cas16(u16* p, u16 o, u16 n) {
    u16 prev = o;
    __asm__ __volatile__("lock; cmpxchgw %2,%1" : "+a"(prev), "+m"(*p) : "r"(n) : "memory", "cc");
    return prev == o;
}
u32 swr_cas8(u8* p, u8 o, u8 n) {
    u8 prev = o;
    __asm__ __volatile__("lock; cmpxchgb %2,%1" : "+a"(prev), "+m"(*p) : "q"(n) : "memory", "cc");
    return prev == o;
}
/* cmpxchg16b clobbers rbx (and we use rdi); tcc won't save them so the asm does */
u32 swr_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
    u64 a[5];
    u64 r;
    a[0] = (u64)p;
    a[1] = o0;
    a[2] = o1;
    a[3] = n0;
    a[4] = n1;
    if (((u64)p) & 15) {
        u32 ok = swr_cas64(p, o0, n0);
        if (ok && !swr_cas64(p + 1, o1, n1)) {
            swr_cas64(p, n0, o0);
            ok = 0;
        }
        return ok;
    } /* unaligned, shouldn't happen */
    __asm__ __volatile__("push %%rbx\n\t"
                         "push %%rdi\n\t"
                         "movq 8(%%rsi),%%rax\n\t"
                         "movq 16(%%rsi),%%rdx\n\t"
                         "movq 24(%%rsi),%%rbx\n\t"
                         "movq 32(%%rsi),%%rcx\n\t"
                         "movq (%%rsi),%%rdi\n\t"
                         "lock; cmpxchg16b (%%rdi)\n\t"
                         "sete %%al\n\t"
                         "movzbq %%al,%%rax\n\t"
                         "pop %%rdi\n\t"
                         "pop %%rbx"
                         : "=a"(r)
                         : "S"(a)
                         : "rcx", "rdx", "memory", "cc");
    return (u32)r;
}
#if defined(_WIN32)
#include <windows.h>
static volatile LONG g_fz;
void swr_fence(void) { InterlockedExchange(&g_fz, 0); } /* xchg is a full barrier on x86 (no __faststorefence in tcc) */
#else
void swr_fence(void) { __asm__ __volatile__("mfence" : : : "memory"); }
#endif
#else
u32 swr_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1) {
#if defined(__x86_64__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
    unsigned __int128 o = ((unsigned __int128)o1 << 64) | o0, n = ((unsigned __int128)n1 << 64) | n0;
    return __sync_bool_compare_and_swap((unsigned __int128*)p, o, n);
#else
    /* no cmpxchg16b: CAS the low half, compare the high half */
    if (__atomic_load_n(&p[1], __ATOMIC_SEQ_CST) != o1) return 0;
    if (!swr_cas64(&p[0], o0, n0)) return 0;
    __atomic_store_n(&p[1], n1, __ATOMIC_SEQ_CST);
    return 1;
#endif
}
#endif

/* ================================================================ system */
#define SYSREG(op0, op1, crn, crm, op2) ((((op0) & 1) << 14) | ((op1) << 11) | ((crn) << 7) | ((crm) << 3) | (op2))
u64 swr_mrs(CPU* c, u32 r) {
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
void swr_msr(CPU* c, u32 r, u64 v) {
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
