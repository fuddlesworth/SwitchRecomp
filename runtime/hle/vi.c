/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* vi + the nvnflinger buffer queue. QueueBuffer presents immediately, there's no compositor. */
#include "hle/vi.h"
#include "core/main.h"
#include "core/options.h"
#include "gpu/renderer.h"
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
static Slot slots[64];
static KEvent* g_vsync_ev;
static KEvent* g_bufavail_ev;
static PMutex g_vilock;
static int g_connected;
static int g_width = 1280, g_height = 720;
static u64 g_present_count;
static PCond g_free_cv;

/* ============================================================ parcels */
typedef struct {
    u8* d;
    u32 size, pos;
} PR;
static u32 pr_u32(PR* p) {
    u32 v = 0;
    if (p->pos + 4 <= p->size) memcpy(&v, p->d + p->pos, 4);
    p->pos += 4;
    return v;
}
static void pr_skip_token(PR* p) {
    pr_u32(p);
    i32 n = (i32)pr_u32(p);
    if (n > 0) p->pos += ((u32)(n + 1) * 2 + 3) & ~3u;
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
    u8* o = GP(addr);
    u32 hdr[4] = {w->n, 0x10, 0, 0x10 + w->n};
    if (size < 0x10 + w->n) return;
    memcpy(o, hdr, 16);
    memcpy(o + 0x10, w->d, w->n);
}

/* ============================================================ present */
static void present_slot(int i) {
    Slot* s = &slots[i];
    if (!s->used) return;
    vi_show_buffer(s->nvmap_id, s->offset, s->w, s->h, s->pitch, s->layout, s->bhl, s->afmt, s->crop);
    g_present_count++;
}

static int binder_transact(u32 id, u32 code, PR* in, PW* out) {
    (void)id;
    switch (code) {
        case 10: { /* CONNECT */
            g_connected = 1;
            u32 qo[4] = {(u32)g_width, (u32)g_height, 0, 0};
            pw_bytes(out, qo, 16);
            pw_u32(out, 0);
            return 0;
        }
        case 11:
            g_connected = 0;
            pw_u32(out, 0);
            return 0;
        case 14: { /* SET_PREALLOCATED_BUFFER */
            i32 slot = (i32)pr_u32(in);
            u32 has = pr_u32(in);
            if (slot >= 0 && slot < 64) {
                Slot* s = &slots[slot];
                memset(s, 0, sizeof *s);
                if (has) {
                    u32 len = pr_u32(in);
                    pr_u32(in);
                    u8* g = in->d + in->pos;
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
                    u8* nb = g + 0x1C;
                    memcpy(&s->afmt, g + 0x10, 4);
                    memcpy(&s->nvmap_id, nb + 0x10, 4);
                    memcpy(&s->stride, nb + 0x30, 4);
                    u8* pl = nb + 0x40;
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
            }
            pw_u32(out, 0);
            return 0;
        }
        case 1: { /* REQUEST_BUFFER */
            i32 slot = (i32)pr_u32(in);
            Slot* s = (slot >= 0 && slot < 64) ? &slots[slot] : NULL;
            if (s && s->used) {
                pw_u32(out, 1);
                pw_flat(out, s->raw, s->rawsize);
            } else
                pw_u32(out, 0);
            pw_u32(out, 0);
            return 0;
        }
        case 3: { /* DEQUEUE_BUFFER */
            pm_lock(&g_vilock);
            int k = -1;
            for (;;) {
                for (int i = 0; i < 64; i++)
                    if (slots[i].used && !slots[i].queued) {
                        k = i;
                        break;
                    }
                if (k >= 0) break;
                pc_wait(&g_free_cv, &g_vilock, 16000000); /* we release on present */
                for (int i = 0; i < 64; i++) slots[i].queued = 0;
            }
            slots[k].queued = 1;
            pm_unlock(&g_vilock);
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
            /* wait on the frame's fence if the GPU isn't done with it yet */
            u32 len = pr_u32(in);
            pr_u32(in);
            u8* qi = in->d + in->pos;
            if (len >= 0x54) {
                u32 nf;
                memcpy(&nf, qi + 0x30, 4);
                if (nf > 4) nf = 4;
                for (u32 i = 0; i < nf; i++) {
                    u32 id, val;
                    memcpy(&id, qi + 0x34 + i * 8, 4);
                    memcpy(&val, qi + 0x38 + i * 8, 4);
                    if (id != 0xFFFFFFFFu) syncpt_wait(id, val, 100000000);
                }
            }
            if (len >= 0x28) {
                /* the buffer's crop, which the renderer shows of it (Tears of the Kingdom renders 1280x720 into the
                   corner of a 1920x1080 buffer in handheld mode), and its transform, which it ignores */
                i32 cr[4];
                u32 tr;
                memcpy(cr, qi + 0x0C, 16);
                memcpy(&tr, qi + 0x20, 4);
                if (slot >= 0 && slot < 64) memcpy(slots[slot].crop, cr, 16);
                {
                    static u32 lt = 0xFFFFFFFFu;
                    static i32 lc[4];
                    if (lt != tr || memcmp(lc, cr, 16)) {
                        lt = tr;
                        memcpy(lc, cr, 16);
                        LOG("vi: crop (%d,%d)-(%d,%d), transform %x%s\n", cr[0], cr[1], cr[2], cr[3], lt,
                            lt ? " (not applied)" : "");
                    }
                }
            }
            if (slot >= 0 && slot < 64) present_slot(slot);
            pm_lock(&g_vilock);
            if (slot >= 0 && slot < 64) slots[slot].queued = 0;
            pc_broadcast(&g_free_cv);
            pm_unlock(&g_vilock);
            kevent_signal(g_bufavail_ev);
            u32 qo[4] = {(u32)g_width, (u32)g_height, 0, 0};
            pw_bytes(out, qo, 16);
            pw_u32(out, 0);
            return 0;
        }
        case 8: {
            i32 slot = (i32)pr_u32(in);
            pm_lock(&g_vilock);
            if (slot >= 0 && slot < 64) slots[slot].queued = 0;
            pc_broadcast(&g_free_cv);
            pm_unlock(&g_vilock);
            pw_u32(out, 0);
            return 0;
        }
        case 9: {
            u32 what = pr_u32(in);
            u32 v = what == 0 ? (u32)g_width : what == 1 ? (u32)g_height : what == 2 ? 1 : what == 3 ? 1 : 0;
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
            PR in;
            in.d = GP(ia);
            u32 hdr[4];
            memcpy(hdr, in.d, 16);
            in.d += hdr[1];
            in.size = hdr[0];
            in.pos = 0;
            pr_skip_token(&in);
            PW* out = calloc(1, sizeof(PW));
            binder_transact(id, code, &in, out);
            pw_out(out, oa, os);
            free(out);
            return 0;
        }
        case 1: return 0;
        case 2: out_copy_h(c, h_new(&g_bufavail_ev->o)); return 0;
    }
    return stub_ok(o, c);
}

static int sysdisp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 1200: out64(c, 1); return 0;
        case 1202:
        case 1203:
            out64(c, (u64)g_width);
            out64(c, (u64)g_height);
            return 0;
        case 3200: {
            out32(c, (u32)g_width);
            out32(c, (u32)g_height);
            out32(c, 0x42700000);
            out32(c, 0);
            return 0;
        }
    }
    (void)o;
    return 0;
}
static int mgrdisp(SvcObj* o, IpcCtx* c) {
    switch (c->cmd) {
        case 2010: {
            static u64 l = 100;
            out64(c, l++);
            return 0;
        }
    }
    (void)o;
    return 0;
}

static void native_window(IpcCtx* c, u32 binder) {
    u64 a, s;
    buf_out(c, 0, &a, &s);
    if (!a) return;
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
                u64 lc = 1, w = (u64)g_width, h = (u64)g_height;
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
            out64(c, (u64)g_width);
            out64(c, (u64)g_height);
            return 0;
        case 2020: {
            native_window(c, 1);
            out64(c, 0x3C);
            return 0;
        }
        case 2021:
        case 2031: return 0;
        case 2030: {
            static u64 l = 200;
            out64(c, l++);
            native_window(c, 1);
            out64(c, 0x3C);
            return 0;
        }
        case 2101: return 0;
        case 2102: out64(c, in32(c, 0) == 2 ? 1 : 0); return 0;
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
        case 5203: out_copy_h(c, h_new(&g_vsync_ev->o)); return 0;
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

void vi_vsync_tick(void) { kevent_signal(g_vsync_ev); }

void svc_vi_init(void) {
    pm_init(&g_vilock);
    pc_init(&g_free_cv);
    g_vsync_ev = kevent_new("vsync", 0);
    g_bufavail_ev = kevent_new("buffer", 0);
    kevent_signal(g_bufavail_ev);
    if (g_opt_docked) {
        g_width = 1920;
        g_height = 1080;
    }
    svc_register("vi:u", new_vi);
    svc_register("vi:s", new_vi);
    svc_register("vi:m", new_vi);
}
