/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* Part of cpu_helpers.c, which includes it: AdvSIMD arithmetic: lanes, saturation, per-lane floating point, and the instruction groups (three-same, three-different, two-register, across lanes, copy, modified and shift immediates, by-element, permute, TBL). Not compiled on its own. */
/* =============================================================== AdvSIMD */
#define VR(n) (&c->v[n])
static u64 eget(const V128* v, int sz, int i) {
    switch (sz) {
        case 0: return v->b[i];
        case 1: return v->h[i];
        case 2: return v->s[i];
        default: return v->d[i];
    }
}
static void eset(V128* v, int sz, int i, u64 x) {
    switch (sz) {
        case 0: v->b[i] = (u8)x; break;
        case 1: v->h[i] = (u16)x; break;
        case 2: v->s[i] = (u32)x; break;
        default: v->d[i] = x;
    }
}
static i64 sx(u64 x, int eb) { return eb >= 64 ? (i64)x : (i64)(x << (64 - eb)) >> (64 - eb); }
static u64 zx(u64 x, int eb) { return eb >= 64 ? x : x & ((1ull << eb) - 1); }
static void qc(CPU* c) { c->fpsr |= 1u << 27; }
static i64 ssat(CPU* c, i64 v, int eb) {
    i64 r = sat_s(v, eb);
    if (r != v) qc(c);
    return r;
}
static u64 usat(CPU* c, i64 v, int eb) {
    u64 r = sat_u(v, eb);
    if ((i64)r != v) qc(c);
    return r;
}
/* sat add/sub, 64 bit safe */
static u64 sqadd_(CPU* c, u64 a, u64 b, int eb) {
    if (eb < 64) return zx((u64)ssat(c, sx(a, eb) + sx(b, eb), eb), eb);
    u64 r = a + b;
    if ((i64)((a ^ r) & (b ^ r)) < 0) {
        qc(c);
        return ((i64)a < 0) ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    }
    return r;
}
static u64 uqadd_(CPU* c, u64 a, u64 b, int eb) {
    if (eb < 64) return usat(c, (i64)(zx(a, eb) + zx(b, eb)), eb);
    u64 r = a + b;
    if (r < a) {
        qc(c);
        return ~0ull;
    }
    return r;
}
static u64 sqsub_(CPU* c, u64 a, u64 b, int eb) {
    if (eb < 64) return zx((u64)ssat(c, sx(a, eb) - sx(b, eb), eb), eb);
    u64 r = a - b;
    if ((i64)((a ^ b) & (a ^ r)) < 0) {
        qc(c);
        return ((i64)a < 0) ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
    }
    return r;
}
static u64 uqsub_(CPU* c, u64 a, u64 b, int eb) {
    if (eb < 64) return usat(c, (i64)zx(a, eb) - (i64)zx(b, eb), eb);
    if (a < b) {
        qc(c);
        return 0;
    }
    return a - b;
}
/* register shifts: SSHL USHL SRSHL URSHL SQSHL UQSHL SQRSHL UQRSHL */
static u64 vshl(CPU* c, u64 a, u64 b, int eb, int U, int round, int sat) {
    int sh = (i8)(u8)b;
    if (sh >= 0) {
        if (!U) {
            i64 x = sx(a, eb);
            if (!sat) return sh >= eb ? 0 : zx((u64)x << sh, eb);
            if (x == 0) return 0;
            if (sh < eb) {
                i64 r = (i64)((u64)x << sh);
                if ((r >> sh) == x && sx(zx((u64)r, eb), eb) == r) return zx((u64)r, eb);
            }
            qc(c);
            return zx(x < 0 ? (u64)1 << (eb - 1) : ((u64)1 << (eb - 1)) - 1, eb);
        } else {
            u64 x = zx(a, eb);
            if (!sat) return sh >= eb ? 0 : zx(x << sh, eb);
            if (x == 0) return 0;
            if (sh < eb && !(x >> (eb - sh))) return zx(x << sh, eb);
            qc(c);
            return zx(~0ull, eb);
        }
    }
    int n = -sh;
    if (!U) {
        i64 x = sx(a, eb);
        if (round) {
            if (n > 64) return 0;
            i64 hi = n >= 64 ? (x < 0 ? -1 : 0) : x >> n;
            return zx((u64)(hi + ((x >> (n - 1)) & 1)), eb);
        }
        return zx((u64)(n >= 64 ? (x < 0 ? -1 : 0) : x >> n), eb);
    } else {
        u64 x = zx(a, eb);
        if (round) {
            if (n > 64) return 0;
            u64 hi = n >= 64 ? 0 : x >> n;
            return zx(hi + ((x >> (n - 1)) & 1), eb);
        }
        return n >= 64 ? 0 : x >> n;
    }
}
static int fprm(CPU* c) { return (c->fpcr >> 22) & 3; }

/* ---- per-lane fp; doing +,-,*,/,sqrt in double then rounding to single is exact ---- */
static f64 rdf(const V128* v, int sz, int i) { return sz ? v->df[i] : (f64)v->f[i]; }
static void wrf(V128* v, int sz, int i, f64 x) {
    if (sz)
        v->df[i] = x;
    else
        v->f[i] = (f32)x;
}
static f64 fmax_(f64 a, f64 b, int sz) { return sz ? fp_max64(a, b) : fp_max32((f32)a, (f32)b); }
static f64 fmin_(f64 a, f64 b, int sz) { return sz ? fp_min64(a, b) : fp_min32((f32)a, (f32)b); }
static f64 fmaxnm_(f64 a, f64 b, int sz) { return sz ? fp_maxnm64(a, b) : fp_maxnm32((f32)a, (f32)b); }
static f64 fminnm_(f64 a, f64 b, int sz) { return sz ? fp_minnm64(a, b) : fp_minnm32((f32)a, (f32)b); }
static f64 fmla_(f64 d, f64 a, f64 b, int sz) { return sz ? fp_fma64(a, b, d) : (f64)fp_fma32((f32)a, (f32)b, (f32)d); }
static f64 fop2(int sel, f64 a, f64 b, int sz) { /* sel = our own op code */
    switch (sel) {
        case 0: return a + b;
        case 1: return a - b;
        case 2: return a * b;
        case 3: return a / b;
        case 4: return fmax_(a, b, sz);
        case 5: return fmin_(a, b, sz);
        case 6: return fmaxnm_(a, b, sz);
        case 7: return fminnm_(a, b, sz);
        case 8: {
            f64 d = a - b;
            return d < 0 ? -d : d;
        }
        case 9: return sz ? fp_mulx64(a, b) : fp_mulx32((f32)a, (f32)b);
        case 10: return sz ? fp_recps64(a, b) : fp_recps32((f32)a, (f32)b);
        case 11: return sz ? fp_rsqrts64(a, b) : fp_rsqrts32((f32)a, (f32)b);
    }
    return 0;
}
static u64 fcmpmask(int sel, f64 a, f64 b, int sz) {
    int r = 0;
    switch (sel) {
        case 0: r = a == b; break;
        case 1: r = a >= b; break;
        case 2: r = a > b; break;
        case 3: r = (a < 0 ? -a : a) >= (b < 0 ? -b : b); break;
        case 4: r = (a < 0 ? -a : a) > (b < 0 ? -b : b); break;
        case 5: r = a <= b; break;
        case 6: r = a < b; break;
    }
    return r ? (sz ? ~0ull : 0xFFFFFFFFull) : 0;
}

/* three-same, vector + scalar */
static int simd_3same(CPU* c, u32 w, int scalar) {
    int Q = (w >> 30) & 1, U = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 11) & 31,
        rn = (w >> 5) & 31, rd = w & 31;
    V128 a = *VR(rn), b = *VR(rm), d = *VR(rd), r;
    memset(&r, 0, 16);
    if (scalar) Q = 0;
    if (op >= 0x18) {
        int sz = size & 1, hi = size >> 1;
        int n = scalar ? 1 : (sz ? 2 : (Q ? 4 : 2));
        if (sz && !Q && !scalar) return 0;
        int pair = 0, sel = -1, cmp = -1, acc = 0;
        if (!U) {
            if (!hi) {
                switch (op) {
                    case 0x18: sel = 6; break;
                    case 0x19: acc = 1; break;
                    case 0x1A: sel = 0; break;
                    case 0x1B: sel = 9; break;
                    case 0x1C: cmp = 0; break;
                    case 0x1E: sel = 4; break;
                    case 0x1F: sel = 10; break;
                    default: return 0;
                }
            } else {
                switch (op) {
                    case 0x18: sel = 7; break;
                    case 0x19: acc = 2; break;
                    case 0x1A: sel = 1; break;
                    case 0x1E: sel = 5; break;
                    case 0x1F: sel = 11; break;
                    default: return 0;
                }
            }
        } else {
            if (!hi) {
                switch (op) {
                    case 0x18:
                        sel = 6;
                        pair = 1;
                        break;
                    case 0x1A:
                        sel = 0;
                        pair = 1;
                        break;
                    case 0x1B: sel = 2; break;
                    case 0x1C: cmp = 1; break;
                    case 0x1D: cmp = 3; break;
                    case 0x1E:
                        sel = 4;
                        pair = 1;
                        break;
                    case 0x1F: sel = 3; break;
                    default: return 0;
                }
            } else {
                switch (op) {
                    case 0x18:
                        sel = 7;
                        pair = 1;
                        break;
                    case 0x1A: sel = 8; break;
                    case 0x1C: cmp = 2; break;
                    case 0x1D: cmp = 4; break;
                    case 0x1E:
                        sel = 5;
                        pair = 1;
                        break;
                    default: return 0;
                }
            }
        }
        if (scalar && (pair || acc || sel == 0 || sel == 1 || sel == 2 || sel == 3 || (sel >= 4 && sel <= 7))) return 0;
        for (int i = 0; i < n; i++) {
            f64 x, y;
            if (pair) {
                int h = n / 2;
                const V128* s = i < h ? &a : &b;
                int k = (i < h ? i : i - h) * 2;
                x = rdf(s, sz, k);
                y = rdf(s, sz, k + 1);
            } else {
                x = rdf(&a, sz, i);
                y = rdf(&b, sz, i);
            }
            if (cmp >= 0) {
                eset(&r, sz ? 3 : 2, i, fcmpmask(cmp, x, y, sz));
                continue;
            }
            if (acc == 1) {
                wrf(&r, sz, i, fmla_(rdf(&d, sz, i), x, y, sz));
                continue;
            }
            if (acc == 2) {
                wrf(&r, sz, i, fmla_(rdf(&d, sz, i), -x, y, sz));
                continue;
            }
            wrf(&r, sz, i, fop2(sel, x, y, sz));
        }
        *VR(rd) = r;
        return 1;
    }
    int eb = 8 << size;
    int n = scalar ? 1 : (Q ? 128 : 64) / eb;
    if (op == 3) { /* 128 bit logic */
        if (scalar) return 0;
        for (int i = 0; i < 2; i++) {
            u64 x = a.d[i], y = b.d[i], z = d.d[i], o;
            if (!U) {
                switch (size) {
                    case 0: o = x & y; break;
                    case 1: o = x & ~y; break;
                    case 2: o = x | y; break;
                    default: o = x | ~y;
                }
            } else {
                switch (size) {
                    case 0: o = x ^ y; break;
                    case 1: o = (z & x) | (~z & y); break;
                    case 2: o = (z & ~y) | (x & y); break;
                    default: o = (z & y) | (x & ~y);
                }
            }
            r.d[i] = o;
        }
        if (!Q) r.d[1] = 0;
        *VR(rd) = r;
        return 1;
    }
    if (size == 3 && !Q && !scalar) return 0;
    for (int i = 0; i < n; i++) {
        u64 x, y, z = eget(&d, size, i), o = 0;
        int pairop = (op >= 0x14 && op <= 0x15) || (op == 0x17 && !U);
        if (pairop) {
            int h = n / 2;
            const V128* s = i < h ? &a : &b;
            int k = (i < h ? i : i - h) * 2;
            x = eget(s, size, k);
            y = eget(s, size, k + 1);
        } else {
            x = eget(&a, size, i);
            y = eget(&b, size, i);
        }
        i64 sa = sx(x, eb), sb = sx(y, eb);
        u64 ua = zx(x, eb), ub = zx(y, eb);
        switch (op) {
            case 0x00:
                if (eb == 64) return 0;
                o = U ? (ua + ub) >> 1 : (u64)((sa + sb) >> 1);
                break;
            case 0x01: o = U ? uqadd_(c, x, y, eb) : sqadd_(c, x, y, eb); break;
            case 0x02:
                if (eb == 64) return 0;
                o = U ? (ua + ub + 1) >> 1 : (u64)((sa + sb + 1) >> 1);
                break;
            case 0x04:
                if (eb == 64) return 0;
                o = U ? (u64)((i64)(ua - ub) >> 1) : (u64)((sa - sb) >> 1);
                break;
            case 0x05: o = U ? uqsub_(c, x, y, eb) : sqsub_(c, x, y, eb); break;
            case 0x06: o = (U ? ua > ub : sa > sb) ? ~0ull : 0; break;
            case 0x07: o = (U ? ua >= ub : sa >= sb) ? ~0ull : 0; break;
            case 0x08: o = vshl(c, x, y, eb, U, 0, 0); break;
            case 0x09: o = vshl(c, x, y, eb, U, 0, 1); break;
            case 0x0A: o = vshl(c, x, y, eb, U, 1, 0); break;
            case 0x0B: { /* rounding sat: round then saturate */
                int sh = (i8)(u8)y;
                if (sh < 0)
                    o = vshl(c, x, y, eb, U, 1, 0);
                else
                    o = vshl(c, x, y, eb, U, 0, 1);
            } break;
            case 0x0C:
                if (eb == 64) return 0;
                o = U ? (ua > ub ? ua : ub) : (u64)(sa > sb ? sa : sb);
                break;
            case 0x0D:
                if (eb == 64) return 0;
                o = U ? (ua < ub ? ua : ub) : (u64)(sa < sb ? sa : sb);
                break;
            case 0x0E:
                if (eb == 64) return 0;
                o = U ? (ua > ub ? ua - ub : ub - ua) : (u64)(sa > sb ? sa - sb : sb - sa);
                break;
            case 0x0F:
                if (eb == 64) return 0;
                o = z + (U ? (ua > ub ? ua - ub : ub - ua) : (u64)(sa > sb ? sa - sb : sb - sa));
                break;
            case 0x10: o = U ? x - y : x + y; break;
            case 0x11: o = U ? (ua == ub ? ~0ull : 0) : ((x & y) ? ~0ull : 0); break;
            case 0x12:
                if (eb == 64) return 0;
                o = U ? z - x * y : z + x * y;
                break;
            case 0x13:
                if (eb == 64) return 0;
                if (U) {
                    if (size) return 0;
                    u32 p = 0;
                    for (int k = 0; k < 8; k++)
                        if ((ua >> k) & 1) p ^= (u32)ub << k;
                    o = p;
                } else
                    o = x * y;
                break;
            case 0x14:
                if (eb == 64) return 0;
                o = U ? (ua > ub ? ua : ub) : (u64)(sa > sb ? sa : sb);
                break;
            case 0x15:
                if (eb == 64) return 0;
                o = U ? (ua < ub ? ua : ub) : (u64)(sa < sb ? sa : sb);
                break;
            case 0x16: {
                if (size != 1 && size != 2) return 0;
                i64 p = 2 * sa * sb;
                if (U) p += (i64)1 << (eb - 1);
                p >>= eb;
                if (sa == sb && sa == -((i64)1 << (eb - 1))) {
                    p = ((i64)1 << (eb - 1)) - 1;
                    qc(c);
                }
                o = (u64)p;
            } break;
            case 0x17:
                if (U) return 0;
                o = x + y;
                break;
            default: return 0;
        }
        if (scalar && size != 3 && !(op == 0x01 || op == 0x05 || op == 0x09 || op == 0x0B || op == 0x16)) return 0;
        eset(&r, size, i, zx(o, eb));
    }
    *VR(rd) = r;
    return 1;
}

/* three-different (long, wide, narrow) */
static int simd_3diff(CPU* c, u32 w, int scalar) {
    int Q = (w >> 30) & 1, U = (w >> 29) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 15,
        rn = (w >> 5) & 31, rd = w & 31;
    V128 a = *VR(rn), b = *VR(rm), d = *VR(rd), r;
    memset(&r, 0, 16);
    if (size == 3 && op != 14) return 0;
    int eb = 8 << size, wb = eb * 2, n = 64 / eb, off = Q ? n : 0;
    if (scalar) {
        if (!(op == 9 || op == 11 || op == 13) || U) return 0;
        n = 1;
        off = 0;
    }
    if (op == 14) { /* PMULL */
        if (U) return 0;
        if (size == 0) {
            for (int i = 0; i < 8; i++) {
                u32 x = a.b[i + off], y = b.b[i + off], p = 0;
                for (int k = 0; k < 8; k++)
                    if ((x >> k) & 1) p ^= y << k;
                r.h[i] = (u16)p;
            }
        } else if (size == 3) {
            v_pmull(&r, &a, &b, 3, Q);
        } else
            return 0;
        *VR(rd) = r;
        return 1;
    }
    if (op == 4 || op == 6) { /* ADDHN/SUBHN/RADDHN/RSUBHN */
        V128 o = Q ? d : r;
        if (!Q) memset(&o, 0, 16);
        for (int i = 0; i < n; i++) {
            u64 x = eget(&a, size + 1, i), y = eget(&b, size + 1, i), s = op == 4 ? x + y : x - y;
            if (U) s += (u64)1 << (eb - 1);
            eset(&o, size, i + off, zx(s >> eb, eb));
        }
        *VR(rd) = o;
        return 1;
    }
    for (int i = 0; i < n; i++) {
        u64 xa, xb = eget(&b, size, i + off);
        i64 sa, sb = sx(xb, eb);
        u64 ua, ub = zx(xb, eb);
        if (op == 1 || op == 3) {
            xa = eget(&a, size + 1, i);
            sa = sx(xa, wb);
            ua = zx(xa, wb);
        } else {
            xa = eget(&a, size, i + off);
            sa = sx(xa, eb);
            ua = zx(xa, eb);
        }
        u64 z = eget(&d, size + 1, i), o;
        switch (op) {
            case 0:
            case 1: o = U ? ua + ub : (u64)(sa + sb); break;
            case 2:
            case 3: o = U ? ua - ub : (u64)(sa - sb); break;
            case 5: o = z + (U ? (ua > ub ? ua - ub : ub - ua) : (u64)(sa > sb ? sa - sb : sb - sa)); break;
            case 7: o = U ? (ua > ub ? ua - ub : ub - ua) : (u64)(sa > sb ? sa - sb : sb - sa); break;
            case 8: o = z + (U ? ua * ub : (u64)(sa * sb)); break;
            case 10: o = z - (U ? ua * ub : (u64)(sa * sb)); break;
            case 12: o = U ? ua * ub : (u64)(sa * sb); break;
            case 9:
            case 11:
            case 13: {
                if (U || size == 0) return 0;
                i64 p = 2 * sa * sb;
                if (sa == sb && sa == -((i64)1 << (eb - 1))) {
                    p = sat_s(0x7FFFFFFFFFFFFFFFll, wb);
                    qc(c);
                }
                if (op == 13)
                    o = (u64)p;
                else if (op == 9)
                    o = sqadd_(c, z, (u64)p, wb);
                else
                    o = sqsub_(c, z, (u64)p, wb);
            } break;
            default: return 0;
        }
        eset(&r, size + 1, i, zx(o, wb));
    }
    *VR(rd) = r;
    return 1;
}

static u64 sqxtn_(CPU* c, u64 x, int eb, int kind) { /* 0 SQXTN 1 UQXTN 2 SQXTUN */
    i64 s = sx(x, eb * 2);
    u64 u = zx(x, eb * 2);
    if (kind == 0) return zx((u64)ssat(c, s, eb), eb);
    if (kind == 1) {
        u64 mx = zx(~0ull, eb);
        if (u > mx) {
            qc(c);
            return mx;
        }
        return u;
    }
    return usat(c, s, eb);
}

/* two-reg misc, vector + scalar */
static int simd_2misc(CPU* c, u32 w, int scalar) {
    int Q = (w >> 30) & 1, U = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
        rd = w & 31;
    V128 a = *VR(rn), d = *VR(rd), r;
    memset(&r, 0, 16);
    if (scalar) Q = 0;
    int eb = 8 << size, n = scalar ? 1 : (Q ? 128 : 64) / eb;
    int sz = size & 1, fn = scalar ? 1 : (sz ? 2 : (Q ? 4 : 2));
    int hi = size >> 1;
    /* ---- fp ---- */
    if (op >= 0x0C && op <= 0x0F && hi) {
        if (sz && !Q && !scalar) return 0;
        for (int i = 0; i < fn; i++) {
            f64 x = rdf(&a, sz, i);
            if (op == 0x0F) {
                if (scalar) return 0;
                if (U) {
                    u64 v = eget(&a, sz ? 3 : 2, i) ^ (sz ? 0x8000000000000000ull : 0x80000000ull);
                    eset(&r, sz ? 3 : 2, i, v);
                } else {
                    u64 v = eget(&a, sz ? 3 : 2, i) & (sz ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull);
                    eset(&r, sz ? 3 : 2, i, v);
                }
                continue;
            }
            int k = op == 0x0C ? (U ? 1 : 2) : op == 0x0D ? (U ? 5 : 0) : op == 0x0E ? (U ? -1 : 6) : -1;
            if (k < 0) return 0;
            eset(&r, sz ? 3 : 2, i, fcmpmask(k, x, 0.0, sz));
        }
        *VR(rd) = r;
        return 1;
    }
    if (op >= 0x16) {
        if (op == 0x16 && !hi) { /* FCVTN / FCVTXN */
            if (scalar && !U) return 0;
            V128 o = Q ? d : r;
            int off = Q ? (sz ? 2 : 4) : 0;
            if (sz) {
                for (int i = 0; i < 2; i++) {
                    f64 x = a.df[i];
                    f32 y = (f32)x;
                    if (U) { /* round to odd */
                        u32 yb;
                        memcpy(&yb, &y, 4);
                        if ((f64)y != x && !isnan64(x) && !(yb & 1)) {
                            f64 dd = (f64)y;
                            if ((dd > x) == (y > 0))
                                yb--;
                            else
                                yb++;
                            yb |= 1;
                            memcpy(&y, &yb, 4);
                        }
                    }
                    if (scalar) {
                        o.d[0] = 0;
                        o.d[1] = 0;
                        o.f[0] = y;
                        break;
                    }
                    o.f[i + off] = y;
                }
            } else {
                if (U) return 0;
                for (int i = 0; i < 4; i++) o.h[i + off] = f32_to_f16(a.f[i]);
            }
            if (!Q && !scalar) o.d[1] = 0;
            *VR(rd) = o;
            return 1;
        }
        if (op == 0x17 && !hi && !U) { /* FCVTL */
            int off = Q ? (sz ? 2 : 4) : 0;
            if (sz) {
                for (int i = 0; i < 2; i++) r.df[i] = (f64)a.f[i + off];
            } else {
                for (int i = 0; i < 4; i++) r.f[i] = f16_to_f32(a.h[i + off]);
            }
            *VR(rd) = r;
            return 1;
        }
        if (sz && !Q && !scalar) return 0;
        for (int i = 0; i < fn; i++) {
            f64 x = rdf(&a, sz, i);
            u64 bits = eget(&a, sz ? 3 : 2, i);
            int eb2 = sz ? 64 : 32;
            int k = (U << 6) | (hi << 5) | op;
            switch (k) {
                case 0x18: wrf(&r, sz, i, fp_round(x, 0)); break;       /* FRINTN */
                case 0x38: wrf(&r, sz, i, fp_round(x, 1)); break;       /* FRINTP */
                case 0x19: wrf(&r, sz, i, fp_round(x, 2)); break;       /* FRINTM */
                case 0x39: wrf(&r, sz, i, fp_round(x, 3)); break;       /* FRINTZ */
                case 0x58: wrf(&r, sz, i, fp_round(x, 4)); break;       /* FRINTA */
                case 0x59: wrf(&r, sz, i, fp_round(x, fprm(c))); break; /* FRINTX */
                case 0x79: wrf(&r, sz, i, fp_round(x, fprm(c))); break; /* FRINTI */
                case 0x1A:
                case 0x3A:
                case 0x1B:
                case 0x3B:
                case 0x1C:
                case 0x5A:
                case 0x7A:
                case 0x5B:
                case 0x7B:
                case 0x5C: {
                    int rm = (k & 0x1F) == 0x1C ? 4 : (k & 0x1F) == 0x1A ? (hi ? 1 : 0) : (hi ? 3 : 2);
                    int uns = U;
                    u64 o = sz ? (uns ? fcvt_u64(x, rm) : (u64)fcvt_s64(x, rm))
                               : (uns ? fcvt_u32(x, rm) : (u64)(u32)fcvt_s32(x, rm));
                    eset(&r, sz ? 3 : 2, i, o);
                } break;
                case 0x1D: wrf(&r, sz, i, sz ? (f64)(i64)bits : (f64)(f32)(i32)(u32)bits); break; /* SCVTF */
                case 0x5D: wrf(&r, sz, i, sz ? (f64)bits : (f64)(f32)(u32)bits); break;           /* UCVTF */
                case 0x3D: wrf(&r, sz, i, sz ? fp_recpe64(x) : (f64)fp_recpe32((f32)x)); break;
                case 0x7D: wrf(&r, sz, i, sz ? fp_rsqrte64(x) : (f64)fp_rsqrte32((f32)x)); break;
                case 0x3C:
                    if (sz) return 0;
                    r.s[i] = urecpe32((u32)bits);
                    break;
                case 0x7C:
                    if (sz) return 0;
                    r.s[i] = ursqrte32((u32)bits);
                    break;
                case 0x7F: wrf(&r, sz, i, sz ? fp_sqrt64(x) : (f64)fp_sqrt32((f32)x)); break;
                case 0x3F: {
                    if (!scalar) return 0; /* FRECPX */
                    if (sz) {
                        u64 b = bits, e = (b >> 52) & 0x7FF;
                        if (e == 0x7FF && (b & 0xFFFFFFFFFFFFFull))
                            r.d[0] = b | 0x8000000000000ull;
                        else
                            r.d[0] = (b & 0x8000000000000000ull) | ((e ? (~e & 0x7FF) : 0x7FE) << 52);
                    } else {
                        u32 b = (u32)bits, e = (b >> 23) & 0xFF;
                        if (e == 0xFF && (b & 0x7FFFFF))
                            r.s[0] = b | 0x400000;
                        else
                            r.s[0] = (b & 0x80000000u) | ((e ? (~e & 0xFF) : 0xFE) << 23);
                    }
                } break;
                default: (void)eb2; return 0;
            }
        }
        *VR(rd) = r;
        return 1;
    }
    /* ---- int ---- */
    if (op == 0x12 || op == 0x14 || (op == 0x13 && U)) { /* XTN SQXTN UQXTN SQXTUN SHLL */
        if (size == 3) return 0;
        if (op == 0x13) {
            if (scalar) return 0;
            int off = Q ? (64 / eb) : 0;
            for (int i = 0; i < 64 / eb; i++) eset(&r, size + 1, i, eget(&a, size, i + off) << eb);
            *VR(rd) = r;
            return 1;
        }
        V128 o = Q ? d : r;
        int m = 64 / eb, off = Q ? m : 0;
        if (scalar) {
            m = 1;
            off = 0;
            memset(&o, 0, 16);
        }
        for (int i = 0; i < m; i++) {
            u64 x = eget(&a, size + 1, i), v;
            if (op == 0x12) {
                if (scalar) return 0;
                v = U ? sqxtn_(c, x, eb, 2) : zx(x, eb);
            } else
                v = sqxtn_(c, x, eb, U ? 1 : 0);
            eset(&o, size, i + off, v);
        }
        if (!Q) o.d[1] = 0;
        *VR(rd) = o;
        return 1;
    }
    if (op == 0x02 || op == 0x06) { /* SADDLP UADDLP SADALP UADALP */
        if (scalar || size == 3) return 0;
        int m = n / 2;
        for (int i = 0; i < m; i++) {
            u64 x = eget(&a, size, 2 * i), y = eget(&a, size, 2 * i + 1);
            u64 s = U ? zx(x, eb) + zx(y, eb) : (u64)(sx(x, eb) + sx(y, eb));
            if (op == 6) s += eget(&d, size + 1, i);
            eset(&r, size + 1, i, zx(s, eb * 2));
        }
        *VR(rd) = r;
        return 1;
    }
    if (size == 3 && !Q && !scalar) return 0;
    for (int i = 0; i < n; i++) {
        u64 x = eget(&a, size, i), o;
        i64 s = sx(x, eb);
        u64 z = eget(&d, size, i);
        int k = (U << 5) | op;
        switch (k) {
            case 0x00:
            case 0x20:
            case 0x01: {
                if (scalar) return 0; /* REV64 REV32 REV16 */
                int cont = k == 0x00 ? 64 : k == 0x20 ? 32 : 16;
                if (eb >= cont) return 0;
                int per = cont / eb;
                int base = i - (i % per);
                o = eget(&a, size, base + per - 1 - (i % per));
            } break;
            case 0x03:
                o = sqadd_(c, z, x, eb); /* SUQADD (approx) */
                {
                    i64 zz = sx(z, eb);
                    u64 xx = zx(x, eb);
                    i64 t;
                    if (eb < 64) {
                        t = zz + (i64)xx;
                        o = zx((u64)ssat(c, t, eb), eb);
                    }
                }
                break;
            case 0x23: {
                u64 zz = zx(z, eb);
                i64 xx = sx(x, eb);
                if (eb < 64) {
                    i64 t = (i64)zz + xx;
                    o = usat(c, t, eb);
                } else
                    o = zz + (u64)xx;
            } break;
            case 0x04:
                if (size == 3) return 0;
                o = (u64)(clz64(zx(x, eb) ^ zx((u64)(s >> 1), eb)) - (64 - eb)) - 1;
                break;
            case 0x24:
                if (size == 3) return 0;
                o = clz64(zx(x, eb)) - (64 - eb);
                break;
            case 0x05:
                if (size) return 0;
                o = popc8((u8)x);
                break;
            case 0x25:
                if (size == 0)
                    o = ~x;
                else if (size == 1) {
                    u8 b = (u8)x;
                    o = (u8)(rbit32(b) >> 24);
                } else
                    return 0;
                break;
            case 0x07:
                if (s == -((i64)1 << (eb - 1)) || (eb == 64 && x == 0x8000000000000000ull)) {
                    qc(c);
                    o = zx(((u64)1 << (eb - 1)) - 1, eb);
                } else
                    o = (u64)(s < 0 ? -s : s);
                break;
            case 0x27:
                if (s == -((i64)1 << (eb - 1)) || (eb == 64 && x == 0x8000000000000000ull)) {
                    qc(c);
                    o = zx(((u64)1 << (eb - 1)) - 1, eb);
                } else
                    o = (u64)(-s);
                break;
            case 0x08: o = s > 0 ? ~0ull : 0; break;
            case 0x28: o = s >= 0 ? ~0ull : 0; break;
            case 0x09: o = s == 0 ? ~0ull : 0; break;
            case 0x29: o = s <= 0 ? ~0ull : 0; break;
            case 0x0A: o = s < 0 ? ~0ull : 0; break;
            case 0x0B: o = (u64)(s < 0 ? -s : s); break;
            case 0x2B: o = (u64)(-s); break;
            default: return 0;
        }
        if (scalar && size != 3 && !(k == 0x03 || k == 0x23 || k == 0x07 || k == 0x27)) return 0;
        eset(&r, size, i, zx(o, eb));
    }
    *VR(rd) = r;
    return 1;
}

static int simd_across(CPU* c, u32 w) {
    int Q = (w >> 30) & 1, U = (w >> 29) & 1, size = (w >> 22) & 3, op = (w >> 12) & 31, rn = (w >> 5) & 31,
        rd = w & 31;
    V128 a = *VR(rn), r;
    memset(&r, 0, 16);
    if (op == 0x0C || op == 0x0F) { /* FMAXNMV FMINNMV FMAXV FMINV, .4s only */
        if (!U || !Q || (size & 1)) return 0;
        int mn = size >> 1;
        f32 x = a.f[0];
        for (int i = 1; i < 4; i++) {
            f32 y = a.f[i];
            x = op == 0x0C ? (mn ? fp_minnm32(x, y) : fp_maxnm32(x, y)) : (mn ? fp_min32(x, y) : fp_max32(x, y));
        }
        /* hw reduces pairwise: (0,1),(2,3) */
        {
            f32 p0, p1;
            if (op == 0x0C) {
                p0 = mn ? fp_minnm32(a.f[0], a.f[1]) : fp_maxnm32(a.f[0], a.f[1]);
                p1 = mn ? fp_minnm32(a.f[2], a.f[3]) : fp_maxnm32(a.f[2], a.f[3]);
                x = mn ? fp_minnm32(p0, p1) : fp_maxnm32(p0, p1);
            } else {
                p0 = mn ? fp_min32(a.f[0], a.f[1]) : fp_max32(a.f[0], a.f[1]);
                p1 = mn ? fp_min32(a.f[2], a.f[3]) : fp_max32(a.f[2], a.f[3]);
                x = mn ? fp_min32(p0, p1) : fp_max32(p0, p1);
            }
        }
        r.f[0] = x;
        *VR(rd) = r;
        return 1;
    }
    if (size == 3 || (size == 2 && !Q)) return 0;
    int eb = 8 << size, n = (Q ? 128 : 64) / eb;
    i64 acc;
    u64 uacc;
    switch (op) {
        case 0x03: {
            i64 s = 0;
            u64 u = 0;
            for (int i = 0; i < n; i++) {
                u64 x = eget(&a, size, i);
                s += sx(x, eb);
                u += zx(x, eb);
            }
            eset(&r, size + 1, 0, zx(U ? u : (u64)s, eb * 2));
        } break;
        case 0x0A:
        case 0x1A: {
            int mx = op == 0x0A;
            acc = sx(eget(&a, size, 0), eb);
            uacc = zx(eget(&a, size, 0), eb);
            for (int i = 1; i < n; i++) {
                u64 x = eget(&a, size, i);
                i64 s = sx(x, eb);
                u64 u = zx(x, eb);
                if (mx) {
                    if (s > acc) acc = s;
                    if (u > uacc) uacc = u;
                } else {
                    if (s < acc) acc = s;
                    if (u < uacc) uacc = u;
                }
            }
            eset(&r, size, 0, zx(U ? uacc : (u64)acc, eb));
        } break;
        case 0x1B: {
            if (U) return 0;
            u64 s = 0;
            for (int i = 0; i < n; i++) s += eget(&a, size, i);
            eset(&r, size, 0, zx(s, eb));
        } break;
        default: return 0;
    }
    *VR(rd) = r;
    return 1;
}

static int simd_copy(CPU* c, u32 w) {
    int Q = (w >> 30) & 1, op = (w >> 29) & 1, imm5 = (w >> 16) & 31, imm4 = (w >> 11) & 15, rn = (w >> 5) & 31,
        rd = w & 31;
    int size = 0;
    while (size < 4 && !((imm5 >> size) & 1)) size++;
    if (size > 3) return 0;
    int idx = imm5 >> (size + 1), eb = 8 << size;
    V128 r;
    memset(&r, 0, 16);
    if (op) {
        if (!Q) return 0;
        int i2 = imm4 >> size;
        V128 d = *VR(rd);
        eset(&d, size, idx, eget(VR(rn), size, i2));
        *VR(rd) = d;
        return 1;
    }
    switch (imm4) {
        case 0: {
            if (size == 3 && !Q) return 0;
            u64 x = eget(VR(rn), size, idx);
            for (int i = 0; i < (Q ? 128 : 64) / eb; i++) eset(&r, size, i, x);
            *VR(rd) = r;
            return 1;
        }
        case 1: {
            if (size == 3 && !Q) return 0;
            u64 x = c->x[rn];
            if (rn == 31) x = 0;
            for (int i = 0; i < (Q ? 128 : 64) / eb; i++) eset(&r, size, i, x);
            *VR(rd) = r;
            return 1;
        }
        case 3: {
            if (!Q) return 0;
            u64 x = rn == 31 ? 0 : c->x[rn];
            eset(VR(rd), size, idx, x);
            return 1;
        }
        case 5: {
            if (size == 3 || (size == 2 && !Q)) return 0;
            i64 v = sx(eget(VR(rn), size, idx), eb);
            if (rd != 31) c->x[rd] = Q ? (u64)v : (u32)v;
            return 1;
        }
        case 7: {
            if ((size == 3) != Q) return 0;
            u64 v = eget(VR(rn), size, idx);
            if (rd != 31) c->x[rd] = v;
            return 1;
        }
    }
    return 0;
}

u64 v_expand_imm(u32 op, u32 cmode, u32 imm8) {
    u64 i = imm8, r = 0;
    switch (cmode >> 1) {
        case 0: r = i | (i << 32); break;
        case 1: r = (i << 8) | (i << 40); break;
        case 2: r = (i << 16) | (i << 48); break;
        case 3: r = (i << 24) | (i << 56); break;
        case 4:
            r = i | (i << 16);
            r |= r << 32;
            break;
        case 5:
            r = (i << 8) | (i << 24);
            r |= r << 32;
            break;
        case 6:
            if (!(cmode & 1))
                r = (i << 8) | 0xFF;
            else
                r = (i << 16) | 0xFFFF;
            r |= r << 32;
            break;
        case 7:
            if (!(cmode & 1) && !op) {
                for (int k = 0; k < 8; k++) r |= i << (k * 8);
            } else if (!(cmode & 1) && op) {
                for (int k = 0; k < 8; k++)
                    if ((i >> k) & 1) r |= 0xFFull << (k * 8);
            } else if ((cmode & 1) && !op) {
                u32 b = ((i & 0x80) << 24) | (((i & 0x40) ? 0x3E000000u : 0x40000000u)) | ((u32)(i & 0x3F) << 19);
                r = b | ((u64)b << 32);
            } else {
                r = ((i & 0x80) << 56) | ((i & 0x40) ? 0x3FC0000000000000ull : 0x4000000000000000ull) |
                    ((i & 0x3F) << 48);
            }
            break;
    }
    return r;
}
static int simd_modimm(CPU* c, u32 w) {
    int Q = (w >> 30) & 1, op = (w >> 29) & 1, cmode = (w >> 12) & 15, rd = w & 31, o2 = (w >> 11) & 1;
    u32 imm8 = (((w >> 16) & 7) << 5) | ((w >> 5) & 31);
    if (o2) return 0;
    if (cmode == 15 && op && !Q) return 0;
    u64 imm = v_expand_imm(op, cmode, imm8);
    V128* d = VR(rd);
    int logic = (cmode < 12 && (cmode & 1)); /* ORR / BIC */
    if (logic) {
        if (!op) {
            d->d[0] |= imm;
            d->d[1] = Q ? d->d[1] | imm : 0;
        } else {
            d->d[0] &= ~imm;
            d->d[1] = Q ? d->d[1] & ~imm : 0;
        }
        return 1;
    }
    if (op && cmode != 14 && cmode != 15) imm = ~imm; /* MVNI */
    d->d[0] = imm;
    d->d[1] = Q ? imm : 0;
    return 1;
}

/* imm shifts */
static int simd_shimm(CPU* c, u32 w, int scalar) {
    int Q = (w >> 30) & 1, U = (w >> 29) & 1, immh = (w >> 19) & 15, immb = (w >> 16) & 7, op = (w >> 11) & 31,
        rn = (w >> 5) & 31, rd = w & 31;
    if (!immh) return 0;
    if (scalar) Q = 0;
    int size = 3;
    while (size >= 0 && !((immh >> size) & 1)) size--;
    int eb = 8 << size;
    int imm = (immh << 3) | immb;
    int shr = 2 * eb - imm, shl = imm - eb;
    V128 a = *VR(rn), d = *VR(rd), r;
    memset(&r, 0, 16);
    int n = scalar ? 1 : (Q ? 128 : 64) / eb;
    if (op == 0x14) { /* SSHLL USHLL */
        if (scalar || size == 3) return 0;
        int m = 64 / eb, off = Q ? m : 0;
        for (int i = 0; i < m; i++) {
            u64 x = eget(&a, size, i + off);
            u64 v = U ? zx(x, eb) << shl : (u64)(sx(x, eb) << shl);
            eset(&r, size + 1, i, zx(v, eb * 2));
        }
        *VR(rd) = r;
        return 1;
    }
    if (op >= 0x10 && op <= 0x13) { /* narrowing: SHRN RSHRN SQSHRN SQRSHRN UQSHRN SQSHRUN ... */
        if (size == 3) return 0;
        int wb = eb * 2;
        shr = 2 * wb - imm - wb;
        shr = wb * 2 - ((immh << 3) | immb) - wb; /* sh = 2*eb - imm, eb = dst size */
        shr = 2 * eb - imm;                       /* eb = dst size */
        int m = scalar ? 1 : 64 / eb, off = Q ? m : 0;
        V128 o = Q ? d : r;
        if (scalar) {
            memset(&o, 0, 16);
            off = 0;
        }
        for (int i = 0; i < m; i++) {
            u64 x = eget(&a, size + 1, i);
            int rnd = op & 1;
            u64 v;
            if (op == 0x10 && !U) {
                if (scalar) return 0;
                v = zx(x >> shr, eb);
            } else if (op == 0x11 && !U) {
                if (scalar) return 0;
                v = zx((x + ((u64)1 << (shr - 1))) >> shr, eb);
            } else {
                int kind;
                i64 sval;
                u64 uval;
                if (U && op < 0x12) {
                    kind = 2;
                } else
                    kind = U ? 1 : 0;
                if (kind == 1) {
                    uval = zx(x, wb);
                    if (rnd) uval = (uval + ((u64)1 << (shr - 1)));
                    uval >>= shr;
                    if (uval > zx(~0ull, eb)) {
                        qc(c);
                        uval = zx(~0ull, eb);
                    }
                    v = uval;
                } else {
                    sval = sx(x, wb);
                    if (rnd) sval += (i64)1 << (shr - 1);
                    sval >>= shr;
                    v = kind == 0 ? zx((u64)ssat(c, sval, eb), eb) : usat(c, sval, eb);
                }
            }
            eset(&o, size, i + off, v);
        }
        if (!Q) o.d[1] = 0;
        *VR(rd) = o;
        return 1;
    }
    if (op == 0x1C || op == 0x1F) { /* SCVTF/UCVTF/FCVTZS/FCVTZU fixed point */
        if (size < 2) return 0;
        if (size == 3 && !Q && !scalar) return 0;
        int fb = shr;
        f64 sc = 1.0;
        for (int k = 0; k < fb; k++) sc *= 2.0;
        for (int i = 0; i < n; i++) {
            u64 x = eget(&a, size, i);
            if (op == 0x1C) {
                f64 v = size == 3 ? (U ? (f64)x : (f64)(i64)x) : (U ? (f64)(u32)x : (f64)(i32)(u32)x);
                v /= sc;
                wrf(&r, size == 3, i, v);
            } else {
                f64 v = rdf(&a, size == 3, i) * sc;
                u64 o = size == 3 ? (U ? fcvt_u64(v, 3) : (u64)fcvt_s64(v, 3))
                                  : (U ? fcvt_u32(v, 3) : (u64)(u32)fcvt_s32(v, 3));
                eset(&r, size, i, o);
            }
        }
        *VR(rd) = r;
        return 1;
    }
    if (size == 3 && !Q && !scalar) return 0;
    if (scalar && size != 3 && !(op == 0x0E || op == 0x0C)) return 0;
    for (int i = 0; i < n; i++) {
        u64 x = eget(&a, size, i), z = eget(&d, size, i), o;
        i64 s = sx(x, eb);
        u64 u = zx(x, eb);
        switch (op) {
            case 0x00:
            case 0x02:
            case 0x04:
            case 0x06: {
                int rnd = (op >> 2) & 1;
                u64 v;
                if (U) {
                    if (shr >= 64)
                        v = rnd ? (u >> 63) : 0;
                    else {
                        v = u >> shr;
                        if (rnd) v += (u >> (shr - 1)) & 1;
                    }
                } else {
                    i64 t;
                    if (shr >= 64)
                        t = rnd ? 0 : (s < 0 ? -1 : 0);
                    else {
                        t = s >> shr;
                        if (rnd) t += (s >> (shr - 1)) & 1;
                    }
                    if (shr >= 64 && rnd) t = 0;
                    v = (u64)t;
                }
                o = op >= 0x02 && (op == 0x02 || op == 0x06) ? z + v : v;
            } break;
            case 0x08:
                if (!U) return 0;
                {
                    u64 m = shr >= 64 ? 0 : zx(~0ull, eb) >> shr;
                    o = (z & ~m) | (shr >= 64 ? 0 : (u >> shr));
                }
                break;
            case 0x0A:
                if (!U)
                    o = u << shl;
                else {
                    u64 m = zx(~0ull, eb) << shl;
                    o = (z & ~m) | ((u << shl) & m);
                }
                break;
            case 0x0C:
                if (!U) return 0; /* SQSHLU */
                if (s < 0) {
                    if (s) qc(c);
                    o = 0;
                } else {
                    u64 v = u << shl;
                    if (shl && (u >> (eb - shl))) {
                        qc(c);
                        v = ~0ull;
                    }
                    o = v;
                }
                break;
            case 0x0E:
                if (U) {
                    u64 v = u << shl;
                    if (shl && (u >> (eb - shl))) {
                        qc(c);
                        v = ~0ull;
                    }
                    o = v;
                } else {
                    i64 v = (i64)((u64)s << shl);
                    if (sx(zx((u64)v, eb), eb) != v || (v >> shl) != s) {
                        qc(c);
                        v = s < 0 ? -((i64)1 << (eb - 1)) : ((i64)1 << (eb - 1)) - 1;
                    }
                    o = (u64)v;
                }
                break;
            default: return 0;
        }
        eset(&r, size, i, zx(o, eb));
    }
    *VR(rd) = r;
    return 1;
}

/* by-element */
static int simd_elem(CPU* c, u32 w, int scalar) {
    int Q = (w >> 30) & 1, U = (w >> 29) & 1, size = (w >> 22) & 3, L = (w >> 21) & 1, Mb = (w >> 20) & 1,
        rm = (w >> 16) & 15, op = (w >> 12) & 15, H = (w >> 11) & 1, rn = (w >> 5) & 31, rd = w & 31;
    if (scalar) Q = 0;
    V128 a = *VR(rn), d = *VR(rd), r;
    memset(&r, 0, 16);
    int fp = !U ? (op == 1 || op == 5 || op == 9) : (op == 9);
    if (fp) {
        int sz = size & 1;
        if (size < 2) return 0;
        int idx = sz ? H : (H << 1) | L;
        if (sz && L) return 0;
        int m = (Mb << 4) | rm;
        if (sz && !Q && !scalar) return 0;
        f64 b = rdf(VR(m), sz, idx);
        int n = scalar ? 1 : (sz ? 2 : (Q ? 4 : 2));
        for (int i = 0; i < n; i++) {
            f64 x = rdf(&a, sz, i), v;
            if (op == 1)
                v = fmla_(rdf(&d, sz, i), x, b, sz);
            else if (op == 5)
                v = fmla_(rdf(&d, sz, i), -x, b, sz);
            else if (!U)
                v = x * b;
            else
                v = sz ? fp_mulx64(x, b) : fp_mulx32((f32)x, (f32)b);
            wrf(&r, sz, i, v);
        }
        *VR(rd) = r;
        return 1;
    }
    if (size != 1 && size != 2) return 0;
    int idx, m;
    if (size == 1) {
        idx = (H << 2) | (L << 1) | Mb;
        m = rm;
    } else {
        idx = (H << 1) | L;
        m = (Mb << 4) | rm;
    }
    int eb = 8 << size;
    u64 bx = eget(VR(m), size, idx);
    i64 sb = sx(bx, eb);
    u64 ub = zx(bx, eb);
    int lng = (op == 2 || op == 3 || op == 6 || op == 7 || op == 10 || op == 11);
    if (lng) {
        int nn = scalar ? 1 : 64 / eb, off = Q ? nn : 0;
        for (int i = 0; i < nn; i++) {
            u64 x = eget(&a, size, i + off);
            i64 sa = sx(x, eb);
            u64 ua = zx(x, eb);
            u64 z = eget(&d, size + 1, i), o;
            switch (op) {
                case 2: o = z + (U ? ua * ub : (u64)(sa * sb)); break;
                case 6: o = z - (U ? ua * ub : (u64)(sa * sb)); break;
                case 10: o = U ? ua * ub : (u64)(sa * sb); break;
                case 3:
                case 7:
                case 11: {
                    if (U) return 0;
                    i64 p = 2 * sa * sb;
                    if (sa == sb && sa == -((i64)1 << (eb - 1))) {
                        p = sat_s(0x7FFFFFFFFFFFFFFFll, eb * 2);
                        qc(c);
                    }
                    o = op == 11 ? (u64)p : op == 3 ? sqadd_(c, z, (u64)p, eb * 2) : sqsub_(c, z, (u64)p, eb * 2);
                } break;
                default: return 0;
            }
            if (scalar && !(op == 3 || op == 7 || op == 11)) return 0;
            eset(&r, size + 1, i, zx(o, eb * 2));
        }
        *VR(rd) = r;
        return 1;
    }
    int n = scalar ? 1 : (Q ? 128 : 64) / eb;
    for (int i = 0; i < n; i++) {
        u64 x = eget(&a, size, i), z = eget(&d, size, i), o;
        i64 sa = sx(x, eb);
        switch (op) {
            case 0:
                if (!U) return 0;
                o = z + x * bx;
                break;
            case 4:
                if (!U) return 0;
                o = z - x * bx;
                break;
            case 8:
                if (U) return 0;
                o = x * bx;
                break;
            case 12:
            case 13: {
                if (U) return 0;
                i64 p = 2 * sa * sb;
                if (op == 13) p += (i64)1 << (eb - 1);
                p >>= eb;
                if (sa == sb && sa == -((i64)1 << (eb - 1))) {
                    p = ((i64)1 << (eb - 1)) - 1;
                    qc(c);
                }
                o = (u64)p;
            } break;
            default: return 0;
        }
        if (scalar && op < 12) return 0;
        eset(&r, size, i, zx(o, eb));
    }
    *VR(rd) = r;
    return 1;
}

static int simd_perm(CPU* c, u32 w) {
    int Q = (w >> 30) & 1, size = (w >> 22) & 3, rm = (w >> 16) & 31, op = (w >> 12) & 7, rn = (w >> 5) & 31,
        rd = w & 31;
    if (size == 3 && !Q) return 0;
    V128 a = *VR(rn), b = *VR(rm), r;
    memset(&r, 0, 16);
    int eb = 8 << size, n = (Q ? 128 : 64) / eb, h = n / 2;
    int part = (op >> 2) & 1;
    for (int i = 0; i < n; i++) {
        u64 v;
        switch (op & 3) {
            case 1: {
                int k = 2 * i + part;
                v = k < n ? eget(&a, size, k) : eget(&b, size, k - n);
            } break; /* UZP */
            case 2: {
                int p = i / 2 * 2 + part;
                v = (i & 1) ? eget(&b, size, p) : eget(&a, size, p);
            } break; /* TRN */
            case 3: {
                int base = part * h;
                v = (i & 1) ? eget(&b, size, base + i / 2) : eget(&a, size, base + i / 2);
            } break; /* ZIP */
            default: return 0;
        }
        eset(&r, size, i, v);
    }
    *VR(rd) = r;
    return 1;
}

void v_tbl(CPU* c, u32 d, u32 n, u32 len, u32 m, int q, int tbx) {
    u8 tab[64];
    for (u32 k = 0; k <= len; k++) memcpy(tab + 16 * k, &c->v[(n + k) & 31], 16);
    V128 idx = c->v[m], r = tbx ? c->v[d] : c->v[d];
    if (!tbx) memset(&r, 0, 16);
    u32 lim = 16 * (len + 1);
    for (int i = 0; i < (q ? 16 : 8); i++) {
        u32 x = idx.b[i];
        if (x < lim)
            r.b[i] = tab[x];
        else if (!tbx)
            r.b[i] = 0;
    }
    if (!q) r.d[1] = 0;
    c->v[d] = r;
}

/* ---- AES ---- */
static const u8 SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
static u8 ISBOX[256];
static int isbox_ok;
static u8 xt(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static u8 gm(u8 a, u8 b) {
    u8 p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = xt(a);
        b >>= 1;
    }
    return p;
}
