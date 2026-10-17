/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NVDEC: one FFmpeg decoder per channel, fed by a per-codec frontend. Each picture goes into the output
   surface its job names, where VIC reads it */
#include "nx/gpu/video/nvdec_int.h"
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>

static u32 g_vframes; /* pictures written, all channels: only for the log */
NvdecCtx* nvdec_new(void) { return calloc(1, sizeof(NvdecCtx)); }
/* the decoder and every picture it holds, or the channel's: the game closed the stream */
static void dec_teardown(NvdecCtx* d) {
    avcodec_free_context(&d->dec);
    av_packet_free(&d->pkt);
    av_frame_free(&d->frame);
    av_frame_free(&d->cur);
    av_frame_free(&d->cpu);
}
void nvdec_free(NvdecCtx* d) {
    if (!d) return;
    dec_teardown(d);
    av_buffer_unref(&d->hw_dev);
    av_frame_free(&d->vp9_prev);
    nvdec_vp9_pend_drop(d);
    free(d);
}
/* FFmpeg's messages go to the game log at warning level and above, a limited number: by default they go to stderr
   at info level, outside the log, without end */
static void ff_log(void* avcl, int level, const char* fmt, va_list vl) {
    static int n, pfx = 1;
    if (level > av_log_get_level() || __atomic_load_n(&n, __ATOMIC_RELAXED) > 20) return;
    int k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
    if (k > 20) return;
    if (k == 20) {
        LOG("FFmpeg: more messages not shown\n");
        return;
    }
    char line[512];
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &pfx);
    LOG("FFmpeg: %s%s", line, line[0] && line[strlen(line) - 1] == '\n' ? "" : "\n");
}
void ffmpeg_log_init(void) {
    av_log_set_level(AV_LOG_WARNING);
    av_log_set_callback(ff_log);
}
/* the decoder starts a picture (the last one it starts in a job is the job's own: missing reference frames it makes up
   come before it): keep a reference to its buffer, which the decoder fills during the job */
static int get_buf(AVCodecContext* c, AVFrame* f, int flags) {
    NvdecCtx* d = c->opaque;
    int r = avcodec_default_get_buffer2(c, f, flags);
    if (r >= 0) {
        av_frame_unref(d->cur);
        r = av_frame_ref(d->cur, f);
        d->cur_new = r >= 0;
    }
    return r;
}
/* what avcodec_open2 chooses from: Vulkan when the decoder offers it, the first software format otherwise. Set only
   on the hardware attempt: the CPU one uses FFmpeg's default */
static enum AVPixelFormat get_format(AVCodecContext* c, const enum AVPixelFormat* fmts) {
    (void)c;
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VULKAN) return *p;
    return fmts[0];
}
/* the picture as the CPU sees it: a hardware one is downloaded into d->cpu first, a software one comes back as is.
   NULL when the download fails, and the caller fails the job as it does for a decode error */
const AVFrame* cpu_pic(NvdecCtx* d, const AVFrame* f) {
    if (f->format != AV_PIX_FMT_VULKAN) {
        /* opened on the Vulkan device and the picture is still software: FFmpeg offered the stream to the
           driver, it would not take it, and FFmpeg itself put the channel on the CPU (see ff_get_format) */
        if (d->hw_try && !d->hw_soft) {
            d->hw_soft = 1;
            LOG("video: the Vulkan driver would not decode %s, FFmpeg keeps the channel on the CPU\n",
                d->app == 3 ? "H.264" : "VP9");
        }
        return f;
    }
    if (!d->cpu) d->cpu = av_frame_alloc();
    if (!d->cpu) return NULL;
    av_frame_unref(d->cpu);
    if (av_hwframe_transfer_data(d->cpu, f, 0) < 0) {
        LOG_LIMIT(1, "video: the decoded picture could not be read back from the GPU\n");
        return NULL;
    }
    LOG_LIMIT(1, "video: NVDEC pictures are decoded on the GPU (Vulkan)\n");
    return d->cpu;
}
/* one attempt at the channel's decoder: with the Vulkan device on the first try, without on the second. 0 leaves the
   half-made decoder behind for the caller's teardown */
static int open_dec(NvdecCtx* d, const AVCodec* codec, int hw) {
    d->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    d->pkt = av_packet_alloc();
    d->frame = av_frame_alloc();
    d->cur = av_frame_alloc();
    if (!d->dec || !d->pkt || !d->frame || !d->cur) return 0;
    /* one thread: the picture is complete when avcodec_send_packet returns, as NVDEC's is when the job ends
       (frame threading would still be decoding it); a hardware decoder runs on the GPU anyway */
    d->dec->thread_count = 1;
    d->dec->opaque = d;
    d->dec->get_buffer2 = get_buf;
    if (hw) {
        /* the machine's Vulkan device, made once per channel and reused by its later decoders */
        if (!d->hw_dev && av_hwdevice_ctx_create(&d->hw_dev, AV_HWDEVICE_TYPE_VULKAN, NULL, NULL, 0) < 0) return 0;
        d->dec->hw_device_ctx = av_buffer_ref(d->hw_dev);
        if (!d->dec->hw_device_ctx) return 0;
        d->dec->get_format = get_format;
    }
    return avcodec_open2(d->dec, codec, NULL) >= 0;
}
/* the decoder for the job's codec: SET_APPLICATION_ID (register 0x200), which the game's TVMR library writes before
   every job, is 3 for H.264 and 9 for VP9 (VP8 is 5, HEVC 7). A channel that changes codec gets a new decoder */
static int dec_open(NvdecCtx* d, u32 app) {
    if (d->dec && d->app == app) return 1;
    if (d->fail) return 0;
    if (app != 3 && app != 9) {
        LOG_LIMIT(4, "video: NVDEC job for codec %u skipped, only H.264 (3) and VP9 (9) are decoded\n", app);
        return 0;
    }
    /* the decoder, and the state that belongs to it, go first */
    dec_teardown(d);
    if (d->vp9_prev) av_frame_unref(d->vp9_prev);
    vp9_head_reset(&d->vp9);
    nvdec_vp9_pend_drop(d);
    d->last_hdr_n = 0;
    ffmpeg_log_init();
    const AVCodec* codec = avcodec_find_decoder(app == 3 ? AV_CODEC_ID_H264 : AV_CODEC_ID_VP9);
    /* The jobs go to the machine's GPU where its Vulkan driver can decode the codec, and to the CPU otherwise.
       SNX_NVDEC_CPU=1 keeps them on the CPU whatever the machine has: a capture replays the same everywhere then */
    const char* cpu = getenv("SNX_NVDEC_CPU");
    int hw = !(cpu && *cpu && *cpu != '0');
    int opened = hw && open_dec(d, codec, 1);
    if (hw && !opened) {
        dec_teardown(d);
        if (d->hw_dev)
            LOG_LIMIT(1, "video: Vulkan does not decode %s here, the CPU does it\n", app == 3 ? "H.264" : "VP9");
        else
            LOG_LIMIT(1, "video: no Vulkan device for the NVDEC jobs, the CPU decodes them\n");
        hw = 0;
    }
    if (!opened) opened = open_dec(d, codec, 0);
    if (!opened) {
        dec_teardown(d);
        d->fail = 1;
        LOG_LIMIT(1, "video: the FFmpeg %s decoder could not be opened\n", app == 3 ? "H.264" : "VP9");
        return 0;
    }
    d->app = app;
    /* the driver may still refuse the stream at its first frame (a picture below its minimum size for one):
       FFmpeg retries get_format without Vulkan then, and cpu_pic says so when it sees the software picture */
    d->hw_try = hw;
    d->hw_soft = 0;
    LOG_LIMIT(1, "video: %s decoder ready (FFmpeg %s, %s)\n", app == 3 ? "H.264" : "VP9", av_version_info(),
              hw ? "NVDEC jobs are sent to the GPU (Vulkan)" : "NVDEC jobs are decoded on the CPU");
    return 1;
}
/* EAGAIN is the normal end of a drain, not a decode error. EOF is unexpected: these per-channel decoders are
   never flushed with a NULL packet between jobs, because that would discard reference pictures. */
static int drain_frames(NvdecCtx* d) {
    int err;
    while ((err = avcodec_receive_frame(d->dec, d->frame)) >= 0) av_frame_unref(d->frame);
    return err == AVERROR(EAGAIN) ? 0 : err;
}
int submit_packet(NvdecCtx* d) {
    d->cur_new = 0;
    int err = avcodec_send_packet(d->dec, d->pkt);
    if (err == AVERROR(EAGAIN)) {
        /* The packet was NOT consumed. Retire the previous output, then retry the same owned packet once.
           Any get_buffer2 callbacks while draining belong to that older job, not this one's surface. */
        err = drain_frames(d);
        if (d->cur_new) av_frame_unref(d->cur);
        d->cur_new = 0;
        if (err < 0) return err;
        err = avcodec_send_packet(d->dec, d->pkt);
    }
    if (err < 0) return err;
    return drain_frames(d);
}
static void fail_job(NvdecCtx* d, const u32* r, u32 mbs) {
    /* A failed first field must not let a later second-field job publish a stale picture. Resend the parameter
       sets too: constructing/submitting them is not proof that the decoder accepted them. */
    if (d->cur) av_frame_unref(d->cur);
    d->cur_new = 0;
    d->last_hdr_n = 0;
    if (d->pkt) av_packet_unref(d->pkt);
    put_status(r, mbs, 1);
}
/* bytes of a block linear plane `pitch` bytes wide and h rows high: whole GOBs (64 bytes by 8 rows), in blocks of
   8 << bh rows */
static u64 bl_size(u32 pitch, u32 h, u32 bh) {
    u32 gy = 8u << bh;
    return (u64)((pitch + 63) & ~63u) * ((h + gy - 1) / gy * gy);
}
/* one row of a plane, n bytes from s: bl_offset(x, y) is bl_offset(x, 0) + bl_offset(0, y), so the row's 16-byte
   runs land at base plus the plane's column table, a whole run in one 16-byte move */
static void row_out(u8* p, const u64* col, u64 base, const u8* s, u32 n) {
    u32 k = 0;
    for (; (k + 1) * 16 <= n; k++) memcpy(p + base + col[k], s + k * 16, 16);
    if (k * 16 < n) memcpy(p + base + col[k], s + k * 16, n - k * 16);
}
/* The picture into its surface as NVDEC writes it: a luma plane and one interleaved chroma plane (NV12), from
   FFmpeg's planar 4:2:0 or from the hardware path's own NV12. 1 when written */
int write_pic(const AVFrame* f, const PicOut* t) {
    u32 w = (u32)f->width, h = (u32)f->height;
    int sy = f->linesize[0], sc = f->linesize[1], sv = f->linesize[2];
    const u8* const* pl = (const u8* const*)f->data;
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P && f->format != AV_PIX_FMT_NV12) {
        LOG_LIMIT(1, "video: the decoder gave pixel format %d, not 4:2:0, which NVDEC can't write\n", f->format);
        return 0;
    }
    /* the surface bounds what is written: the picture setup's size and the planes' widths */
    if (w > t->w) w = t->w;
    if (h > t->h) h = t->h;
    if (w > t->pitch_y) w = t->pitch_y;
    if ((w + 1) / 2 * 2 > t->pitch_c) w = t->pitch_c & ~1u;
    if (w > 4096) w = 4096;
    u32 ch = (t->h + 1) / 2;
    u8* py = iova_ptr(t->luma, bl_size(t->pitch_y, t->h, t->bh));
    u8* pc = iova_ptr(t->chroma, bl_size(t->pitch_c, ch, t->bh));
    if (!py || !pc) {
        LOG_LIMIT(4, "video: NVDEC output surface (luma iova %llx, chroma %llx) not mapped, picture dropped\n",
                  (unsigned long long)t->luma, (unsigned long long)t->chroma);
        return 0;
    }
    /* 4:2:0 chroma covers pixel pairs: an odd width or height has a last pair, or row, of one pixel */
    u32 cw = (w + 1) / 2 * 2;
    /* a column's offset depends on the block height alone: the two planes' rows share one table */
    u64 col[256]; /* a row's runs: 4096 bytes at most */
    for (u32 k = 0; k * 16 < cw; k++) col[k] = bl_offset(k * 16, 0, 0, t->pitch_y, t->h, t->bh, 0);
    for (u32 y = 0; y < h; y++)
        row_out(py, col, bl_offset(0, y, 0, t->pitch_y, t->h, t->bh, 0), pl[0] + (u64)y * sy, w);
    u8 row[4096];
    for (u32 y = 0; y < (h + 1) / 2; y++) {
        const u8* src;
        if (f->format == AV_PIX_FMT_NV12)
            src = pl[1] + (u64)y * sc; /* the hardware path hands its chroma interleaved already */
        else {
            const u8* u = pl[1] + (u64)y * sc;
            const u8* v = pl[2] + (u64)y * sv;
            for (u32 x = 0; x < cw / 2; x++) {
                row[2 * x] = u[x];
                row[2 * x + 1] = v[x];
            }
            src = row;
        }
        row_out(pc, col, bl_offset(0, y, 0, t->pitch_c, ch, t->bh, 0), src, cw);
    }
    g_vframes++;
    return 1;
}
/* nvdec_status_s, its iova in register 0x424. Players check it: an all-zero status means the frame failed */
void put_status(const u32* r, u32 mbs, int failed) {
    u64 st_iova = (u64)r[0x424 / 4] << 8;
    u8* sp = st_iova ? iova_ptr(st_iova, 16) : NULL;
    if (!sp) return;
    u32 v[4] = {failed ? 0 : mbs, failed ? mbs : 0, 100000, failed ? 1 : 0};
    memcpy(sp, v, 16);
}
void nvdec_execute(NvdecCtx* d, const u32* r) {
    u32 app = r[0x200 / 4];
    if (!dec_open(d, app)) {
        fail_job(d, r, 0);
        return;
    }
    /* SNX_NVDEC_RECORD points at a directory: every job lands there for the replay test. Codecs without
       a frontend yet record registers only; H.264 and VP9 add their picture setup and bitstream when mapped */
    {
        static unsigned rec_n;
        const char* recdir = getenv("SNX_NVDEC_RECORD");
        if (recdir) {
            NvdecRecBlob blobs[3];
            unsigned nblobs = 0;
            if (app == 3) {
                u8* ps = iova_ptr((u64)r[0x404 / 4] << 8, 0x300);
                u32 slen = ps ? rd32(ps, 0x48) : 0;
                u8* bs = slen ? iova_ptr((u64)r[0x408 / 4] << 8, slen) : NULL;
                if (ps) blobs[nblobs++] = (NvdecRecBlob){"ps", ps, 0x300};
                if (bs) blobs[nblobs++] = (NvdecRecBlob){"bs", bs, slen};
            } else if (app == 9) {
                u8* ps = iova_ptr((u64)r[0x404 / 4] << 8, 0x100);
                if (ps) {
                    u32 slen = rd32(ps, 0x30);
                    u8* prob = iova_ptr((u64)r[0x5C0 / 4] << 8, 0xEA0);
                    u8* bs = slen && slen <= (16u << 20) ? iova_ptr((u64)r[0x408 / 4] << 8, slen) : NULL;
                    blobs[nblobs++] = (NvdecRecBlob){"ps", ps, 0x100};
                    if (prob) blobs[nblobs++] = (NvdecRecBlob){"prob", prob, 0xEA0};
                    if (bs) blobs[nblobs++] = (NvdecRecBlob){"bs", bs, slen};
                }
            }
            if (nvdec_rec_job(recdir, rec_n, app, r, blobs, nblobs))
                LOG_LIMIT(4, "video: NVDEC job %u not recorded to %s\n", rec_n, recdir);
            rec_n++;
            if (rec_n % 120 == 0) LOG("video: %u NVDEC jobs recorded to %s\n", rec_n, recdir);
        }
    }
    {
        static u32 njobs;
        njobs++;
        if (njobs % 120 == 0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n", njobs, g_vframes);
    }
    if (app == 9) {
        nvdec_vp9_job(d, r);
        return;
    }
    u64 ps_iova = (u64)r[0x404 / 4] << 8, bs_iova = (u64)r[0x408 / 4] << 8;
    u8* ps = iova_ptr(ps_iova, 0x300);
    if (!ps) {
        LOG_LIMIT(4, "video: NVDEC picture setup at iova %llx not mapped\n", (unsigned long long)ps_iova);
        fail_job(d, r, 0);
        return;
    }
    u32 wmb = rd32(ps, 0x64), hmb = rd32(ps, 0x68), mbs = wmb * hmb;
    u32 slen = rd32(ps, 0x48);
    if (!slen || slen > (16u << 20)) {
        LOG_LIMIT(4, "video: NVDEC job with a %u byte bitstream skipped\n", slen);
        fail_job(d, r, mbs);
        return;
    }
    u8* bs = iova_ptr(bs_iova, slen);
    if (!bs) {
        LOG_LIMIT(4, "video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n", (unsigned long long)bs_iova, slen);
        fail_job(d, r, mbs);
        return;
    }
    u8 hdr[sizeof d->last_hdr];
    u32 hn = nvdec_h264_build_headers(ps, hdr, sizeof hdr);
    if (!hn) {
        LOG_LIMIT(1, "video: NVDEC SPS and PPS don't fit in %u bytes, job skipped\n", (unsigned)sizeof hdr);
        fail_job(d, r, mbs);
        return;
    }
    /* the rebuilt SPS and PPS ride along only when they change: the decoder keeps the last ones */
    u32 prefix = (hn != d->last_hdr_n || memcmp(hdr, d->last_hdr, hn)) ? hn : 0;
    int err = nvdec_h264_packet(d->pkt, hdr, prefix, bs, slen);
    if (err >= 0) err = submit_packet(d);
    av_packet_unref(d->pkt);
    if (err >= 0) {
        memcpy(d->last_hdr, hdr, hn);
        d->last_hdr_n = hn;
    }
    /* Where the picture goes: CurrPicIdx (bits 2..8 of +0xB4) picks one of the 17 surfaces, and the picture setup says
       how NVDEC lays it out. tileFormat (+0x6C bits 0..1) 0, the only one the game's TVMR library writes, is block
       linear with 2 << gob_height (bits 2..4) GOBs per block. pitch_luma and pitch_chroma (+0x8C, +0x90) are the
       planes' widths in bytes, and the frame offsets (+0x9C, +0xA8, in 256 bytes) are added to the surface's.
       A field pair is one picture: the second field's job (field_pic_flag and second_field, bits 5 and 7 of +0xB0)
       writes it again, complete */
    u32 idx = (rd32(ps, 0xB4) >> 2) & 0x7F, lay = rd32(ps, 0x6C), b0 = rd32(ps, 0xB0);
    PicOut pt = {0};
    if (idx < 17) {
        pt.luma = ((u64)r[0x430 / 4 + idx] + rd32(ps, 0x9C)) << 8;
        pt.chroma = ((u64)r[0x474 / 4 + idx] + rd32(ps, 0xA8)) << 8;
    }
    pt.pitch_y = rd32(ps, 0x8C);
    pt.pitch_c = rd32(ps, 0x90);
    pt.bh = ((lay >> 2) & 7) + 1;
    pt.w = wmb * 16;
    pt.h = hmb * 16;
    int wrote = 0;
    if (err >= 0 && (d->cur_new || (b0 & 0xA0) == 0xA0) && d->cur->buf[0]) {
        if (lay & 3)
            LOG_LIMIT(1, "video: NVDEC output tile format %u isn't handled (block linear, 0, is), picture dropped\n",
                      lay & 3);
        else if (idx >= 17 || !pt.luma || !pt.chroma)
            LOG_LIMIT(4, "video: NVDEC job names output surface %u, which has no address, picture dropped\n", idx);
        else {
            const AVFrame* pf = cpu_pic(d, d->cur);
            wrote = pf && write_pic(pf, &pt);
        }
    }
    if (err < 0 || !wrote)
        fail_job(d, r, mbs);
    else
        put_status(r, mbs, 0);
    {
        static int said;
        if (said < 3 || (err < 0 && said < 8)) {
            said++;
            char es[AV_ERROR_MAX_STRING_SIZE] = "ok";
            if (err < 0) av_strerror(err, es, sizeof es);
            LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder %s, picture %s surface %u (luma iova %llx, "
                "pitch %u, block height %u)\n",
                wmb, hmb, slen, es, wrote ? "written to" : "not written to", idx, (unsigned long long)pt.luma,
                pt.pitch_y, 1u << pt.bh);
        }
    }
}
