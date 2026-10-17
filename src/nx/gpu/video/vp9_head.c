/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors.
   VP9 frame header composer for NVDEC: the hardware gets picture info, entropy
   probabilities and raw tile data separately; FFmpeg wants a whole VP9 frame.
   Logic ported from Eden (GPL-3.0-or-later, originally yuzu GPL-2.0-or-later)
   src/video_core/host1x/codecs/vp9.cpp. */
#include "nx/gpu/video/vp9_head.h"
#include "nx/gpu/video/vp9_head_coef.h" /* the coef default table: large, generated from Eden */
#include <string.h>

/* Vp9 entropy based off Eden's Vp9EntropyProbs */
static const u8 def_y_mode[36] = {65,  32, 18, 144, 162, 194, 41, 51, 98, 132, 68,  18, 165, 217, 196, 45, 40, 78,
                                  173, 80, 19, 176, 240, 193, 64, 35, 46, 221, 135, 38, 194, 248, 121, 96, 85, 29};
static const u8 def_partition[64] = {
    199, 122, 141, 0,  147, 63, 159, 0,  148, 133, 118, 0,  121, 104, 114, 0,  174, 73, 87, 0,  92, 41,
    83,  0,   82,  99, 50,  0,  53,  39, 39,  0,   177, 58, 59,  0,   68,  26, 63,  0,  52, 79, 25, 0,
    17,  14,  12,  0,  222, 34, 30,  0,  72,  16,  44,  0,  58,  32,  12,  0,  10,  7,  6,  0,
};
static const u8 def_switchable[8] = {235, 162, 36, 255, 34, 3, 149, 144};
static const u8 def_inter_mode[28] = {2,  173, 34, 0,  7,  145, 85, 0,  7,  166, 63, 0,  7,  94,
                                      66, 0,   8,  64, 46, 0,   17, 81, 31, 0,   25, 29, 30, 0};
static const u8 def_intra_inter[4] = {9, 102, 187, 225};
static const u8 def_comp_inter[5] = {9, 102, 187, 225, 0};
static const u8 def_single_ref[10] = {33, 16, 77, 74, 142, 142, 172, 170, 238, 247};
static const u8 def_comp_ref[5] = {50, 126, 123, 221, 226};
static const u8 def_tx32[6] = {3, 136, 37, 5, 52, 13};
static const u8 def_tx16[4] = {20, 152, 15, 101};
static const u8 def_tx8[2] = {100, 66};
static const u8 def_skip[3] = {192, 128, 64};
static const u8 def_joints[3] = {32, 64, 96};
static const u8 def_sign[2] = {128, 128};
static const u8 def_classes[20] = {224, 144, 192, 168, 192, 176, 192, 198, 198, 245,
                                   216, 128, 176, 160, 176, 176, 192, 198, 198, 208};
static const u8 def_class0[2] = {216, 208};
static const u8 def_prob_bits[20] = {136, 140, 148, 160, 176, 192, 224, 234, 234, 240,
                                     136, 140, 148, 160, 176, 192, 224, 234, 234, 240};
static const u8 def_class0_fr[12] = {128, 128, 64, 96, 112, 64, 128, 128, 64, 96, 112, 64};
static const u8 def_fr[6] = {64, 96, 64, 64, 96, 64};
static const u8 def_class0_hp[2] = {160, 160};
static const u8 def_highprec[2] = {128, 128};

void vp9_head_reset(Vp9HeadCtx* c) {
    memset(c, 0, sizeof *c);
    memcpy(c->prev.y_mode, def_y_mode, sizeof def_y_mode);
    memcpy(c->prev.partition, def_partition, sizeof def_partition);
    memcpy(c->prev.coef, def_coef, sizeof def_coef);
    memcpy(c->prev.switchable, def_switchable, sizeof def_switchable);
    memcpy(c->prev.inter_mode, def_inter_mode, sizeof def_inter_mode);
    memcpy(c->prev.intra_inter, def_intra_inter, sizeof def_intra_inter);
    memcpy(c->prev.comp_inter, def_comp_inter, sizeof def_comp_inter);
    memcpy(c->prev.single_ref, def_single_ref, sizeof def_single_ref);
    memcpy(c->prev.comp_ref, def_comp_ref, sizeof def_comp_ref);
    memcpy(c->prev.tx32, def_tx32, sizeof def_tx32);
    memcpy(c->prev.tx16, def_tx16, sizeof def_tx16);
    memcpy(c->prev.tx8, def_tx8, sizeof def_tx8);
    memcpy(c->prev.skip, def_skip, sizeof def_skip);
    memcpy(c->prev.joints, def_joints, sizeof def_joints);
    memcpy(c->prev.sign, def_sign, sizeof def_sign);
    memcpy(c->prev.classes, def_classes, sizeof def_classes);
    memcpy(c->prev.class0, def_class0, sizeof def_class0);
    memcpy(c->prev.prob_bits, def_prob_bits, sizeof def_prob_bits);
    memcpy(c->prev.class0_fr, def_class0_fr, sizeof def_class0_fr);
    memcpy(c->prev.fr, def_fr, sizeof def_fr);
    memcpy(c->prev.class0_hp, def_class0_hp, sizeof def_class0_hp);
    memcpy(c->prev.highprec, def_highprec, sizeof def_highprec);
    for (int i = 0; i < 4; i++) c->ctxs[i] = c->prev;
    c->inited = 1;
}

/*  bit writer (uncompressed header)  */
typedef struct {
    u8* out;
    u32 cap, len;
    u32 buf;
    int nbits; /* bits in buf (left-aligned in low 8?) */
} BitW;
static void bitw_init(BitW* w, u8* out, u32 cap) {
    w->out = out;
    w->cap = cap;
    w->len = 0;
    w->buf = 0;
    w->nbits = 0;
}
static void bitw_bits(BitW* w, u32 v, int n) {
    while (n > 0) {
        int room = 8 - w->nbits;
        int take = n < room ? n : room;
        int shift = n - take;
        w->buf = (w->buf << take) | ((v >> shift) & ((1u << take) - 1));
        w->nbits += take;
        n -= take;
        if (w->nbits == 8) {
            if (w->len < w->cap) w->out[w->len++] = (u8)w->buf;
            w->buf = 0;
            w->nbits = 0;
        }
    }
}
static void bitw_bit(BitW* w, int b) { bitw_bits(w, b ? 1u : 0u, 1); }
static void bitw_flush(BitW* w) {
    if (w->nbits && w->len < w->cap) w->out[w->len++] = (u8)(w->buf << (8 - w->nbits));
    w->buf = 0;
    w->nbits = 0;
}

/*  range encoder (compressed header)  */
typedef struct {
    u8* out;
    u32 cap, len;
    u32 low;
    u32 range;
    int count;
} RangeE;
static void range_init(RangeE* e, u8* out, u32 cap) {
    e->out = out;
    e->cap = cap;
    e->len = 0;
    e->low = 0;
    /* Eden's constructor writes one false bit: 0xFF -> 0x80, no output yet */
    e->range = 0x80;
    e->count = -24;
}
static void range_put(RangeE* e, u8 b) {
    if (e->len < e->cap) e->out[e->len++] = b;
}
static void range_write(RangeE* e, int bit, int prob) {
    u32 range = e->range;
    u32 split = 1 + (((range - 1) * (u32)prob) >> 8);
    u32 rng = bit ? range - split : split;
    if (bit) e->low += split;
    int shift = rng ? (int)__builtin_clz(rng) - 24 : 0; /* __builtin_clz(0) is undefined: rng 0 shifts nothing */
    rng <<= shift;
    e->count += shift;
    if (e->count >= 0) {
        int off = shift - e->count;
        if ((((e->low << (off - 1)) >> 31) & 1) != 0) {
            /* carry propagation over 0xff run */
            int pos = (int)e->len - 1;
            while (pos >= 0 && e->out[pos] == 0xFF) {
                e->out[pos] = 0;
                pos--;
            }
            if (pos >= 0) e->out[pos]++;
        }
        range_put(e, (u8)(e->low >> (24 - off)));
        e->low <<= off;
        shift = e->count;
        e->low &= 0xFFFFFF;
        e->count -= 8;
    }
    e->low <<= shift;
    e->range = rng;
}
static void range_write_eq(RangeE* e, int bit) { range_write(e, bit, 128); }
static void range_write_bits(RangeE* e, int v, int n) {
    for (int i = n - 1; i >= 0; i--) range_write_eq(e, (v >> i) & 1);
}
static void range_end(RangeE* e) {
    for (int i = 0; i < 32; i++) range_write_eq(e, 0);
}

/*  probability helpers (VP9 spec 6.3.5/6.3.6)  */
static int recenter_nonneg(int np, int op) {
    if (np > op * 2) return np;
    if (np >= op) return (np - op) * 2;
    return (op - np) * 2 - 1;
}
static int remap_prob(int np, int op) {
    np--;
    op--;
    u8 i;
    if (op * 2 <= 0xFF) {
        int r = recenter_nonneg(np, op) - 1;
        i = (u8)(r > 0 ? r : 0);
    } else {
        int a = 0xFF - 1 - np, b = 0xFF - 1 - op;
        int r = recenter_nonneg(a, b) - 1;
        i = (u8)(r > 0 ? r : 0);
    }
    return (i + 7) % 13 == 0 ? (i + 7) / 13 - 1 : i + 20 - (i + 7) / 13;
}
static void write_prob_upd(RangeE* e, u8 np, u8 op) {
    int upd = np != op;
    range_write(e, upd, 252);
    if (!upd) return;
    /* EncodeTermSubExp */
    int v = remap_prob(np, op);
    int lt16 = v < 16;
    range_write_eq(e, !lt16);
    if (lt16) {
        range_write_bits(e, v, 4);
    } else {
        int lt32 = v < 32;
        range_write_eq(e, !lt32);
        if (lt32)
            range_write_bits(e, v - 16, 4);
        else {
            int lt64 = v < 64;
            range_write_eq(e, !lt64);
            if (lt64)
                range_write_bits(e, v - 32, 5);
            else {
                v -= 64;
                int size = 8, mask = (1 << size) - 191, delta = v - mask;
                if (delta < 0)
                    range_write_bits(e, v, size - 1);
                else {
                    range_write_bits(e, delta / 2 + mask, size - 1);
                    range_write_eq(e, delta & 1);
                }
            }
        }
    }
}
static void write_prob_arr(RangeE* e, const u8* np, const u8* op, int n) {
    for (int i = 0; i < n; i++) write_prob_upd(e, np[i], op[i]);
}
static void write_prob_arr4(RangeE* e, const u8* np, const u8* op, int n) {
    for (int i = 0; i < n; i += 4) {
        write_prob_upd(e, np[i], op[i]);
        write_prob_upd(e, np[i + 1], op[i + 1]);
        write_prob_upd(e, np[i + 2], op[i + 2]);
    }
}
static void write_mv_upd(RangeE* e, u8 np, u8 op) {
    int upd = np != op;
    range_write(e, upd, 252);
    if (upd) range_write_bits(e, np >> 1, 7);
}
static void write_coef_upd(RangeE* e, int tx_mode, const u8* np, const u8* op) {
    const int block = 2 * 2 * 6 * 6 * 3;
    for (int b = 0; b < 4; b++) {
        const u8* nb = np + b * block;
        const u8* ob = op + b * block;
        int upd = memcmp(nb, ob, block) != 0;
        range_write_eq(e, upd);
        if (upd) {
            int idx = 0;
            for (int i = 0; i < 2; i++)
                for (int j = 0; j < 2; j++)
                    for (int k = 0; k < 6; k++)
                        for (int l = 0; l < 6; l++) {
                            if (k != 0 || l < 3) {
                                write_prob_upd(e, nb[idx], ob[idx]);
                                write_prob_upd(e, nb[idx + 1], ob[idx + 1]);
                                write_prob_upd(e, nb[idx + 2], ob[idx + 2]);
                            }
                            idx += 3;
                        }
        }
        if (b == tx_mode) break;
    }
}

/*  picture info parsing (guest layout)  */
static u16 rd16(const u8* p, u32 o) { return (u16)(p[o] | (p[o + 1] << 8)); }
static u32 rd32p(const u8* p, u32 o) {
    u32 v;
    memcpy(&v, p + o, 4);
    return v;
}
static int calc_min_tile_cols(int w) {
    int sb = (w + 63) / 64, m = 0;
    while ((64 << m) < sb) m++;
    return m;
}
static int calc_max_tile_cols(int w) {
    int sb = (w + 63) / 64, m = 1;
    while ((sb >> m) >= 4) m++;
    return m - 1;
}

/* entropy conversion from guest EntropyProbs to Vp9Ent */
static void convert_entropy(const u8* g, Vp9Ent* e) {
    /* y modes: e8[4] at 0x47C, e0e7[4][8] at 0x480 */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 9; j++) e->y_mode[j + 9 * i] = j < 8 ? g[0x480 + i * 8 + j] : g[0x47C + i];
    memcpy(e->partition, g + 0x4E0, 64);
    /* coef: 2304 at 0x5A0 -> 1728 skipping every 4th */
    const u8* src = g + 0x5A0;
    int j = 0;
    for (int i = 0; i < 2304; i += 4) {
        e->coef[j++] = src[i];
        e->coef[j++] = src[i + 1];
        e->coef[j++] = src[i + 2];
    }
    memcpy(e->switchable, g + 0x52A, 8);
    memcpy(e->inter_mode, g + 0x400, 28);
    memcpy(e->intra_inter, g + 0x41C, 4);
    memcpy(e->comp_inter, g + 0x532, 5);
    memcpy(e->single_ref, g + 0x580, 10);
    memcpy(e->comp_ref, g + 0x58A, 5);
    memcpy(e->tx32, g + 0x476, 6);
    memcpy(e->tx16, g + 0x472, 4);
    memcpy(e->tx8, g + 0x470, 2);
    memcpy(e->skip, g + 0x537, 3);
    memcpy(e->joints, g + 0x53B, 3);
    memcpy(e->sign, g + 0x53E, 2);
    memcpy(e->classes, g + 0x54C, 20);
    memcpy(e->class0, g + 0x540, 2);
    memcpy(e->prob_bits, g + 0x56C, 20);
    memcpy(e->class0_fr, g + 0x560, 12);
    memcpy(e->fr, g + 0x542, 6);
    memcpy(e->class0_hp, g + 0x548, 2);
    memcpy(e->highprec, g + 0x54A, 2);
}

/* segmentation writer, updates ctx last_seg */
static void write_segmentation(BitW* w, Vp9HeadCtx* c, const u8* pic, const u8* entropy_g) {
    int enabled = pic[0x80] != 0;
    bitw_bit(w, enabled);
    if (!enabled) return;
    int update_map = pic[0x81] != 0;
    bitw_bit(w, update_map);
    if (update_map) {
        for (int i = 0; i < 7; i++) {
            u8 p = entropy_g[0x387 + i];
            int coded = p != 255;
            bitw_bit(w, coded);
            if (coded) bitw_bits(w, p, 8);
        }
        int temporal = pic[0x82] != 0;
        bitw_bit(w, temporal);
        if (temporal)
            for (int i = 0; i < 3; i++) {
                u8 p = entropy_g[0x38E + i];
                int coded = p != 255;
                bitw_bit(w, coded);
                if (coded) bitw_bits(w, p, 8);
            }
    }
    /* compare with last */
    int same = memcmp(pic + 0x80, c->last_seg, 0x64) == 0;
    if (same) {
        bitw_bit(w, 0);
        return;
    }
    memcpy(c->last_seg, pic + 0x80, 0x64);
    bitw_bit(w, 1);
    bitw_bit(w, pic[0x83] != 0);
    static const int feat_bits[4] = {8, 6, 2, 0};
    for (int i = 0; i < 8; i++)
        for (int f = 0; f < 4; f++) {
            int en = pic[0x84 + i * 4 + f] != 0;
            bitw_bit(w, en);
            if (en) {
                int v = (int)(int16_t)rd16(pic, 0xA4 + (i * 4 + f) * 2);
                if (f < 3) {
                    /* signed with feat_bits */
                    int mag = v < 0 ? -v : v, sign = v < 0;
                    bitw_bits(w, (mag << 1) | sign, feat_bits[f] + 1);
                }
            }
        }
}

/* compose one frame: headers + bitstream into out. Returns total bytes, 0 on failure.
   pic is 0x100 PictureInfo bytes, entropy_g is 0xEA0 EntropyProbs bytes. */
u32 vp9_compose_frame(Vp9HeadCtx* c, const u8* pic, const u8* entropy_g, const u8* bs, u32 bs_len, u8* out, u32 out_cap,
                      const u64 curr_off[4], const u64 next_off[4], int has_next, int last_shown) {
    if (!c->inited) vp9_head_reset(c);
    Vp9Ent cur;
    convert_entropy(entropy_g, &cur);
    u32 flags = rd32p(pic, 0x68);
    int is_key = (flags & 1) != 0, err_res = (flags & 8) != 0, intra_only_pic = (flags & 32) != 0;
    int width = (int)(int16_t)rd16(pic, 0x60), height = (int)(int16_t)rd16(pic, 0x62);
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return 0;
    /* a picture's visibility only appears in the next job's setup (bit 4 = last picture was shown):
       0 means the pending one was a hidden alt-ref and its header must say show_frame = 0 */
    int show_frame = has_next ? last_shown : 1, intra_only = is_key; /* a keyframe is intra-only */
    if (is_key) {
        /* a keyframe's probability updates are coded from the defaults, where a decoder resets its
           context: seed the context slot with them. vp9_head_reset would also clear last_seg, which
           write_segmentation compares the picture against, so only the slot is taken from it */
        Vp9HeadCtx tmp;
        vp9_head_reset(&tmp);
        c->ctxs[0] = tmp.prev;
        c->swap_ref = 0;
    } else {
        /* hidden pictures carry the setup's intra_only flag; shown pictures never do */
        intra_only = show_frame ? 0 : intra_only_pic;
    }
    /* loop filter deltas return to the VP9 defaults at a keyframe and at an intra-only frame (6.2.9) */
    if (intra_only) {
        c->loop_ref[0] = 1;
        c->loop_ref[1] = 0;
        c->loop_ref[2] = -1;
        c->loop_ref[3] = -1;
        memset(c->loop_mode, 0, sizeof c->loop_mode);
    }
    int lossless = pic[0x76] != 0, tx_mode = pic[0x77], allow_hp = pic[0x78] != 0;
    int interp = pic[0x79], ref_mode = pic[0x7A];
    int tile_cols = pic[0x7E], tile_rows = pic[0x7F];
    int first_level = pic[0x70], sharp = pic[0x71], base_q = pic[0x72];
    int y_dc = (int8_t)pic[0x73], uv_ac = (int8_t)pic[0x74], uv_dc = (int8_t)pic[0x75];
    int mode_ref_on = pic[0xE4] != 0;

    /*  uncompressed header into tmp  */
    u8 uhdr[256];
    BitW w;
    bitw_init(&w, uhdr, sizeof uhdr);
    bitw_bits(&w, 2, 2);
    bitw_bits(&w, 0, 2);
    bitw_bit(&w, 0);
    bitw_bit(&w, !is_key);
    bitw_bit(&w, show_frame);
    bitw_bit(&w, err_res);
    if (is_key) {
        bitw_bits(&w, 0x498342, 24);
        bitw_bits(&w, 0, 3);
        bitw_bit(&w, 0);
        bitw_bits(&w, width - 1, 16);
        bitw_bits(&w, height - 1, 16);
        bitw_bit(&w, 0);
    } else {
        if (!show_frame) bitw_bit(&w, intra_only);
        if (!err_res) bitw_bits(&w, 0, 2);
        /* refresh from surfaces as the hardware does: current's slot 3 feeds next's refs */
        int ref_idx[3] = {0, 1, 2};
        u64 nxt[4];
        for (int i = 0; i < 4; i++) nxt[i] = has_next ? next_off[i] : curr_off[i];
        u32 refresh = 0;
        for (int i = 0; i < 3; i++)
            if (curr_off[3] == nxt[i]) refresh |= 1u << i;
        int ref_diff = curr_off[1] != curr_off[2];
        int next_swap = nxt[1] == curr_off[2] || nxt[2] == curr_off[1];
        if (ref_diff && next_swap) c->swap_ref = !c->swap_ref;
        if (c->swap_ref) {
            /* swapped ref indices: the golden/alt refresh bits swap with them (Eden) */
            u32 g = (refresh >> 1) & 1u, a = (refresh >> 2) & 1u;
            refresh = (refresh & ~6u) | (a << 1u) | (g << 2u);
        }
        int swap_now = (ref_diff && next_swap) ^ c->swap_ref;
        if (swap_now) {
            ref_idx[0] = 0;
            ref_idx[1] = 2;
            ref_idx[2] = 1;
        }
        if (intra_only) {
            bitw_bits(&w, 0x498342, 24);
            bitw_bits(&w, refresh, 8);
            bitw_bits(&w, width - 1, 16);
            bitw_bits(&w, height - 1, 16);
            bitw_bit(&w, 0);
        } else {
            bitw_bits(&w, refresh, 8);
            int8_t bias[4];
            bias[0] = 0;
            bias[1] = (int8_t)pic[0x6D];
            bias[2] = (int8_t)pic[0x6E];
            bias[3] = (int8_t)pic[0x6F];
            for (int i = 1; i < 4; i++) {
                bitw_bits(&w, ref_idx[i - 1], 3);
                bitw_bit(&w, bias[i] & 1);
            }
            bitw_bit(&w, 1);
            bitw_bit(&w, 0);
            bitw_bit(&w, allow_hp);
            bitw_bit(&w, interp == 4);
            if (interp != 4) bitw_bits(&w, interp, 2);
        }
    }
    if (!err_res) {
        bitw_bit(&w, 1);
        bitw_bit(&w, 1);
    }
    int ctx_idx = 0;
    c->prev = c->ctxs[ctx_idx];
    c->ctxs[ctx_idx] = cur;
    bitw_bits(&w, ctx_idx, 2);
    bitw_bits(&w, first_level, 6);
    bitw_bits(&w, sharp, 3);
    bitw_bit(&w, mode_ref_on);
    if (mode_ref_on) {
        int upd_r[4], upd_m[2], any = 0;
        for (int i = 0; i < 4; i++) {
            upd_r[i] = c->loop_ref[i] != (int8_t)pic[0xE5 + i];
            any |= upd_r[i];
        }
        for (int i = 0; i < 2; i++) {
            upd_m[i] = c->loop_mode[i] != (int8_t)pic[0xE9 + i];
            any |= upd_m[i];
        }
        bitw_bit(&w, any);
        if (any) {
            for (int i = 0; i < 4; i++) {
                bitw_bit(&w, upd_r[i]);
                if (upd_r[i]) {
                    int v = (int8_t)pic[0xE5 + i], mag = v < 0 ? -v : v, sign = v < 0;
                    bitw_bits(&w, (mag << 1) | sign, 7);
                }
            }
            for (int i = 0; i < 2; i++) {
                bitw_bit(&w, upd_m[i]);
                if (upd_m[i]) {
                    int v = (int8_t)pic[0xE9 + i], mag = v < 0 ? -v : v, sign = v < 0;
                    bitw_bits(&w, (mag << 1) | sign, 7);
                }
            }
            for (int i = 0; i < 4; i++) c->loop_ref[i] = (int8_t)pic[0xE5 + i];
            for (int i = 0; i < 2; i++) c->loop_mode[i] = (int8_t)pic[0xE9 + i];
        }
    }
    bitw_bits(&w, base_q, 8);
    bitw_bit(&w, y_dc != 0);
    if (y_dc) bitw_bits(&w, y_dc & 0xF, 4);
    bitw_bit(&w, uv_dc != 0);
    if (uv_dc) bitw_bits(&w, uv_dc & 0xF, 4);
    bitw_bit(&w, uv_ac != 0);
    if (uv_ac) bitw_bits(&w, uv_ac & 0xF, 4);
    write_segmentation(&w, c, pic, entropy_g);
    int min_tc = calc_min_tile_cols(width), max_tc = calc_max_tile_cols(width);
    int diff = tile_cols - min_tc, mask = diff > 0 ? (1 << diff) - 1 : 0;
    if (tile_cols < max_tc)
        bitw_bits(&w, mask << 1, diff + 1);
    else
        bitw_bits(&w, mask, diff);
    bitw_bit(&w, tile_rows != 0);
    if (tile_rows) bitw_bit(&w, tile_rows > 1);

    /*  compressed header  */
    u8 chdr[8192];
    RangeE e;
    range_init(&e, chdr, sizeof chdr);
    int upd_probs = !is_key && show_frame;
    Vp9Ent prev = c->prev;
    if (!lossless) {
        if (tx_mode >= 3) {
            range_write_bits(&e, 3, 2);
            range_write_eq(&e, tx_mode == 4);
        } else
            range_write_bits(&e, tx_mode, 2);
    }
    if (tx_mode == 4) {
        write_prob_arr(&e, cur.tx8, prev.tx8, 2);
        write_prob_arr(&e, cur.tx16, prev.tx16, 4);
        write_prob_arr(&e, cur.tx32, prev.tx32, 6);
        if (upd_probs) {
            memcpy(prev.tx8, cur.tx8, 2);
            memcpy(prev.tx16, cur.tx16, 4);
            memcpy(prev.tx32, cur.tx32, 6);
        }
    }
    write_coef_upd(&e, tx_mode, cur.coef, prev.coef);
    write_prob_arr(&e, cur.skip, prev.skip, 3);
    if (upd_probs) {
        memcpy(prev.coef, cur.coef, sizeof prev.coef);
        memcpy(prev.skip, cur.skip, 3);
    }
    if (!intra_only) {
        write_prob_arr4(&e, cur.inter_mode, prev.inter_mode, 28);
        if (interp == 4) {
            write_prob_arr(&e, cur.switchable, prev.switchable, 8);
            if (upd_probs) memcpy(prev.switchable, cur.switchable, 8);
        }
        write_prob_arr(&e, cur.intra_inter, prev.intra_inter, 4);
        if ((pic[0x6D] & 1) != (pic[0x6E] & 1) || (pic[0x6D] & 1) != (pic[0x6F] & 1)) {
            if (ref_mode >= 1) {
                range_write_eq(&e, 1);
                range_write_eq(&e, ref_mode == 2);
            } else
                range_write_eq(&e, 0);
        }
        if (ref_mode == 2) {
            write_prob_arr(&e, cur.comp_inter, prev.comp_inter, 5);
            if (upd_probs) memcpy(prev.comp_inter, cur.comp_inter, 5);
        }
        if (ref_mode != 1) {
            write_prob_arr(&e, cur.single_ref, prev.single_ref, 10);
            if (upd_probs) memcpy(prev.single_ref, cur.single_ref, 10);
        }
        if (ref_mode != 0) {
            write_prob_arr(&e, cur.comp_ref, prev.comp_ref, 5);
            if (upd_probs) memcpy(prev.comp_ref, cur.comp_ref, 5);
        }
        for (int i = 0; i < 36; i++) write_prob_upd(&e, cur.y_mode[i], prev.y_mode[i]);
        write_prob_arr4(&e, cur.partition, prev.partition, 64);
        for (int i = 0; i < 3; i++) write_mv_upd(&e, cur.joints[i], prev.joints[i]);
        if (upd_probs) {
            memcpy(prev.inter_mode, cur.inter_mode, 28);
            memcpy(prev.intra_inter, cur.intra_inter, 4);
            memcpy(prev.y_mode, cur.y_mode, 36);
            memcpy(prev.partition, cur.partition, 64);
            memcpy(prev.joints, cur.joints, 3);
        }
        for (int i = 0; i < 2; i++) {
            write_mv_upd(&e, cur.sign[i], prev.sign[i]);
            for (int k = 0; k < 10; k++) write_mv_upd(&e, cur.classes[i * 10 + k], prev.classes[i * 10 + k]);
            write_mv_upd(&e, cur.class0[i], prev.class0[i]);
            for (int k = 0; k < 10; k++) write_mv_upd(&e, cur.prob_bits[i * 10 + k], prev.prob_bits[i * 10 + k]);
        }
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 3; k++) {
                    int idx = i * 6 + j * 3 + k;
                    write_mv_upd(&e, cur.class0_fr[idx], prev.class0_fr[idx]);
                }
            for (int k = 0; k < 3; k++) write_mv_upd(&e, cur.fr[i * 3 + k], prev.fr[i * 3 + k]);
        }
        if (allow_hp)
            for (int i = 0; i < 2; i++) {
                write_mv_upd(&e, cur.class0_hp[i], prev.class0_hp[i]);
                write_mv_upd(&e, cur.highprec[i], prev.highprec[i]);
            }
        if (upd_probs) {
            memcpy(prev.sign, cur.sign, 2);
            memcpy(prev.classes, cur.classes, 20);
            memcpy(prev.class0, cur.class0, 2);
            memcpy(prev.prob_bits, cur.prob_bits, 20);
            memcpy(prev.class0_fr, cur.class0_fr, 12);
            memcpy(prev.fr, cur.fr, 6);
            memcpy(prev.class0_hp, cur.class0_hp, 2);
            memcpy(prev.highprec, cur.highprec, 2);
        }
    }
    range_end(&e);
    if (upd_probs) c->prev = prev;

    /* the compressed header's size is the last 16 bits of the uncompressed header (Eden's order) */
    u32 chdr_len = e.len;
    bitw_bits(&w, chdr_len, 16);
    bitw_flush(&w);
    u32 uhdr_len = w.len;
    u32 total = uhdr_len + chdr_len + bs_len;
    if (total > out_cap) return 0;
    memcpy(out, uhdr, uhdr_len);
    memcpy(out + uhdr_len, chdr, chdr_len);
    memcpy(out + uhdr_len + chdr_len, bs, bs_len);
    return total;
}
