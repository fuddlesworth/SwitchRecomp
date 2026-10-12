/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC: H.264 decoding with FFmpeg's libavcodec. The game passes no SPS or PPS: they are rebuilt from the picture
   setup struct (nvdec_h264_pic_s, its iova in register 0x404) */
#include "video/host1x_int.h"
#ifndef SWR_NO_H264
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>

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
    if (cfi == 3) bw_u(&w, 0, 1); /* separate_colour_plane_flag */
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
    u32 sn = nal_out(out + o, cap - o, 0x67, rb, w.bytes);
    if (!sn) return 0;
    o += sn;
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
    u32 pn = nal_out(out + o, cap - o, 0x68, rb, w.bytes);
    if (!pn) return 0;
    return o + pn;
}
#endif

/* ------------------------------------------------------------ decoder and the last picture */
#ifndef SWR_NO_H264
static AVCodecContext* g_dec;
static AVPacket* g_pkt;
static AVFrame* g_frame;
/* SPS + PPS: full scaling lists with emulation prevention bytes come to about 800 */
static u8 g_last_hdr[1024];
static u32 g_last_hdr_n;
static u32 g_fcap;
#endif
static int g_dec_fail;
u8* g_fy;
u8* g_fuv;
u32 g_fw, g_fh;
u32 g_vframes;
#ifndef SWR_NO_H264
/* FFmpeg's messages go to the game log at warning level and above, a limited number: by default they go to stderr
   at info level, outside the log, without end */
static void ff_log(void* avcl, int level, const char* fmt, va_list vl) {
    static int n, pfx = 1;
    if (level > av_log_get_level() || __atomic_load_n(&n, __ATOMIC_RELAXED) > 20) return;
    int k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
    if (k > 20) return;
    if (k == 20) {
        LOG("video: FFmpeg: more messages not shown\n");
        return;
    }
    char line[512];
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &pfx);
    LOG("video: FFmpeg: %s%s", line, line[0] && line[strlen(line) - 1] == '\n' ? "" : "\n");
}
#endif
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
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ff_log);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    g_dec = codec ? avcodec_alloc_context3(codec) : NULL;
    g_pkt = av_packet_alloc();
    g_frame = av_frame_alloc();
    if (g_dec) {
        /* NVDEC returns each job's picture with the job: one thread keeps frame threading from holding pictures back
           (LOW_DELAY only turns frame threading off too). Reordering is still the decoder's: a stream with B frames,
           or POC steps that need reordering, gets its pictures a few jobs late */
        g_dec->thread_count = 1;
        g_dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    if (!g_dec || !g_pkt || !g_frame || avcodec_open2(g_dec, codec, NULL) < 0) {
        g_dec_fail = 1;
        avcodec_free_context(&g_dec);
        av_packet_free(&g_pkt);
        av_frame_free(&g_frame);
        LOG("video: the FFmpeg H.264 decoder could not be opened\n");
        return 0;
    }
    LOG("video: H.264 decoder ready (FFmpeg %s, NVDEC jobs are decoded on the CPU)\n", av_version_info());
    return 1;
#endif
}
#ifndef SWR_NO_H264
/* The picture as VIC reads it: a luma plane and one interleaved chroma plane (NV12), from FFmpeg's planar 4:2:0 */
static void keep_frame(const AVFrame* f) {
    u32 w = (u32)f->width, h = (u32)f->height;
    int sy = f->linesize[0], sc = f->linesize[1], sv = f->linesize[2];
    const u8* const* pl = (const u8* const*)f->data;
    if (!w || !h || w > 4096 || h > 4096) return;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        static int said;
        if (!said++) LOG("video: the decoder gave pixel format %d, not 4:2:0, which VIC can't take\n", f->format);
        return;
    }
    u32 need = w * h;
    if (g_fcap < need) {
        free(g_fy);
        free(g_fuv);
        g_fy = malloc(need);
        g_fuv = malloc(need / 2 + w);
        g_fcap = need;
        if (!g_fy || !g_fuv) {
            /* no picture: VIC sees a 0x0 one (g_fw, g_fh) until an allocation succeeds */
            free(g_fy);
            free(g_fuv);
            g_fy = g_fuv = NULL;
            g_fcap = g_fw = g_fh = 0;
            WARN("video: out of memory for a %ux%u picture\n", w, h);
            return;
        }
    }
    for (u32 y = 0; y < h; y++) memcpy(g_fy + (u64)y * w, pl[0] + (u64)y * sy, w);
    for (u32 y = 0; y < h / 2; y++) {
        u8* d = g_fuv + (u64)y * w;
        const u8* u = pl[1] + (u64)y * sc;
        const u8* v = pl[2] + (u64)y * sv;
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
/* nvdec_status_s, its iova in register 0x424. Players check it: an all-zero status means the frame failed */
static void put_status(const u32* r, u32 mbs, int failed) {
    u64 st_iova = (u64)r[0x424 / 4] << 8;
    u8* sp = st_iova ? iova_ptr(st_iova, 16) : NULL;
    if (!sp) return;
    u32 v[4] = {failed ? 0 : mbs, failed ? mbs : 0, 100000, failed ? 1 : 0};
    memcpy(sp, v, 16);
}
void nvdec_execute(u32* r) {
    if (!dec_open()) {
        put_status(r, 0, 1);
        return;
    }
#ifndef SWR_NO_H264
    /* SET_APPLICATION_ID (register 0x200) names the codec: 3 is H.264, which the game's TVMR library writes before
       every H.264 job (VP8 is 5, HEVC 7, VP9 9). Anything else fed to the H.264 decoder would only be garbage */
    u32 app = r[0x200 / 4];
    if (app != 3) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: NVDEC job for codec %u skipped, only H.264 (3) is decoded\n", app);
        }
        put_status(r, 0, 1);
        return;
    }
    u64 ps_iova = (u64)r[0x404 / 4] << 8, bs_iova = (u64)r[0x408 / 4] << 8;
    u8* ps = iova_ptr(ps_iova, 0x300);
    if (!ps) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: NVDEC picture setup at iova %llx not mapped\n", (unsigned long long)ps_iova);
        }
        put_status(r, 0, 1);
        return;
    }
    u32 mbs = rd32(ps, 0x64) * rd32(ps, 0x68);
    u32 slen = rd32(ps, 0x48);
    if (!slen || slen > (16u << 20)) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: NVDEC job with a %u byte bitstream skipped\n", slen);
        }
        put_status(r, mbs, 1);
        return;
    }
    u8* bs = iova_ptr(bs_iova, slen);
    if (!bs) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n", (unsigned long long)bs_iova, slen);
        }
        put_status(r, mbs, 1);
        return;
    }
    u8 hdr[sizeof g_last_hdr];
    u32 hn = build_headers(ps, hdr, sizeof hdr);
    if (!hn) {
        static int said;
        if (!said++) LOG("video: NVDEC SPS and PPS don't fit in %u bytes, job skipped\n", (unsigned)sizeof hdr);
        put_status(r, mbs, 1);
        return;
    }
    static u8* buf;
    static u32 bcap;
    /* the largest packet is the headers, a 3 byte start code and the bitstream: rewriting length prefixes as start
       codes keeps the size */
    u32 need = hn + slen + 8;
    if (bcap < need) {
        u8* nb = realloc(buf, need);
        if (!nb) {
            WARN("video: out of memory for a %u byte NVDEC packet\n", need);
            put_status(r, mbs, 1);
            return;
        }
        buf = nb;
        bcap = need;
    }
    u32 o = 0;
    if (hn != g_last_hdr_n || memcmp(hdr, g_last_hdr, hn)) {
        memcpy(buf, hdr, hn);
        o = hn;
        memcpy(g_last_hdr, hdr, hn);
        g_last_hdr_n = hn;
    }
    if (slen >= 4 && bs[0] == 0 && bs[1] == 0 && (bs[2] == 1 || (bs[2] == 0 && bs[3] == 1))) {
        memcpy(buf + o, bs, slen);
        o += slen;
    } else {
        /* mp4-style 4 byte BE lengths -> start codes; otherwise just prepend one */
        u32 p = 0, ok = 1;
        while (p + 4 <= slen) {
            u32 L = ((u32)bs[p] << 24) | ((u32)bs[p + 1] << 16) | ((u32)bs[p + 2] << 8) | bs[p + 3];
            if (!L || L > slen - p - 4 || (bs[p + 4] & 0x80)) {
                ok = 0;
                break;
            }
            p += 4 + L;
        }
        if (ok && p == slen) {
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
    /* err < 0 when the decoder rejected the job: the status the game reads says so */
    g_pkt->data = buf;
    g_pkt->size = (int)o;
    int err = avcodec_send_packet(g_dec, g_pkt);
    int out = 0;
    while (avcodec_receive_frame(g_dec, g_frame) == 0) {
        keep_frame(g_frame);
        av_frame_unref(g_frame);
        out = 1;
    }
    put_status(r, mbs, err < 0);
    {
        static u32 njobs;
        njobs++;
        if (njobs % 120 == 0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n", njobs, g_vframes);
    }
    {
        static int said;
        if (said < 3 || (err < 0 && said < 8)) {
            said++;
            char es[AV_ERROR_MAX_STRING_SIZE] = "ok";
            if (err < 0) av_strerror(err, es, sizeof es);
            LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder %s, picture %s (%ux%u)\n", rd32(ps, 0x64),
                rd32(ps, 0x68), slen, es, out ? "out" : "pending", g_fw, g_fh);
        }
    }
#endif
}
