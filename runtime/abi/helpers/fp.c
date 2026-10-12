/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Part of cpu_helpers.c, which includes it: floating point with exact ARM semantics: rounding, conversion to integers, min and max, NaNs, square root. Not compiled on its own. */
/* =============================================================== rounding */
static f64 fabs_(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    b &= ~(1ull << 63);
    memcpy(&x, &b, 8);
    return x;
}
static int isnan64(f64 x) { return x != x; }
static int isnan32(f32 x) { return x != x; }
/* infinity from the exponent bits: comparing against a large constant also catches finite values near the maximum */
static int isinf32(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    return (b & 0x7FFFFFFFu) == 0x7F800000u;
}
static int isinf64(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    return (b & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull;
}
static int snan32(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    return isnan32(x) && !(b & 0x00400000u);
}
static int snan64(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    return isnan64(x) && !(b & 0x0008000000000000ull);
}
static f32 quiet32(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    b |= 0x00400000u;
    memcpy(&x, &b, 4);
    return x;
}
static f64 quiet64(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    b |= 0x0008000000000000ull;
    memcpy(&x, &b, 8);
    return x;
}
/* FPProcessNaNs, for when a or b is a NaN: the first signalling NaN quieted, else the first quiet NaN */
static f32 pnan32(f32 a, f32 b) {
    if (snan32(a)) return quiet32(a);
    if (snan32(b)) return quiet32(b);
    return isnan32(a) ? a : b;
}
static f64 pnan64(f64 a, f64 b) {
    if (snan64(a)) return quiet64(a);
    if (snan64(b)) return quiet64(b);
    return isnan64(a) ? a : b;
}
static f64 trunc_(f64 x) {
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    f64 r = (f64)(i64)x;
    if (r == 0 && x < 0) return -0.0;
    return r;
}
static f64 floor_(f64 x) {
    f64 t = trunc_(x);
    if (t > x) t -= 1.0;
    if (t == 0 && x < 0) return x == 0 ? x : -0.0;
    return t;
}
static f64 ceil_(f64 x) {
    f64 t = trunc_(x);
    if (t < x) t += 1.0;
    if (t == 0 && x < 0) return -0.0;
    return t;
}
static f64 rne_(f64 x) {
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    f64 t = floor_(x), d = x - t;
    f64 r;
    if (d > 0.5)
        r = t + 1;
    else if (d < 0.5)
        r = t;
    else
        r = (((i64)t) & 1) ? t + 1 : t;
    if (r == 0 && (x < 0 || (x == 0 && 1 / x < 0))) return -0.0;
    return r;
}
static f64 rna_(f64 x) {
    if (!(fabs_(x) < 4503599627370496.0)) return x;
    f64 t = trunc_(x);
    if (fabs_(x - t) >= 0.5) t += x < 0 ? -1.0 : 1.0;
    if (t == 0 && x < 0) return -0.0;
    return t;
}
f64 fp_round(f64 x, int rm) {
    if (x == 0) return x;
    if (isnan64(x)) return quiet64(x);
    switch (rm) {
        case 0: return rne_(x);
        case 1: return ceil_(x);
        case 2: return floor_(x);
        case 3: return trunc_(x);
        default: return rna_(x);
    }
}
f32 fp_round32(f32 x, int rm) { return (f32)fp_round((f64)x, rm); }

i64 fcvt_s64(f64 x, int rm) {
    if (isnan64(x)) return 0;
    x = fp_round(x, rm);
    if (x >= 9223372036854775808.0) return 0x7FFFFFFFFFFFFFFFll;
    if (x < -9223372036854775808.0) return (i64)0x8000000000000000ull;
    return (i64)x;
}
u64 fcvt_u64(f64 x, int rm) {
    if (isnan64(x)) return 0;
    x = fp_round(x, rm);
    if (x >= 18446744073709551616.0) return ~0ull;
    if (x <= 0) return 0;
    if (x >= 9223372036854775808.0) return (u64)(i64)(x - 9223372036854775808.0) + 0x8000000000000000ull;
    return (u64)(i64)x;
}
i32 fcvt_s32(f64 x, int rm) {
    if (isnan64(x)) return 0;
    x = fp_round(x, rm);
    if (x >= 2147483648.0) return 0x7FFFFFFF;
    if (x < -2147483648.0) return (i32)0x80000000u;
    return (i32)x;
}
u32 fcvt_u32(f64 x, int rm) {
    if (isnan64(x)) return 0;
    x = fp_round(x, rm);
    if (x >= 4294967296.0) return 0xFFFFFFFFu;
    if (x <= 0) return 0;
    return (u32)(i64)x;
}

/* ================================================================ min/max */
f32 fp_max32(f32 a, f32 b) {
    if (isnan32(a) || isnan32(b)) return pnan32(a, b);
    if (a == 0 && b == 0) {
        u32 x, y;
        memcpy(&x, &a, 4);
        memcpy(&y, &b, 4);
        return (x & y) >> 31 ? a : (x >> 31 ? b : a);
    }
    return a > b ? a : b;
}
f32 fp_min32(f32 a, f32 b) {
    if (isnan32(a) || isnan32(b)) return pnan32(a, b);
    if (a == 0 && b == 0) {
        u32 x, y;
        memcpy(&x, &a, 4);
        memcpy(&y, &b, 4);
        return (x | y) >> 31 ? (x >> 31 ? a : b) : a;
    }
    return a < b ? a : b;
}
f64 fp_max64(f64 a, f64 b) {
    if (isnan64(a) || isnan64(b)) return pnan64(a, b);
    if (a == 0 && b == 0) {
        u64 x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return (x & y) >> 63 ? a : (x >> 63 ? b : a);
    }
    return a > b ? a : b;
}
f64 fp_min64(f64 a, f64 b) {
    if (isnan64(a) || isnan64(b)) return pnan64(a, b);
    if (a == 0 && b == 0) {
        u64 x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return (x | y) >> 63 ? (x >> 63 ? a : b) : a;
    }
    return a < b ? a : b;
}
static int qnan32(f32 a) { return isnan32(a) && !snan32(a); }
static int qnan64(f64 a) { return isnan64(a) && !snan64(a); }
f32 fp_maxnm32(f32 a, f32 b) {
    if (qnan32(a) && !isnan32(b)) return b;
    if (qnan32(b) && !isnan32(a)) return a;
    return fp_max32(a, b);
}
f32 fp_minnm32(f32 a, f32 b) {
    if (qnan32(a) && !isnan32(b)) return b;
    if (qnan32(b) && !isnan32(a)) return a;
    return fp_min32(a, b);
}
f64 fp_maxnm64(f64 a, f64 b) {
    if (qnan64(a) && !isnan64(b)) return b;
    if (qnan64(b) && !isnan64(a)) return a;
    return fp_max64(a, b);
}
f64 fp_minnm64(f64 a, f64 b) {
    if (qnan64(a) && !isnan64(b)) return b;
    if (qnan64(b) && !isnan64(a)) return a;
    return fp_min64(a, b);
}

/* sqrt: the compiler's (sqrtsd). tcc has no builtin: Newton steps from an exponent-halving guess, then the last ulp
   picked by search */
#if defined(__TINYC__)
f64 fp_sqrt64(f64 x) {
    if (isnan64(x) || x == 0) return x;
    if (x < 0) {
        u64 n = 0x7FF8000000000000ull;
        f64 r;
        memcpy(&r, &n, 8);
        return r;
    }
    if (x > 1.7e308) return x; /* inf */
    u64 b;
    memcpy(&b, &x, 8);
    b = (b >> 1) + 0x1FF8000000000000ull;
    f64 r;
    memcpy(&r, &b, 8);
    for (int i = 0; i < 6; i++) r = 0.5 * (r + x / r);
    /* fix up the last ulp */
    {
        f64 lo = r;
        u64 t;
        memcpy(&t, &lo, 8);
        for (int k = -2; k <= 2; k++) {
            u64 u = t + k;
            f64 y;
            memcpy(&y, &u, 8);
            f64 e1 = y * y - x, e0 = r * r - x;
            if (fabs_(e1) < fabs_(e0)) r = y;
        }
    }
    return r;
}
#else
f64 fp_sqrt64(f64 x) {
    if (x < 0) {
        u64 n = 0x7FF8000000000000ull;
        memcpy(&x, &n, 8);
        return x;
    }
    return __builtin_sqrt(x);
}
#endif
f32 fp_sqrt32(f32 x) {
    if (x < 0) {
        u32 n = 0x7FC00000u;
        memcpy(&x, &n, 4);
        return x;
    }
    return (f32)fp_sqrt64((f64)x);
}
/* the same as the FMLA32/FMLA64 macros in cpu.h, see there */
f32 fp_fma32(f32 a, f32 b, f32 c) { return FMLA32(c, a, b); }
f64 fp_fma64(f64 a, f64 b, f64 c) { return FMLA64(c, a, b); }
static int signbit32(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    return (int)(b >> 31);
}
static int signbit64(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    return (int)(b >> 63);
}
/* FPMulX: infinity times zero is 2 with the sign of the product, zeros included */
f32 fp_mulx32(f32 a, f32 b) {
    if (isnan32(a) || isnan32(b)) return pnan32(a, b);
    if ((a == 0 && isinf32(b)) || (b == 0 && isinf32(a))) return signbit32(a) ^ signbit32(b) ? -2.0f : 2.0f;
    return a * b;
}
f64 fp_mulx64(f64 a, f64 b) {
    if (isnan64(a) || isnan64(b)) return pnan64(a, b);
    if ((a == 0 && isinf64(b)) || (b == 0 && isinf64(a))) return signbit64(a) ^ signbit64(b) ? -2.0 : 2.0;
    return a * b;
}

/* ---- recip estimates, straight from the ARM ARM (RecipEstimate / RecipSqrtEstimate) ---- */
static u32 recip_est(u32 a) { /* a in [256,511] */
    a = a * 2 + 1;
    u32 b = (1u << 19) / a;
    return (b + 1) >> 1;
}
static u32 rsqrt_est(u32 a) { /* a in [128,511] */
    if (a < 256)
        a = a * 2 + 1;
    else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    u32 b = 512;
    while (a * (b + 1) * (b + 1) < (1u << 28)) b++;
    return (b + 1) >> 1;
}
/* FPRecipEstimate / FPRSqrtEstimate, f32 and f64, assume RN and no FZ (an overflow gives +-inf whatever FPCR's
   rounding mode) */
static u64 recpe_bits(u64 op, int N) {
    int eb = N == 64 ? 11 : 8, fbits = N == 64 ? 52 : 23;
    u64 s = op >> (N - 1) << (N - 1);
    int e = (int)((op >> fbits) & ((1u << eb) - 1));
    u64 f = op & ((1ull << fbits) - 1);
    u64 emax = (1u << eb) - 1;
    if (e == (int)emax && f) return op | (1ull << (fbits - 1)); /* NaN -> quiet */
    if (e == (int)emax) return s;                               /* inf -> 0 */
    if (e == 0 && f == 0) return s | (emax << fbits);           /* 0 -> inf */
    u64 fr = N == 64 ? f : f << 29;                             /* 52 bits */
    if (e == 0 && !(fr >> 50)) return s | (emax << fbits);      /* overflow -> inf (RN) */
    if (e == 0) {
        if (!(fr & (1ull << 51))) {
            e = -1;
            fr = (fr << 2) & ((1ull << 52) - 1);
        } else
            fr = (fr << 1) & ((1ull << 52) - 1);
    }
    u32 sc = (u32)(256 + (fr >> 44));
    u32 est = recip_est(sc);
    int re = (N == 64 ? 2045 : 253) - e;
    u64 rf = (u64)(est & 0xFF) << 44;
    if (re == 0)
        rf = (1ull << 51) | (rf >> 1);
    else if (re == -1) {
        rf = (1ull << 50) | (rf >> 2);
        re = 0;
    }
    return s | ((u64)re << fbits) | (N == 64 ? rf : rf >> 29);
}
static u64 rsqrte_bits(u64 op, int N) {
    int eb = N == 64 ? 11 : 8, fbits = N == 64 ? 52 : 23;
    u64 s = op >> (N - 1);
    int e = (int)((op >> fbits) & ((1u << eb) - 1));
    u64 f = op & ((1ull << fbits) - 1);
    u64 emax = (1u << eb) - 1;
    if (e == (int)emax && f) return op | (1ull << (fbits - 1));
    if (e == 0 && f == 0) return (s << (N - 1)) | (emax << fbits);
    if (s) return (emax << fbits) | (1ull << (fbits - 1)); /* default NaN */
    if (e == (int)emax) return 0;
    u64 fr = N == 64 ? f : f << 29;
    if (e == 0) {
        while (!(fr & (1ull << 51))) {
            fr <<= 1;
            e--;
        }
        fr = (fr << 1) & ((1ull << 52) - 1);
    }
    u32 sc = !(e & 1) ? (u32)(256 + (fr >> 44)) : (u32)(128 + (fr >> 45));
    u32 est = rsqrt_est(sc);
    int re = ((N == 64 ? 3068 : 380) - e) / 2;
    return ((u64)re << fbits) | ((u64)(est & 0xFF) << (fbits - 8));
}
f32 fp_recpe32(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    b = (u32)recpe_bits(b, 32);
    memcpy(&x, &b, 4);
    return x;
}
f64 fp_recpe64(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    b = recpe_bits(b, 64);
    memcpy(&x, &b, 8);
    return x;
}
f32 fp_rsqrte32(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    b = (u32)rsqrte_bits(b, 32);
    memcpy(&x, &b, 4);
    return x;
}
f64 fp_rsqrte64(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    b = rsqrte_bits(b, 64);
    memcpy(&x, &b, 8);
    return x;
}
/* FPRecipStepFused / FPRSqrtStepFused: the first operand is negated before anything else (a NaN in it comes out with
   its sign flipped), then 2 + a*b or (3 + a*b) / 2 with one rounding. For the second, halving the larger operand
   instead of the sum keeps 3 + a*b from overflowing on the way (and is exact: only a tiny operand would lose a bit,
   and then the product doesn't reach the result) */
f32 fp_recps32(f32 a, f32 b) {
    a = -a;
    if (isnan32(a) || isnan32(b)) return pnan32(a, b);
    if ((a == 0 && isinf32(b)) || (b == 0 && isinf32(a))) return 2.0f;
    return fp_fma32(a, b, 2.0f);
}
f64 fp_recps64(f64 a, f64 b) {
    a = -a;
    if (isnan64(a) || isnan64(b)) return pnan64(a, b);
    if ((a == 0 && isinf64(b)) || (b == 0 && isinf64(a))) return 2.0;
    return fp_fma64(a, b, 2.0);
}
f32 fp_rsqrts32(f32 a, f32 b) {
    a = -a;
    if (isnan32(a) || isnan32(b)) return pnan32(a, b);
    if ((a == 0 && isinf32(b)) || (b == 0 && isinf32(a))) return 1.5f;
    return fabs_(a) >= fabs_(b) ? fp_fma32(a * 0.5f, b, 1.5f) : fp_fma32(a, b * 0.5f, 1.5f);
}
f64 fp_rsqrts64(f64 a, f64 b) {
    a = -a;
    if (isnan64(a) || isnan64(b)) return pnan64(a, b);
    if ((a == 0 && isinf64(b)) || (b == 0 && isinf64(a))) return 1.5;
    return fabs_(a) >= fabs_(b) ? fp_fma64(a * 0.5, b, 1.5) : fp_fma64(a, b * 0.5, 1.5);
}
u32 urecpe32(u32 x) {
    if (!(x & 0x80000000u)) return 0xFFFFFFFFu;
    return (recip_est(x >> 23) | 0x100) << 23;
}
u32 ursqrte32(u32 x) {
    if (!(x & 0xC0000000u)) return 0xFFFFFFFFu;
    return (rsqrt_est(x >> 23) | 0x100) << 23;
}

/* ---- half precision ---- */
u16 f32_to_f16(f32 x) {
    u32 b;
    memcpy(&b, &x, 4);
    u32 s = (b >> 16) & 0x8000;
    int e = (int)((b >> 23) & 0xFF) - 127 + 15;
    u32 m = b & 0x7FFFFF;
    if (((b >> 23) & 0xFF) == 0xFF) return (u16)(s | 0x7C00 | (m ? 0x200 | (m >> 13) : 0));
    if (e >= 31) return (u16)(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (u16)s;
        m |= 0x800000;
        u32 sh = (u32)(14 - e);
        u32 r = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (r & 1))) r++;
        return (u16)(s | r);
    }
    u32 r = ((u32)e << 10) | (m >> 13);
    u32 rem = m & 0x1FFF;
    if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++;
    return (u16)(s | r);
}
f32 f16_to_f32(u16 h) {
    u32 s = (u32)(h & 0x8000) << 16;
    int e = (h >> 10) & 0x1F;
    u32 m = h & 0x3FF, r;
    if (e == 0x1F)
        r = s | 0x7F800000 | (m << 13) | (m ? 0x00400000u : 0); /* a signalling NaN comes out quiet */
    else if (e == 0) {
        if (!m)
            r = s;
        else {
            e = 1;
            while (!(m & 0x400)) {
                m <<= 1;
                e--;
            }
            m &= 0x3FF;
            r = s | ((u32)(e - 15 + 127) << 23) | (m << 13);
        }
    } else
        r = s | ((u32)(e - 15 + 127) << 23) | (m << 13);
    f32 f;
    memcpy(&f, &r, 4);
    return f;
}
/* straight from the double: going through f32 would round twice */
u16 f64_to_f16(f64 x) {
    u64 b;
    memcpy(&b, &x, 8);
    u32 s = (u32)(b >> 48) & 0x8000;
    int e = (int)((b >> 52) & 0x7FF) - 1023 + 15;
    u64 m = b & 0xFFFFFFFFFFFFFull;
    if (((b >> 52) & 0x7FF) == 0x7FF) return (u16)(s | 0x7C00 | (m ? 0x200 | (u32)(m >> 42) : 0));
    if (e >= 31) return (u16)(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (u16)s;
        m |= 1ull << 52;
        u32 sh = (u32)(43 - e);
        u64 r = m >> sh, rem = m & ((1ull << sh) - 1), half = 1ull << (sh - 1);
        if (rem > half || (rem == half && (r & 1))) r++;
        return (u16)(s | r);
    }
    u32 r = ((u32)e << 10) | (u32)(m >> 42);
    u64 rem = m & ((1ull << 42) - 1);
    if (rem > (1ull << 41) || (rem == (1ull << 41) && (r & 1))) r++;
    return (u16)(s | r);
}
