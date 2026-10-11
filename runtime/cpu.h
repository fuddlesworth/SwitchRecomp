/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* A57 CPU state + helpers shared by the runtime and the generated C.
   Keep it small, every generated TU includes it. */
#ifndef SWR_CPU_H
#define SWR_CPU_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;

/* prologue of every generated function. On gcc/clang guest memory comes in as a restrict param so stores
   through it can't alias c->x (restrict on a local doesn't buy anything). tcc gets the plain version */
#if defined(__TINYC__)
#define SWR_FN(n)                                                                                                      \
    void n(CPU* c, u64 e) {                                                                                            \
        u8* const M = c->mem;
#else
#define SWR_FN(n)                                                                                                      \
    static void n##_b(CPU* __restrict__ c, u8* __restrict__ M, u64 e);                                                 \
    void n(CPU* __restrict__ c, u64 e) { n##_b(c, c->mem, e); }                                                        \
    static void n##_b(CPU* __restrict__ c, u8* __restrict__ M, u64 e) {
#endif
#if defined(__TINYC__)
#define RESTRICT
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#define SWR_INLINE  static inline
#else
#define RESTRICT    __restrict__
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define SWR_INLINE  static inline __attribute__((always_inline))
#endif

#if defined(_WIN32)
#define SWR_EXPORT __declspec(dllexport)
#else
#define SWR_EXPORT __attribute__((visibility("default")))
#endif

typedef union V128 {
    u8 b[16];
    i8 sb[16];
    u16 h[8];
    i16 sh[8];
    u32 s[4];
    i32 ss[4];
    u64 d[2];
    i64 sd[2];
    f32 f[4];
    f64 df[2];
} V128;

struct CPU;
typedef void (*GuestFn)(struct CPU* RESTRICT c, u64 entry);

/* runtime callbacks used by generated code */
typedef struct RtAPI {
    void (*svc)(struct CPU* c, u32 imm);
    void (*undef)(struct CPU* c, u64 pc, u32 w);
    void (*note_hint)(u64 pc);
    u64 (*ticks)(void); /* CNTVCT @ 19.2 MHz */
    void (*brk)(struct CPU* c, u64 pc, u32 imm);
    void (*fpcr_changed)(struct CPU* c);
    void (*sev)(void);
    void (*wfe)(struct CPU* c);
} RtAPI;

typedef struct CPU {
    u64 x[32]; /* x[31] is SP, XZR is handled at translation time */
    u64 pc;
    u32 n, z, cf, vf; /* NZCV kept as separate fields */
    V128 v[32];
    u32 fpcr, fpsr;
    u64 tpidr, tpidrro;
    u64 excl_addr, excl_val, excl_val2;
    u32 excl_size;
    u32 excl_on;
    u8* mem;       /* guest memory base */
    GuestFn* disp; /* lookup: (pc-code_lo)>>2 -> fn */
    u64 code_lo, code_hi;
    const RtAPI* rt;
    void* thread; /* owning HLE thread */
    int depth;
    u32 pad_;
    u64 user[8];
} CPU;

/* ---- memory: base + addr, no page tables ---- */
#define MP(a)       (M + (u64)(a))
#define RD8(a)      (*(u8*)MP(a))
#define RD16(a)     (*(u16*)MP(a))
#define RD32(a)     (*(u32*)MP(a))
#define RD64(a)     (*(u64*)MP(a))
#define RDS8(a)     (*(i8*)MP(a))
#define RDS16(a)    (*(i16*)MP(a))
#define RDS32(a)    (*(i32*)MP(a))
#define WR8(a, v)   (*(u8*)MP(a) = (u8)(v))
#define WR16(a, v)  (*(u16*)MP(a) = (u16)(v))
#define WR32(a, v)  (*(u32*)MP(a) = (u32)(v))
#define WR64(a, v)  (*(u64*)MP(a) = (u64)(v))
#define RDF32(a)    (*(f32*)MP(a))
#define RDF64(a)    (*(f64*)MP(a))
#define RDQ(dst, a) memcpy(&(dst), MP(a), 16)
#define WRQ(a, src) memcpy(MP(a), &(src), 16)

/* ---- calls ---- */
SWR_INLINE GuestFn swr_lookup(CPU* c, u64 pc);
extern GuestFn swr_interp_entry;
SWR_INLINE GuestFn swr_lookup(CPU* c, u64 pc) {
    u64 o = pc - c->code_lo;
    if (LIKELY(o < c->code_hi - c->code_lo && !(pc & 3))) {
        GuestFn f = c->disp[o >> 2];
        if (LIKELY(f != 0)) return f;
    }
    return swr_interp_entry;
}
#define CALLDYN(t)                                                                                                     \
    do {                                                                                                               \
        u64 t__ = (t);                                                                                                 \
        swr_lookup(c, t__)(c, t__);                                                                                    \
    } while (0)

/* ---- conditions ---- */
SWR_INLINE int cond_ok(const CPU* c, u32 cond) {
    int r;
    switch (cond >> 1) {
        case 0: r = c->z; break;
        case 1: r = c->cf; break;
        case 2: r = c->n; break;
        case 3: r = c->vf; break;
        case 4: r = c->cf && !c->z; break;
        case 5: r = c->n == c->vf; break;
        case 6: r = !c->z && c->n == c->vf; break;
        default: return 1;
    }
    return (cond & 1) ? !r : r;
}
SWR_INLINE void set_nzcv(CPU* c, u32 f) {
    c->n = (f >> 3) & 1;
    c->z = (f >> 2) & 1;
    c->cf = (f >> 1) & 1;
    c->vf = f & 1;
}
SWR_INLINE u32 get_nzcv(const CPU* c) { return (c->n << 31) | (c->z << 30) | (c->cf << 29) | (c->vf << 28); }

/* ---- flag setting arith ---- */
SWR_INLINE u64 adds64(CPU* c, u64 a, u64 b, u32 cin) {
    u64 r = a + b + cin;
    c->n = (u32)(r >> 63);
    c->z = r == 0;
    c->cf = cin ? r <= a : r < a;
    c->vf = (u32)(((~(a ^ b)) & (a ^ r)) >> 63);
    return r;
}
SWR_INLINE u32 adds32(CPU* c, u32 a, u32 b, u32 cin) {
    u32 r = a + b + cin;
    c->n = r >> 31;
    c->z = r == 0;
    c->cf = cin ? r <= a : r < a;
    c->vf = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
SWR_INLINE u64 subs64(CPU* c, u64 a, u64 b) {
    u64 r = a - b;
    c->n = (u32)(r >> 63);
    c->z = r == 0;
    c->cf = a >= b;
    c->vf = (u32)(((a ^ b) & (a ^ r)) >> 63);
    return r;
}
SWR_INLINE u32 subs32(CPU* c, u32 a, u32 b) {
    u32 r = a - b;
    c->n = r >> 31;
    c->z = r == 0;
    c->cf = a >= b;
    c->vf = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
SWR_INLINE u64 sbcs64(CPU* c, u64 a, u64 b, u32 cin) { return adds64(c, a, ~b, cin); }
SWR_INLINE u32 sbcs32(CPU* c, u32 a, u32 b, u32 cin) { return adds32(c, a, ~b, cin); }
SWR_INLINE void logic64(CPU* c, u64 r) {
    c->n = (u32)(r >> 63);
    c->z = r == 0;
    c->cf = 0;
    c->vf = 0;
}
SWR_INLINE void logic32(CPU* c, u32 r) {
    c->n = r >> 31;
    c->z = r == 0;
    c->cf = 0;
    c->vf = 0;
}

/* ---- shifts ---- */
SWR_INLINE u64 ror64(u64 x, u32 s) {
    s &= 63;
    return s ? (x >> s) | (x << (64 - s)) : x;
}
SWR_INLINE u32 ror32(u32 x, u32 s) {
    s &= 31;
    return s ? (x >> s) | (x << (32 - s)) : x;
}
SWR_INLINE u64 shf64(u64 x, u32 t, u32 s) {
    switch (t) {
        case 0: return x << s;
        case 1: return x >> s;
        case 2: return (u64)((i64)x >> s);
        default: return ror64(x, s);
    }
}
SWR_INLINE u32 shf32(u32 x, u32 t, u32 s) {
    switch (t) {
        case 0: return x << s;
        case 1: return x >> s;
        case 2: return (u32)((i32)x >> s);
        default: return ror32(x, s);
    }
}
SWR_INLINE u64 ext64(u64 x, u32 opt, u32 sh) {
    switch (opt) {
        case 0: x = (u8)x; break;
        case 1: x = (u16)x; break;
        case 2: x = (u32)x; break;
        case 4: x = (u64)(i64)(i8)x; break;
        case 5: x = (u64)(i64)(i16)x; break;
        case 6: x = (u64)(i64)(i32)x; break;
        default: break;
    }
    return x << sh;
}

/* ---- bits ---- */
#if defined(__TINYC__)
SWR_INLINE u32 clz64(u64 x) {
    u32 n = 0;
    if (!x) return 64;
    while (!(x >> 63)) {
        x <<= 1;
        n++;
    }
    return n;
}
SWR_INLINE u32 clz32(u32 x) {
    u32 n = 0;
    if (!x) return 32;
    while (!(x >> 31)) {
        x <<= 1;
        n++;
    }
    return n;
}
SWR_INLINE u32 popc8(u8 x) {
    u32 n = 0;
    while (x) {
        n += x & 1;
        x >>= 1;
    }
    return n;
}
SWR_INLINE u32 bswap32(u32 x) { return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24); }
SWR_INLINE u64 bswap64(u64 x) { return ((u64)bswap32((u32)x) << 32) | bswap32((u32)(x >> 32)); }
#else
SWR_INLINE u32 clz64(u64 x) { return x ? (u32)__builtin_clzll(x) : 64; }
SWR_INLINE u32 clz32(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }
SWR_INLINE u32 popc8(u8 x) { return (u32)__builtin_popcount(x); }
SWR_INLINE u32 bswap32(u32 x) { return __builtin_bswap32(x); }
SWR_INLINE u64 bswap64(u64 x) { return __builtin_bswap64(x); }
#endif
SWR_INLINE u32 cls64(u64 x) { return clz64(x ^ ((u64)((i64)x >> 1))) - 1; }
SWR_INLINE u32 cls32(u32 x) { return clz32(x ^ ((u32)((i32)x >> 1))) - 1; }
SWR_INLINE u64 rbit64(u64 x) {
    x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
    x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
    return bswap64(x);
}
SWR_INLINE u32 rbit32(u32 x) { return (u32)(rbit64(x) >> 32); }
SWR_INLINE u64 rev16_64(u64 x) { return ((x >> 8) & 0x00FF00FF00FF00FFull) | ((x & 0x00FF00FF00FF00FFull) << 8); }
SWR_INLINE u64 rev32_64(u64 x) {
    u64 y = bswap64(x);
    return (y >> 32) | (y << 32);
}

/* ---- div: x/0 = 0, INT_MIN/-1 = INT_MIN on arm ---- */
SWR_INLINE u64 udiv64(u64 a, u64 b) { return b ? a / b : 0; }
SWR_INLINE u32 udiv32(u32 a, u32 b) { return b ? a / b : 0; }
SWR_INLINE i64 sdiv64(i64 a, i64 b) {
    if (!b) return 0;
    if (b == -1) return (i64)(0 - (u64)a);
    return a / b;
}
SWR_INLINE i32 sdiv32(i32 a, i32 b) {
    if (!b) return 0;
    if (b == -1) return (i32)(0u - (u32)a);
    return a / b;
}

/* ---- 64x64 high multiply ---- */
SWR_INLINE u64 umulh64(u64 a, u64 b) {
#if defined(__SIZEOF_INT128__) && !defined(__TINYC__)
    return (u64)(((unsigned __int128)a * b) >> 64);
#else
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}
SWR_INLINE i64 smulh64(i64 a, i64 b) {
    u64 r = umulh64((u64)a, (u64)b);
    if (a < 0) r -= (u64)b;
    if (b < 0) r -= (u64)a;
    return (i64)r;
}

/* ---- SBFM/UBFM/BFM, interpreter only ---- */
SWR_INLINE u64 bmask(u32 n) { return n >= 64 ? ~0ull : ((1ull << n) - 1); }
SWR_INLINE u64 ubfm_(u64 src, u32 immr, u32 imms, u32 ds) {
    if (imms >= immr) return (src >> immr) & bmask(imms - immr + 1);
    return (src & bmask(imms + 1)) << (ds - immr);
}
SWR_INLINE u64 sbfm_(u64 src, u32 immr, u32 imms, u32 ds) {
    u64 r;
    u32 top;
    if (imms >= immr) {
        r = (src >> immr) & bmask(imms - immr + 1);
        top = imms - immr;
    } else {
        r = (src & bmask(imms + 1)) << (ds - immr);
        top = ds - immr + imms;
    }
    if ((r >> top) & 1) r |= ~bmask(top + 1);
    return ds == 32 ? (u32)r : r;
}
SWR_INLINE u64 bfm_(u64 dst, u64 src, u32 immr, u32 imms, u32 ds) {
    u64 r;
    if (imms >= immr) {
        u32 w = imms - immr + 1;
        u64 m = bmask(w);
        r = (dst & ~m) | ((src >> immr) & m);
    } else {
        u32 w = imms + 1, p = ds - immr;
        u64 m = bmask(w) << p;
        r = (dst & ~m) | ((src << p) & m);
    }
    return ds == 32 ? (u32)r : r;
}
u64 swr_decode_bitmask(u32 N, u32 imms, u32 immr, u32 ds); /* cpu_helpers.c */

/* ---- CRC32 ---- */
u32 swr_crc32(u32 acc, u64 val, int bytes, int castagnoli);

/* ---- fp (cpu_helpers.c) ---- */
f32 fp_max32(f32, f32);
f32 fp_min32(f32, f32);
f64 fp_max64(f64, f64);
f64 fp_min64(f64, f64);
f32 fp_maxnm32(f32, f32);
f32 fp_minnm32(f32, f32);
f64 fp_maxnm64(f64, f64);
f64 fp_minnm64(f64, f64);
i64 fcvt_s64(f64 x, int rm);
u64 fcvt_u64(f64 x, int rm);
i32 fcvt_s32(f64 x, int rm);
u32 fcvt_u32(f64 x, int rm);
f64 fp_round(f64 x, int rm);
f32 fp_round32(f32 x, int rm);
u16 f32_to_f16(f32 x);
f32 f16_to_f32(u16 h);
u16 f64_to_f16(f64 x);
f32 fp_recpe32(f32 x);
f32 fp_rsqrte32(f32 x);
f64 fp_recpe64(f64 x);
f64 fp_rsqrte64(f64 x);
f32 fp_recps32(f32 a, f32 b);
f32 fp_rsqrts32(f32 a, f32 b);
f64 fp_recps64(f64 a, f64 b);
f64 fp_rsqrts64(f64 a, f64 b);
u32 urecpe32(u32 x);
u32 ursqrte32(u32 x);
f32 fp_mulx32(f32 a, f32 b);
f64 fp_mulx64(f64 a, f64 b);
void fp_cmp32(CPU* c, f32 a, f32 b);
void fp_cmp64(CPU* c, f64 a, f64 b);
f32 fp_sqrt32(f32 x);
f64 fp_sqrt64(f64 x);
f32 fp_fma32(f32 a, f32 b, f32 c);
f64 fp_fma64(f64 a, f64 b, f64 c);
f32 fp_nan32(f32 r, f32 a, f32 b);
f64 fp_nan64(f64 r, f64 a, f64 b);
f32 fp_abd32(f32 a, f32 b);
f32 fp_frintx32(f32 x);
f64 fp_frintx64(f64 x);
/* rm: 0 RNE, 1 +inf, 2 -inf, 3 zero, 4 RNA, 5 use FPCR */

/* ---- saturation ---- */
SWR_INLINE i64 sat_s(i64 v, int bits) {
    i64 mx = (bits == 64) ? 0x7FFFFFFFFFFFFFFFll : ((1ll << (bits - 1)) - 1), mn = -mx - 1;
    return v > mx ? mx : v < mn ? mn : v;
}
SWR_INLINE u64 sat_u(i64 v, int bits) {
    u64 mx = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    return v < 0 ? 0 : (u64)v > mx ? mx : (u64)v;
}

/* ---- vector ops, the big/rare ones live in cpu_helpers.c ---- */
void v_tbl(CPU* c, u32 d, u32 n, u32 len, u32 m, int q, int tbx);
void v_aes(V128* d, const V128* k, int dec);
void v_aesmc(V128* d, const V128* s, int inv);
void v_sha1(CPU* c, u32 op, u32 d, u32 n, u32 m);
void v_sha256(CPU* c, u32 op, u32 d, u32 n, u32 m);
void v_pmull(V128* d, const V128* a, const V128* b, int size, int hi);
u64 v_expand_imm(u32 op, u32 cmode, u32 imm8);

SWR_INLINE void dc_zva(u8* M, u64 a) { memset(MP(a & ~63ull), 0, 64); }

/* ---- LDXR/STXR: value-based monitor using CAS ---- */
#if defined(__TINYC__)
u32 swr_cas64(u64* p, u64 o, u64 n);
u32 swr_cas32(u32* p, u32 o, u32 n);
u32 swr_cas16(u16* p, u16 o, u16 n);
u32 swr_cas8(u8* p, u8 o, u8 n);
u32 swr_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1);
#define SWR_FENCE() swr_fence()
void swr_fence(void);
#else
SWR_INLINE u32 swr_cas64(u64* p, u64 o, u64 n) {
    return __atomic_compare_exchange_n(p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SWR_INLINE u32 swr_cas32(u32* p, u32 o, u32 n) {
    return __atomic_compare_exchange_n(p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SWR_INLINE u32 swr_cas16(u16* p, u16 o, u16 n) {
    return __atomic_compare_exchange_n(p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
SWR_INLINE u32 swr_cas8(u8* p, u8 o, u8 n) {
    return __atomic_compare_exchange_n(p, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
u32 swr_cas128(u64* p, u64 o0, u64 o1, u64 n0, u64 n1);
#define SWR_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#endif
SWR_INLINE u64 ldx(CPU* c, u8* M, u64 a, u32 sz) {
    u64 v;
    switch (sz) {
        case 0: v = RD8(a); break;
        case 1: v = RD16(a); break;
        case 2: v = RD32(a); break;
        default: v = RD64(a);
    }
    c->excl_addr = a;
    c->excl_val = v;
    c->excl_size = sz;
    c->excl_on = 1;
    return v;
}
SWR_INLINE u32 stx(CPU* c, u8* M, u64 a, u32 sz, u64 v) {
    u32 ok = 0;
    if (c->excl_on && c->excl_addr == a && c->excl_size == sz) {
        switch (sz) {
            case 0: ok = swr_cas8((u8*)MP(a), (u8)c->excl_val, (u8)v); break;
            case 1: ok = swr_cas16((u16*)MP(a), (u16)c->excl_val, (u16)v); break;
            case 2: ok = swr_cas32((u32*)MP(a), (u32)c->excl_val, (u32)v); break;
            default: ok = swr_cas64((u64*)MP(a), c->excl_val, v);
        }
    }
    c->excl_on = 0;
    return ok ? 0 : 1;
}
SWR_INLINE void ldxp(CPU* c, u8* M, u64 a, u32 sz, u64* v1, u64* v2) {
    if (sz == 2) {
        u64 t = RD64(a);
        *v1 = (u32)t;
        *v2 = t >> 32;
        c->excl_val = t;
        c->excl_val2 = 0;
    } else {
        *v1 = RD64(a);
        *v2 = RD64(a + 8);
        c->excl_val = *v1;
        c->excl_val2 = *v2;
    }
    c->excl_addr = a;
    c->excl_size = sz | 0x10;
    c->excl_on = 1;
}
SWR_INLINE u32 stxp(CPU* c, u8* M, u64 a, u32 sz, u64 v1, u64 v2) {
    u32 ok = 0;
    if (c->excl_on && c->excl_addr == a && c->excl_size == (sz | 0x10)) {
        if (sz == 2)
            ok = swr_cas64((u64*)MP(a), c->excl_val, (u32)v1 | ((u64)v2 << 32));
        else
            ok = swr_cas128((u64*)MP(a), c->excl_val, c->excl_val2, v1, v2);
    }
    c->excl_on = 0;
    return ok ? 0 : 1;
}

u64 swr_mrs(CPU* c, u32 sysreg);
void swr_msr(CPU* c, u32 sysreg, u64 v);

/* ---- codegen macros ---- */
#define RX(n) ((n) == 31 ? (u64)0 : c->x[n])
#define SETX(n, e)                                                                                                     \
    do {                                                                                                               \
        u64 t__ = (e);                                                                                                 \
        if ((n) != 31) c->x[n] = t__;                                                                                  \
    } while (0)
#define SETS(D_, E_)                                                                                                   \
    do {                                                                                                               \
        f32 t__ = (E_);                                                                                                \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[0] = 0;                                                                                                 \
        p__->d[1] = 0;                                                                                                 \
        p__->f[0] = t__;                                                                                               \
    } while (0)
#define SETD(D_, E_)                                                                                                   \
    do {                                                                                                               \
        f64 t__ = (E_);                                                                                                \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->df[0] = t__;                                                                                              \
    } while (0)
#define SETSB(D_, E_)                                                                                                  \
    do {                                                                                                               \
        u32 t__ = (u32)(E_);                                                                                           \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->d[0] = t__;                                                                                               \
    } while (0)
#define SETDB(D_, E_)                                                                                                  \
    do {                                                                                                               \
        u64 t__ = (u64)(E_);                                                                                           \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->d[0] = t__;                                                                                               \
    } while (0)
#define SETH(D_, E_)                                                                                                   \
    do {                                                                                                               \
        u16 t__ = (u16)(E_);                                                                                           \
        V128* p__ = &c->v[D_];                                                                                         \
        p__->d[1] = 0;                                                                                                 \
        p__->d[0] = t__;                                                                                               \
    } while (0)
#define SETHB(d, e) SETH(d, e)
#define FPRM        ((int)((c->fpcr >> 22) & 3))
#if defined(__TINYC__)
#define CBAR()                                                                                                         \
    do {                                                                                                               \
    } while (0)
#else
#define CBAR() __asm__ __volatile__("" ::: "memory")
#endif
#define YIELD()                                                                                                        \
    do {                                                                                                               \
    } while (0)
#if defined(__FMA__) && !defined(__TINYC__)
#define FMLA32(d, a, b) __builtin_fmaf((a), (b), (d))
#define FMLA64(d, a, b) __builtin_fma((a), (b), (d))
#else
#define FMLA32(d, a, b) ((f32)((f64)(a) * (f64)(b) + (f64)(d)))
#define FMLA64(d, a, b) ((d) + (a) * (b))
#endif
SWR_INLINE void fcmp_(CPU* c, f64 a, f64 b) {
    if (a < b) {
        c->n = 1;
        c->z = 0;
        c->cf = 0;
        c->vf = 0;
    } else if (a == b) {
        c->n = 0;
        c->z = 1;
        c->cf = 1;
        c->vf = 0;
    } else if (a > b) {
        c->n = 0;
        c->z = 0;
        c->cf = 1;
        c->vf = 0;
    } else {
        c->n = 0;
        c->z = 0;
        c->cf = 1;
        c->vf = 1;
    }
}
SWR_INLINE i32 fcvtzs32(f64 x) {
    if (x != x) return 0;
    if (x >= 2147483648.0) return 0x7FFFFFFF;
    if (x <= -2147483649.0) return (i32)0x80000000u;
    return (i32)x;
}
SWR_INLINE u32 fcvtzu32(f64 x) {
    if (x != x || x <= -1.0) return 0;
    if (x >= 4294967296.0) return 0xFFFFFFFFu;
    return (u32)(i64)x;
}
SWR_INLINE i64 fcvtzs64(f64 x) {
    if (x != x) return 0;
    if (x >= 9223372036854775808.0) return 0x7FFFFFFFFFFFFFFFll;
    if (x < -9223372036854775808.0) return (i64)0x8000000000000000ull;
    return (i64)x;
}
SWR_INLINE u64 fcvtzu64(f64 x) { return fcvt_u64(x, 3); }
SWR_INLINE f64 fp_pow2(int n) {
    f64 r = 1.0;
    while (n-- > 0) r *= 2.0;
    return r;
}
SWR_INLINE u32 vfp_imm32(u32 i) {
    u32 b6 = (i >> 6) & 1;
    u32 e = ((b6 ^ 1) << 7) | ((b6 ? 0x1Fu : 0) << 2) | ((i >> 4) & 3);
    return ((i >> 7) << 31) | (e << 23) | ((i & 15) << 19);
}
SWR_INLINE u64 vfp_imm64(u32 i) {
    u64 b6 = (i >> 6) & 1;
    u64 e = ((b6 ^ 1) << 10) | ((b6 ? 0xFFull : 0) << 2) | ((i >> 4) & 3);
    return ((u64)(i >> 7) << 63) | (e << 52) | ((u64)(i & 15) << 48);
}
int swr_simd(CPU* c, u8* M, u32 w);
#endif
