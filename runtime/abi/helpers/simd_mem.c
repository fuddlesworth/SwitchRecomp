/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Part of cpu_helpers.c, which includes it: AdvSIMD loads and stores: LD1-4, ST1-4, LDnR and single lanes. Not compiled on its own. */
/* ---- LD1-4 / ST1-4 / LDnR / single lane ---- */
static int simd_ldst(CPU* c, u8* M, u32 w) {
    int Q = (w >> 30) & 1, L = (w >> 22) & 1, post = (w >> 23) & 1, rm = (w >> 16) & 31, rn = (w >> 5) & 31,
        rt = w & 31;
    u64 addr = c->x[rn];
    u64 total = 0;
    if (!((w >> 24) & 1)) { /* multiple structures */
        int op = (w >> 12) & 15, size = (w >> 10) & 3, rpt, sel;
        switch (op) {
            case 0:
                rpt = 1;
                sel = 4;
                break;
            case 2:
                rpt = 4;
                sel = 1;
                break;
            case 4:
                rpt = 1;
                sel = 3;
                break;
            case 6:
                rpt = 3;
                sel = 1;
                break;
            case 7:
                rpt = 1;
                sel = 1;
                break;
            case 8:
                rpt = 1;
                sel = 2;
                break;
            case 10:
                rpt = 2;
                sel = 1;
                break;
            default: return 0;
        }
        if (size == 3 && !Q && sel > 1) return 0;
        int eb = 8 << size, eby = eb / 8, n = (Q ? 128 : 64) / eb;
        u64 a = addr;
        for (int r = 0; r < rpt; r++)
            for (int e = 0; e < n; e++)
                for (int s = 0; s < sel; s++) {
                    int t = (rt + r + s) & 31;
                    if (L) {
                        u64 v;
                        switch (size) {
                            case 0: v = RD8(a); break;
                            case 1: v = RD16(a); break;
                            case 2: v = RD32(a); break;
                            default: v = RD64(a);
                        }
                        eset(VR(t), size, e, v);
                    } else {
                        u64 v = eget(VR(t), size, e);
                        switch (size) {
                            case 0: WR8(a, v); break;
                            case 1: WR16(a, v); break;
                            case 2: WR32(a, v); break;
                            default: WR64(a, v);
                        }
                    }
                    a += eby;
                }
        if (L && !Q)
            for (int r = 0; r < rpt; r++)
                for (int s = 0; s < sel; s++) c->v[(rt + r + s) & 31].d[1] = 0;
        total = a - addr;
    } else { /* single element / replicate */
        int R = (w >> 21) & 1, op = (w >> 13) & 7, S = (w >> 12) & 1, size = (w >> 10) & 3;
        int sel = (((op & 1) << 1) | R) + 1;
        int sc = op >> 1, idx;
        if (sc == 3) { /* LDnR */
            if (!L || S) return 0;
            int eb = 8 << size, n = (Q ? 128 : 64) / eb;
            u64 a = addr;
            for (int s = 0; s < sel; s++) {
                u64 v;
                switch (size) {
                    case 0: v = RD8(a); break;
                    case 1: v = RD16(a); break;
                    case 2: v = RD32(a); break;
                    default: v = RD64(a);
                }
                V128 r;
                memset(&r, 0, 16);
                for (int e = 0; e < n; e++) eset(&r, size, e, v);
                c->v[(rt + s) & 31] = r;
                a += eb / 8;
            }
            total = a - addr;
        } else {
            int esz;
            if (sc == 0) {
                esz = 0;
                idx = (Q << 3) | (S << 2) | size;
            } else if (sc == 1) {
                if (size & 1) return 0;
                esz = 1;
                idx = (Q << 2) | (S << 1) | (size >> 1);
            } else {
                if (size & 2) return 0;
                if (size & 1) {
                    if (S) return 0;
                    esz = 3;
                    idx = Q;
                } else {
                    esz = 2;
                    idx = (Q << 1) | S;
                }
            }
            u64 a = addr;
            int by = 1 << esz;
            for (int s = 0; s < sel; s++) {
                int t = (rt + s) & 31;
                if (L) {
                    u64 v;
                    switch (esz) {
                        case 0: v = RD8(a); break;
                        case 1: v = RD16(a); break;
                        case 2: v = RD32(a); break;
                        default: v = RD64(a);
                    }
                    eset(VR(t), esz, idx, v);
                } else {
                    u64 v = eget(VR(t), esz, idx);
                    switch (esz) {
                        case 0: WR8(a, v); break;
                        case 1: WR16(a, v); break;
                        case 2: WR32(a, v); break;
                        default: WR64(a, v);
                    }
                }
                a += by;
            }
            total = a - addr;
        }
    }
    if (post) {
        if (rm == 31)
            c->x[rn] = addr + total;
        else
            c->x[rn] = addr + c->x[rm];
    }
    return 1;
}
