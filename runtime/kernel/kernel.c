/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* HLE kernel objects: handles, references, waiting and waking, events. Threads are in thread.c, synchronization in
   sync.c, the SVCs in svc.c and svc_mem.c */
#include "kernel/kernel_int.h"

PMutex g_klock;
u64 g_title_id = 0x0100000000001000ull;
u64 g_sysres_size;
u64 g_next_tid = 0x50;

SvcRec g_svc_ring[64];
u32 g_svc_ring_n;

/* ============================================================ handles */
#define MAXH 4096
static KObj* htab[MAXH];
static u16 hgen[MAXH];
static int hnext = 1;
void kobj_ref(KObj* o) {
    if (o) __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
}
void kobj_unref(KObj* o) {
    if (o && __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) == 0 && o->destroy) o->destroy(o);
}
u32 h_new(KObj* o) {
    pm_lock(&g_klock);
    for (int k = 0; k < MAXH; k++) {
        int i = hnext;
        hnext = hnext + 1 >= MAXH ? 1 : hnext + 1;
        if (!htab[i]) {
            htab[i] = o;
            kobj_ref(o);
            hgen[i] = (u16)((hgen[i] + 1) & 0x7FFF);
            if (!hgen[i]) hgen[i] = 1;
            pm_unlock(&g_klock);
            return ((u32)hgen[i] << 15) | (u32)i;
        }
    }
    pm_unlock(&g_klock);
    fatal("no free handles");
    return 0;
}
KObj* h_get_nl(u32 h) {
    if (h == H_CUR_THREAD) return (KObj*)kthread_cur();
    u32 i = h & 0x7FFF;
    if (!i || i >= MAXH || !htab[i] || hgen[i] != (h >> 15)) return NULL;
    return htab[i];
}
KObj* h_get(u32 h) {
    pm_lock(&g_klock);
    KObj* o = h_get_nl(h);
    pm_unlock(&g_klock);
    return o;
}
KObj* h_get_type(u32 h, int t) {
    KObj* o = h_get(h);
    return (o && o->type == t) ? o : NULL;
}
/* the ref keeps the object alive while the caller uses it, even if another thread closes the handle meanwhile */
KObj* h_get_ref(u32 h, int t) {
    pm_lock(&g_klock);
    KObj* o = h_get_nl(h);
    if (o && t && o->type != t) o = NULL;
    kobj_ref(o);
    pm_unlock(&g_klock);
    return o;
}
int h_close(u32 h) {
    pm_lock(&g_klock);
    u32 i = h & 0x7FFF;
    if (!i || i >= MAXH || !htab[i] || hgen[i] != (h >> 15)) {
        pm_unlock(&g_klock);
        return R_BADHANDLE;
    }
    KObj* o = htab[i];
    htab[i] = NULL;
    pm_unlock(&g_klock);
    kobj_unref(o);
    return 0;
}

/* ============================================================ waits */
void waiter_add(KObj* o, KThread* t) {
    if (o->nwait == o->capwait) {
        o->capwait = o->capwait ? o->capwait * 2 : 4;
        o->waiters = realloc(o->waiters, sizeof(KThread*) * o->capwait);
    }
    o->waiters[o->nwait++] = t;
}
void waiter_del(KObj* o, KThread* t) {
    for (int i = 0; i < o->nwait; i++)
        if (o->waiters[i] == t) {
            o->waiters[i] = o->waiters[--o->nwait];
            return;
        }
}
void wake(KThread* t, int result) {
    t->woken = 1;
    t->wait_result = result;
    pc_signal(&t->cv);
}
void kobj_signal_nl(KObj* o) {
    for (int i = 0; i < o->nwait; i++) wake(o->waiters[i], 0);
}
/* call with the lock held. 1 = woken, 0 = timeout */
int sleep_nl(KThread* t, i64 timeout) {
    core_leave(t); /* snx_svc takes the core back on the way out */
    if (timeout < 0) {
        while (!t->woken) pc_wait(&t->cv, &g_klock, -1);
        return 1;
    }
    u64 end = plat_ns() + (u64)timeout;
    while (!t->woken) {
        u64 now = plat_ns();
        if (now >= end) return 0;
        pc_wait(&t->cv, &g_klock, (i64)(end - now));
    }
    return 1;
}

/* ============================================================ events */
static int ev_sig(KObj* o) { return ((KEvent*)o)->signaled; }
static void ev_destroy(KObj* o) {
    free(o->waiters);
    free(o);
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    e->o.type = KO_EVENT;
    e->o.is_signaled = ev_sig;
    e->o.destroy = ev_destroy;
    e->o.name = name;
    return e;
}
void kevent_signal(KEvent* e) {
    pm_lock(&g_klock);
    e->signaled = 1;
    kobj_signal_nl(&e->o);
    pm_unlock(&g_klock);
}
void kevent_clear(KEvent* e) {
    pm_lock(&g_klock);
    e->signaled = 0;
    pm_unlock(&g_klock);
}
