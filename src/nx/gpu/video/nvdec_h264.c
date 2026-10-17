/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC H.264 frontend: the game passes no SPS or PPS, they are rebuilt here from the picture
   setup struct (nvdec_h264_pic_s, its iova in register 0x404) */
#include "nx/gpu/video/nvdec_int.h"
/* RBSP bit writer, MSB first: one bit, n bits, unsigned and signed Exp-Golomb */
typedef struct {
    u8* p;
    u32 cap, bytes; /* output buffer and bytes written so far */
    u32 acc; /* bit accumulator, nb bits pending */
    int nb;
} BW;
static void bw_init(BW* w, u8* p, u32 cap) {
    memset(w, 0, sizeof *w);
    w->p = p;
    w->cap = cap;
}
static void bw_bit(BW* w, u32 b) {
    w->acc = (w->acc << 1) | (b & 1);
    if (++w->nb == 8) {
        if (w->bytes < w->cap) w->p[w->bytes++] = (u8)w->acc;
        w->acc = 0;
        w->nb = 0;
    }
}
static void bw_u(BW* w, u32 v, int n) {
    for (int i = n - 1; i >= 0; i--) bw_bit(w, (v >> i) & 1);
}
static void bw_ue(BW* w, u32 v) {
    u32 x = v + 1;
    int n = 0;
    while ((x >> n) > 1) n++;
    for (int i = 0; i < n; i++) bw_bit(w, 0);
    bw_u(w, x, n + 1);
}
static void bw_se(BW* w, i32 v) { bw_ue(w, v > 0 ? (u32)(2 * v - 1) : (u32)(-2 * v)); }
static void bw_trailing(BW* w) {
    bw_bit(w, 1);
    while (w->nb) bw_bit(w, 0);
}
/* start code + emulation prevention bytes. 0 when it doesn't fit in cap */
static u32 nal_out(u8* dst, u32 cap, u8 hdr, const u8* rbsp, u32 n) {
    u32 o = 0, i = 0;
    if (cap < 5) return 0;
    dst[o++] = 0;
    dst[o++] = 0;
    dst[o++] = 0;
    dst[o++] = 1;
    dst[o++] = hdr;
    int z = 0;
    for (; i < n && o + 2 < cap; i++) {
        if (z >= 2 && rbsp[i] <= 3) {
            dst[o++] = 3;
            z = 0;
        }
        dst[o++] = rbsp[i];
        z = rbsp[i] == 0 ? z + 1 : 0;
    }
    return i == n ? o : 0;
}
static i32 sext(u32 v, int bits) { return (i32)(v << (32 - bits)) >> (32 - bits); }
/* zig-zag position to raster position, for the scaling lists in the pic setup */
static const u8 zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const u8 zz8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                           41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                           30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
/* one scaling list as zig-zag deltas of the setup's raster values */
static void scaling_list(BW* w, const u8* raster, int n, const u8* zz) {
    int last = 8;
    for (int j = 0; j < n; j++) {
        int v = raster[zz[j]];
        int d = v - last;
        if (d > 127) d -= 256;
        if (d < -128) d += 256;
        bw_se(w, d);
        last = v;
    }
}
u32 nvdec_h264_build_headers(const u8* ps, u8* out, u32 cap) {
    u8 rb[1024];
    BW w;
    u32 o = 0;
    u32 log2_poc = rd32(ps, 0x58), dpoc0 = rd32(ps, 0x5C), fmo = rd32(ps, 0x60), wmb = rd32(ps, 0x64),
        hmb = rd32(ps, 0x68);
    u32 ecm = rd32(ps, 0x70), pop = rd32(ps, 0x74), l0 = rd32(ps, 0x78), l1 = rd32(ps, 0x7C), dfc = rd32(ps, 0x80),
        rpc = rd32(ps, 0x84), t8 = rd32(ps, 0x88);
    u32 b0 = rd32(ps, 0xB0), b1 = rd32(ps, 0xB4), bl = rd32(ps, 0x2D0);
    u32 mbaff = b0 & 1, d8 = (b0 >> 1) & 1, wp = (b0 >> 2) & 1, cip = (b0 >> 3) & 1, l2fn = (b0 >> 8) & 15,
        cfi = (b0 >> 12) & 3, poct = (b0 >> 14) & 3;
    i32 qp26 = sext((b0 >> 16) & 63, 6), cqo = sext((b0 >> 22) & 31, 5), cqo2 = sext((b0 >> 27) & 31, 5);
    u32 wbi = b1 & 3, qpz = (bl >> 1) & 1;
    /* SPS, High profile so the PPS may use 8x8 transform / scaling lists */
    bw_init(&w, rb, sizeof rb);
    bw_u(&w, 100, 8); /* profile_idc */
    bw_u(&w, 0, 8); /* constraint_set flags */
    bw_u(&w, 51, 8); /* level_idc */
    bw_ue(&w, 0); /* seq_parameter_set_id */
    bw_ue(&w, cfi ? cfi : 1); /* chroma_format_idc, 4:2:0 when the pic setup leaves it 0 */
    if (cfi == 3) bw_u(&w, 0, 1); /* separate_colour_plane_flag */
    bw_ue(&w, 0); /* bit_depth_luma_minus8 */
    bw_ue(&w, 0); /* bit_depth_chroma_minus8 */
    bw_u(&w, qpz, 1); /* qpprime_y_zero_transform_bypass_flag */
    bw_u(&w, 0, 1); /* seq_scaling_matrix_present_flag: the PPS carries the lists */
    bw_ue(&w, l2fn); /* log2_max_frame_num_minus4 */
    bw_ue(&w, poct); /* pic_order_cnt_type */
    if (poct == 0)
        bw_ue(&w, log2_poc); /* log2_max_pic_order_cnt_lsb_minus4 */
    else if (poct == 1) {
        bw_u(&w, dpoc0, 1); /* delta_pic_order_always_zero_flag */
        bw_se(&w, 0); /* offset_for_non_ref_pic */
        bw_se(&w, 0); /* offset_for_top_to_bottom_field */
        bw_ue(&w, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }
    bw_ue(&w, 16); /* max_num_ref_frames */
    bw_u(&w, 0, 1); /* gaps_in_frame_num_value_allowed_flag */
    bw_ue(&w, wmb ? wmb - 1 : 0); /* pic_width_in_mbs_minus1 */
    bw_ue(&w, fmo ? (hmb ? hmb - 1 : 0) : (hmb / 2 ? hmb / 2 - 1 : 0)); /* pic_height_in_map_units_minus1 */
    bw_u(&w, fmo, 1); /* frame_mbs_only_flag */
    if (!fmo) bw_u(&w, mbaff, 1); /* mb_adaptive_frame_field_flag */
    bw_u(&w, d8, 1); /* direct_8x8_inference_flag */
    bw_u(&w, 0, 1); /* frame_cropping_flag */
    bw_u(&w, 0, 1); /* vui_parameters_present_flag */
    bw_trailing(&w);
    u32 sn = nal_out(out + o, cap - o, 0x67, rb, w.bytes);
    if (!sn) return 0;
    o += sn;
    /* PPS, scaling matrices taken from the pic setup */
    bw_init(&w, rb, sizeof rb);
    bw_ue(&w, 0); /* pic_parameter_set_id */
    bw_ue(&w, 0); /* seq_parameter_set_id */
    bw_u(&w, ecm, 1); /* entropy_coding_mode_flag */
    bw_u(&w, pop, 1); /* bottom_field_pic_order_in_frame_present_flag */
    bw_ue(&w, 0); /* num_slice_groups_minus1 */
    bw_ue(&w, l0); /* num_ref_idx_l0_default_active_minus1 */
    bw_ue(&w, l1); /* num_ref_idx_l1_default_active_minus1 */
    bw_u(&w, wp, 1); /* weighted_pred_flag */
    bw_u(&w, wbi, 2); /* weighted_bipred_idc */
    bw_se(&w, qp26); /* pic_init_qp_minus26 */
    bw_se(&w, 0); /* pic_init_qs_minus26 */
    bw_se(&w, cqo); /* chroma_qp_index_offset */
    bw_u(&w, dfc, 1); /* deblocking_filter_control_present_flag */
    bw_u(&w, cip, 1); /* constrained_intra_pred_flag */
    bw_u(&w, rpc, 1); /* redundant_pic_cnt_present_flag */
    bw_u(&w, t8, 1); /* transform_8x8_mode_flag */
    bw_u(&w, 1, 1); /* pic_scaling_matrix_present_flag */
    for (int i = 0; i < 6; i++) {
        bw_u(&w, 1, 1); /* present flag of the 4x4 list */
        scaling_list(&w, ps + 0x1C0 + i * 16, 16, zz4);
    }
    if (t8)
        for (int i = 0; i < 2; i++) {
            bw_u(&w, 1, 1); /* present flag of the 8x8 list */
            scaling_list(&w, ps + 0x220 + i * 64, 64, zz8);
        }
    bw_se(&w, cqo2); /* second_chroma_qp_index_offset */
    bw_trailing(&w);
    u32 pn = nal_out(out + o, cap - o, 0x68, rb, w.bytes);
    if (!pn) return 0;
    return o + pn;
}
static u32 nal_length(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static int nal_header(u8 b) { return !(b & 0x80) && (b & 31) && (b & 31) < 24; }
/* NVDEC jobs normally contain Annex B. Also accept complete four-byte-length-prefixed access units and a
   single bare NAL. Validate the whole length-prefixed input before selecting it: a length of 256..511 starts
   with 00 00 01 too. Never turn a truncated length-prefixed packet into a made-up bare NAL. */
int nvdec_h264_packet(AVPacket* pkt, const u8* hdr, u32 hn, const u8* bs, u32 slen) {
    u32 p = 0;
    while (slen - p >= 4) {
        u32 n = nal_length(bs + p);
        if (!n || n > slen - p - 4 || !nal_header(bs[p + 4])) break;
        p += 4 + n;
    }
    int lengths = slen && p == slen;
    u32 zeros = 0;
    while (zeros < slen && !bs[zeros]) ++zeros;
    int annexb = zeros >= 2 && zeros + 1 < slen && bs[zeros] == 1 && nal_header(bs[zeros + 1]);
    if (!slen || (!lengths && !annexb && !nal_header(bs[0]))) return AVERROR_INVALIDDATA;
    /* av_new_packet owns/refcounts the storage and reserves AV_INPUT_BUFFER_PADDING_SIZE extra bytes.
       Shrinking after assembly zeros that padding at the actual end, including on shorter subsequent jobs. */
    av_packet_unref(pkt);
    int err = av_new_packet(pkt, (int)(hn + slen + 3));
    if (err < 0) return err;
    u8* out = pkt->data;
    memcpy(out, hdr, hn);
    u32 o = hn;
    if (lengths) {
        for (p = 0; p < slen;) {
            u32 n = nal_length(bs + p);
            memcpy(out + o, "\0\0\0\1", 4);
            memcpy(out + o + 4, bs + p + 4, n);
            o += 4 + n;
            p += 4 + n;
        }
    } else {
        if (!annexb) {
            memcpy(out + o, "\0\0\1", 3);
            o += 3;
        }
        memcpy(out + o, bs, slen);
        o += slen;
    }
    av_shrink_packet(pkt, (int)o);
    return 0;
}
