/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the audio renderer's RequestUpdate, answered in the real format with no sound produced */
#include "hle/services_int.h"
#include "kernel/ipc.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include <stdlib.h>
#include <string.h>

/* what the renderer keeps of each voice between updates. The 4 wave buffers of a voice are a ring: the client fills
   slots after its WaveBuffersIndex and the DSP plays from cur onwards, so they are consumed in ring order */
typedef struct {
    u64 played;   /* PlayedSampleCount */
    u32 consumed; /* PlayedWaveBuffersCount */
    u32 cur;      /* the slot playing */
    i64 pos;      /* samples into it */
    double frac;  /* the part of a sample the last update didn't play */
    i64 len[4];
    u8 valid[4], loop[4];
    u8 stopped;
} VSt;
typedef struct {
    u64 last_ns;
    VSt v[4096];
} ARState;
static void ar_destroy(SvcObj* o) { free((void*)(size_t)o->u[4]); }
/* in o->u[4], made by the first RequestUpdate. Two threads can be in the same renderer's dispatch at once */
static ARState* ar_state(SvcObj* o) {
    u64 cur = __atomic_load_n(&o->u[4], __ATOMIC_ACQUIRE);
    if (cur) return (ARState*)(size_t)cur;
    ARState* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (__atomic_compare_exchange_n(&o->u[4], &cur, (u64)(size_t)s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        o->destroy = ar_destroy;
        return s;
    }
    free(s);
    return (ARState*)(size_t)cur;
}
/* buffer slot k is done: the DSP moves on to the next one */
static void vst_consume(VSt* v, u32 k) {
    v->valid[k] = 0;
    v->consumed++;
    v->cur = (v->cur + 1) & 3;
}

/* RequestUpdate layout as in yuzu's InfoUpdater.
   header (in and out): revision, behaviour, memory_pools, voices, voice_resources, effects, mixes, sinks,
   performance, splitter, render_info, pad[4], total. */
int audren_update(SvcObj* o, IpcCtx* c) {
    u64 ia, is, oa, os;
    buf_in(c, 0, &ia, &is);
    buf_out(c, 0, &oa, &os);
    if (!ia || !oa || is < 0x40) return RC(153, 41); /* InvalidUpdateInfo */
    u8* in = GP(ia);
    u8* out = GP(oa);
    u32 ih[16];
    memcpy(ih, in, 64);
    u32 rev = ih[0];
    int revn = (int)((rev >> 24) & 0xFF) - '0';
    if (revn < 0) revn = 0;
    u32 behav = ih[1], mp = ih[2], vo = ih[3], vr = ih[4], ef = ih[5];
    /* the sections read below have to be in the input buffer: their sizes come from the game */
    if (0x40 + (u64)behav + mp + vr + vo + ef > is) {
        WARN_LIMIT(4, "audren: update sections (%u %u %u %u %u bytes) don't fit in the %llu byte input\n", behav, mp,
                   vr, vo, ef, (unsigned long long)is);
        return RC(153, 41);
    }
    u32 npools = mp / 0x20, nvoices = vo / 0x170, neff = ef / 0xC0, nsinks = ih[7] / 0x140;
    u8* p = in + 0x40 + behav;
    u8* pools_in = p;
    p += mp;
    p += vr;
    u8* voices_in = p;
    p += vo;
    u8* eff_in = p;
    int eff_v2 = revn >= 9;
    u32 o_mp = npools * 0x10, o_vo = nvoices * 0x10, o_ef = neff * (eff_v2 ? 0x90 : 0x10), o_sk = nsinks * 0x20,
        o_pf = 0x10, o_bh = 0xB0, o_ri = revn >= 5 ? 0x10 : 0;
    u64 total = 0x40 + (u64)o_mp + o_vo + o_ef + o_sk + o_pf + o_bh + o_ri;
    if (total > os) {
        WARN_LIMIT(4, "audren: output buffer too small (%llu > %llu)\n", (unsigned long long)total,
                   (unsigned long long)os);
        return RC(153, 4);
    }
    ARState* s = ar_state(o);
    if (!s) {
        WARN("audren: out of memory for the voice states\n");
        return RC(153, 2);
    }
    u32 oh[16];
    memset(oh, 0, sizeof oh);
    oh[0] = rev;
    oh[1] = o_bh;
    oh[2] = o_mp;
    oh[3] = o_vo;
    oh[4] = 0;
    oh[5] = o_ef;
    oh[6] = 0;
    oh[7] = o_sk;
    oh[8] = o_pf;
    oh[9] = 0;
    oh[10] = o_ri;
    oh[15] = (u32)total;
    LOG_LIMIT(1, "audren: REV%d, %u pools, %u voices, %u effects, %u sinks\n", revn, npools, nvoices, neff, nsinks);
    memset(out, 0, total);
    memcpy(out, oh, 64);
    u8* q = out + 0x40;
    /* pools: RequestAttach(4)->Attached(5), RequestDetach(2)->Detached(3) */
    for (u32 i = 0; i < npools; i++) {
        u32 st;
        memcpy(&st, pools_in + i * 0x20 + 0x10, 4);
        u32 ns = st == 4 ? 5 : st == 2 ? 3 : st;
        memcpy(q + i * 0x10, &ns, 4);
    }
    q += o_mp;
    /* consume voice buffers at real playback speed: the time since the last update, scaled by the voice's rate and
       pitch. don't consume instantly, streaming players wait for buffers to pile up */
    u64 now = plat_ns();
    double dt = s->last_ns ? (double)(now - s->last_ns) / 1e9 : 0.005;
    s->last_ns = now;
    for (u32 i = 0; i < nvoices; i++) {
        u8* vi = voices_in + i * 0x170;
        u8* vout = q + i * 0x10;
        u32 id;
        memcpy(&id, vi, 4);
        id &= 4095;
        u8 in_use = vi[9], is_new = vi[8], play = vi[0xA];
        VSt* v = &s->v[id];
        if (is_new) {
            memset(v, 0, sizeof *v);
            v->stopped = 1;
        }
        if (in_use) {
            /* new buffers (sent_to_DSP==0) take their slot */
            for (int w = 0; w < 4; w++) {
                u8* wb = vi + 0x60 + w * 0x38;
                u64 addr, size;
                i32 st, en;
                memcpy(&addr, wb, 8);
                memcpy(&size, wb + 8, 8);
                memcpy(&st, wb + 0x10, 4);
                memcpy(&en, wb + 0x14, 4);
                if (addr && size && !wb[0x1A]) {
                    i64 n = (i64)en - (i64)st;
                    v->len[w] = n > 0 ? n : 1;
                    v->loop[w] = wb[0x18];
                    v->valid[w] = 1;
                }
            }
            /* FlushWaveBufferCount (+0x159): that many buffers from WaveBuffersIndex (+0x40) are dropped, as consumed */
            u32 nflush = vi[0x159] < 4 ? vi[0x159] : 4, wi;
            memcpy(&wi, vi + 0x40, 4);
            for (u32 k = 0; k < nflush; k++) vst_consume(v, (wi + k) & 3);
            if (nflush) v->pos = 0;
            if (play == 1) { /* Stopped: what was queued is consumed, the voice starts over */
                if (!v->stopped) {
                    for (u32 k = 0; k < 4; k++)
                        if (v->valid[k]) vst_consume(v, k);
                    v->pos = 0;
                    v->played = 0;
                    v->frac = 0;
                    v->stopped = 1;
                }
            } else
                v->stopped = 0;
            if (play == 0) { /* Started */
                u32 rate;
                float pitch;
                memcpy(&rate, vi + 0xC, 4);
                memcpy(&pitch, vi + 0x1C, 4);
                if (!rate || rate > 192000) rate = 48000;
                if (!(pitch > 0.0f) || pitch > 8.0f) pitch = 1.0f;
                double want = dt * (double)rate * (double)pitch + v->frac;
                i64 adv = (i64)want;
                v->frac = want - (double)adv;
                v->played += (u64)adv;
                while (adv > 0 && v->valid[v->cur]) {
                    i64 left = v->len[v->cur] - v->pos;
                    if (adv < left) {
                        v->pos += adv;
                        break;
                    }
                    adv -= left;
                    v->pos = 0;
                    if (v->loop[v->cur]) { /* looping buffers never finish */
                        adv %= v->len[v->cur];
                        continue;
                    }
                    vst_consume(v, v->cur);
                }
            }
        }
        memcpy(vout, &v->played, 8);
        memcpy(vout + 8, &v->consumed, 4);
    }
    q += o_vo;
    /* effects: Enabled(3) / Disabled(4) */
    for (u32 i = 0; i < neff; i++) {
        u8 en = eff_in[i * 0xC0 + 2];
        q[i * (eff_v2 ? 0x90 : 0x10)] = en ? 3 : 4;
    }
    q += o_ef;
    q += o_sk;
    q += o_pf;
    q += o_bh;
    if (o_ri) {
        u64 frames = o->u[5]++;
        memcpy(q, &frames, 8);
    }
    return 0;
}
