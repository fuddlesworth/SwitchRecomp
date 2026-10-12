/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* audio services (audren, audout, hwopus): answered in the real formats, with no sound produced */
#include "hle/services_int.h"
/* audio: pretend, no output */
KEvent* g_aud_ev;

/* IAudioOut: nothing is played, but appended buffers come back at 48 kHz speed like on the console.
   Players time video against the released buffers (CRI movies): never releasing them stalls the video.
   Buffers are laid out on a play clock that runs only while the audout is started: a buffer is released once the
   clock passes its end, and then waits in rel[] for GetReleasedAudioOutBuffers */
typedef struct AudOut {
    struct AudOut* next; /* g_outs */
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

/* the open IAudioOuts, which audio_clock looks at to signal their buffer events */
static AudOut* g_outs;
static PMutex g_outs_lk;
static int g_outs_init; /* 0, 1 while pm_init runs, 2 ready: the first audout can open before audio_clock runs */
static void outs_init(void) {
    int s = 0;
    if (__atomic_load_n(&g_outs_init, __ATOMIC_ACQUIRE) == 2) return;
    if (__atomic_compare_exchange_n(&g_outs_init, &s, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        pm_init(&g_outs_lk);
        __atomic_store_n(&g_outs_init, 2, __ATOMIC_RELEASE);
    } else
        while (__atomic_load_n(&g_outs_init, __ATOMIC_ACQUIRE) != 2) plat_yield();
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
        kevent_signal(g_aud_ev);
        now = plat_ns();
        pm_lock(&g_outs_lk);
        for (AudOut* q = g_outs; q; q = q->next) {
            pm_lock(&q->lk);
            audout_release(q, now);
            int any = q->rn != 0;
            pm_unlock(&q->lk);
            if (any) kevent_signal(q->ev);
        }
        pm_unlock(&g_outs_lk);
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
        case 7: out_copy_h(c, ev_handle(g_aud_ev)); return 0;
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
        KEvent* e = kevent_new(names[i], 0);
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
    pm_lock(&g_outs_lk);
    for (AudOut** p = &g_outs; *p; p = &(*p)->next)
        if (*p == q) {
            *p = q->next;
            break;
        }
    pm_unlock(&g_outs_lk);
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
        q->ev = kevent_new("audio out buffers", 0);
        kobj_ref(&q->ev->o);
        r->destroy = audout_destroy;
        outs_init();
        pm_lock(&g_outs_lk);
        q->next = g_outs;
        g_outs = q;
        pm_unlock(&g_outs_lk);
        put_name(c, "DeviceOut");
        static int said;
        if (!said++) LOG("audout: opened (the game asked for %u channels, it gets 48000 Hz stereo s16)\n", want_ch);
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

/* hwopus: no Opus decoder is built in. A decode consumes nothing and returns no samples */
static int hwopus_dec(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0 || c->cmd == 4) {
        static int said;
        if (!said++) LOG("hwopus: Opus decoding isn't implemented, the game gets no samples\n");
        out32(c, 0);
        out32(c, 0);
        return 0;
    }
    return stub_ok(o, c);
}
static int hwopus_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 2:
        case 4: out_obj(c, svcobj_new("IHardwareOpusDecoder", hwopus_dec, 0)); return 0;
        case 1:
        case 3:
        case 5: out32(c, 0x10000); return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_hwopus(void) { return svcobj_new("hwopus", hwopus_disp, 0); }
