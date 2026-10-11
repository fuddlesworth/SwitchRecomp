/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* HIPC/CMIF (+domains). Requests are served on the calling thread, no server threads and
   no context switches. */
#include "kernel/ipc.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================ service objects */
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t ds) {
    SvcObj* o = calloc(1, sizeof *o);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    if (ds) o->data = calloc(1, ds);
    return o;
}
void svcobj_unref(SvcObj* o) {
    if (!o) return;
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        if (o->destroy) o->destroy(o);
        free(o->data);
        free(o);
    }
}
static void domain_unref(Domain* d) {
    if (!d) return;
    pm_lock(&d->lk);
    int r = --d->refs;
    pm_unlock(&d->lk);
    if (r) return;
    for (int i = 0; i < d->n; i++)
        if (d->obj[i]) svcobj_unref(d->obj[i]);
    free(d->obj);
    free(d);
}
static void sess_destroy(KObj* k) {
    KSession* s = (KSession*)k;
    svcobj_unref(s->obj);
    domain_unref(s->domain);
    free(k->waiters);
    free(s);
}
static u32 domain_add(Domain* d, SvcObj* o) {
    pm_lock(&d->lk);
    int id = -1;
    for (int i = 1; i < d->n; i++)
        if (!d->obj[i]) {
            id = i;
            break;
        } /* reuse freed ids */
    if (id < 0) {
        if (d->n == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 16;
            d->obj = realloc(d->obj, sizeof(SvcObj*) * d->cap);
        }
        id = d->n++;
    }
    d->obj[id] = o;
    pm_unlock(&d->lk);
    return (u32)id + 1;
}
u32 session_new(SvcObj* obj) {
    KSession* s = calloc(1, sizeof *s);
    s->o.type = KO_SESSION;
    s->o.destroy = sess_destroy;
    s->o.name = "session";
    s->obj = obj;
    return h_new(&s->o);
}

/* only warn the first few times per command, some games call them in a loop */
int ipc_warn_once(const char* name, u32 cmd) {
    static struct {
        const char* n;
        u32 cmd, cnt;
    } t[512];
    static int nt;
    for (int i = 0; i < nt; i++)
        if (t[i].cmd == cmd && !strcmp(t[i].n, name)) return ++t[i].cnt <= 3;
    if (nt < 512) {
        t[nt].n = name;
        t[nt].cmd = cmd;
        t[nt].cnt = 1;
        nt++;
    }
    return 1;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(o->name, c->cmd)) LOG("  [stub] %s cmd %u -> ok\n", o->name, c->cmd);
    return 0;
}

/* ============================================================ registry */
typedef struct {
    char name[16];
    SvcCtor ctor;
} SvcReg;
static SvcReg regs[256];
static int nregs;
void svc_register(const char* n, SvcCtor c) {
    strncpy(regs[nregs].name, n, 15);
    regs[nregs].ctor = c;
    nregs++;
}
static int generic_stub(SvcObj* o, IpcCtx* c) {
    WARN("  [service not implemented] %s cmd %u (replying ok)\n", o->name, c->cmd);
    return 0;
}
SvcObj* svc_open(const char* name) {
    for (int i = 0; i < nregs; i++)
        if (!strcmp(regs[i].name, name)) return regs[i].ctor();
    return NULL;
}

/* ============================================================ buffers */
int buf_a(IpcCtx* c, int i, u64* a, u64* s) {
    if (i >= c->na) {
        *a = 0;
        *s = 0;
        return 0;
    }
    *a = c->a[i].addr;
    *s = c->a[i].size;
    return 1;
}
int buf_b(IpcCtx* c, int i, u64* a, u64* s) {
    if (i >= c->nb) {
        *a = 0;
        *s = 0;
        return 0;
    }
    *a = c->b[i].addr;
    *s = c->b[i].size;
    return 1;
}
int buf_xs(IpcCtx* c, int i, u64* a, u64* s) {
    if (i >= c->nx) {
        *a = 0;
        *s = 0;
        return 0;
    }
    *a = c->x[i].addr;
    *s = c->x[i].size;
    return 1;
}
int buf_cs(IpcCtx* c, int i, u64* a, u64* s) {
    if (i >= c->nc) {
        *a = 0;
        *s = 0;
        return 0;
    }
    *a = c->c[i].addr;
    *s = c->c[i].size;
    return 1;
}
int buf_in(IpcCtx* c, int i, u64* a, u64* s) {
    if (i < c->na && c->a[i].addr && c->a[i].size) return buf_a(c, i, a, s);
    if (i < c->nx && c->x[i].addr) return buf_xs(c, i, a, s);
    if (i < c->na) return buf_a(c, i, a, s);
    *a = 0;
    *s = 0;
    return 0;
}
int buf_out(IpcCtx* c, int i, u64* a, u64* s) {
    if (i < c->nb && c->b[i].addr && c->b[i].size) return buf_b(c, i, a, s);
    if (i < c->nc && c->c[i].addr) return buf_cs(c, i, a, s);
    if (c->nc == 1 && i > 0 && c->c[0].addr) { return 0; }
    if (i < c->nb) return buf_b(c, i, a, s);
    *a = 0;
    *s = 0;
    return 0;
}
void buf_x_out(IpcCtx* c, int i, u64 a, u64 s) {
    if (c->nx_out < MAX_BUFS) {
        c->x_out_addr[c->nx_out] = a;
        c->x_out_size[c->nx_out] = s;
        c->nx_out++;
    }
    (void)i;
}

void out_copy_h(IpcCtx* c, u32 h) { c->out_copy[c->nout_copy++] = h; }
void out_move_h(IpcCtx* c, u32 h) { c->out_move[c->nout_move++] = h; }
void out_obj(IpcCtx* c, SvcObj* o) { c->out_objs[c->nout_objs++] = o; }

char g_ipc_ring[32][80];
u32 g_ipc_ring_n;
/* ============================================================ parsing */
static void parse(IpcCtx* c) {
    u8* m = c->msg;
    u32 w0, w1;
    memcpy(&w0, m, 4);
    memcpy(&w1, m + 4, 4);
    c->type = w0 & 0xFFFF;
    u32 nx = (w0 >> 16) & 15, na = (w0 >> 20) & 15, nb = (w0 >> 24) & 15, nw = (w0 >> 28) & 15;
    u32 ndata = w1 & 0x3FF, cflags = (w1 >> 10) & 15;
    int special = (w1 >> 31) & 1;
    u8* p = m + 8;
    if (special) {
        u32 sh;
        memcpy(&sh, p, 4);
        p += 4;
        c->has_pid = sh & 1;
        c->ncopy = (sh >> 1) & 15;
        c->nmove = (sh >> 5) & 15;
        if (c->has_pid) {
            memcpy(&c->pid, p, 8);
            p += 8;
        }
        for (int i = 0; i < c->ncopy; i++) {
            memcpy(&c->copy_h[i], p, 4);
            p += 4;
        }
        for (int i = 0; i < c->nmove; i++) {
            memcpy(&c->move_h[i], p, 4);
            p += 4;
        }
    }
    c->nx = nx;
    for (u32 i = 0; i < nx; i++) {
        u32 a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        p += 8;
        c->x[i].addr = b | ((u64)((a >> 12) & 15) << 32) | ((u64)((a >> 6) & 7) << 36);
        c->x[i].size = a >> 16;
        c->x_counters[i] = (a & 0x3F) | ((a >> 9) & 7) << 6;
    }
    IpcBuf* lists[3] = {c->a, c->b, c->w};
    u32 cnt[3] = {na, nb, nw};
    for (int k = 0; k < 3; k++)
        for (u32 i = 0; i < cnt[k]; i++) {
            u32 s, a, f;
            memcpy(&s, p, 4);
            memcpy(&a, p + 4, 4);
            memcpy(&f, p + 8, 4);
            p += 12;
            lists[k][i].addr = a | ((u64)((f >> 28) & 15) << 32) | ((u64)((f >> 2) & 7) << 36);
            lists[k][i].size = s | ((u64)((f >> 24) & 15) << 32);
            lists[k][i].flags = f & 3;
        }
    c->na = na;
    c->nb = nb;
    c->nw = nw;
    /* raw data is 16-aligned from the message start */
    u8* raw = m + (((p - m) + 15) & ~15);
    u8* raw_end = p + ndata * 4;
    /* C descriptors follow the data */
    u32 nc = cflags == 0 ? 0 : cflags == 1 ? 0 : cflags == 2 ? 1 : cflags - 2;
    u8* cp = m + 8 + (u32)(p - (m + 8)) + ndata * 4; /* p = data start, not aligned yet */
    cp = raw_end;
    c->nc = nc;
    for (u32 i = 0; i < nc && i < MAX_BUFS; i++) {
        u32 a, b;
        memcpy(&a, cp, 4);
        memcpy(&b, cp + 4, 4);
        cp += 8;
        c->c[i].addr = a | ((u64)(b & 0xFFFF) << 32);
        c->c[i].size = b >> 16;
    }
    /* TIPC (newer SDKs): type is 16+cmd, payload right after the header, no SFCI */
    if (c->type >= 16) {
        c->tipc = 1;
        c->cmd = c->type - 16;
        c->raw = p;
        c->raw_size = 0x100;
        return;
    }
    /* CMIF */
    u8* d = raw;
    c->is_domain = 0;
    if (c->sess && c->sess->is_domain && (c->type == 4 || c->type == 6)) {
        u8 dcmd = d[0];
        u8 nobj = d[1];
        u16 dlen;
        memcpy(&dlen, d + 2, 2);
        memcpy(&c->domain_obj, d + 4, 4);
        c->is_domain = 1 + dcmd; /* 2 = request, 3 = close */
        c->nin_objs = nobj > 8 ? 8 : nobj;
        for (int i = 0; i < c->nin_objs; i++) memcpy(&c->in_objs[i], d + 16 + dlen + i * 4, 4);
        d += 16;
    }
    u32 magic;
    memcpy(&magic, d, 4);
    memcpy(&c->cmd, d + 8, 4);
    memcpy(&c->token, d + 12, 4);
    c->raw = d + 16;
    c->raw_size = ndata * 4 > ((u32)(d + 16 - (m + 8))) ? ndata * 4 : 0x80;
    c->raw_size = 0x100;
    (void)magic;
}

/* ============================================================ reply */
static void respond(IpcCtx* c) {
    u8* m = c->msg;
    memset(m, 0, 0x100);
    int dom = c->is_domain != 0 && !c->tipc;
    u32 nhandles = c->nout_copy + c->nout_move;
    if (!dom) nhandles += c->nout_objs; /* out objects become new session handles */
    u32 w0 = 0;
    w0 |= (u32)(c->nx_out & 15) << 16;
    u8* p = m + 8;
    u32 special = (c->nout_copy || c->nout_move || (!dom && c->nout_objs)) ? 1 : 0;
    u32 hdr_words = 2 + (special ? 1 + nhandles : 0) + c->nx_out * 2;
    u32 pad_bytes = ((hdr_words * 4 + 15) & ~15) - hdr_words * 4;
    /* SFCO(16) + payload; domains add a 16 byte header and object ids */
    u32 payload = (c->out_size + 3) & ~3u;
    u32 data_bytes = 16 + payload + (dom ? 16 + c->nout_objs * 4 : 0);
    u32 ndata = (pad_bytes + data_bytes + 3) / 4 + (16 / 4); /* same slack the real kernel leaves */
    memcpy(m, &w0, 4);
    u32 w1 = ndata | (special ? 0x80000000u : 0);
    memcpy(m + 4, &w1, 4);
    if (special) {
        u32 nmove = c->nout_move + (dom ? 0 : c->nout_objs);
        u32 sh = (c->nout_copy << 1) | (nmove << 5);
        memcpy(p, &sh, 4);
        p += 4;
        for (int i = 0; i < c->nout_copy; i++) {
            memcpy(p, &c->out_copy[i], 4);
            p += 4;
        }
        for (int i = 0; i < c->nout_move; i++) {
            memcpy(p, &c->out_move[i], 4);
            p += 4;
        }
        if (!dom)
            for (int i = 0; i < c->nout_objs; i++) {
                u32 h = session_new(c->out_objs[i]);
                memcpy(p, &h, 4);
                p += 4;
            }
    }
    for (int i = 0; i < c->nx_out; i++) {
        u64 a = c->x_out_addr[i], s = c->x_out_size[i];
        u32 x0 = (u32)(i & 0x3F) | (u32)(((a >> 36) & 7) << 6) | (u32)(((i >> 6) & 7) << 9) |
                 (u32)(((a >> 32) & 15) << 12) | (u32)(s << 16),
            x1 = (u32)a;
        memcpy(p, &x0, 4);
        memcpy(p + 4, &x1, 4);
        p += 8;
    }
    if (c->tipc) { /* TIPC reply: result + payload after the header */
        w1 = (1 + payload / 4) | (special ? 0x80000000u : 0);
        memcpy(m + 4, &w1, 4);
        memcpy(p, &c->rc, 4);
        memcpy(p + 4, c->out, payload);
        return;
    }
    u8* d = m + (((p - m) + 15) & ~15);
    if (dom) {
        u32 n = c->nout_objs;
        memcpy(d, &n, 4);
        d += 16;
    }
    u32 magic = 0x4F434653;
    memcpy(d, &magic, 4);
    u32 ver = 0;
    memcpy(d + 4, &ver, 4);
    memcpy(d + 8, &c->rc, 4);
    memcpy(d + 12, &c->token, 4);
    memcpy(d + 16, c->out, payload);
    if (dom) {
        u8* o = d + 16 + payload;
        for (int i = 0; i < c->nout_objs; i++) {
            u32 id = domain_add(c->sess->domain, c->out_objs[i]);
            memcpy(o + i * 4, &id, 4);
        }
    }
}

static SvcObj* domain_get(KSession* s, u32 id) {
    Domain* d = s->domain;
    if (!d) return NULL;
    pm_lock(&d->lk);
    SvcObj* o = (id && id <= (u32)d->n) ? d->obj[id - 1] : NULL;
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
    pm_unlock(&d->lk);
    return o;
}

void ipc_handle(KThread* t, u32 handle, u64 msg_addr) {
    CPU* cpu = &t->cpu;
    KSession* s = (KSession*)h_get_type(handle, KO_SESSION);
    if (!s) {
        WARN("SendSyncRequest to invalid handle %x (pc=%llx)\n", handle, (unsigned long long)cpu->pc);
        cpu->x[0] = 0xE401;
        return;
    }
    IpcCtx* c = calloc(1, sizeof *c);
    c->th = t;
    c->msg = GP(msg_addr);
    c->msg_addr = msg_addr;
    c->sess = s;
    parse(c);
    cpu->x[0] = 0;
    if (c->type == 2 || c->type == 15) { /* client closes the handle itself afterwards */
        free(c);
        return;
    }
    if (c->type == 5 || c->type == 7) { /* control */
        switch (c->cmd) {
            case 0: {
                s->is_domain = 1;
                if (!s->domain) {
                    Domain* d = calloc(1, sizeof *d);
                    pm_init(&d->lk);
                    d->refs = 1;
                    s->domain = d;
                    s->obj->refs++;
                    domain_add(d, s->obj);
                }
                out32(c, 1);
                break;
            }
            case 2:
            case 4: {
                s->obj->refs++;
                u32 h = session_new(s->obj);
                KSession* ns = (KSession*)h_get_type(h, KO_SESSION);
                if (s->is_domain && s->domain) {
                    ns->is_domain = 1;
                    ns->domain = s->domain;
                    pm_lock(&s->domain->lk);
                    s->domain->refs++;
                    pm_unlock(&s->domain->lk);
                }
                out_move_h(c, h);
                break;
            }
            case 3: out32(c, 0x8000); break;
            default: WARN("control %u\n", c->cmd); break;
        }
        c->is_domain = 0;
        respond(c);
        free(c);
        return;
    }
    SvcObj* obj = s->obj;
    if (c->is_domain) {
        if (c->is_domain == 3) { /* close a domain object */
            Domain* d = s->domain;
            SvcObj* old = NULL;
            if (d) {
                pm_lock(&d->lk);
                if (c->domain_obj && c->domain_obj <= (u32)d->n) {
                    old = d->obj[c->domain_obj - 1];
                    d->obj[c->domain_obj - 1] = NULL;
                }
                pm_unlock(&d->lk);
            }
            if (old) svcobj_unref(old);
            c->is_domain = 1;
            respond(c);
            free(c);
            return;
        }
        obj = domain_get(s, c->domain_obj);
        if (!obj) {
            WARN("invalid domain object %u\n", c->domain_obj);
            c->rc = 0x1D60A;
            respond(c);
            free(c);
            return;
        }
    }
    LOGV("IPC %s cmd %u\n", obj->name, c->cmd);
    c->rc = (u32)obj->dispatch(obj, c);
    {
        u32 i = __atomic_fetch_add(&g_ipc_ring_n, 1, __ATOMIC_RELAXED) & 31;
        u32 in0 = 0, in1 = 0;
        memcpy(&in0, c->raw, 4);
        memcpy(&in1, c->raw + 4, 4);
        snprintf(g_ipc_ring[i], 80, "%s cmd %u (in %x %x) -> %x", obj->name, c->cmd, in0, in1, c->rc);
    }
    respond(c);
    if (c->is_domain) svcobj_unref(obj); /* drop the ref from domain_get */
    free(c);
}
