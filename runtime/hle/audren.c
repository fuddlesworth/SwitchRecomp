/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the audio renderer's RequestUpdate, answered in the real format with no sound produced */
#include "hle/services_int.h"
#include "kernel/ipc.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include <stdlib.h>
#include <string.h>

/* RequestUpdate layout as in yuzu's InfoUpdater.
   header (in and out): revision, behaviour, memory_pools, voices, voice_resources, effects, mixes, sinks,
   performance, splitter, render_info, pad[4], total. */
int audren_update(SvcObj* o, IpcCtx* c) {
    u64 ia, is, oa, os;
    buf_in(c, 0, &ia, &is);
    buf_out(c, 0, &oa, &os);
    if (!ia || !oa || is < 0x40) return 0;
    u8* in = GP(ia);
    u8* out = GP(oa);
    u32 ih[16];
    memcpy(ih, in, 64);
    u32 rev = ih[0];
    int revn = (int)((rev >> 24) & 0xFF) - '0';
    if (revn < 0) revn = 0;
    u32 behav = ih[1], mp = ih[2], vo = ih[3], vr = ih[4], ef = ih[5];
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
    oh[15] = 0x40 + o_mp + o_vo + o_ef + o_sk + o_pf + o_bh + o_ri;
    if (oh[15] > os) {
        WARN("audren: output buffer too small (%u > %llu)\n", oh[15], (unsigned long long)os);
        return RC(153, 4);
    }
    static int said;
    if (!said) {
        said = 1;
        LOG("audren: REV%d, %u pools, %u voices, %u effects, %u sinks\n", revn, npools, nvoices, neff, nsinks);
    }
    memset(out, 0, oh[15]);
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
    /* consume voice buffers at real playback speed: 240 samples per 5ms update at 48k, scaled by the
     voice's rate and pitch. don't consume instantly, streaming players wait for buffers to pile up */
    typedef struct {
        u64 played;
        u32 consumed, qn;
        i64 pos;
        i64 q[8];
        u8 loop[8];
    } VSt;
    if (!o->u[4]) { o->u[4] = (u64)(size_t)calloc(4096, sizeof(VSt)); }
    VSt* vst = (VSt*)(size_t)o->u[4];
    for (u32 i = 0; i < nvoices; i++) {
        u8* vi = voices_in + i * 0x170;
        u8* vout = q + i * 0x10;
        u32 id;
        memcpy(&id, vi, 4);
        id &= 4095;
        u8 in_use = vi[9], is_new = vi[8], play = vi[0xA];
        VSt* v = &vst[id];
        if (is_new) memset(v, 0, sizeof *v);
        if (in_use) {
            /* queue new buffers (sent_to_DSP==0) */
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
                    if (n <= 0) n = 1;
                    if (v->qn < 8) {
                        v->q[v->qn] = n;
                        v->loop[v->qn] = wb[0x18];
                        v->qn++;
                    } else
                        v->consumed++;
                }
            }
            if (play == 0 && v->qn) { /* Started */
                u32 rate;
                float pitch;
                memcpy(&rate, vi + 0x18, 4);
                memcpy(&pitch, vi + 0x1C, 4);
                if (!rate || rate > 192000) rate = 48000;
                if (!(pitch > 0.0f) || pitch > 8.0f) pitch = 1.0f;
                i64 adv = (i64)(240.0 * (double)rate * (double)pitch / 48000.0 + 0.5);
                if (adv < 1) adv = 1;
                v->played += (u64)adv;
                while (adv > 0 && v->qn) {
                    i64 left = v->q[0] - v->pos;
                    if (adv < left) {
                        v->pos += adv;
                        adv = 0;
                        break;
                    }
                    adv -= left;
                    v->pos = 0;
                    if (v->loop[0]) {
                        if (v->q[0] <= 0) break;
                        continue;
                    } /* looping buffers never finish */
                    v->consumed++;
                    memmove(v->q, v->q + 1, (v->qn - 1) * sizeof(i64));
                    memmove(v->loop, v->loop + 1, v->qn - 1);
                    v->qn--;
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
