/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* Part of cpu_helpers.c, which includes it: AES, SHA and PMULL. Not compiled on its own. */
void v_aes(V128* d, const V128* k, int dec) {
    if (!isbox_ok) {
        for (int i = 0; i < 256; i++) ISBOX[SBOX[i]] = (u8)i;
        isbox_ok = 1;
    }
    V128 s = *d;
    for (int i = 0; i < 16; i++) s.b[i] ^= k->b[i];
    V128 t;
    if (!dec) {
        for (int i = 0; i < 16; i++) {
            int col = i / 4, row = i % 4;
            t.b[i] = SBOX[s.b[((col + row) % 4) * 4 + row]];
        }
    } else {
        for (int i = 0; i < 16; i++) {
            int col = i / 4, row = i % 4;
            t.b[((col + row) % 4) * 4 + row] = ISBOX[s.b[i]];
        }
    }
    *d = t;
}
void v_aesmc(V128* d, const V128* s, int inv) {
    V128 r;
    for (int col = 0; col < 4; col++) {
        const u8* a = &s->b[col * 4];
        for (int row = 0; row < 4; row++) {
            if (!inv)
                r.b[col * 4 + row] =
                    (u8)(gm(a[row], 2) ^ gm(a[(row + 1) % 4], 3) ^ a[(row + 2) % 4] ^ a[(row + 3) % 4]);
            else
                r.b[col * 4 + row] = (u8)(gm(a[row], 14) ^ gm(a[(row + 1) % 4], 11) ^ gm(a[(row + 2) % 4], 13) ^
                                          gm(a[(row + 3) % 4], 9));
        }
    }
    *d = r;
}

/* ---- SHA1 / SHA256 ---- */
static u32 rol32(u32 x, int s) { return (x << s) | (x >> (32 - s)); }
static u32 ror32_(u32 x, int s) { return (x >> s) | (x << (32 - s)); }
void v_sha1(CPU* c, u32 op, u32 d, u32 n, u32 m) {
    V128 X = c->v[d], Y = c->v[n], W = c->v[m];
    if (op < 3) {
        u32 y = Y.s[0];
        for (int i = 0; i < 4; i++) {
            u32 t;
            u32 b = X.s[1], cc = X.s[2], dd = X.s[3];
            if (op == 0)
                t = (b & cc) | (~b & dd);
            else if (op == 1)
                t = b ^ cc ^ dd;
            else
                t = (b & cc) | (b & dd) | (cc & dd);
            y = y + rol32(X.s[0], 5) + t + W.s[i];
            X.s[1] = rol32(X.s[1], 30);
            u32 ny = X.s[3];
            X.s[3] = X.s[2];
            X.s[2] = X.s[1];
            X.s[1] = X.s[0];
            X.s[0] = y;
            y = ny;
        }
        c->v[d] = X;
        return;
    }
    if (op == 3) {
        V128 r;
        r.d[0] = X.d[1];
        r.d[1] = Y.d[0];
        for (int i = 0; i < 4; i++) r.s[i] ^= X.s[i] ^ W.s[i];
        c->v[d] = r;
        return;
    }
    if (op == 0x10) {
        V128 r;
        memset(&r, 0, 16);
        r.s[0] = rol32(Y.s[0], 30);
        c->v[d] = r;
        return;
    }
    if (op == 0x11) {
        V128 T;
        for (int i = 0; i < 4; i++) T.s[i] = X.s[i] ^ (i < 3 ? Y.s[i + 1] : 0);
        V128 r;
        r.s[0] = rol32(T.s[0], 1);
        r.s[1] = rol32(T.s[1], 1);
        r.s[2] = rol32(T.s[2], 1);
        r.s[3] = rol32(T.s[3], 1) ^ rol32(T.s[0], 2);
        c->v[d] = r;
        return;
    }
}
void v_sha256(CPU* c, u32 op, u32 d, u32 n, u32 m) {
    V128 X = c->v[d], Y = c->v[n], W = c->v[m];
    if (op == 4 || op == 5) {
        V128 x = op == 4 ? X : Y, y = op == 4 ? Y : X;
        for (int i = 0; i < 4; i++) {
            u32 ch = (y.s[0] & y.s[1]) ^ (~y.s[0] & y.s[2]);
            u32 maj = (x.s[0] & x.s[1]) ^ (x.s[0] & x.s[2]) ^ (x.s[1] & x.s[2]);
            u32 t = y.s[3] + (ror32_(y.s[0], 6) ^ ror32_(y.s[0], 11) ^ ror32_(y.s[0], 25)) + ch + W.s[i];
            x.s[3] += t;
            y.s[3] = t + (ror32_(x.s[0], 2) ^ ror32_(x.s[0], 13) ^ ror32_(x.s[0], 22)) + maj;
            /* rotate y:x */
            u32 all[8] = {x.s[0], x.s[1], x.s[2], x.s[3], y.s[0], y.s[1], y.s[2], y.s[3]};
            u32 rr[8];
            for (int k = 0; k < 8; k++) rr[k] = all[(k + 7) % 8];
            for (int k = 0; k < 4; k++) {
                x.s[k] = rr[k];
                y.s[k] = rr[k + 4];
            }
        }
        c->v[d] = op == 4 ? x : y;
        return;
    }
    if (op == 6) {
        V128 r;
        u32 T0[4] = {X.s[1], X.s[2], X.s[3], Y.s[0]};
        (void)T0;
        {
            u32 T1[4] = {W.s[2], W.s[3], 0, 0};
            u32 T2[4] = {Y.s[1], Y.s[2], Y.s[3], W.s[0]};
            for (int i = 0; i < 2; i++) {
                u32 e = T1[i];
                r.s[i] = X.s[i] + (ror32_(e, 17) ^ ror32_(e, 19) ^ (e >> 10)) + T2[i];
            }
            for (int i = 2; i < 4; i++) {
                u32 e = r.s[i - 2];
                r.s[i] = X.s[i] + (ror32_(e, 17) ^ ror32_(e, 19) ^ (e >> 10)) + T2[i];
            }
        }
        c->v[d] = r;
        return;
    }
    if (op == 0x12) {
        V128 r;
        u32 T[4] = {X.s[1], X.s[2], X.s[3], Y.s[0]};
        for (int i = 0; i < 4; i++) {
            u32 e = T[i];
            r.s[i] = X.s[i] + (ror32_(e, 7) ^ ror32_(e, 18) ^ (e >> 3));
        }
        c->v[d] = r;
        return;
    }
}
void v_pmull(V128* d, const V128* a, const V128* b, int size, int hi) {
    (void)size;
    u64 x = a->d[hi], y = b->d[hi], lo = 0, h = 0;
    for (int k = 0; k < 64; k++)
        if ((x >> k) & 1) {
            lo ^= y << k;
            if (k) h ^= y >> (64 - k);
        }
    d->d[0] = lo;
    d->d[1] = h;
}
