/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* audio services (audren, audout, hwopus): answered in the real formats, with no sound produced. hwopus decodes the
   game's Opus packets with FFmpeg */
#include "hle/services_int.h"
#ifndef SWR_NO_H264
#include "video/host1x.h"
#include <libavcodec/avcodec.h>
#include <math.h>
#endif
/* audio: pretend, no output */

/* IAudioOut: nothing is played, but appended buffers come back at 48 kHz speed like on the console.
   Players time video against the released buffers (CRI movies): never releasing them stalls the video.
   Buffers are laid out on a play clock that runs only while the audout is started: a buffer is released once the
   clock passes its end, and then waits in rel[] for GetReleasedAudioOutBuffers */
typedef struct AudOut {
    struct AudOut* next; /* audio.outs */
    u64 tag[64], start[64], end[64], frames[64];
    u32 head, n;
    u64 rel[64];
    u32 rhead, rn;
    u64 last_end, played;
    u64 base, anchor; /* the play clock: base, plus the time since anchor while started */
    int started;
    u32 channels;
    float volume;
    KEvent* ev; /* RegisterBufferEvent */
    PMutex lk;
} AudOut;

static struct {
    KEvent* ev; /* the audio renderer's event, signaled every 5ms by audio_clock */
    /* the open IAudioOuts, which audio_clock looks at to signal their buffer events */
    AudOut* outs;
    PMutex outs_lk;
    int outs_init; /* 0, 1 while pm_init runs, 2 ready: the first audout can open before audio_clock runs */
} audio;
void audio_init(void) { audio.ev = owned_event("audio renderer"); }
static void outs_init(void) {
    int s = 0;
    if (__atomic_load_n(&audio.outs_init, __ATOMIC_ACQUIRE) == 2) return;
    if (__atomic_compare_exchange_n(&audio.outs_init, &s, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pm_init(&audio.outs_lk);
        __atomic_store_n(&audio.outs_init, 2, __ATOMIC_RELEASE);
    } else
        while (__atomic_load_n(&audio.outs_init, __ATOMIC_ACQUIRE) != 2) plat_yield();
}

/* the rest of audout's helpers take q->lk held */
static u64 audout_clock(AudOut* q, u64 now) { return q->started ? q->base + (now - q->anchor) : q->base; }
/* buffers the clock has passed move to rel[] */
static void audout_release(AudOut* q, u64 now) {
    u64 t = audout_clock(q, now);
    while (q->n && q->end[q->head] <= t) {
        q->rel[(q->rhead + q->rn) & 63] = q->tag[q->head];
        q->rn++;
        q->played += q->frames[q->head];
        q->head = (q->head + 1) & 63;
        q->n--;
    }
}

/* renderer event fires every 5ms (240 samples @ 48k) like HOS. Each IAudioOut's buffer event is signaled while it
   has released buffers the game hasn't taken: the SDK clears it after each wait */
void audio_clock(void* arg) {
    (void)arg;
    outs_init();
    u64 next = plat_ns();
    for (;;) {
        next += 5000000ull;
        u64 now = plat_ns();
        if (next > now)
            plat_sleep_ns((i64)(next - now));
        else if (now - next > 50000000ull)
            next = now;
        kevent_signal(audio.ev);
        now = plat_ns();
        pm_lock(&audio.outs_lk);
        for (AudOut* q = audio.outs; q; q = q->next) {
            pm_lock(&q->lk);
            audout_release(q, now);
            int any = q->rn != 0;
            pm_unlock(&q->lk);
            if (any) kevent_signal(q->ev);
        }
        pm_unlock(&audio.outs_lk);
    }
}

/* IAudioRenderer: u[0..2] sample rate, sample count and mix buffer count, u[3] state (0 started, 1 stopped),
   u[4] and u[5] audren.c's, u[6] the rendering time limit */
static int audren_obj(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: out32(c, (u32)o->u[0]); return 0;
        case 1: out32(c, (u32)o->u[1]); return 0;
        case 2: out32(c, (u32)o->u[2]); return 0;
        case 3: out32(c, (u32)o->u[3]); return 0;
        case 4:
        case 10: return audren_update(o, c);
        case 5: o->u[3] = 0; return 0;
        case 6: o->u[3] = 1; return 0;
        case 7: out_copy_h(c, ev_handle(audio.ev)); return 0;
        case 8: o->u[6] = in32(c, 0); return 0;   /* SetRenderingTimeLimit */
        case 9: out32(c, (u32)o->u[6]); return 0; /* GetRenderingTimeLimit */
        case 11: return 0;                        /* ExecuteAudioRendererRendering */
    }
    return stub_ok(o, c);
}
/* IAudioDevice: its system, input and output events in u[0..2], made with the object and kept by it. They start
   signaled, as on the console */
static void auddev_destroy(SvcObj* o) {
    for (int i = 0; i < 3; i++)
        if (o->u[i]) kobj_unref(&((KEvent*)(size_t)o->u[i])->o);
}
static int auddev_disp(SvcObj* o, IpcCtx* c) {
    static const char* nm = "AudioTvOutput";
    switch (c->cmd) {
        case 0:
        case 6: {
            u64 a, s;
            if (buf_out(c, 0, &a, &s) && a && s >= 0x100) {
                memset(GP(a), 0, 0x100);
                strcpy((char*)GP(a), nm);
                out32(c, 1);
            } else
                out32(c, 0);
            return 0;
        }
        case 2:
        case 8: out32(c, 0x3F800000); return 0;
        case 3:
        case 10:
        case 13: {
            u64 a, s;
            if (buf_out(c, 0, &a, &s) && a) {
                memset(GP(a), 0, s);
                strncpy((char*)GP(a), nm, s);
            }
            return 0;
        }
        case 4: out_copy_h(c, ev_handle((KEvent*)(size_t)o->u[0])); return 0;  /* QueryAudioDeviceSystemEvent */
        case 11: out_copy_h(c, ev_handle((KEvent*)(size_t)o->u[1])); return 0; /* QueryAudioDeviceInputEvent */
        case 12: out_copy_h(c, ev_handle((KEvent*)(size_t)o->u[2])); return 0; /* QueryAudioDeviceOutputEvent */
        case 5: out32(c, 2); return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* new_auddev(void) {
    static const char* const names[3] = {"audio device system", "audio device input", "audio device output"};
    SvcObj* d = svcobj_new("IAudioDevice", auddev_disp, 0);
    for (int i = 0; i < 3; i++) {
        KEvent* e = kevent_new(names[i]);
        kobj_ref(&e->o);
        kevent_signal(e);
        d->u[i] = (u64)(size_t)e;
    }
    d->destroy = auddev_destroy;
    return d;
}
static int audren_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:   /* OpenAudioRenderer */
        case 3: { /* OpenAudioRendererForManualExecution: the same parameters */
            SvcObj* r = svcobj_new("IAudioRenderer", audren_obj, 0);
            r->u[0] = in32(c, 0);
            r->u[1] = in32(c, 4);
            r->u[2] = in32(c, 8);
            r->u[3] = 1;
            r->u[6] = 100; /* the rendering time limit starts at 100% */
            out_obj(c, r);
            return 0;
        }
        case 1: {
            /* AudioRendererParameter: voice count at 0x10, sink count 0x14, effect count 0x18 */
            u64 sz = 0x4000;
            u32 voices = in32(c, 0x10), sinks = in32(c, 0x14), effects = in32(c, 0x18);
            sz += (u64)voices * 0x400 + sinks * 0x200 + effects * 0x800 + 0x40000;
            out64(c, sz);
            return 0;
        }
        case 2:
        case 4: out_obj(c, new_auddev()); return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_audren(void) { return svcobj_new("audren:u", audren_disp, 0); }

/* a device name into the game's out buffer, cut to fit and zero padded */
static int put_name(IpcCtx* c, const char* nm) {
    u64 a, s;
    if (!buf_out(c, 0, &a, &s) || !a || !s) return 0;
    u64 n = s < 0x100 ? s : 0x100;
    memset(GP(a), 0, n);
    memcpy(GP(a), nm, strlen(nm) < n ? strlen(nm) : n - 1);
    return 1;
}
static void audout_destroy(SvcObj* o) {
    AudOut* q = (AudOut*)o->data;
    pm_lock(&audio.outs_lk);
    for (AudOut** p = &audio.outs; *p; p = &(*p)->next)
        if (*p == q) {
            *p = q->next;
            break;
        }
    pm_unlock(&audio.outs_lk);
    kobj_unref(&q->ev->o);
}
static int audout_obj(SvcObj* o, IpcCtx* c) {
    AudOut* q = (AudOut*)o->data;
    switch (c->cmd) {
        case 0: { /* GetAudioOutState: 0 started, 1 stopped */
            pm_lock(&q->lk);
            int st = q->started;
            pm_unlock(&q->lk);
            out32(c, st ? 0 : 1);
            return 0;
        }
        case 1: /* Start: the play clock runs from now */
            pm_lock(&q->lk);
            if (!q->started) {
                q->anchor = plat_ns();
                q->started = 1;
            }
            pm_unlock(&q->lk);
            return 0;
        case 2: { /* Stop: the play clock stops where it is */
            u64 now = plat_ns();
            pm_lock(&q->lk);
            if (q->started) {
                audout_release(q, now);
                q->base = audout_clock(q, now);
                q->started = 0;
            }
            pm_unlock(&q->lk);
            return 0;
        }
        case 3:
        case 7: {
            u64 tag = in64(c, 0), a, sz, bytes = 0; /* AppendAudioOutBuffer(Auto): AudioOutBuffer.data_size at +0x18 */
            if (buf_in(c, 0, &a, &sz) && a && sz >= 0x20) memcpy(&bytes, GP(a + 0x18), 8);
            if (bytes > 0xFFFFFFFFull) bytes = 0xFFFFFFFFull; /* keeps frames * 1e9 in 64 bits */
            u64 now = plat_ns();
            pm_lock(&q->lk);
            u64 frames = bytes / (2ull * q->channels);
            audout_release(q, now);
            if (q->n + q->rn >= 64) {
                /* the SDK's AppendAudioOutBuffer returns false for BufferRingFull (0x1099) */
                pm_unlock(&q->lk);
                return RC(153, 8);
            }
            u64 t = audout_clock(q, now), st = q->last_end > t ? q->last_end : t;
            u32 k = (q->head + q->n) & 63;
            q->tag[k] = tag;
            q->start[k] = st;
            q->end[k] = st + frames * 1000000000ull / 48000;
            q->frames[k] = frames;
            q->n++;
            q->last_end = q->end[k];
            pm_unlock(&q->lk);
            return 0;
        }
        case 4: out_copy_h(c, ev_handle(q->ev)); return 0; /* RegisterBufferEvent */
        case 5:
        case 8: {
            u64 a = 0, sz = 0;
            u32 cnt = 0;
            buf_out(c, 0, &a, &sz);
            if (a && sz) memset(GP(a), 0, sz); /* GetReleasedAudioOutBuffers(Auto): unused slots read as null */
            u64 now = plat_ns();
            pm_lock(&q->lk);
            audout_release(q, now);
            while (q->rn && (u64)(cnt + 1) * 8 <= sz) {
                if (a) memcpy(GP(a + (u64)cnt * 8), &q->rel[q->rhead], 8);
                q->rhead = (q->rhead + 1) & 63;
                q->rn--;
                cnt++;
            }
            pm_unlock(&q->lk);
            out32(c, cnt);
            return 0;
        }
        case 6: { /* ContainsAudioOutBuffer: appended and not yet taken back by the game */
            u64 tag = in64(c, 0);
            int f = 0;
            pm_lock(&q->lk);
            audout_release(q, plat_ns());
            for (u32 k = 0; k < q->n; k++)
                if (q->tag[(q->head + k) & 63] == tag) f = 1;
            for (u32 k = 0; k < q->rn; k++)
                if (q->rel[(q->rhead + k) & 63] == tag) f = 1;
            pm_unlock(&q->lk);
            out8(c, f);
            return 0;
        }
        case 9: { /* GetAudioOutBufferCount: appended and not played yet */
            pm_lock(&q->lk);
            audout_release(q, plat_ns());
            u32 cnt = q->n;
            pm_unlock(&q->lk);
            out32(c, cnt);
            return 0;
        }
        case 10: { /* GetAudioOutPlayedSampleCount: the finished buffers and what the clock played of the next */
            u64 now = plat_ns();
            pm_lock(&q->lk);
            audout_release(q, now);
            u64 played = q->played, t = audout_clock(q, now);
            if (q->n && t > q->start[q->head]) {
                u64 part = (t - q->start[q->head]) * 48000 / 1000000000ull;
                played += part < q->frames[q->head] ? part : q->frames[q->head];
            }
            pm_unlock(&q->lk);
            out64(c, played);
            return 0;
        }
        case 11: { /* FlushAudioOutBuffers: what is queued comes back now, unplayed */
            pm_lock(&q->lk);
            audout_release(q, plat_ns());
            while (q->n) {
                q->rel[(q->rhead + q->rn) & 63] = q->tag[q->head];
                q->rn++;
                q->head = (q->head + 1) & 63;
                q->n--;
            }
            q->last_end = 0;
            int any = q->rn != 0;
            pm_unlock(&q->lk);
            if (any) kevent_signal(q->ev);
            out8(c, 1);
            return 0;
        }
        case 12: { /* SetAudioOutVolume */
            u32 v = in32(c, 0);
            pm_lock(&q->lk);
            memcpy(&q->volume, &v, 4);
            pm_unlock(&q->lk);
            return 0;
        }
        case 13: { /* GetAudioOutVolume */
            u32 v;
            pm_lock(&q->lk);
            memcpy(&v, &q->volume, 4);
            pm_unlock(&q->lk);
            out32(c, v);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static int audout_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0 || c->cmd == 2) { /* ListAudioOuts(Auto) */
        out32(c, put_name(c, "DeviceOut"));
        return 0;
    }
    if (c->cmd == 1 || c->cmd == 3) { /* OpenAudioOut(Auto): AudioOutInputParameter, sample rate and channel count */
        u32 want_ch = in32(c, 4) & 0xFFFF;
        SvcObj* r = svcobj_new("IAudioOut", audout_obj, sizeof(AudOut));
        AudOut* q = (AudOut*)r->data;
        pm_init(&q->lk);
        q->channels = 2; /* what the reply below tells the game it gets */
        q->volume = 1.0f;
        q->ev = kevent_new("audio out buffers");
        kobj_ref(&q->ev->o);
        r->destroy = audout_destroy;
        outs_init();
        pm_lock(&audio.outs_lk);
        q->next = audio.outs;
        audio.outs = q;
        pm_unlock(&audio.outs_lk);
        put_name(c, "DeviceOut");
        LOG_LIMIT(1, "audout: opened (the game asked for %u channels, it gets 48000 Hz stereo s16)\n", want_ch);
        out32(c, 48000);
        out32(c, q->channels);
        out32(c, 2); /* PCM s16 */
        out32(c, 1); /* stopped */
        out_obj(c, r);
        return 0;
    }
    return stub_ok(o, c);
}
SvcObj* new_audout(void) { return svcobj_new("audout:u", audout_disp, 0); }

/* hwopus: Opus packets decoded with FFmpeg's Opus decoder, part of the FFmpeg build SWR_H264 brings in.
   The commands are the ones the SDK's nn::codec client in the game sends (SDK 6.5: manager 0 to 3, decoder 8 and 9),
   read from its disassembly, and Switchbrew's hwopus page and Ryujinx (MIT, Ryujinx.HLE Services/Audio
   HardwareOpusDecoderManager) for the older decode commands and the ones newer SDKs add.
   A packet starts with an 8 byte header, big-endian: the payload's size, then the encoder's final range (not checked:
   FFmpeg doesn't give its own). The reply is the bytes consumed, the samples per channel (interleaved s16 at the
   decoder's rate) and, from the decoder's command 4 on, the time the decode took in microseconds.
   Results: module 111 description 17 is the one the SDK reports as an invalid packet, 385 the one it reports as a
   failed open. It treats any other result as fatal */
#define OPUS_BAD_PACKET  RC(111, 17)
#define OPUS_OPEN_FAILED RC(111, 385)
#define OPUS_MAX_48K     5760 /* samples per channel in the longest packet, 120 ms at 48 kHz */
typedef struct {
    u32 rate, channels;
    PMutex lk;
#ifndef SWR_NO_H264
    AVCodecContext* dec;
    AVPacket* pkt;
    AVFrame* frame;
    u8* in; /* the payload, with the padding FFmpeg reads past its end */
    u32 incap;
    /* FFmpeg's decoder gives 48 kHz. Lower rates go through a low-pass FIR and keep every step-th sample: hist holds
       each channel's last taps - 1 input samples, x is the filter's input (those, then a packet's samples) */
    u32 step, taps;
    float* fir;
    float* hist;
    float* x;
#endif
} HwOpus;
#ifndef SWR_NO_H264
/* windowed sinc (Blackman), cut off at 90% of the output rate's Nyquist frequency, unity gain */
static void opus_fir(float* h, u32 n, u32 step) {
    double fc = 0.45 / step, sum = 0, m = (n - 1) / 2.0;
    for (u32 i = 0; i < n; i++) {
        double t = i - m, s = t == 0 ? 2 * fc : sin(2 * M_PI * fc * t) / (M_PI * t);
        double w = 0.42 - 0.5 * cos(2 * M_PI * i / (n - 1)) + 0.08 * cos(4 * M_PI * i / (n - 1));
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (u32 i = 0; i < n; i++) h[i] = (float)(h[i] / sum);
}
static i16 opus_s16(float v) {
    v *= 32768.0f;
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (i16)lrintf(v);
}
#endif
static void hwopus_destroy(SvcObj* o) {
    HwOpus* h = (HwOpus*)o->data;
#ifndef SWR_NO_H264
    avcodec_free_context(&h->dec);
    av_packet_free(&h->pkt);
    av_frame_free(&h->frame);
    free(h->in);
    free(h->fir);
    free(h->hist);
    free(h->x);
#else
    (void)h;
#endif
}
/* the decoder for rate and channels: streams Opus streams, stereo of them coupled, map[c] the stream channel of
   output channel c (as libopus' multistream decoder takes them). 0 when it can't be made */
static int hwopus_open(HwOpus* h, u32 rate, u32 channels, u32 streams, u32 stereo, const u8* map) {
    h->rate = rate;
    h->channels = channels;
    pm_init(&h->lk);
#ifdef SWR_NO_H264
    (void)streams;
    (void)stereo;
    (void)map;
    LOG_LIMIT(1, "hwopus: this build has no Opus decoder (FFmpeg comes with SWR_H264)\n");
    return 0;
#else
    if (rate != 48000 && rate != 24000 && rate != 16000 && rate != 12000 && rate != 8000) return 0;
    if (!channels || channels > 255 || !streams || stereo > streams || streams + stereo > 255) return 0;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return 0;
    ffmpeg_log_init();
    h->dec = avcodec_alloc_context3(codec);
    h->pkt = av_packet_alloc();
    h->frame = av_frame_alloc();
    h->step = 48000 / rate;
    h->taps = h->step > 1 ? 24 * h->step + 1 : 1;
    h->fir = malloc(h->taps * sizeof(float));
    h->hist = calloc((size_t)channels * h->taps, sizeof(float));
    h->x = malloc((h->taps + OPUS_MAX_48K) * sizeof(float));
    if (!h->dec || !h->pkt || !h->frame || !h->fir || !h->hist || !h->x) return 0;
    if (h->step > 1) opus_fir(h->fir, h->taps, h->step);
    /* the OpusHead FFmpeg takes the layout from (RFC 7845): no pre-skip, the game trims that itself. Mapping family 0
       is one mono or stereo stream, 255 any number of streams with the mapping as given */
    int multi = streams != 1 || channels > 2 || map;
    u32 n = multi ? 21 + channels : 19;
    u8* eh = av_mallocz(n + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!eh) return 0;
    memcpy(eh, "OpusHead", 8);
    eh[8] = 1;
    eh[9] = (u8)channels;
    eh[12] = 0x80; /* the input rate, 48000, little-endian */
    eh[13] = 0xBB;
    eh[18] = multi ? 255 : 0;
    if (multi) {
        eh[19] = (u8)streams;
        eh[20] = (u8)stereo;
        for (u32 i = 0; i < channels; i++) eh[21 + i] = map ? map[i] : (u8)i;
    }
    h->dec->extradata = eh;
    h->dec->extradata_size = (int)n;
    h->dec->ch_layout.nb_channels = (int)channels;
    if (avcodec_open2(h->dec, codec, NULL) < 0 || h->dec->ch_layout.nb_channels != (int)channels) return 0;
    return 1;
#endif
}
/* one packet (header and payload) into out, interleaved s16 */
static u32 hwopus_decode(HwOpus* h, const u8* in, u64 in_size, i16* out, u64 out_size, int reset, u32* consumed,
                         u32* samples) {
#ifdef SWR_NO_H264
    (void)h;
    (void)in;
    (void)in_size;
    (void)out;
    (void)out_size;
    (void)reset;
    (void)consumed;
    (void)samples;
    return OPUS_BAD_PACKET;
#else
    if (reset) {
        avcodec_flush_buffers(h->dec);
        memset(h->hist, 0, (size_t)h->channels * h->taps * sizeof(float));
    }
    if (in_size < 8) return OPUS_BAD_PACKET;
    u32 len = ((u32)in[0] << 24) | ((u32)in[1] << 16) | ((u32)in[2] << 8) | in[3];
    if (!len || len > in_size - 8) return OPUS_BAD_PACKET;
    if (h->incap < len) {
        u8* nb = realloc(h->in, (size_t)len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!nb) return OPUS_BAD_PACKET;
        h->in = nb;
        h->incap = len;
    }
    memcpy(h->in, in + 8, len);
    memset(h->in + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    h->pkt->data = h->in;
    h->pkt->size = (int)len;
    if (avcodec_send_packet(h->dec, h->pkt) < 0) return OPUS_BAD_PACKET;
    u32 ch = h->channels, step = h->step, keep = h->taps - 1, got = 0;
    int bad = 0;
    while (avcodec_receive_frame(h->dec, h->frame) == 0) {
        u32 n = (u32)h->frame->nb_samples, m = n / step;
        if (n > OPUS_MAX_48K || h->frame->format != AV_SAMPLE_FMT_FLTP ||
            (u64)(got + m) * ch * sizeof(i16) > out_size) {
            bad = 1;
            av_frame_unref(h->frame);
            continue;
        }
        for (u32 c = 0; c < ch; c++) {
            const float* s = (const float*)h->frame->extended_data[c];
            i16* d = out + (u64)got * ch + c;
            if (step == 1) {
                for (u32 i = 0; i < n; i++) d[(u64)i * ch] = opus_s16(s[i]);
                continue;
            }
            float* hist = h->hist + (u64)c * h->taps;
            memcpy(h->x, hist, keep * sizeof(float));
            memcpy(h->x + keep, s, n * sizeof(float));
            for (u32 k = 0; k < m; k++) {
                const float* xp = h->x + (u64)k * step;
                float acc = 0;
                for (u32 j = 0; j < h->taps; j++) acc += h->fir[j] * xp[j];
                d[(u64)k * ch] = opus_s16(acc);
            }
            memcpy(hist, h->x + n, keep * sizeof(float));
        }
        got += m;
        av_frame_unref(h->frame);
    }
    if (bad) return OPUS_BAD_PACKET;
    *consumed = len + 8;
    *samples = got;
    return 0;
#endif
}
static int hwopus_dec(SvcObj* o, IpcCtx* c) {
    HwOpus* h = (HwOpus*)o->data;
    switch (c->cmd) {
        case 1: /* SetContext */
        case 3: /* SetContextForMultiStream */ return 0;
        case 0: /* DecodeInterleavedOld */
        case 2: /* DecodeInterleavedForMultiStreamOld */
        case 4: /* DecodeInterleavedWithPerfOld */
        case 5: /* DecodeInterleavedForMultiStreamWithPerfOld */
        case 6: /* DecodeInterleavedWithPerfAndResetOld: from here on a reset flag comes first */
        case 7: /* DecodeInterleavedForMultiStreamWithPerfAndResetOld */
        case 8: /* DecodeInterleaved */
        case 9: /* DecodeInterleavedForMultiStream */ {
            u64 ia, is, oa, os;
            if (!buf_in(c, 0, &ia, &is) || !ia || !buf_out(c, 0, &oa, &os) || !oa) return OPUS_BAD_PACKET;
            int reset = c->cmd >= 6 && (in8(c, 0) & 1);
            u32 consumed = 0, samples = 0;
            u64 t0 = plat_ns();
            pm_lock(&h->lk);
            u32 rc = hwopus_decode(h, GP(ia), is, (i16*)GP(oa), os, reset, &consumed, &samples);
            pm_unlock(&h->lk);
            if (rc) {
                LOG_LIMIT(8, "hwopus: a %llu byte packet didn't decode, the game is told it's invalid\n",
                          (unsigned long long)is);
                return (int)rc;
            }
            out32(c, consumed);
            out32(c, samples);
            if (c->cmd >= 4) out64(c, (plat_ns() - t0) / 1000);
            return 0;
        }
    }
    return stub_ok(o, c);
}
/* a work buffer the game allocates and passes as transfer memory, which the decoder here never touches: about what a
   libopus decoder needs per stream */
static u32 hwopus_work_size(u32 streams) { return 0x8000 + 0x8000 * (streams ? streams : 1); }
/* OpusMultiStreamParameters(Ex), the X buffer of manager commands 2, 3, 6 and 7: rate, channels, streams, coupled
   streams, (Ex: flags and padding,) then the channel mapping, 0x100 bytes */
static int hwopus_ms_params(IpcCtx* c, u32 v[4], const u8** map, int ex) {
    u64 a, s;
    u32 need = (ex ? 0x18 : 0x10) + 0x100;
    if (!buf_in(c, 0, &a, &s) || !a || s < need) return 0;
    memcpy(v, GP(a), 16);
    *map = GP(a) + (ex ? 0x18 : 0x10);
    return 1;
}
static int hwopus_disp(SvcObj* o, IpcCtx* c) {
    u32 v[4];
    const u8* map;
    switch (c->cmd) {
        case 0:   /* OpenHardwareOpusDecoder: rate, channels, work buffer size, transfer memory */
        case 4: { /* OpenHardwareOpusDecoderEx [12.0.0+]: rate, channels, flags, padding, work buffer size */
            SvcObj* r = svcobj_new("IHardwareOpusDecoder", hwopus_dec, sizeof(HwOpus));
            r->destroy = hwopus_destroy;
            u32 rate = in32(c, 0), ch = in32(c, 4);
            if (ch > 2 || !hwopus_open((HwOpus*)r->data, rate, ch, 1, ch == 2, NULL)) {
                LOG_LIMIT(4, "hwopus: no decoder for %u Hz, %u channels\n", rate, ch);
                svcobj_unref(r);
                return OPUS_OPEN_FAILED;
            }
            out_obj(c, r);
            return 0;
        }
        case 2:   /* OpenHardwareOpusDecoderForMultiStream: parameters (X), work buffer size, transfer memory */
        case 6: { /* OpenHardwareOpusDecoderForMultiStreamEx [12.0.0+] */
            if (!hwopus_ms_params(c, v, &map, c->cmd == 6)) return OPUS_OPEN_FAILED;
            SvcObj* r = svcobj_new("IHardwareOpusDecoder", hwopus_dec, sizeof(HwOpus));
            r->destroy = hwopus_destroy;
            if (!hwopus_open((HwOpus*)r->data, v[0], v[1], v[2], v[3], map)) {
                LOG_LIMIT(4, "hwopus: no decoder for %u Hz, %u channels in %u streams (%u stereo)\n", v[0], v[1], v[2],
                          v[3]);
                svcobj_unref(r);
                return OPUS_OPEN_FAILED;
            }
            out_obj(c, r);
            return 0;
        }
        case 1: /* GetWorkBufferSize: rate, channels */
        case 5: /* GetWorkBufferSizeEx [12.0.0+] */
        case 8: /* GetWorkBufferSizeExEx [17.0.0+] */ out32(c, hwopus_work_size(1)); return 0;
        case 3: /* GetWorkBufferSizeForMultiStream: parameters (X) */
        case 7: /* GetWorkBufferSizeForMultiStreamEx [12.0.0+] */
        case 9: /* GetWorkBufferSizeForMultiStreamExEx [17.0.0+] */
            out32(c, hwopus_work_size(hwopus_ms_params(c, v, &map, c->cmd != 3) ? v[2] : 1));
            return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_hwopus(void) { return svcobj_new("hwopus", hwopus_disp, 0); }
