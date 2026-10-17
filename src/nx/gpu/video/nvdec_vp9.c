/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC VP9 frontend: whole frames are composed from picture info, entropy probabilities and tile data,
   then decoded. Inter frames wait one job so refresh flags come from the next job's surfaces and its
   "last frame shown" flag proves visibility, as the hardware sees it */
#include "nx/gpu/video/nvdec_int.h"
#include <errno.h>
#include <libavutil/error.h>

/* the decoded frame goes to the surface this job names; 0 when composed and decoded, an FFmpeg error otherwise */
static int vp9_decode_one(NvdecCtx* d, const u8* ps, const u8* prob, const u8* bs, u32 slen, const u64 coff[4],
                          const u64 noff[4], int has_next, int last_shown, u64 luma, u64 chroma, u32 py, u32 pc, u32 w,
                          u32 h, u32* mbs_out) {
    /* the composed packet is the frame body plus rebuilt headers: it can grow the body by a lot */
    int err = av_new_packet(d->pkt, (int)(slen + 16384));
    if (err < 0) return err;
    u32 o = vp9_compose_frame(&d->vp9, ps, prob, bs, slen, d->pkt->data, (u32)d->pkt->size, coff, noff, has_next, last_shown);
    if (!o) {
        av_packet_unref(d->pkt);
        return AVERROR_INVALIDDATA;
    }
    av_shrink_packet(d->pkt, (int)o);
    err = submit_packet(d);
    av_packet_unref(d->pkt);
    if (err >= 0 && d->cur_new && d->cur->buf[0] && d->cur->width > 0) {
        u32 wmb = ((u32)d->cur->width + 15) / 16, hmb = ((u32)d->cur->height + 15) / 16;
        if (mbs_out) *mbs_out = wmb * hmb;
        /* remember the good picture to show again when a later job fails */
        if (!d->vp9_prev) d->vp9_prev = av_frame_alloc();
        if (d->vp9_prev) {
            av_frame_unref(d->vp9_prev);
            if (av_frame_ref(d->vp9_prev, d->cur) < 0) av_frame_unref(d->vp9_prev);
        }
    }
    const AVFrame* show = NULL;
    if (d->cur_new && d->cur->buf[0] && err >= 0)
        show = d->cur;
    else if (d->vp9_prev && d->vp9_prev->buf[0])
        show = d->vp9_prev; /* failed: repeat the last good picture, still clean */
    else if (d->cur_new && d->cur->buf[0])
        show = d->cur; /* nothing good yet: partial is better than green */
    if (show) {
        PicOut pt = {luma, chroma, py, pc, 1, w, h};
        if (luma && chroma) write_pic(show, &pt);
        if (show == d->vp9_prev) err = 0; /* concealed: report success */
    }
    return err;
}

void nvdec_vp9_job(NvdecCtx* d, const u32* r) {
    u64 ps_iova = (u64)r[0x404 / 4] << 8;
    u8* ps = iova_ptr(ps_iova, 0x100);
    if (!ps) {
        LOG_LIMIT(4, "video: NVDEC VP9 picture setup at iova %llx not mapped\n", (unsigned long long)ps_iova);
        put_status(r, 0, 1);
        return;
    }
    /* VP9 PictureInfo: bitstream size at +0x30, current frame dims at +0x60 (w, h, luma pitch,
       chroma pitch as u16) */
    u32 slen = rd32(ps, 0x30);
    u32 vw = (u32)(ps[0x60] | (ps[0x61] << 8)), vh = (u32)(ps[0x62] | (ps[0x63] << 8));
    u32 wmb = 0, hmb = 0, mbs = 0;
    if (vw && vh && vw < 4096 && vh < 4096) {
        wmb = (vw + 15) / 16;
        hmb = (vh + 15) / 16;
        mbs = wmb * hmb;
    }
    if (slen > (16u << 20)) {
        LOG_LIMIT(4, "video: NVDEC VP9 bitstream of %u bytes skipped\n", slen);
        put_status(r, mbs, 1);
        return;
    }
    u8* bs = slen ? iova_ptr((u64)r[0x408 / 4] << 8, slen) : NULL;
    if (slen && !bs) {
        LOG_LIMIT(4, "video: NVDEC VP9 bitstream (%u bytes) not mapped\n", slen);
        put_status(r, mbs, 1);
        return;
    }
    u64 prob_iova = (u64)r[0x5C0 / 4] << 8;
    u8* prob = iova_ptr(prob_iova, 0xEA0);
    if (!prob) {
        LOG_LIMIT(4, "video: NVDEC VP9 probabilities at iova %llx not mapped\n", (unsigned long long)prob_iova);
        put_status(r, mbs, 1);
        return;
    }
    u64 coff[4];
    nvdec_vp9_offsets(r, coff);
    u64 luma = coff[3], chroma = (u64)r[0x474 / 4 + 3] << 8;
    u32 py = (u32)(ps[0x64] | (ps[0x65] << 8)), pc = (u32)(ps[0x66] | (ps[0x67] << 8));
    if (!py) py = wmb * 16;
    if (!pc) pc = wmb * 16;
    u32 w = wmb * 16, h = hmb * 16;
    int is_key = (rd32(ps, 0x68) & 1) != 0;
    int last_shown = (rd32(ps, 0x68) & 0x10) != 0; /* bit 4: the previous job's picture was shown */
    if (is_key) {
        /* flush pending first so its refs see this job's surfaces */
        if (d->pend_have) {
            u32 pmbs = d->pend_mbs;
            vp9_decode_one(d, d->pend_ps, d->pend_prob, d->pend_bs, d->pend_len, d->pend_off, coff, 1, last_shown,
                           d->pend_luma, d->pend_chroma, d->pend_py, d->pend_pc, d->pend_w, d->pend_h, &pmbs);
            free(d->pend_bs);
            d->pend_bs = NULL;
            d->pend_have = 0;
        }
        u32 m2 = mbs;
        int err = vp9_decode_one(d, ps, prob, bs, slen, coff, NULL, 0, 1, luma, chroma, py, pc, w, h, &m2);
        put_status(r, m2, err < 0);
        {
            static int said;
            if (said < 3 || (err < 0 && said < 8)) {
                said++;
                char es[AV_ERROR_MAX_STRING_SIZE] = "ok";
                if (err < 0) av_strerror(err, es, sizeof es);
                LOG("video: NVDEC VP9 job: %ux%u MBs, %u bytes, decoder %s\n", wmb, hmb, slen, es);
            }
        }
        return;
    }
    /* inter frame: decode the pending one against this job, then hold this job */
    if (d->pend_have) {
        u32 pmbs = d->pend_mbs;
        vp9_decode_one(d, d->pend_ps, d->pend_prob, d->pend_bs, d->pend_len, d->pend_off, coff, 1, last_shown,
                       d->pend_luma, d->pend_chroma, d->pend_py, d->pend_pc, d->pend_w, d->pend_h, &pmbs);
        free(d->pend_bs);
        d->pend_bs = NULL;
        d->pend_have = 0;
    }
    u8* pb = malloc(slen ? slen : 1);
    if (!pb) {
        put_status(r, mbs, 1);
        return;
    }
    memcpy(d->pend_ps, ps, 0x100);
    memcpy(d->pend_prob, prob, 0xEA0);
    if (slen) memcpy(pb, bs, slen);
    free(d->pend_bs);
    d->pend_bs = pb;
    d->pend_len = slen;
    for (int i = 0; i < 4; i++) d->pend_off[i] = coff[i];
    d->pend_luma = luma;
    d->pend_chroma = chroma;
    d->pend_py = py;
    d->pend_pc = pc;
    d->pend_w = w;
    d->pend_h = h;
    d->pend_mbs = mbs;
    d->pend_have = 1;
    put_status(r, mbs, 0);
}
