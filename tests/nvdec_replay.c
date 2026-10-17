/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVDEC frontends. The synthetic checks run anywhere: the inputs are drawn up
   here and the bytes the builders compose from them are pinned. The game
   captures are local files (NVDEC_CAPTURES, tests/data/nvdec by default): the
   replay half needs them and reports CTest's skip, 77, without them. */
#include "nx/gpu/video/nvdec_int.h"
#include "nx/gpu/video/vp9_head.h"
#include <libavcodec/avcodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

#ifndef NVDEC_CAPTURES
#define NVDEC_CAPTURES "."
#endif

/* one recorded job, loaded from a capture directory */
typedef struct {
    u32 app;
    u32 regs[1024];
    u8* ps;
    u32 ps_n;
    u8* prob;
    u32 prob_n;
    u8* bs;
    u32 bs_n;
} CapJob;

static int read_whole(const char* path, u8** data, u32* len, u32 max, char* err, unsigned errcap) {
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) {
        snprintf(err, errcap, "open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || (unsigned long)n > max) {
        snprintf(err, errcap, "size %s", path);
        fclose(f);
        return -1;
    }
    *data = malloc(n > 0 ? (size_t)n : 1);
    if (!*data) {
        snprintf(err, errcap, "oom %s", path);
        fclose(f);
        return -1;
    }
    if (n > 0 && fread(*data, 1, (size_t)n, f) != (size_t)n) {
        snprintf(err, errcap, "read %s", path);
        free(*data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *len = (u32)n;
    return 0;
}

/* meta.txt lines: "app 3", "regs 4096", then "<name> <len>" per blob. Every file must match its
   length exactly; the first mismatch fails naming the file */
static int load_job(const char* dir, CapJob* j, char* err, unsigned errcap) {
    char path[4096], meta[4096], name[64];
    unsigned mlen, moff;
    u8* mdata = NULL;
    u32 msize = 0;
    unsigned u;
    memset(j, 0, sizeof *j);
    if (snprintf(path, sizeof path, "%s/meta.txt", dir) >= (int)sizeof path) {
        snprintf(err, errcap, "path %s", dir);
        return -1;
    }
    if (read_whole(path, &mdata, &msize, sizeof meta - 1, err, errcap)) return -1;
    memcpy(meta, mdata, msize);
    meta[msize] = 0;
    free(mdata);
    mdata = NULL;
    if (errcap) err[0] = 0;
    mlen = msize;
    moff = 0;
    while (moff < mlen) {
        char* line = meta + moff;
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        moff += (unsigned)strlen(line) + 1;
        if (!line[0]) continue;
        if (sscanf(line, "app %u", &j->app) == 1) continue;
        if (sscanf(line, "regs %u", &u) == 1 && u == 4096) {
            if (snprintf(path, sizeof path, "%s/regs.bin", dir) >= (int)sizeof path ||
                read_whole(path, &mdata, &msize, 4096, err, errcap) || msize != 4096) {
                if (mdata) free(mdata);
                if (!err[0]) snprintf(err, errcap, "size %s/regs.bin", dir);
                return -1;
            }
            memcpy(j->regs, mdata, 4096);
            free(mdata);
            mdata = NULL;
            continue;
        }
        if (sscanf(line, "%63s %u", name, &u) == 2) {
            u8** slot = NULL;
            u32 *slot_n = NULL, cap = 0;
            if (!strcmp(name, "ps")) {
                slot = &j->ps;
                slot_n = &j->ps_n;
                cap = 4096;
            } else if (!strcmp(name, "prob")) {
                slot = &j->prob;
                slot_n = &j->prob_n;
                cap = 8192;
            } else if (!strcmp(name, "bs")) {
                slot = &j->bs;
                slot_n = &j->bs_n;
                cap = 16u << 20;
            } else {
                snprintf(err, errcap, "unknown blob %s", name);
                return -1;
            }
            if (snprintf(path, sizeof path, "%s/%s.bin", dir, name) >= (int)sizeof path ||
                read_whole(path, slot, slot_n, cap, err, errcap) || *slot_n != u) {
                if (!err[0]) snprintf(err, errcap, "size %s", path);
                free(j->ps);
                free(j->prob);
                free(j->bs);
                j->ps = j->prob = j->bs = NULL;
                return -1;
            }
            continue;
        }
        snprintf(err, errcap, "line %s", line);
        return -1;
    }
    if (!j->app) {
        snprintf(err, errcap, "no app %s/meta.txt", dir);
        return -1;
    }
    return 0;
}

static void free_job(CapJob* j) {
    free(j->ps);
    free(j->prob);
    free(j->bs);
    j->ps = j->prob = j->bs = NULL;
}

/* the crc32 table, built once */
static const u32* crc_tab(void) {
    static u32 tab[256];
    static int init;
    if (!init) {
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            tab[i] = v;
        }
        init = 1;
    }
    return tab;
}

static u32 crc32_b(const u8* p, u32 n) {
    const u32* tab = crc_tab();
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* decoded-image fingerprints for the VP9 golden: crc32 of each output Y plane, in order */
static u32 vp9_frame_crc[32];
static unsigned vp9_ncrc;
static int vp9_fw, vp9_fh;
static u32 crc32_y(const AVFrame* f) {
    const u32* tab = crc_tab();
    u32 c = 0xFFFFFFFFu;
    for (int y = 0; y < f->height; y++) {
        const u8* row = f->data[0] + (size_t)y * f->linesize[0];
        for (int x = 0; x < f->width; x++) c = tab[(c ^ row[x]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

/* one composed packet through FFmpeg: 0 when it decodes without complaint */
static int feed_packet(AVCodecContext* vd, AVPacket* vpkt, AVFrame* vfr, const u8* data, u32 len) {
    int err;
    vpkt->data = (u8*)data;
    vpkt->size = (int)len;
    err = avcodec_send_packet(vd, vpkt);
    for (;;) {
        int r = avcodec_receive_frame(vd, vfr);
        if (r == 0) {
            if (vp9_ncrc < 32) {
                vp9_fw = vfr->width;
                vp9_fh = vfr->height;
                vp9_frame_crc[vp9_ncrc++] = crc32_y(vfr);
            }
            av_frame_unref(vfr);
            continue;
        }
        if (r != AVERROR(EAGAIN) && r != AVERROR_EOF && err >= 0) err = r;
        break;
    }
    return err;
}

int main(void) {
    /* synthetic picture setup: 80x45 macroblocks, everything else zero */
    static u8 ps[0x300];
    memset(ps, 0, sizeof ps);
    ps[0x64] = 80;
    ps[0x68] = 45;
    static u8 out[2048];
    u32 n = nvdec_h264_build_headers(ps, out, sizeof out);
    check(n > 0, "h264 headers built");
    check(n >= 5 && memcmp(out, "\0\0\0\1\x67", 5) == 0, "h264 SPS NAL first");
    int pps = 0;
    for (u32 i = 5; i + 5 <= n; i++) {
        if (memcmp(out + i, "\0\0\0\1\x68", 5) == 0) {
            pps = 1;
            break;
        }
    }
    check(pps, "h264 PPS NAL present");
    /* the synthetic capture's slice composes to one golden packet */
    {
        static u8 bs[64];
        static const u8 golden_pkt[] = {
            0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33, 0xac, 0xe1, 0x10, 0x14, 0x02, 0xc0, 0x80, 0x00,
            0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x61, 0x1f, 0xff, 0xf0, 0x8f, 0xff, 0xf8, 0x47, 0xff, 0xfc,
            0x23, 0xff, 0xfe, 0x11, 0xff, 0xff, 0x08, 0xff, 0xff, 0xc0, 0x00, 0x00, 0x01, 0x65, 0x01, 0x02,
            0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
            0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22,
            0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32,
            0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
        };
        static u8 hdr[1024];
        AVPacket* pkt = av_packet_alloc();
        u32 hn = 0;
        int same = 0;
        for (u32 i = 0; i < sizeof bs; i++) bs[i] = i ? (u8)i : 0x65;
        if (pkt) {
            hn = nvdec_h264_build_headers(ps, hdr, sizeof hdr);
            same = hn > 0 && nvdec_h264_packet(pkt, hdr, hn, bs, sizeof bs) == 0 &&
                   pkt->size == (int)sizeof golden_pkt && memcmp(pkt->data, golden_pkt, (size_t)pkt->size) == 0;
            if (!same) {
                fprintf(stderr, "FAIL: h264 packet differs (%d bytes): ", pkt->size);
                for (int i = 0; i < pkt->size; i++) fprintf(stderr, "%02x", pkt->data[i]);
                fprintf(stderr, "\n");
            }
            av_packet_free(&pkt);
        }
        check(same, "h264 golden packet");
    }
    /* synthetic VP9: a keyframe, a shown inter frame and a hidden one compose from drawn-up
       inputs, no decoder involved. The tiles are not real, so the pins are hashes of the bytes:
       a composer change anywhere moves them, captures or no captures. */
    {
        static u8 kps[0x100], ips[0x100], entropy[0xEA0], bs[64], vout[1 << 16];
        static const u64 cur[4] = {0x11400000, 0x11400000, 0x11400000, 0x11400000};
        static const u64 nxt[4] = {0x11500000, 0x11500000, 0x11500000, 0x11500000};
        Vp9HeadCtx c;
        for (u32 i = 0; i < sizeof bs; i++) bs[i] = (u8)i;
        kps[0x61] = 0x05; /* 1280 wide */
        kps[0x62] = 0xD0;
        kps[0x63] = 0x02; /* 720 tall */
        kps[0x68] = 1;    /* a keyframe */
        kps[0x7E] = 2;    /* tile columns */
        kps[0x7F] = 1;    /* tile rows */
        memcpy(ips, kps, sizeof ips);
        ips[0x68] = 0; /* an inter frame */
        memset(&c, 0, sizeof c);
        u32 k = vp9_compose_frame(&c, kps, entropy, bs, sizeof bs, vout, sizeof vout, cur, nxt, 1, 1);
        check(k > 4 && vout[1] == 0x49 && vout[2] == 0x83 && vout[3] == 0x42, "vp9 keyframe composes");
        u32 kh = crc32_b(vout, k);
        u32 i1 = vp9_compose_frame(&c, ips, entropy, bs, sizeof bs, vout, sizeof vout, nxt, nxt, 1, 1);
        check(i1 > 0, "vp9 inter frame composes");
        u32 ih = crc32_b(vout, i1);
        u32 i2 = vp9_compose_frame(&c, ips, entropy, bs, sizeof bs, vout, sizeof vout, nxt, nxt, 1, 0);
        check(i2 > 0, "vp9 hidden frame composes");
        u32 hh = crc32_b(vout, i2);
        char got[128];
        snprintf(got, sizeof got, "vp9 compose pins (key %08x inter %08x hidden %08x)", kh, ih, hh);
        check(kh == 0x3cf58f23u && ih == 0x80fc1e63u && hh == 0x4e7a8293u, got);
    }
    /* the recorded game captures: present only on a machine that has run the recorder
       (SNX_NVDEC_RECORD points it at a directory; tests/data/nvdec is the default here) */
    const char* caps = getenv("SNX_NVDEC_CAPTURES");
    int have_caps = 0;
    if (!caps || !*caps) caps = NVDEC_CAPTURES;
    {
        char probe[4096];
        FILE* f;
        if (snprintf(probe, sizeof probe, "%s/vp9-fe3h/job-00/meta.txt", caps) < (int)sizeof probe &&
            (f = fopen(probe, "rb")) != NULL) {
            fclose(f);
            have_caps = 1;
        }
    }
    if (have_caps) {
        /* a truncated capture is rejected, naming the short file */
        {
            CapJob j;
            char err[256];
            char dir[4096];
            snprintf(dir, sizeof dir, "%s/h264-trunc", caps);
            int r = load_job(dir, &j, err, sizeof err);
            check(r != 0 && strstr(err, "ps.bin") != NULL, "truncated ps rejected naming ps.bin");
            if (!r) free_job(&j);
        }
        /* FE3H VP9 captures compose and decode in order. Jobs 00-03 must come out clean (pins
           today's good frames); the whole 00-18 run must reach 19 packets with no decoder errors. */
        {
            typedef struct {
                u8 ps[0x100];
                u8 prob[0xEA0];
                u8* bs;
                u32 bs_n;
                u64 off[4];
                int key;
            } VJob;
            static const unsigned njobs = 20;
            static VJob jobs[20];
            static u8 pps_buf[0x100], pprob[0xEA0];
            unsigned nj = 0;
            for (unsigned j = 0; j < njobs; j++) {
                CapJob cj;
                char dir[512], err[256];
                snprintf(dir, sizeof dir, "%s/vp9-fe3h/job-%02u", caps, j);
                if (load_job(dir, &cj, err, sizeof err) != 0) {
                    fprintf(stderr, "FAIL: vp9 load job %u: %s\n", j, err);
                    failures++;
                    break;
                }
                if (cj.app != 9 || cj.ps_n != 0x100 || cj.prob_n != 0xEA0 || !cj.bs_n) {
                    fprintf(stderr, "FAIL: vp9 job %u short capture\n", j);
                    failures++;
                    free_job(&cj);
                    break;
                }
                memcpy(jobs[j].ps, cj.ps, 0x100);
                memcpy(jobs[j].prob, cj.prob, 0xEA0);
                jobs[j].bs = cj.bs;
                jobs[j].bs_n = cj.bs_n;
                cj.bs = NULL; /* stolen */
                nvdec_vp9_offsets(cj.regs, jobs[j].off);
                jobs[j].key = (rd32(cj.ps, 0x68) & 1) != 0;
                free_job(&cj);
                nj++;
            }
            /* offsets pin: job00 is the keyframe, all its refs are its own surface */
            if (nj > 0) {
                static const u64 want[4] = {0x11460000, 0x11460000, 0x11460000, 0x11460000};
                check(memcmp(jobs[0].off, want, sizeof want) == 0, "vp9 job00 offsets");
            }
            {
                const AVCodec* vc = avcodec_find_decoder(AV_CODEC_ID_VP9);
                AVCodecContext* vd = NULL;
                AVPacket* vpkt = NULL;
                AVFrame* vfr = NULL;
                int vd_ok = vc != NULL;
                if (vd_ok) {
                    vd = avcodec_alloc_context3(vc);
                    vd_ok = vd != NULL;
                    if (vd_ok) {
                        vd->thread_count = 1;
                        vd_ok = avcodec_open2(vd, vc, NULL) >= 0;
                    }
                    vpkt = av_packet_alloc();
                    vfr = av_frame_alloc();
                    vd_ok = vd_ok && vpkt && vfr;
                }
                check(vd_ok, "vp9 decoder opened");
                if (vd_ok && nj == njobs) {
                    Vp9HeadCtx vctx;
                    u8* pbs = NULL;
                    u32 plen = 0;
                    u64 poff[4] = {0, 0, 0, 0};
                    u8* vout2 = malloc(4u << 20);
                    int emitted = 0, first_err = -1;
                    memset(&vctx, 0, sizeof vctx);
                    if (!vout2) {
                        fprintf(stderr, "FAIL: vp9 oom\n");
                        failures++;
                    }
                    for (unsigned j = 0; j < nj && vout2; j++) {
                        u32 nbs = jobs[j].bs_n;
                        if (pbs) {
                            u32 o = vp9_compose_frame(&vctx, pps_buf, pprob, pbs, plen, vout2, 4u << 20, poff,
                                                      jobs[j].off, 1, (rd32(jobs[j].ps, 0x68) & 0x10) != 0);
                            if (!o || feed_packet(vd, vpkt, vfr, vout2, o) < 0) {
                                if (!o) fprintf(stderr, "FAIL: vp9 compose pending at job %u\n", j);
                                if (first_err < 0) first_err = emitted;
                            }
                            emitted++;
                            free(pbs);
                            pbs = NULL;
                        }
                        if (jobs[j].key) {
                            u64 zero[4] = {0, 0, 0, 0};
                            u32 o = vp9_compose_frame(&vctx, jobs[j].ps, jobs[j].prob, jobs[j].bs, nbs, vout2,
                                                      4u << 20, jobs[j].off, zero, 0, 1);
                            if (!o || feed_packet(vd, vpkt, vfr, vout2, o) < 0) {
                                if (!o) fprintf(stderr, "FAIL: vp9 compose job %u\n", j);
                                if (first_err < 0) first_err = emitted;
                            }
                            emitted++;
                        } else {
                            memcpy(pps_buf, jobs[j].ps, 0x100);
                            memcpy(pprob, jobs[j].prob, 0xEA0);
                            free(pbs);
                            pbs = malloc(nbs);
                            if (!pbs) {
                                fprintf(stderr, "FAIL: vp9 oom at job %u\n", j);
                                failures++;
                                break;
                            }
                            memcpy(pbs, jobs[j].bs, nbs);
                            plen = nbs;
                            memcpy(poff, jobs[j].off, sizeof poff);
                        }
                    }
                    free(pbs);
                    free(vout2);
                    check(emitted == 19, "vp9 19 packets emitted");
                    if (first_err >= 0) fprintf(stderr, "vp9 first bad packet: %d\n", first_err);
                    check(first_err < 0 || first_err > 3, "vp9 jobs 00-03 decode clean");
                    /* full run: zero decoder errors on all 19, 1280x720. Per-frame Y-plane
                       crc32s pin the pictures (no duplicated consecutive frames except real
                       repeats); the count pins the run length. */
                    check(first_err < 0, "vp9 jobs 00-18 decode clean");
                    check(vp9_fw == 1280 && vp9_fh == 720, "vp9 1280x720");
                    {
                        static const u32 golden_crc[] = {
                            0xd595ffbb, 0xd55e9fda, 0xdaf783cd, 0x9dcd25b2, 0x218072e7, 0x2da3d834, 0x9e2f11cf,
                            0xa7227ba6, 0x0d580780, 0x5e2a457d, 0x763cf6af, 0xa41386ad, 0xf7c7a146, 0x4fc16d01,
                            0x08587a9c, 0x2580ab69, 0x76e4c18f, 0xc1569b2b, 0xd9140a54,
                        };
                        unsigned same = vp9_ncrc == sizeof golden_crc / sizeof golden_crc[0];
                        for (unsigned i = 0; same && i < vp9_ncrc; i++) same = vp9_frame_crc[i] == golden_crc[i];
                        if (!same) {
                            fprintf(stderr, "FAIL: vp9 frames differ (%u images):", vp9_ncrc);
                            for (unsigned i = 0; i < vp9_ncrc; i++) fprintf(stderr, " %08x", vp9_frame_crc[i]);
                            fprintf(stderr, "\n");
                        }
                        check(same, "vp9 frame hashes");
                    }
                    /* hidden-frame path: a picture whose next setup has bit 4 clear was an alt-ref
                       (show_frame=0). FFmpeg must parse and decode it as invisible (no picture out).
                       The frame after a hidden one cannot be synthesized this way (an encoder builds
                       it against the hidden flag; flipping the bit makes it corrupt), so the chain
                       resumes only with real captures. */
                    {
                        Vp9HeadCtx hctx;
                        u8* hout = malloc(1u << 20);
                        unsigned before = vp9_ncrc;
                        u64 zero[4] = {0, 0, 0, 0};
                        int hok = hout != NULL;
                        memset(&hctx, 0, sizeof hctx);
                        if (hok) {
                            u32 o = vp9_compose_frame(&hctx, jobs[0].ps, jobs[0].prob, jobs[0].bs, jobs[0].bs_n,
                                                      hout, 1u << 20, jobs[0].off, zero, 0, 1);
                            hok = o > 0 && feed_packet(vd, vpkt, vfr, hout, o) >= 0;
                        }
                        if (hok) {
                            u32 o = vp9_compose_frame(&hctx, jobs[1].ps, jobs[1].prob, jobs[1].bs, jobs[1].bs_n,
                                                      hout, 1u << 20, jobs[1].off, jobs[2].off, 1, 0);
                            hok = o > 0 && feed_packet(vd, vpkt, vfr, hout, o) >= 0;
                        }
                        check(hok, "vp9 hidden frame decodes");
                        check(vp9_ncrc == before + 1, "vp9 hidden frame emits no picture");
                        if (vp9_ncrc > before) check(vp9_frame_crc[before] == 0xd595ffbb, "vp9 hidden key hash");
                        free(hout);
                    }
                }
                av_packet_free(&vpkt);
                av_frame_free(&vfr);
                avcodec_free_context(&vd);
            }
            for (unsigned j = 0; j < nj; j++) free(jobs[j].bs);
        }
    }
    /* byte-exact pin of the moved builder: any change here is a behavior change */
    static const u8 golden[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x33, 0xac, 0xe1, 0x10, 0x14, 0x02, 0xc0,
        0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x61, 0x1f, 0xff, 0xf0, 0x8f, 0xff,
        0xf8, 0x47, 0xff, 0xfc, 0x23, 0xff, 0xfe, 0x11, 0xff, 0xff, 0x08, 0xff, 0xff, 0xc0,
    };
    if (n != (u32)sizeof golden || memcmp(out, golden, n) != 0) {
        fprintf(stderr, "FAIL: h264 headers differ from golden (%u vs %u bytes)\ngot: ", n, (u32)sizeof golden);
        for (u32 i = 0; i < n; i++) fprintf(stderr, "%02x", out[i]);
        fprintf(stderr, "\n");
        failures++;
    }
    if (failures) return 1;
    printf("h264 headers: %u bytes, SPS+PPS present\n", n);
    if (!have_caps) {
        printf("no captures under %s: synthetic checks only\n", caps);
        return 77; /* CTest skip: the recorder fills the directory on the machine that has the game */
    }
    return 0;
}
