/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* vi + the nvnflinger buffer queue. QueueBuffer presents immediately, there's no compositor. */
#include "hle/vi.h"
#include "core/main.h"
#include "core/options.h"
#include "gpu/gpu.h"
#include "hle/nvdrv.h"
#include "hle/services.h"
#include "kernel/ipc.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used;
    u32 afmt;
    u32 nvmap_id;
    u32 w, h, stride, fmt, layout, bhl, pitch, offset;
    u64 size;
    u8 raw[0x16C];
    u32 rawsize;
    int queued;
    i32 crop[4]; /* the part of the buffer shown (left, top, right, bottom); all 0 for the whole buffer */
} Slot;
static struct {
    Slot slots[64]; /* lock */
    KEvent* vsync_ev;
    KEvent* bufavail_ev;
    PMutex lock;
    int width, height;
    PCond free_cv;
    /* the last crop and transform QUEUE_BUFFER logged (lock) */
    u32 crop_transform;
    i32 crop[4];
    u64 managed_layer, stray_layer; /* the next CreateManagedLayer and CreateStrayLayer ids */
} vi = {.width = 1280, .height = 720, .crop_transform = 0xFFFFFFFFu, .managed_layer = 100, .stray_layer = 200};

/* ============================================================ parcels */
typedef struct {
    u8* d;
    u32 size, pos;
} PR;
/* reads stop at the end of the parcel: a read past it gives 0 and leaves pos at the end */
static u32 pr_u32(PR* p) {
    u32 v = 0;
    if (p->size - p->pos >= 4) {
        memcpy(&v, p->d + p->pos, 4);
        p->pos += 4;
    } else
        p->pos = p->size;
    return v;
}
/* n bytes of the parcel, or NULL when it has fewer left */
static const u8* pr_ptr(PR* p, u32 n) {
    if (p->size - p->pos < n) {
        p->pos = p->size;
        return NULL;
    }
    const u8* r = p->d + p->pos;
    p->pos += n;
    return r;
}
static void pr_skip_token(PR* p) {
    pr_u32(p);
    i32 n = (i32)pr_u32(p);
    if (n <= 0) return;
    u64 skip = ((u64)(n + 1) * 2 + 3) & ~3ull;
    p->pos = skip > p->size - p->pos ? p->size : p->pos + (u32)skip;
}
typedef struct {
    u8 d[0x800];
    u32 n;
} PW;
static void pw_u32(PW* w, u32 v) {
    memcpy(w->d + w->n, &v, 4);
    w->n += 4;
}
static void pw_bytes(PW* w, const void* p, u32 n) {
    memcpy(w->d + w->n, p, n);
    w->n += (n + 3) & ~3u;
}
static void pw_flat(PW* w, const void* p, u32 n) {
    pw_u32(w, n);
    pw_u32(w, 0);
    pw_bytes(w, p, n);
}
static void pw_out(PW* w, u64 addr, u64 size) {
    u32 hdr[4] = {w->n, 0x10, 0, 0x10 + w->n};
    if (!addr || size < 0x10 + w->n) {
        WARN("binder: reply parcel (%u bytes) does not fit the game's buffer (%llu bytes)\n", 0x10 + w->n,
             (unsigned long long)size);
        return;
    }
    u8* o = GP(addr);
    memcpy(o, hdr, 16);
    memcpy(o + 0x10, w->d, w->n);
}

/* ============================================================ present */
static void present_slot(int i) {
    pm_lock(&vi.lock);
    Slot s = vi.slots[i];
    pm_unlock(&vi.lock);
    if (!s.used) return;
    vi_show_buffer(s.nvmap_id, s.offset, s.w, s.h, s.pitch, s.layout, s.bhl, s.afmt, s.crop);
}
/* a queued frame, shown on the GPU thread after the work before it */
typedef struct {
    i32 slot;
    u32 nf, id[4], val[4];
} QueuedFrame;
static void present_queued(void* p) {
    QueuedFrame* q = p;
    for (u32 i = 0; i < q->nf; i++)
        if (q->id[i] != 0xFFFFFFFFu && !syncpt_wait(q->id[i], q->val[i], 1000000000))
            WARN_LIMIT(8,
                       "binder: QUEUE_BUFFER slot %d: fence syncpoint %u did not reach %u in 1 s, presenting anyway\n",
                       q->slot, q->id[i], q->val[i]);
    if (q->slot >= 0 && q->slot < 64) present_slot(q->slot);
    pm_lock(&vi.lock);
    if (q->slot >= 0 && q->slot < 64) vi.slots[q->slot].queued = 0;
    pc_broadcast(&vi.free_cv);
    pm_unlock(&vi.lock);
    kevent_signal(vi.bufavail_ev);
    free(q);
}

static int binder_transact(u32 id, u32 code, PR* in, PW* out) {
    (void)id;
    switch (code) {
        case 10: { /* CONNECT */
            u32 qo[4] = {(u32)vi.width, (u32)vi.height, 0, 0};
            pw_bytes(out, qo, 16);
            pw_u32(out, 0);
            return 0;
        }
        case 11: pw_u32(out, 0); return 0;
        case 14: { /* SET_PREALLOCATED_BUFFER */
            i32 slot = (i32)pr_u32(in);
            u32 has = pr_u32(in);
            if (slot >= 0 && slot < 64) {
                Slot ns;
                memset(&ns, 0, sizeof ns);
                Slot* s = &ns;
                u32 len = has ? pr_u32(in) : 0;
                if (has) pr_u32(in);
                /* the fields below reach 0x84 bytes into the GraphicBuffer */
                const u8* g = has ? pr_ptr(in, len) : NULL;
                if (has && (!g || len < 0x84)) {
                    WARN("vi: buffer %d: GraphicBuffer of %u bytes is too short\n", slot, len);
                    g = NULL;
                }
                if (g) {
                    if (g_verbose >= 2) {
                        LOG("graphicbuffer len=%x:", len);
                        for (int k = 0; k < 0x60; k += 4) {
                            u32 q;
                            memcpy(&q, g + k, 4);
                            LOG(" %x", q);
                        }
                        LOG("\n");
                    }
                    if (len > sizeof s->raw) len = sizeof s->raw;
                    memcpy(s->raw, g, len);
                    s->rawsize = len;
                    /* NvGraphicBuffer layout: hdr(12) unk0 nvmap_id unk2 magic pid type usage format ext_format stride total_size num_planes unk12 planes[] */
                    /* GraphicBuffer: 'GBFR' + 10 words, followed by the NvGraphicBuffer minus its NativeHandle */
                    const u8* nb = g + 0x1C;
                    memcpy(&s->afmt, g + 0x10, 4);
                    memcpy(&s->nvmap_id, nb + 0x10, 4);
                    memcpy(&s->stride, nb + 0x30, 4);
                    const u8* pl = nb + 0x40;
                    memcpy(&s->w, pl, 4);
                    memcpy(&s->h, pl + 4, 4);
                    u64 cf;
                    memcpy(&cf, pl + 8, 8);
                    s->fmt = (u32)cf;
                    memcpy(&s->layout, pl + 0x10, 4);
                    memcpy(&s->pitch, pl + 0x14, 4);
                    memcpy(&s->offset, pl + 0x1C, 4);
                    memcpy(&s->bhl, pl + 0x24, 4);
                    s->used = 1;
                    LOG("vi: buffer %d  %ux%u pitch=%u layout=%u bhl=%u fmt=%x nvmap=%u\n", slot, s->w, s->h, s->pitch,
                        s->layout, s->bhl, s->fmt, s->nvmap_id);
                }
                pm_lock(&vi.lock);
                vi.slots[slot] = ns;
                pm_unlock(&vi.lock);
            }
            pw_u32(out, 0);
            return 0;
        }
        case 1: { /* REQUEST_BUFFER */
            i32 slot = (i32)pr_u32(in);
            pm_lock(&vi.lock);
            Slot* s = (slot >= 0 && slot < 64) ? &vi.slots[slot] : NULL;
            if (s && s->used) {
                pw_u32(out, 1);
                pw_flat(out, s->raw, s->rawsize);
            } else
                pw_u32(out, 0);
            pm_unlock(&vi.lock);
            pw_u32(out, 0);
            return 0;
        }
        case 3: { /* DEQUEUE_BUFFER */
            /* a slot is held from DEQUEUE until QUEUE or CANCEL. With every buffer held this waits for one; when the
               wait times out the holds are dropped, which hands out a buffer the game still holds. The log says when
               that happens, with the slots held */
            pm_lock(&vi.lock);
            int k = -1, nused = 0;
            for (int i = 0; i < 64; i++) nused += vi.slots[i].used;
            if (!nused) { /* no buffers yet: nothing would ever free one */
                pm_unlock(&vi.lock);
                WARN("binder: DEQUEUE_BUFFER before any buffer was set\n");
                pw_u32(out, 0);
                pw_u32(out, 0);
                pw_u32(out, (u32)-19); /* NO_INIT */
                return 0;
            }
            for (;;) {
                for (int i = 0; i < 64; i++)
                    if (vi.slots[i].used && !vi.slots[i].queued) {
                        k = i;
                        break;
                    }
                if (k >= 0) break;
                /* a slot comes free when the GPU thread shows its frame. If none does for a second, something
                   went wrong: free them all rather than hang the game */
                if (pc_wait(&vi.free_cv, &vi.lock, 1000000000)) {
                    u64 held = 0;
                    for (int i = 0; i < 64; i++)
                        if (vi.slots[i].used && vi.slots[i].queued) held |= 1ull << i;
                    WARN_LIMIT(
                        8, "binder: DEQUEUE_BUFFER found all %d buffers held (slots %llx) for 1 s: releasing them\n",
                        nused, (unsigned long long)held);
                    for (int i = 0; i < 64; i++) vi.slots[i].queued = 0;
                }
            }
            vi.slots[k].queued = 1;
            pm_unlock(&vi.lock);
            pw_u32(out, (u32)k);
            pw_u32(out, 1);
            u32 fence[9];
            memset(fence, 0, sizeof fence);
            fence[1] = 0xFFFFFFFFu;
            for (int i = 1; i < 4; i++) fence[1 + i * 2] = 0xFFFFFFFFu;
            pw_flat(out, fence, 0x24);
            pw_u32(out, 0);
            return 0;
        }
        case 7: { /* QUEUE_BUFFER */
            i32 slot = (i32)pr_u32(in);
            u32 len = pr_u32(in);
            pr_u32(in);
            const u8* qi = pr_ptr(in, len);
            if (!qi) len = 0;
            QueuedFrame* q = calloc(1, sizeof *q);
            q->slot = slot;
            if (len >= 0x54) { /* the frame's fences */
                memcpy(&q->nf, qi + 0x30, 4);
                if (q->nf > 4) q->nf = 4;
                for (u32 i = 0; i < q->nf; i++) {
                    memcpy(&q->id[i], qi + 0x34 + i * 8, 4);
                    memcpy(&q->val[i], qi + 0x38 + i * 8, 4);
                }
            }
            if (len >= 0x28) {
                /* the buffer's crop, which the renderer shows of it (Tears of the Kingdom renders 1280x720 into the
                   corner of a 1920x1080 buffer in handheld mode), and its transform, which it ignores */
                i32 cr[4];
                u32 tr;
                memcpy(cr, qi + 0x0C, 16);
                memcpy(&tr, qi + 0x20, 4);
                pm_lock(&vi.lock);
                if (slot >= 0 && slot < 64) memcpy(vi.slots[slot].crop, cr, 16);
                int changed = vi.crop_transform != tr || memcmp(vi.crop, cr, 16);
                vi.crop_transform = tr;
                memcpy(vi.crop, cr, 16);
                pm_unlock(&vi.lock);
                if (changed)
                    LOG("vi: crop (%d,%d)-(%d,%d), transform %x%s\n", cr[0], cr[1], cr[2], cr[3], tr,
                        tr ? " (not applied)" : "");
            }
            /* the compositor shows the frame once the GPU has drawn it (its acquire waits on the frame's fence), and
               QueueBuffer returns at once: shown here, it held the game's queue lock while the GPU thread caught up,
               so its CPU and GPU work never overlapped */
            gpu_call(present_queued, q);
            u32 qo[4] = {(u32)vi.width, (u32)vi.height, 0, 0};
            pw_bytes(out, qo, 16);
            pw_u32(out, 0);
            return 0;
        }
        case 8: { /* CANCEL_BUFFER: the slot is free again, as after a present */
            i32 slot = (i32)pr_u32(in);
            pm_lock(&vi.lock);
            if (slot >= 0 && slot < 64) vi.slots[slot].queued = 0;
            pc_broadcast(&vi.free_cv);
            pm_unlock(&vi.lock);
            kevent_signal(vi.bufavail_ev);
            pw_u32(out, 0);
            return 0;
        }
        case 9: { /* QUERY: width, height, format, min undequeued buffers; 6/7 the default width and height */
            u32 what = pr_u32(in);
            u32 v = what == 0 || what == 6   ? (u32)vi.width
                    : what == 1 || what == 7 ? (u32)vi.height
                    : what == 2 || what == 3 ? 1
                                             : 0;
            pw_u32(out, v);
            pw_u32(out, 0);
            return 0;
        }
        case 2:
        case 13:
        case 12: pw_u32(out, 0); return 0;
    }
    WARN("binder: code %u not implemented\n", code);
    pw_u32(out, 0);
    return 0;
}

static int hos_binder(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 0:
        case 3: {
            u32 id = in32(c, 0), code = in32(c, 4);
            u64 ia, is, oa, os;
            buf_in(c, 0, &ia, &is);
            buf_out(c, 0, &oa, &os);
            if (!ia || is < 16) {
                WARN("binder: transaction %u without a parcel\n", code);
                return RC(114, 1);
            }
            /* the parcel header {data size, data offset, objects size, objects offset}: the data has to be in the
               buffer */
            PR in;
            u32 hdr[4];
            memcpy(hdr, GP(ia), 16);
            in.d = GP(ia) + (hdr[1] < is ? hdr[1] : is);
            in.size = hdr[1] < is ? (u32)(is - hdr[1] < hdr[0] ? is - hdr[1] : hdr[0]) : 0;
            in.pos = 0;
            pr_skip_token(&in);
            PW* out = calloc(1, sizeof(PW));
            binder_transact(id, code, &in, out);
            pw_out(out, oa, os);
            free(out);
            return 0;
        }
        case 1: return 0;
        case 2: out_copy_h(c, h_new(&vi.bufavail_ev->o)); return 0;
    }
    return stub_ok(o, c);
}

static int sysdisp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 1200: out64(c, 1); return 0;
        case 1202: out64(c, 255); return 0; /* GetZOrderCountMax */
        case 1203:                          /* GetDisplayLogicalResolution: two s32 */
            out32(c, (u32)vi.width);
            out32(c, (u32)vi.height);
            return 0;
        case 3200: {
            out32(c, (u32)vi.width);
            out32(c, (u32)vi.height);
            out32(c, 0x42700000);
            out32(c, 0);
            return 0;
        }
    }
    return stub_ok(o, c);
}
static int mgrdisp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 2010: {
            out64(c, __atomic_fetch_add(&vi.managed_layer, 1, __ATOMIC_RELAXED));
            return 0;
        }
        case 2300: /* AcquireLayerTexturePresentingEvent */
        case 2302: /* GetDisplayHotplugEvent */
        case 2303: /* GetDisplayModeChangedEvent */ out_copy_h(c, h_new(&kevent_new("vi display")->o)); return 0;
    }
    return stub_ok(o, c);
}

static void native_window(IpcCtx* c, u32 binder) {
    u64 a, s;
    buf_out(c, 0, &a, &s);
    if (!a || s < 0x38) {
        WARN("vi: no room for the native window parcel (%llu bytes)\n", (unsigned long long)s);
        return;
    }
    /* HOS Parcel: flat_binder_object {type=2, flags, u64 handle, u64 0, char name[8]="dispdrv", ...} + object table {0} */
    u32 data[10];
    memset(data, 0, sizeof data);
    data[0] = 2;
    data[1] = 1;
    data[2] = binder;
    data[3] = 0;
    memcpy(&data[6], "dispdrv\0", 8);
    u32 hdr[4] = {0x28, 0x10, 4, 0x38};
    u8* p = GP(a);
    memset(p, 0, s < 0x100 ? s : 0x100);
    memcpy(p, hdr, 16);
    memcpy(p + 0x10, data, 0x28);
}
static int appdisp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 100:
        case 103: out_obj(c, svcobj_new("IHOSBinderDriver", hos_binder, 0)); return 0;
        case 101: out_obj(c, svcobj_new("ISystemDisplayService", sysdisp, 0)); return 0;
        case 102: out_obj(c, svcobj_new("IManagerDisplayService", mgrdisp, 0)); return 0;
        case 1000: {
            u64 a, s;
            buf_out(c, 0, &a, &s);
            if (a) {
                u8 info[0x60];
                memset(info, 0, sizeof info);
                strcpy((char*)info, "Default");
                info[0x40] = 1;
                u64 lc = 1, w = (u64)vi.width, h = (u64)vi.height;
                memcpy(info + 0x48, &lc, 8);
                memcpy(info + 0x50, &w, 8);
                memcpy(info + 0x58, &h, 8);
                memcpy(GP(a), info, s < 0x60 ? s : 0x60);
            }
            out64(c, 1);
            return 0;
        }
        case 1010:
        case 1011: out64(c, 1); return 0;
        case 1020:
        case 1101: return 0;
        case 1102:
            out64(c, (u64)vi.width);
            out64(c, (u64)vi.height);
            return 0;
        case 2020: {
            native_window(c, 1);
            out64(c, 0x3C);
            return 0;
        }
        case 2021:
        case 2031: return 0;
        case 2030: {
            out64(c, __atomic_fetch_add(&vi.stray_layer, 1, __ATOMIC_RELAXED));
            native_window(c, 1);
            out64(c, 0x3C);
            return 0;
        }
        case 2101: return 0;
        case 2102: {                                   /* ConvertScalingMode: nn::vi's ScalingMode to the layer's */
            static const u8 conv[5] = {3, 0, 1, 2, 4}; /* None, Freeze, ScaleToWindow, ScaleAndCrop, PreserveAspect */
            u32 m = in32(c, 0);
            if (m >= 5) return RC(114, 1);
            out64(c, conv[m]);
            return 0;
        }
        case 2450:
        case 2451:
            out64(c, 0);
            out64(c, 0);
            return 0;
        case 2460:
            out64(c, 0x1000);
            out64(c, 0x1000);
            return 0;
        case 5202:
        case 5203: out_copy_h(c, h_new(&vi.vsync_ev->o)); return 0;
    }
    return stub_ok(o, c);
}
static int vi_root(SvcObj* o, IpcCtx* c) {
    if (c->cmd <= 3) {
        out_obj(c, svcobj_new("IApplicationDisplayService", appdisp, 0));
        return 0;
    }
    return stub_ok(o, c);
}
static SvcObj* new_vi(void) { return svcobj_new("vi", vi_root, 0); }

void vi_vsync_tick(void) { kevent_signal(vi.vsync_ev); }

void svc_vi_init(void) {
    pm_init(&vi.lock);
    pc_init(&vi.free_cv);
    /* the service keeps these for the whole run: its reference keeps them alive when the game closes its handles */
    vi.vsync_ev = kevent_new("vsync");
    kobj_ref(&vi.vsync_ev->o);
    vi.bufavail_ev = kevent_new("buffer");
    kobj_ref(&vi.bufavail_ev->o);
    kevent_signal(vi.bufavail_ev);
    if (g_opt_docked) {
        vi.width = 1920;
        vi.height = 1080;
    }
    svc_register("vi:u", new_vi);
    svc_register("vi:s", new_vi);
    svc_register("vi:m", new_vi);
}
