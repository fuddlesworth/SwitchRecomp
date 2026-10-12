/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* ASTC LDR 2D decoder, written from the Khronos spec.
   Desktop GPUs mostly lack ASTC so we decode on upload. */
#include "gpu/texture/astc.h"
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- bits */
static u32 getbits(const u8* d, int pos, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) {
        int p = pos + i;
        v |= (u32)((d[p >> 3] >> (p & 7)) & 1) << i;
    }
    return v;
}
static u32 getbits_rev(const u8* d, int pos, int n) { /* reads bits backwards from the block end */
    u32 v = 0;
    for (int i = 0; i < n; i++) {
        int p = 127 - (pos + i);
        v |= (u32)((d[p >> 3] >> (p & 7)) & 1) << i;
    }
    return v;
}

/* ---------------------------------------------------------------- ISE (trits / quints) */
typedef struct {
    int bits, trits, quints;
} Q;
static const Q qtab[21] = {{1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {0, 0, 1}, {1, 1, 0}, {3, 0, 0}, {1, 0, 1},
                           {2, 1, 0}, {4, 0, 0}, {2, 0, 1}, {3, 1, 0}, {5, 0, 0}, {3, 0, 1}, {4, 1, 0},
                           {6, 0, 0}, {4, 0, 1}, {5, 1, 0}, {7, 0, 0}, {5, 0, 1}, {6, 1, 0}, {8, 0, 0}};
static int ise_bits(int count, int q) {
    const Q* t = &qtab[q];
    return t->bits * count + (t->trits ? (8 * count + 4) / 5 : 0) + (t->quints ? (7 * count + 2) / 3 : 0);
}
static void decode_trits(u32 T, int out[5]) {
    int C;
#define TB(i) ((int)((T >> (i)) & 1))
    if (((T >> 2) & 7) == 7) {
        C = (int)((((T >> 5) & 7) << 2) | (T & 3));
        out[4] = 2;
        out[3] = 2;
    } else {
        C = (int)(T & 0x1F);
        if (((T >> 5) & 3) == 3) {
            out[4] = 2;
            out[3] = TB(7);
        } else {
            out[4] = TB(7);
            out[3] = (int)((T >> 5) & 3);
        }
    }
#undef TB
    int c0 = C & 1, c1 = (C >> 1) & 1, c2 = (C >> 2) & 1, c3 = (C >> 3) & 1, c4 = (C >> 4) & 1;
    if ((C & 3) == 3) {
        out[2] = 2;
        out[1] = c4;
        out[0] = (c3 << 1) | (c2 & ~c3 & 1);
    } else if (((C >> 2) & 3) == 3) {
        out[2] = 2;
        out[1] = 2;
        out[0] = C & 3;
    } else {
        out[2] = c4;
        out[1] = (C >> 2) & 3;
        out[0] = (c1 << 1) | (c0 & ~c1 & 1);
    }
}
static void decode_quints(u32 Qv, int out[3]) {
    int q[3];
    if (((Qv >> 1) & 3) == 3 && ((Qv >> 5) & 3) == 0) {
        q[2] = (int)(((Qv & 1) << 2) | ((((Qv >> 4) & 1) & ~(Qv & 1)) << 1) | (((Qv >> 3) & 1) & ~(Qv & 1)));
        q[1] = 4;
        q[0] = 4;
    } else {
        int C;
        if (((Qv >> 1) & 3) == 3) {
            q[2] = 4;
            C = (int)((((Qv >> 3) & 3) << 3) | ((~(Qv >> 5) & 3) << 1) | (Qv & 1));
        } else {
            q[2] = (int)((Qv >> 5) & 3);
            C = (int)(Qv & 0x1F);
        }
        if ((C & 7) == 5) {
            q[1] = 4;
            q[0] = (C >> 3) & 3;
        } else {
            q[1] = (C >> 3) & 3;
            q[0] = C & 7;
        }
    }
    for (int i = 0; i < 3; i++) out[i] = q[i];
}
/* decode 'count' ISE values into out[] (low bits | trit/quint << bits) */
static void ise_decode(const u8* d, int pos, int count, int q, int* vals, int rev) {
    const Q* t = &qtab[q];
    int nb = t->bits;
#define GB(p, n) (rev ? getbits_rev(d, (p), (n)) : getbits(d, (p), (n)))
    if (t->trits) {
        for (int blk = 0; blk < count; blk += 5) {
            u32 m[5] = {0}, T = 0;
            int n = count - blk < 5 ? count - blk : 5;
            static const int tb[5][2] = {{0, 2}, {2, 2}, {4, 1}, {5, 2}, {7, 1}};
            for (int i = 0; i < 5; i++) {
                if (i < n) {
                    m[i] = GB(pos, nb);
                    pos += nb;
                } else
                    m[i] = 0;
                u32 tv = 0;
                if (i < n) {
                    tv = GB(pos, tb[i][1]);
                    pos += tb[i][1];
                }
                T |= tv << tb[i][0];
            }
            /* trit bits past count aren't stored, clamp pos */
            int tr[5];
            decode_trits(T, tr);
            for (int i = 0; i < n; i++) vals[blk + i] = (int)(m[i] | ((u32)tr[i] << nb));
        }
    } else if (t->quints) {
        for (int blk = 0; blk < count; blk += 3) {
            u32 m[3] = {0}, Qv = 0;
            int n = count - blk < 3 ? count - blk : 3;
            static const int qb[3][2] = {{0, 3}, {3, 2}, {5, 2}};
            for (int i = 0; i < 3; i++) {
                if (i < n) {
                    m[i] = GB(pos, nb);
                    pos += nb;
                    u32 qv = GB(pos, qb[i][1]);
                    pos += qb[i][1];
                    Qv |= qv << qb[i][0];
                }
            }
            int qq[3];
            decode_quints(Qv, qq);
            for (int i = 0; i < n; i++) vals[blk + i] = (int)(m[i] | ((u32)qq[i] << nb));
        }
    } else {
        for (int i = 0; i < count; i++) {
            vals[i] = (int)GB(pos, nb);
            pos += nb;
        }
    }
#undef GB
}

/* ---------------------------------------------------------------- unquantize */
static int unq_color(int v, int q) {
    const Q* t = &qtab[q];
    int nb = t->bits;
    if (!t->trits && !t->quints) { /* bit-replicate to 8 bits */
        int x = v;
        int r = x << (8 - nb);
        int sh = 8 - nb;
        while (sh > 0) {
            sh -= nb;
            r |= sh >= 0 ? x << sh : x >> (-sh);
        }
        return r & 255;
    }
    int m = v & ((1 << nb) - 1), tq = v >> nb;
    int A = (m & 1) ? 0x1FF : 0, B = 0, C = 0, D = tq;
    int b = (m >> 1) & 1, c = (m >> 2) & 1, dd = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
    if (t->trits) {
        switch (nb) {
            case 1:
                C = 204;
                B = 0;
                break;
            case 2:
                C = 93;
                B = (b << 8) | (b << 4) | (b << 2) | (b << 1);
                break;
            case 3:
                C = 44;
                B = (c << 8) | (b << 7) | (c << 3) | (b << 2) | (c << 1) | b;
                break;
            case 4:
                C = 22;
                B = (dd << 8) | (c << 7) | (b << 6) | (dd << 2) | (c << 1) | b;
                break;
            case 5:
                C = 11;
                B = (e << 8) | (dd << 7) | (c << 6) | (b << 5) | (e << 1) | dd;
                break;
            case 6:
                C = 5;
                B = (f << 8) | (e << 7) | (dd << 6) | (c << 5) | (b << 4) | f;
                break;
            default: C = 0; B = 0;
        }
    } else {
        switch (nb) {
            case 1:
                C = 113;
                B = 0;
                break;
            case 2:
                C = 54;
                B = (b << 8) | (b << 3) | (b << 2);
                break;
            case 3:
                C = 26;
                B = (c << 8) | (b << 7) | (c << 2) | (b << 1) | c;
                break;
            case 4:
                C = 13;
                B = (dd << 8) | (c << 7) | (b << 6) | (dd << 1) | c;
                break;
            case 5:
                C = 6;
                B = (e << 8) | (dd << 7) | (c << 6) | (b << 5) | e;
                break;
            default: C = 0; B = 0;
        }
    }
    int T2 = D * C + B;
    T2 ^= A;
    T2 = (A & 0x80) | (T2 >> 2);
    return T2 & 255;
}
static int unq_weight(int v, int q) {
    const Q* t = &qtab[q];
    int nb = t->bits;
    int r;
    if (!t->trits && !t->quints) {
        switch (nb) {
            case 1: r = v ? 63 : 0; break;
            case 2: r = (v << 4) | (v << 2) | v; break;
            case 3: r = (v << 3) | v; break;
            case 4: r = (v << 2) | (v >> 2); break;
            case 5: r = (v << 1) | (v >> 4); break;
            default: r = v;
        }
    } else {
        int m = v & ((1 << nb) - 1), tq = v >> nb;
        int A = (m & 1) ? 0x7F : 0, B = 0, C = 0, D = tq;
        int b = (m >> 1) & 1, c = (m >> 2) & 1;
        if (t->trits) {
            switch (nb) {
                case 0: {
                    static const int tab[3] = {0, 32, 63};
                    r = tab[tq];
                    goto done;
                }
                case 1:
                    C = 50;
                    B = 0;
                    break;
                case 2:
                    C = 23;
                    B = (b << 6) | (b << 2) | b;
                    break;
                case 3:
                    C = 11;
                    B = (c << 6) | (b << 5) | (c << 1) | b;
                    break;
            }
        } else {
            switch (nb) {
                case 0: {
                    static const int tab[5] = {0, 16, 32, 47, 63};
                    r = tab[tq];
                    goto done;
                }
                case 1:
                    C = 28;
                    B = 0;
                    break;
                case 2:
                    C = 13;
                    B = (b << 6) | (b << 1);
                    break;
            }
        }
        int T2 = D * C + B;
        T2 ^= A;
        T2 = (A & 0x20) | (T2 >> 2);
        r = T2;
    }
done:
    if (r > 32) r++;
    return r;
}

/* ---------------------------------------------------------------- endpoints */
static void bit_transfer_signed(int* a, int* b) {
    *b >>= 1;
    *b |= *a & 0x80;
    *a >>= 1;
    *a &= 0x3F;
    if (*a & 0x20) *a -= 0x40;
}
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static void blue_contract(int* r, int* g, int* b) {
    *r = (*r + *b) >> 1;
    *g = (*g + *b) >> 1;
}
static void decode_endpoints(int mode, const int* v, int e0[4], int e1[4]) {
    switch (mode) {
        case 0:
            e0[0] = e0[1] = e0[2] = v[0];
            e0[3] = 255;
            e1[0] = e1[1] = e1[2] = v[1];
            e1[3] = 255;
            break;
        case 1: {
            int L0 = (v[0] >> 2) | (v[1] & 0xC0), L1 = L0 + (v[1] & 0x3F);
            if (L1 > 255) L1 = 255;
            e0[0] = e0[1] = e0[2] = L0;
            e0[3] = 255;
            e1[0] = e1[1] = e1[2] = L1;
            e1[3] = 255;
            break;
        }
        case 4:
            e0[0] = e0[1] = e0[2] = v[0];
            e0[3] = v[2];
            e1[0] = e1[1] = e1[2] = v[1];
            e1[3] = v[3];
            break;
        case 5: {
            int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
            bit_transfer_signed(&v1, &v0);
            bit_transfer_signed(&v3, &v2);
            e0[0] = e0[1] = e0[2] = v0;
            e0[3] = v2;
            e1[0] = e1[1] = e1[2] = clamp255(v0 + v1);
            e1[3] = clamp255(v2 + v3);
            break;
        }
        case 6:
            e0[0] = (v[0] * v[3]) >> 8;
            e0[1] = (v[1] * v[3]) >> 8;
            e0[2] = (v[2] * v[3]) >> 8;
            e0[3] = 255;
            e1[0] = v[0];
            e1[1] = v[1];
            e1[2] = v[2];
            e1[3] = 255;
            break;
        case 8: {
            int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
            if (s1 >= s0) {
                e0[0] = v[0];
                e0[1] = v[2];
                e0[2] = v[4];
                e0[3] = 255;
                e1[0] = v[1];
                e1[1] = v[3];
                e1[2] = v[5];
                e1[3] = 255;
            } else {
                e0[0] = v[1];
                e0[1] = v[3];
                e0[2] = v[5];
                e0[3] = 255;
                e1[0] = v[0];
                e1[1] = v[2];
                e1[2] = v[4];
                e1[3] = 255;
                blue_contract(&e0[0], &e0[1], &e0[2]);
                blue_contract(&e1[0], &e1[1], &e1[2]);
            }
            break;
        }
        case 9: {
            int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3], v4 = v[4], v5 = v[5];
            bit_transfer_signed(&v1, &v0);
            bit_transfer_signed(&v3, &v2);
            bit_transfer_signed(&v5, &v4);
            /* the sums stay unclamped through blue_contract and are clamped at the end, as for mode 13 */
            if (v1 + v3 + v5 >= 0) {
                e0[0] = v0;
                e0[1] = v2;
                e0[2] = v4;
                e0[3] = 255;
                e1[0] = v0 + v1;
                e1[1] = v2 + v3;
                e1[2] = v4 + v5;
                e1[3] = 255;
            } else {
                e0[0] = v0 + v1;
                e0[1] = v2 + v3;
                e0[2] = v4 + v5;
                e0[3] = 255;
                e1[0] = v0;
                e1[1] = v2;
                e1[2] = v4;
                e1[3] = 255;
                blue_contract(&e0[0], &e0[1], &e0[2]);
                blue_contract(&e1[0], &e1[1], &e1[2]);
            }
            for (int i = 0; i < 3; i++) {
                e0[i] = clamp255(e0[i]);
                e1[i] = clamp255(e1[i]);
            }
            break;
        }
        case 10:
            e0[0] = (v[0] * v[3]) >> 8;
            e0[1] = (v[1] * v[3]) >> 8;
            e0[2] = (v[2] * v[3]) >> 8;
            e0[3] = v[4];
            e1[0] = v[0];
            e1[1] = v[1];
            e1[2] = v[2];
            e1[3] = v[5];
            break;
        case 12: {
            int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
            if (s1 >= s0) {
                e0[0] = v[0];
                e0[1] = v[2];
                e0[2] = v[4];
                e0[3] = v[6];
                e1[0] = v[1];
                e1[1] = v[3];
                e1[2] = v[5];
                e1[3] = v[7];
            } else {
                e0[0] = v[1];
                e0[1] = v[3];
                e0[2] = v[5];
                e0[3] = v[7];
                e1[0] = v[0];
                e1[1] = v[2];
                e1[2] = v[4];
                e1[3] = v[6];
                blue_contract(&e0[0], &e0[1], &e0[2]);
                blue_contract(&e1[0], &e1[1], &e1[2]);
            }
            break;
        }
        case 13: {
            int w[8];
            for (int i = 0; i < 8; i++) w[i] = v[i];
            bit_transfer_signed(&w[1], &w[0]);
            bit_transfer_signed(&w[3], &w[2]);
            bit_transfer_signed(&w[5], &w[4]);
            bit_transfer_signed(&w[7], &w[6]);
            if (w[1] + w[3] + w[5] >= 0) {
                e0[0] = w[0];
                e0[1] = w[2];
                e0[2] = w[4];
                e0[3] = w[6];
                e1[0] = w[0] + w[1];
                e1[1] = w[2] + w[3];
                e1[2] = w[4] + w[5];
                e1[3] = w[6] + w[7];
            } else {
                e0[0] = w[0] + w[1];
                e0[1] = w[2] + w[3];
                e0[2] = w[4] + w[5];
                e0[3] = w[6] + w[7];
                e1[0] = w[0];
                e1[1] = w[2];
                e1[2] = w[4];
                e1[3] = w[6];
                blue_contract(&e0[0], &e0[1], &e0[2]);
                blue_contract(&e1[0], &e1[1], &e1[2]);
            }
            for (int i = 0; i < 4; i++) {
                e0[i] = clamp255(e0[i]);
                e1[i] = clamp255(e1[i]);
            }
            break;
        }
        default: /* HDR modes -> magenta, LDR only */
            e0[0] = 255;
            e0[1] = 0;
            e0[2] = 255;
            e0[3] = 255;
            for (int i = 0; i < 4; i++) e1[i] = e0[i];
            break;
    }
}

/* ---------------------------------------------------------------- partitions */
static u32 hash52(u32 p) {
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}
static int select_partition(int seed, int x, int y, int z, int count, int small) {
    if (small) {
        x <<= 1;
        y <<= 1;
        z <<= 1;
    }
    seed += (count - 1) * 1024;
    u32 rnum = hash52((u32)seed);
    u8 s1 = rnum & 0xF, s2 = (rnum >> 4) & 0xF, s3 = (rnum >> 8) & 0xF, s4 = (rnum >> 12) & 0xF,
       s5 = (rnum >> 16) & 0xF, s6 = (rnum >> 20) & 0xF, s7 = (rnum >> 24) & 0xF, s8 = (rnum >> 28) & 0xF;
    u8 s9 = (rnum >> 18) & 0xF, s10 = (rnum >> 22) & 0xF, s11 = (rnum >> 26) & 0xF,
       s12 = ((rnum >> 30) | (rnum << 2)) & 0xF;
    s1 *= s1;
    s2 *= s2;
    s3 *= s3;
    s4 *= s4;
    s5 *= s5;
    s6 *= s6;
    s7 *= s7;
    s8 *= s8;
    s9 *= s9;
    s10 *= s10;
    s11 *= s11;
    s12 *= s12;
    int sh1, sh2, sh3;
    if (seed & 1) {
        sh1 = (seed & 2) ? 4 : 5;
        sh2 = (count == 3) ? 6 : 5;
    } else {
        sh1 = (count == 3) ? 6 : 5;
        sh2 = (seed & 2) ? 4 : 5;
    }
    sh3 = (seed & 0x10) ? sh1 : sh2;
    s1 >>= sh1;
    s2 >>= sh2;
    s3 >>= sh1;
    s4 >>= sh2;
    s5 >>= sh1;
    s6 >>= sh2;
    s7 >>= sh1;
    s8 >>= sh2;
    s9 >>= sh3;
    s10 >>= sh3;
    s11 >>= sh3;
    s12 >>= sh3;
    int a = s1 * x + s2 * y + s11 * z + (rnum >> 14), b = s3 * x + s4 * y + s12 * z + (rnum >> 10),
        c = s5 * x + s6 * y + s9 * z + (rnum >> 6), d = s7 * x + s8 * y + s10 * z + (rnum >> 2);
    a &= 0x3F;
    b &= 0x3F;
    c &= 0x3F;
    d &= 0x3F;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

/* ---------------------------------------------------------------- block */
static void error_block(u8* out, int bw, int bh) {
    for (int i = 0; i < bw * bh; i++) {
        out[i * 4] = 255;
        out[i * 4 + 1] = 0;
        out[i * 4 + 2] = 255;
        out[i * 4 + 3] = 255;
    }
}
static int decode_block_mode(u32 bm, int* gw, int* gh, int* dual, int* wq) {
    int R, H = (bm >> 9) & 1, D = (bm >> 10) & 1, A, B;
    if (!(bm & 3)) { /* low 2 bits == 0 */
        R = ((bm >> 4) & 1) | ((bm >> 1) & 6);
        A = (bm >> 5) & 3;
        B = (bm >> 9) & 3;
        switch ((bm >> 7) & 3) {
            case 0:
                *gw = 12;
                *gh = A + 2;
                break;
            case 1:
                *gw = A + 2;
                *gh = 12;
                break;
            case 2:
                A = (bm >> 5) & 3;
                B = (bm >> 9) & 3;
                *gw = A + 6;
                *gh = B + 6;
                D = 0;
                H = 0;
                break;
            case 3:
                switch ((bm >> 5) & 3) {
                    case 0:
                        *gw = 6;
                        *gh = 10;
                        break;
                    case 1:
                        *gw = 10;
                        *gh = 6;
                        break;
                    default: return 0;
                }
                break;
        }
    } else {
        R = ((bm >> 4) & 1) | ((bm & 3) << 1);
        A = (bm >> 5) & 3;
        B = (bm >> 7) & 3;
        switch ((bm >> 2) & 3) {
            case 0:
                *gw = B + 4;
                *gh = A + 2;
                break;
            case 1:
                *gw = B + 8;
                *gh = A + 2;
                break;
            case 2:
                *gw = A + 2;
                *gh = B + 8;
                break;
            case 3:
                if ((bm >> 8) & 1) {
                    *gw = ((bm >> 7) & 1) + 2;
                    *gh = A + 2;
                } else {
                    *gw = A + 2;
                    *gh = ((bm >> 7) & 1) + 6;
                }
                break;
        }
    }
    if (R < 2) return 0;
    /* R 2..7 -> weight ranges 2,3,4,5,6,8 (H=0) or 10,12,16,20,24,32 (H=1): qtab indices 0..11 */
    static const int qidx[2][8] = {{-1, -1, 0, 1, 2, 3, 4, 5}, {-1, -1, 6, 7, 8, 9, 10, 11}};
    *wq = qidx[H][R];
    *dual = D;
    return 1;
}
static void decode_block(const u8* blk, int bw, int bh, u8* out) {
    u32 bm = getbits(blk, 0, 11);
    if ((bm & 0x1FF) == 0x1FC) { /* void extent = solid color */
        /* bit 9 makes it HDR (FP16 colours), which an LDR decoder can't give, and bits 10-11 are reserved as 1s:
           otherwise the block is an error */
        if (((bm >> 9) & 1) || ((bm >> 10) & 3) != 3) {
            error_block(out, bw, bh);
            return;
        }
        u32 r = getbits(blk, 64, 16), g = getbits(blk, 80, 16), b = getbits(blk, 96, 16), a = getbits(blk, 112, 16);
        for (int i = 0; i < bw * bh; i++) {
            out[i * 4] = (u8)(r >> 8);
            out[i * 4 + 1] = (u8)(g >> 8);
            out[i * 4 + 2] = (u8)(b >> 8);
            out[i * 4 + 3] = (u8)(a >> 8);
        }
        return;
    }
    int gw, gh, dual, wq;
    if (!decode_block_mode(bm, &gw, &gh, &dual, &wq) || gw > bw || gh > bh) {
        error_block(out, bw, bh);
        return;
    }
    /* qtab: 0:2 1:3 2:4 3:5 4:6 5:8 6:10 7:12 8:16 9:20 10:24 11:32 12:40 ... (the weight range is its index) */
    int qw = wq;
    int nparts = (int)getbits(blk, 11, 2) + 1;
    int nweights = gw * gh * (dual ? 2 : 1);
    int wbits = ise_bits(nweights, qw);
    if (nweights > 64 || wbits < 24 || wbits > 96 || (nparts == 4 && dual)) {
        error_block(out, bw, bh);
        return;
    }
    int cem[4];
    int pos;
    int part_seed = 0;
    int extra_cem_bits = 0;
    if (nparts == 1) {
        cem[0] = (int)getbits(blk, 13, 4);
        pos = 17;
    } else {
        part_seed = (int)getbits(blk, 13, 10);
        u32 cemv = getbits(blk, 23, 6);
        if (!(cemv & 3)) {
            int m = (int)(cemv >> 2);
            for (int i = 0; i < nparts; i++) cem[i] = m;
            pos = 29;
        } else {
            int base = (int)((cemv & 3) - 1) * 4;
            extra_cem_bits = 3 * nparts - 4;
            int wstart = 128 - wbits;
            u32 extra = getbits(blk, wstart - extra_cem_bits, extra_cem_bits);
            u32 cm = (cemv >> 2) | (extra << 4);
            int c[4], m[4];
            for (int i = 0; i < nparts; i++) {
                c[i] = (int)(cm & 1);
                cm >>= 1;
            }
            for (int i = 0; i < nparts; i++) {
                m[i] = (int)(cm & 3);
                cm >>= 2;
            }
            for (int i = 0; i < nparts; i++) cem[i] = base + c[i] * 4 + m[i];
            pos = 29;
        }
    }
    int nvals = 0;
    for (int i = 0; i < nparts; i++) nvals += ((cem[i] >> 2) + 1) * 2;
    if (nvals > 18) {
        error_block(out, bw, bh);
        return;
    }
    int avail = 128 - wbits - pos - extra_cem_bits - (dual ? 2 : 0);
    /* pick the highest color quant level that fits; colour endpoints have 6 levels at least (qtab 4), a block
       with less room is an error */
    int cq = -1;
    for (int q = 20; q >= 4; q--) {
        if (ise_bits(nvals, q) <= avail) {
            cq = q;
            break;
        }
    }
    if (cq < 0) {
        error_block(out, bw, bh);
        return;
    }
    int cvals[18];
    ise_decode(blk, pos, nvals, cq, cvals, 0);
    for (int i = 0; i < nvals; i++) cvals[i] = unq_color(cvals[i], cq);
    int plane2_comp = -1;
    if (dual) plane2_comp = (int)getbits(blk, 128 - wbits - extra_cem_bits - 2, 2);
    int ep[4][2][4];
    int vi = 0;
    for (int i = 0; i < nparts; i++) {
        decode_endpoints(cem[i], cvals + vi, ep[i][0], ep[i][1]);
        vi += ((cem[i] >> 2) + 1) * 2;
    }
    /* weights */
    int wv[128];
    ise_decode(blk, 0, nweights, qw, wv, 1);
    for (int i = 0; i < nweights; i++) wv[i] = unq_weight(wv[i], qw);
    /* infill: weight grid -> texel grid */
    int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    for (int t = 0; t < bh; t++)
        for (int s = 0; s < bw; s++) {
            int cs = Ds * s, ct = Dt * t;
            int gs = (cs * (gw - 1) + 32) >> 6, gt = (ct * (gh - 1) + 32) >> 6;
            int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int pw[2];
            for (int pl = 0; pl < (dual ? 2 : 1); pl++) {
                int v0 = js + jt * gw;
                int p00 = wv[(v0) * (dual ? 2 : 1) + pl];
                int p01 = js + 1 < gw ? wv[(v0 + 1) * (dual ? 2 : 1) + pl] : 0;
                int p10 = jt + 1 < gh ? wv[(v0 + gw) * (dual ? 2 : 1) + pl] : 0;
                int p11 = (js + 1 < gw && jt + 1 < gh) ? wv[(v0 + gw + 1) * (dual ? 2 : 1) + pl] : 0;
                pw[pl] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
            int part = nparts > 1 ? select_partition(part_seed, s, t, 0, nparts, bw * bh < 31) : 0;
            u8* o = out + (t * bw + s) * 4;
            for (int c = 0; c < 4; c++) {
                int w = (dual && c == plane2_comp) ? pw[1] : pw[0];
                int e0 = ep[part][0][c], e1 = ep[part][1][c];
                int C0 = e0 * 257, C1 = e1 * 257;
                int C = (C0 * (64 - w) + C1 * w + 32) >> 6;
                o[c] = (u8)(C >> 8);
            }
        }
}

void astc_decode_image(const u8* src, u32 w, u32 h, u32 bw, u32 bh, u8* dst) {
    u32 bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    u8 blk[12 * 12 * 4];
    for (u32 j = 0; j < by; j++)
        for (u32 i = 0; i < bx; i++) {
            decode_block(src + ((u64)j * bx + i) * 16, (int)bw, (int)bh, blk);
            for (u32 y = 0; y < bh; y++) {
                u32 py = j * bh + y;
                if (py >= h) break;
                for (u32 x = 0; x < bw; x++) {
                    u32 px = i * bw + x;
                    if (px >= w) break;
                    memcpy(dst + ((u64)py * w + px) * 4, blk + (y * bw + x) * 4, 4);
                }
            }
        }
}
