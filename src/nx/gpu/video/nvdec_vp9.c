/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC VP9 frontend: whole frames are composed from picture info, entropy probabilities and tile data,
   then decoded. Inter frames wait one job so refresh flags come from the next job's surfaces and its
   "last frame shown" flag proves visibility, as the hardware sees it */
#include "nx/gpu/video/nvdec_int.h"
#include <libavutil/error.h>

/* let the held job go: the channel no longer waits on it */
void nvdec_vp9_pend_drop(NvdecCtx* d) {
    free(d->pend.bs);
    d->pend.bs = NULL;
    d->pend_have = 0;
}

/* decode one job into the surface it names: next is the next job's surfaces, read when there is one,
   and last_shown is bit 4 of the next job's setup. 0, or an FFmpeg error; mbs_out, when given, gets
   the decoded frame's macroblock count */
static int vp9_decode_one(NvdecCtx* d, const Vp9Job* j, const u64 next[4], int has_next, int last_shown, u32* mbs_out) {
    /* the composed packet is the frame body plus rebuilt headers: it can grow the body by a lot */
    int err = av_new_packet(d->pkt, (int)(j->len + 16384));
    if (err < 0) return err;
    u32 o = vp9_compose_frame(&d->vp9, j->ps, j->prob, j->bs, j->len, d->pkt->data, (u32)d->pkt->size, j->off, next,
                              has_next, last_shown);
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
        PicOut pt = {j->luma, j->chroma, j->pitch_y, j->pitch_c, 1, j->w, j->h};
        if (j->luma && j->chroma) {
            const AVFrame* pf = cpu_pic(d, show);
            if (pf) write_pic(pf, &pt);
        }
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

    /* the job's pieces in one place, the tile data still the guest's */
    Vp9Job j = {.bs = bs, .len = slen, .luma = luma, .chroma = chroma,
                .pitch_y = py, .pitch_c = pc, .w = w, .h = h};
    memcpy(j.ps, ps, sizeof j.ps);
    memcpy(j.prob, prob, sizeof j.prob);
    memcpy(j.off, coff, sizeof j.off);

    /* the held job's turn: this job's surfaces say what it refreshed, and this job's bit 4 says
       whether the hardware showed it */
    if (d->pend_have) {
        vp9_decode_one(d, &d->pend, coff, 1, last_shown, NULL);
        nvdec_vp9_pend_drop(d);
    }
    if (is_key) {
        /* a keyframe decodes on arrival and is its own reference */
        u32 m = mbs;
        int err = vp9_decode_one(d, &j, j.off, 0, 1, &m);
        put_status(r, m, err < 0);
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
    /* hold the job: only the next job's surfaces can say what it refreshed, and whether it was shown */
    u8* pb = malloc(slen ? slen : 1);
    if (!pb) {
        put_status(r, mbs, 1);
        return;
    }
    if (slen) memcpy(pb, bs, slen);
    j.bs = pb;
    d->pend = j;
    d->pend_have = 1;
    put_status(r, mbs, 0);
}
