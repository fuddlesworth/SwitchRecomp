/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the NVDEC files share: the per-channel decoder, the picture target, and the frontend seam */
#ifndef SNX_NVDEC_INT_H
#define SNX_NVDEC_INT_H
#include "nx/gpu/video/host1x_int.h"
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include "nx/gpu/video/vp9_head.h"

/* ------------------------------------------------------------ a decoder per channel */
/* NVDEC writes each picture into the surface its job names as it decodes it, in decode order, and the game shows the
   surfaces in display order itself. FFmpeg hands pictures back in display order instead, some jobs later, and not at
   all for one it judges out of order: so the picture is taken from the decoder's own buffer for it (get_buffer2),
   once the job's packet is decoded, and FFmpeg's output is drained and dropped */
typedef struct {
    u64 luma, chroma;     /* iovas */
    u32 pitch_y, pitch_c; /* the planes' widths in bytes */
    u32 bh;               /* GOBs per block, log2 */
    u32 w, h;             /* the picture in pixels */
} PicOut;
/* a VP9 job's decode inputs, kept whole while it waits a job: a picture's refresh flags and its visibility
   only appear in the next job's surfaces, so the pieces the guest owns are copied in. bs is the channel's
   own copy while held, the guest's for a job decoded on arrival */
typedef struct {
    u8 ps[0x100];
    u8 prob[0xEA0];
    u8* bs;
    u32 len;
    u64 off[4];       /* Last, Golden, AltRef, Current luma iovas */
    u64 luma, chroma; /* the output surface */
    u32 pitch_y, pitch_c, w, h;
} Vp9Job;
struct NvdecCtx {
    u32 app; /* the codec the decoder was opened for (SET_APPLICATION_ID): 3 H.264, 9 VP9 */
    AVCodecContext* dec;
    AVPacket* pkt;
    AVFrame* frame; /* FFmpeg's output, dropped */
    AVFrame* cur;   /* a reference to the buffer of the picture the decoder started last */
    int cur_new;    /* started during this job */
    AVBufferRef* hw_dev; /* the Vulkan device the decoder's pictures come from, made when the machine has one */
    AVFrame* cpu;        /* a hardware picture downloaded for writing */
    int hw_try;          /* the decoder was opened on the Vulkan device: its pictures should come back hardware */
    int hw_soft;         /* they did not: FFmpeg put the channel on the CPU after a failed hardware start */
    /* SPS + PPS as last sent: full scaling lists with emulation prevention bytes come to about 800 */
    u8 last_hdr[1024];
    u32 last_hdr_n;
    Vp9HeadCtx vp9;    /* per-channel VP9 header state */
    AVFrame* vp9_prev; /* last good VP9 picture, shown again when a job fails */
    Vp9Job pend;       /* the held job, waiting for the next job's surfaces */
    int pend_have;
    int fail; /* the decoder could not be opened */
};

/* ------------------------------------------------------------ H.264 frontend (nvdec_h264.c) */
/* SPS/PPS rebuilt from nvdec_h264_pic_s. out gets the two NAL units, the count out, 0 when they don't fit in cap */
u32 nvdec_h264_build_headers(const u8* ps, u8* out, u32 cap);
/* the job's packet: the headers when they changed, then the bitstream with its length prefixes rewritten as start
   codes. Written into pkt, which owns the storage afterwards. 0, or the FFmpeg error when the bitstream isn't
   H.264 at all */
int nvdec_h264_packet(AVPacket* pkt, const u8* hdr, u32 hn, const u8* bs, u32 slen);

/* the picture into its surface as NVDEC writes it (nvdec.c) */
int write_pic(const AVFrame* f, const PicOut* t);
/* the picture as the CPU sees it: a hardware one is downloaded into the channel's own frame first, a software
   one comes back as is. NULL when the download fails: the caller fails the job (nvdec.c) */
const AVFrame* cpu_pic(struct NvdecCtx* d, const AVFrame* f);
/* nvdec_status_s, its iova in register 0x424 (nvdec.c) */
void put_status(const u32* r, u32 mbs, int failed);
/* send the job's packet (d->pkt) and drain FFmpeg's output; cur_new names the picture this job started (nvdec.c) */
int submit_packet(struct NvdecCtx* d);
/* one VP9 job: gather, compose, decode, write, status (nvdec_vp9.c) */
void nvdec_vp9_job(struct NvdecCtx* d, const u32* r);
/* let go of the held VP9 job and its copies (nvdec_vp9.c) */
void nvdec_vp9_pend_drop(struct NvdecCtx* d);

/* ------------------------------------------------------------ VP9 frontend (nvdec_vp9.c) */
/* surface luma iovas for Last, Golden, AltRef, Current: registers 0x430/4+idx hold page numbers */
static inline void nvdec_vp9_offsets(const u32* regs, u64* curr) {
    for (unsigned i = 0; i < 4; i++) curr[i] = (u64)regs[0x430 / 4 + i] << 8;
}

/* ------------------------------------------------------------ job recorder (nvdec_rec.c) */
/* one recorded blob: written as <name>.bin next to regs.bin and meta.txt. File layout: job-%05u/
   holds meta.txt (lines "app 3", "regs 4096", then "<name> <len>" per blob), regs.bin (1024 words
   little-endian), and one .bin per blob with data */
typedef struct {
    const char* name;
    const u8* data;
    u32 len;
} NvdecRecBlob;
/* 0 = written. Nonzero only skips the record, the job must proceed: NULL blob data is skipped,
   never dereferenced */
int nvdec_rec_job(const char* dir, unsigned n, u32 app, const u32* regs, const NvdecRecBlob* blobs, unsigned nblobs);
#endif
