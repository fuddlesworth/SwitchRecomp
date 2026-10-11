/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC: H.264 decoding with OpenH264, from the SPS and PPS the game passes in registers */
#include "video/host1x_int.h"
/* ------------------------------------------------------------ H.264: SPS/PPS rebuilt from nvdec_h264_pic_s */
typedef struct {
    u8* p;
    u32 cap, bytes;
    u32 acc;
    int nb;
} BW;
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
/* start code + emulation prevention bytes */
static u32 nal_out(u8* dst, u32 cap, u8 hdr, const u8* rbsp, u32 n) {
    u32 o = 0;
    if (cap < 5) return 0;
    dst[o++] = 0;
    dst[o++] = 0;
    dst[o++] = 0;
    dst[o++] = 1;
    dst[o++] = hdr;
    int z = 0;
    for (u32 i = 0; i < n && o + 2 < cap; i++) {
        if (z >= 2 && rbsp[i] <= 3) {
            dst[o++] = 3;
            z = 0;
        }
        dst[o++] = rbsp[i];
        z = rbsp[i] == 0 ? z + 1 : 0;
    }
    return o;
}
u32 rd32(const u8* p, u32 off) {
    u32 v;
    memcpy(&v, p + off, 4);
    return v;
}
static i32 sext(u32 v, int bits) { return (i32)(v << (32 - bits)) >> (32 - bits); }
static const u8 zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const u8 zz8[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                           41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                           30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
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
static u32 build_headers(const u8* ps, u8* out, u32 cap) {
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
    memset(&w, 0, sizeof w);
    w.p = rb;
    w.cap = sizeof rb;
    bw_u(&w, 100, 8);
    bw_u(&w, 0, 8);
    bw_u(&w, 51, 8);
    bw_ue(&w, 0);
    bw_ue(&w, cfi ? cfi : 1);
    bw_ue(&w, 0);
    bw_ue(&w, 0);
    bw_u(&w, qpz, 1);
    bw_u(&w, 0, 1);
    bw_ue(&w, l2fn);
    bw_ue(&w, poct);
    if (poct == 0)
        bw_ue(&w, log2_poc);
    else if (poct == 1) {
        bw_u(&w, dpoc0, 1);
        bw_se(&w, 0);
        bw_se(&w, 0);
        bw_ue(&w, 0);
    }
    bw_ue(&w, 16);
    bw_u(&w, 0, 1);
    bw_ue(&w, wmb ? wmb - 1 : 0);
    bw_ue(&w, fmo ? (hmb ? hmb - 1 : 0) : (hmb / 2 ? hmb / 2 - 1 : 0));
    bw_u(&w, fmo, 1);
    if (!fmo) bw_u(&w, mbaff, 1);
    bw_u(&w, d8, 1);
    bw_u(&w, 0, 1);
    bw_u(&w, 0, 1);
    bw_trailing(&w);
    o += nal_out(out + o, cap - o, 0x67, rb, w.bytes);
    /* PPS, scaling matrices taken from the pic setup */
    memset(&w, 0, sizeof w);
    w.p = rb;
    w.cap = sizeof rb;
    bw_ue(&w, 0);
    bw_ue(&w, 0);
    bw_u(&w, ecm, 1);
    bw_u(&w, pop, 1);
    bw_ue(&w, 0);
    bw_ue(&w, l0);
    bw_ue(&w, l1);
    bw_u(&w, wp, 1);
    bw_u(&w, wbi, 2);
    bw_se(&w, qp26);
    bw_se(&w, 0);
    bw_se(&w, cqo);
    bw_u(&w, dfc, 1);
    bw_u(&w, cip, 1);
    bw_u(&w, rpc, 1);
    bw_u(&w, t8, 1);
    bw_u(&w, 1, 1);
    for (int i = 0; i < 6; i++) {
        bw_u(&w, 1, 1);
        scaling_list(&w, ps + 0x1C0 + i * 16, 16, zz4);
    }
    if (t8)
        for (int i = 0; i < 2; i++) {
            bw_u(&w, 1, 1);
            scaling_list(&w, ps + 0x220 + i * 64, 64, zz8);
        }
    bw_se(&w, cqo2);
    bw_trailing(&w);
    o += nal_out(out + o, cap - o, 0x68, rb, w.bytes);
    return o;
}

/* ------------------------------------------------------------ decoder and the last picture */
#ifndef SWR_NO_H264
static ISVCDecoder* g_dec;
#endif
static int g_dec_fail;
u8* g_fy;
u8* g_fuv;
u32 g_fw, g_fh, g_fcap;
u32 g_vframes;
static u8 g_last_hdr[512];
static u32 g_last_hdr_n;
static int dec_open(void) {
#ifdef SWR_NO_H264
    if (!g_dec_fail) {
        g_dec_fail = 1;
        LOG("video: this build has no H.264 decoder (the game's videos stay black)\n");
    }
    return 0;
#else
    if (g_dec) return 1;
    if (g_dec_fail) return 0;
    if (WelsCreateDecoder(&g_dec) || !g_dec) {
        g_dec_fail = 1;
        LOG("video: OpenH264 decoder could not be created\n");
        return 0;
    }
    SDecodingParam p;
    memset(&p, 0, sizeof p);
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    p.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    if ((*g_dec)->Initialize(g_dec, &p)) {
        g_dec_fail = 1;
        LOG("video: OpenH264 decoder initialize failed\n");
        return 0;
    }
    LOG("video: H.264 decoder ready (OpenH264, NVDEC jobs are decoded on the CPU)\n");
    return 1;
#endif
}
#ifndef SWR_NO_H264
static void keep_frame(unsigned char** pl, SBufferInfo* bi) {
    u32 w = (u32)bi->UsrData.sSystemBuffer.iWidth, h = (u32)bi->UsrData.sSystemBuffer.iHeight;
    int sy = bi->UsrData.sSystemBuffer.iStride[0], sc = bi->UsrData.sSystemBuffer.iStride[1];
    if (!w || !h || w > 4096 || h > 4096) return;
    u32 need = w * h;
    if (g_fcap < need) {
        free(g_fy);
        free(g_fuv);
        g_fy = malloc(need);
        g_fuv = malloc(need / 2 + w);
        g_fcap = need;
    }
    for (u32 y = 0; y < h; y++) memcpy(g_fy + (u64)y * w, pl[0] + (u64)y * sy, w);
    for (u32 y = 0; y < h / 2; y++) {
        u8* d = g_fuv + (u64)y * w;
        const u8* u = pl[1] + (u64)y * sc;
        const u8* v = pl[2] + (u64)y * sc;
        for (u32 x = 0; x < w / 2; x++) {
            d[2 * x] = u[x];
            d[2 * x + 1] = v[x];
        }
    }
    g_fw = w;
    g_fh = h;
    g_vframes++;
}
#endif
void nvdec_execute(u32* r) {
    if (!dec_open()) return;
#ifndef SWR_NO_H264
    u64 ps_iova = (u64)r[0x404 / 4] << 8, bs_iova = (u64)r[0x408 / 4] << 8;
    u8* ps = iova_ptr(ps_iova, 0x300);
    if (!ps) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: NVDEC picture setup at iova %llx not mapped\n", (unsigned long long)ps_iova);
        }
        return;
    }
    u32 slen = rd32(ps, 0x48);
    if (!slen || slen > (16u << 20)) return;
    u8* bs = iova_ptr(bs_iova, slen);
    if (!bs) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n", (unsigned long long)bs_iova, slen);
        }
        return;
    }
    u8 hdr[512];
    u32 hn = build_headers(ps, hdr, sizeof hdr);
    static u8* buf;
    static u32 bcap;
    u32 need = hn + slen + 8;
    if (bcap < need) {
        buf = realloc(buf, need);
        bcap = need;
    }
    u32 o = 0;
    if (hn != g_last_hdr_n || memcmp(hdr, g_last_hdr, hn)) {
        memcpy(buf, hdr, hn);
        o = hn;
        memcpy(g_last_hdr, hdr, hn);
        g_last_hdr_n = hn;
    }
    if (bs[0] == 0 && bs[1] == 0 && (bs[2] == 1 || (bs[2] == 0 && bs[3] == 1))) {
        memcpy(buf + o, bs, slen);
        o += slen;
    } else {
        /* mp4-style 4 byte BE lengths -> start codes; otherwise just prepend one */
        u32 p = 0, ok = 1;
        while (p + 4 <= slen) {
            u32 L = ((u32)bs[p] << 24) | ((u32)bs[p + 1] << 16) | ((u32)bs[p + 2] << 8) | bs[p + 3];
            if (!L || p + 4 + L > slen || (bs[p + 4] & 0x80)) {
                ok = 0;
                break;
            }
            p += 4 + L;
        }
        if (ok && p == slen) {
            static u8* t;
            static u32 tc;
            if (tc < slen + 64) {
                t = realloc(t, slen + 64);
                tc = slen + 64;
            }
            (void)t;
            if (bcap < o + slen + slen / 4 + 16) {
                bcap = o + slen + slen / 4 + 16;
                buf = realloc(buf, bcap);
            }
            for (p = 0; p < slen;) {
                u32 L = ((u32)bs[p] << 24) | ((u32)bs[p + 1] << 16) | ((u32)bs[p + 2] << 8) | bs[p + 3];
                buf[o++] = 0;
                buf[o++] = 0;
                buf[o++] = 0;
                buf[o++] = 1;
                memcpy(buf + o, bs + p + 4, L);
                o += L;
                p += 4 + L;
            }
        } else {
            buf[o++] = 0;
            buf[o++] = 0;
            buf[o++] = 1;
            memcpy(buf + o, bs, slen);
            o += slen;
        }
    }
    unsigned char* pl[3] = {0, 0, 0};
    SBufferInfo bi;
    memset(&bi, 0, sizeof bi);
    DECODING_STATE st = (*g_dec)->DecodeFrameNoDelay(g_dec, buf, (int)o, pl, &bi);
    if (bi.iBufferStatus == 1 && pl[0]) keep_frame(pl, &bi);
    /* nvdec_status_s at method 0x424. players check it, an all-zero status means the frame failed */
    {
        u64 st_iova = (u64)r[0x424 / 4] << 8;
        u8* sp = st_iova ? iova_ptr(st_iova, 16) : NULL;
        if (sp) {
            u32 mbs = rd32(ps, 0x64) * rd32(ps, 0x68);
            u32 v[4] = {st ? 0 : mbs, st ? mbs : 0, 100000, st ? 1 : 0};
            memcpy(sp, v, 16);
        }
    }
    {
        static u32 njobs;
        njobs++;
        if (njobs % 120 == 0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n", njobs, g_vframes);
    }
    {
        static int said;
        if (said < 3 || (st && said < 8)) {
            said++;
            LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder state %x, picture %s (%ux%u)\n", rd32(ps, 0x64),
                rd32(ps, 0x68), slen, (unsigned)st, bi.iBufferStatus == 1 ? "out" : "pending", g_fw, g_fh);
        }
    }
#endif
}
