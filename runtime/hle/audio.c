/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* audio services (audren, audout, hwopus): answered in the real formats, with no sound produced */
#include "hle/services_int.h"
/* audio: pretend, no output */
KEvent* g_aud_ev;
/* renderer event fires every 5ms (240 samples @ 48k) like HOS */
void audio_clock(void* arg) {
    (void)arg;
    u64 next = plat_ns();
    for (;;) {
        next += 5000000ull;
        u64 now = plat_ns();
        if (next > now)
            plat_sleep_ns((i64)(next - now));
        else if (now - next > 50000000ull)
            next = now;
        kevent_signal(g_aud_ev);
    }
}
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
        case 8:
        case 9: return 0;
        case 11: out32(c, 0); return 0;
    }
    return quiet_ok(o, c);
}
static int auddev_disp(SvcObj* o, IpcCtx* c) {
    (void)o;
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
        case 4:
        case 11:
        case 12: {
            KEvent* e = kevent_new("audio device", 0);
            e->signaled = 1;
            out_copy_h(c, ev_handle(e));
            return 0;
        }
        case 5: out32(c, 2); return 0;
    }
    return 0;
}
static int audren_disp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0: {
            SvcObj* r = svcobj_new("IAudioRenderer", audren_obj, 0x100);
            memcpy(r->data, c->raw, 0x34 < 0x100 ? 0x34 : 0x100);
            r->u[0] = in32(c, 0);
            r->u[1] = in32(c, 4);
            r->u[2] = in32(c, 8);
            r->u[3] = 1;
            out_obj(c, r);
            return 0;
        }
        case 1: {
            u64 sz = 0x4000;
            u32 voices = in32(c, 0x10), sinks = in32(c, 0x14), effects = in32(c, 0x1C);
            sz += (u64)voices * 0x400 + sinks * 0x200 + effects * 0x800 + 0x40000;
            out64(c, sz);
            return 0;
        }
        case 2:
        case 4: out_obj(c, svcobj_new("IAudioDevice", auddev_disp, 0)); return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_audren(void) { return svcobj_new("audren:u", audren_disp, 0); }
/* IAudioOut: nothing is played, but appended buffers come back at 48 kHz stereo s16 speed like on the console.
   Players time video against the released buffers (CRI movies): never releasing them stalls the video */
typedef struct {
    u64 tag[64], end[64], frames[64];
    u32 head, n;
    u64 last_end, played;
    int started;
    PMutex lk;
} AudOut;
static int audout_obj(SvcObj* o, IpcCtx* c) {
    AudOut* q = (AudOut*)o->data;
    switch (c->cmd) {
        case 0: out32(c, q->started ? 0 : 1); return 0; /* GetAudioOutState: 0 started, 1 stopped */
        case 1: q->started = 1; return 0;
        case 2: q->started = 0; return 0;
        case 3:
        case 7: {
            u64 tag = in64(c, 0), a, sz, bytes = 0; /* AppendAudioOutBuffer(Auto): AudioOutBuffer.data_size at +0x18 */
            if (buf_in(c, 0, &a, &sz) && a && sz >= 0x20) memcpy(&bytes, GP(a + 0x18), 8);
            u64 frames = bytes / 4, now = plat_ns();
            pm_lock(&q->lk);
            u64 st = q->last_end > now ? q->last_end : now;
            q->last_end = st + frames * 1000000000ull / 48000;
            if (q->n < 64) {
                u32 k = (q->head + q->n) & 63;
                q->tag[k] = tag;
                q->end[k] = q->last_end;
                q->frames[k] = frames;
                q->n++;
            }
            pm_unlock(&q->lk);
            return 0;
        }
        case 4: out_copy_h(c, ev_handle(g_aud_ev)); return 0;
        case 5:
        case 8: {
            u64 a = 0, sz = 0;
            u32 cnt = 0;
            buf_out(c, 0, &a, &sz);
            if (a && sz) memset(GP(a), 0, sz); /* GetReleasedAudioOutBuffers(Auto): unused slots read as null */
            u64 now = plat_ns();
            pm_lock(&q->lk);
            while (q->n && q->end[q->head] <= now && (u64)(cnt + 1) * 8 <= sz) {
                if (a) memcpy(GP(a + (u64)cnt * 8), &q->tag[q->head], 8);
                q->played += q->frames[q->head];
                q->head = (q->head + 1) & 63;
                q->n--;
                cnt++;
            }
            pm_unlock(&q->lk);
            out32(c, cnt);
            return 0;
        }
        case 6: {
            u64 tag = in64(c, 0);
            int f = 0;
            pm_lock(&q->lk);
            for (u32 k = 0; k < q->n; k++)
                if (q->tag[(q->head + k) & 63] == tag) f = 1;
            pm_unlock(&q->lk);
            out8(c, f);
            return 0;
        }
        case 9: out32(c, q->n); return 0;       /* GetAudioOutBufferCount */
        case 10: out64(c, q->played); return 0; /* GetAudioOutPlayedSampleCount */
    }
    return 0;
}
static int audout_disp(SvcObj* o, IpcCtx* c) {
    if (c->cmd == 0 || c->cmd == 2) {
        u64 a, s;
        if (buf_out(c, 0, &a, &s) && a) { strcpy((char*)GP(a), "DeviceOut"); }
        out32(c, 1);
        return 0;
    }
    if (c->cmd == 1 || c->cmd == 3) {
        out32(c, 48000);
        out32(c, 2);
        out32(c, 2);
        out32(c, 1);
        out_obj(c, svcobj_new("IAudioOut", audout_obj, sizeof(AudOut)));
        return 0;
    }
    return quiet_ok(o, c);
}
SvcObj* new_audout(void) { return svcobj_new("audout:u", audout_disp, 0); }

static int hwopus_dec(SvcObj* o, IpcCtx* c) {
    (void)o;
    if (c->cmd == 0 || c->cmd == 4) {
        out32(c, 0);
        out32(c, 0);
        return 0;
    }
    return 0;
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
