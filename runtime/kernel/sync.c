/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* synchronization: the kernel side of mutexes and condition variables, address arbitration, WaitSynchronization */
#include "kernel/kernel_int.h"
/* ============================================================ kernel mutex arbitration */
static KThread* g_mwait; /* waiters on a mutex (wait_kind 2), condvar (3) or address (4) */
static void wl_add(KThread* t) {
    t->next_waiter = g_mwait;
    g_mwait = t;
}
static void wl_del(KThread* t) {
    KThread** p = &g_mwait;
    while (*p) {
        if (*p == t) {
            *p = t->next_waiter;
            t->next_waiter = NULL;
            return;
        }
        p = &(*p)->next_waiter;
    }
}
static int wl_has(KThread* t) {
    for (KThread* w = g_mwait; w; w = w->next_waiter)
        if (w == t) return 1;
    return 0;
}
/* best priority first (lowest number), FIFO within */
static KThread* wl_best(int kind, u64 key) {
    KThread* best = NULL;
    for (KThread* w = g_mwait; w; w = w->next_waiter) {
        if (w->wait_kind != kind) continue;
        if ((kind == 2 && w->wait_addr != key) || (kind == 3 && w->wait_key != key) ||
            (kind == 4 && w->wait_addr != key))
            continue;
        if (!best || w->prio <= best->prio) best = w; /* list is reversed so <= picks the oldest */
    }
    return best;
}
static int wl_count(int kind, u64 key) {
    int n = 0;
    for (KThread* w = g_mwait; w; w = w->next_waiter)
        if (w->wait_kind == kind && ((kind == 3 ? w->wait_key : w->wait_addr) == key)) n++;
    return n;
}

static u32* U32(u64 a) { return (u32*)GP(a); }
static void unlock_nl(u64 addr) {
    KThread* n = wl_best(2, addr);
    if (!n) {
        __atomic_store_n(U32(addr), 0, __ATOMIC_SEQ_CST);
        return;
    }
    wl_del(n);
    n->wait_kind = 0;
    int more = wl_count(2, addr) > 0;
    __atomic_store_n(U32(addr), n->wait_tag | (more ? HANDLE_WAIT : 0), __ATOMIC_SEQ_CST);
    wake(n, 0);
}
/* the thread a mutex tag names. Like the console's handle table lookup without pseudo handles: the tag holds a real
   thread handle, so the current-thread pseudo handle, a closed handle or another object's handle name no owner */
static KThread* tag_owner_nl(u32 tag) {
    u32 h = tag & ~HANDLE_WAIT;
    if (h == H_CUR_THREAD) return NULL;
    KObj* o = h_get_nl(h);
    return o && o->type == KO_THREAD ? (KThread*)o : NULL;
}
u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag) {
    if (addr & 3) return R_BADADDR;
    pm_lock(&g_klock);
    u32 v = __atomic_load_n(U32(addr), __ATOMIC_SEQ_CST);
    if (v != (owner | HANDLE_WAIT)) { /* caller rereads the word and retries */
        pm_unlock(&g_klock);
        return 0;
    }
    if (!tag_owner_nl(owner)) { /* waiting on it would never end: the console answers InvalidHandle */
        pm_unlock(&g_klock);
        return R_BADHANDLE;
    }
    t->wait_kind = 2;
    t->wait_addr = addr;
    t->wait_tag = tag;
    t->woken = 0;
    wl_add(t);
    sleep_nl(t, -1);
    u32 r = t->wait_result;
    pm_unlock(&g_klock);
    return r;
}
u32 svc_arbitrate_unlock(u64 addr) {
    if (addr & 3) return R_BADADDR;
    pm_lock(&g_klock);
    unlock_nl(addr);
    pm_unlock(&g_klock);
    return 0;
}
u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout) {
    if (maddr & 3) return R_BADADDR;
    pm_lock(&g_klock);
    unlock_nl(maddr);
    /* the console marks the condition variable as having waiters before it looks at the timeout */
    __atomic_store_n(U32(key), 1, __ATOMIC_SEQ_CST);
    if (timeout == 0) {
        pm_unlock(&g_klock);
        return R_TIMEOUT;
    }
    t->wait_kind = 3;
    t->wait_key = key;
    t->wait_addr = maddr;
    t->wait_tag = tag;
    t->woken = 0;
    wl_add(t);
    int ok = sleep_nl(t, timeout);
    u32 r;
    if (!ok) {
        if (wl_has(t)) { wl_del(t); }
        t->wait_kind = 0;
        r = R_TIMEOUT;
        if (!wl_count(3, key)) __atomic_store_n(U32(key), 0, __ATOMIC_SEQ_CST);
    } else
        r = t->wait_result;
    pm_unlock(&g_klock);
    return r;
}
u32 svc_signal_cv(u64 key, i32 count) {
    pm_lock(&g_klock);
    int n = 0;
    while (count <= 0 || n < count) {
        KThread* w = wl_best(3, key);
        if (!w) break;
        wl_del(w);
        n++;
        u64 ma = w->wait_addr;
        for (;;) {
            u32 v = __atomic_load_n(U32(ma), __ATOMIC_SEQ_CST);
            if (v == 0) {
                u32 z = 0;
                if (__atomic_compare_exchange_n(U32(ma), &z, w->wait_tag, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    w->wait_kind = 0;
                    wake(w, 0);
                    break;
                }
            } else {
                if (__atomic_compare_exchange_n(U32(ma), &v, v | HANDLE_WAIT, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    if (tag_owner_nl(v)) {
                        w->wait_kind = 2;
                        wl_add(w);
                    } else { /* the mutex names no thread, so nobody would hand it over: InvalidState, as on console */
                        w->wait_kind = 0;
                        wake(w, R_BADSTATE);
                    }
                    break;
                }
            }
        }
    }
    if (!wl_count(3, key)) __atomic_store_n(U32(key), 0, __ATOMIC_SEQ_CST);
    pm_unlock(&g_klock);
    return 0;
}

/* ============================================================ address arbitration */
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i32 value, i64 timeout) {
    if (addr & 3) return R_BADADDR;
    pm_lock(&g_klock);
    i32* p = (i32*)GP(addr);
    i32 cur = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    switch (type) {
        case 0:
            if (cur >= value) {
                pm_unlock(&g_klock);
                return R_BADSTATE;
            }
            break;
        case 1:
            for (;;) {
                if (cur >= value) {
                    pm_unlock(&g_klock);
                    return R_BADSTATE;
                }
                if (__atomic_compare_exchange_n(p, &cur, cur - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
            }
            break;
        case 2:
            if (cur != value) {
                pm_unlock(&g_klock);
                return R_BADSTATE;
            }
            break;
        default: pm_unlock(&g_klock); return R_BADENUM;
    }
    if (timeout == 0) {
        pm_unlock(&g_klock);
        return R_TIMEOUT;
    }
    t->wait_kind = 4;
    t->wait_addr = addr;
    t->woken = 0;
    wl_add(t);
    int ok = sleep_nl(t, timeout);
    u32 r = ok ? t->wait_result : R_TIMEOUT;
    if (!ok && wl_has(t)) wl_del(t);
    t->wait_kind = 0;
    pm_unlock(&g_klock);
    return r;
}
static void wake_addr_nl(u64 addr, i32 count) {
    int n = 0;
    while (count <= 0 || n < count) {
        KThread* w = wl_best(4, addr);
        if (!w) break;
        wl_del(w);
        w->wait_kind = 0;
        wake(w, 0);
        n++;
    }
}
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count) {
    if (addr & 3) return R_BADADDR;
    {
        static int said[3];
        if (type < 3 && !said[type]) {
            said[type] = 1;
            LOG("kernel: SignalToAddress type %u in use\n", type);
        }
    }
    pm_lock(&g_klock);
    i32* p = (i32*)GP(addr);
    if (type == 1) {
        i32 exp = value;
        if (!__atomic_compare_exchange_n(p, &exp, value + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            pm_unlock(&g_klock);
            return R_BADSTATE;
        }
    } else if (type == 2) {
        /* SignalAndModifyByWaitingCountIfEqual, as the console's kernel computes it: +1 with nobody waiting, -1 when
           this signal wakes every waiter (count <= 0 wakes them all), unchanged when some are left waiting */
        int total = wl_count(4, addr);
        i32 nv = total == 0 ? value + 1 : count <= 0 ? value - 1 : total <= count ? value - 1 : value;
        i32 exp = value;
        if (nv != value) {
            if (!__atomic_compare_exchange_n(p, &exp, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                pm_unlock(&g_klock);
                return R_BADSTATE;
            }
        } else if (__atomic_load_n(p, __ATOMIC_SEQ_CST) != value) {
            pm_unlock(&g_klock);
            return R_BADSTATE;
        }
    } else if (type != 0) {
        pm_unlock(&g_klock);
        return R_BADENUM;
    }
    wake_addr_nl(addr, count);
    pm_unlock(&g_klock);
    return 0;
}

/* ============================================================ WaitSynchronization */
/* the objects hold a ref for the whole wait, so a handle closed meanwhile by another thread doesn't free what this
   thread sits in the waiter list of. The refs are dropped after unlocking: destroying an object doesn't happen under
   g_klock */
static u32 wait_sync_nl(KThread* t, KObj** objs, u32 n, i64 timeout, u32* out_idx) {
    u64 end = timeout < 0 ? 0 : plat_ns() + (u64)timeout;
    for (;;) {
        /* the console's order: a signaled object wins, then a zero timeout, then a pending cancel */
        for (u32 i = 0; i < n; i++)
            if (objs[i]->is_signaled(objs[i])) {
                *out_idx = i;
                return 0;
            }
        if (timeout == 0) return R_TIMEOUT;
        if (t->cancel) {
            t->cancel = 0;
            return R_CANCELLED;
        }
        i64 rem = -1;
        if (timeout > 0) {
            u64 now = plat_ns();
            if (now >= end) return R_TIMEOUT;
            rem = (i64)(end - now);
        }
        t->woken = 0;
        t->wait_kind = 1;
        t->wait_name = n ? objs[0]->name : "no handles";
        t->wait_timeout = timeout; /* n==0 is legal: sleep until timeout/cancel */
        for (u32 i = 0; i < n; i++) waiter_add(objs[i], t);
        sleep_nl(t, rem);
        for (u32 i = 0; i < n; i++) waiter_del(objs[i], t);
        t->wait_kind = 0;
        if (t->cancel) { /* svcCancelSynchronization ended this wait */
            t->cancel = 0;
            return R_CANCELLED;
        }
    }
}
u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx) {
    if (n > 64) return R_OUTOFRANGE;
    KObj* objs[64];
    u32 hs[64];
    for (u32 i = 0; i < n; i++) hs[i] = gr32(hptr + i * 4);
    u32 got = 0, r;
    pm_lock(&g_klock);
    for (; got < n; got++) {
        objs[got] = h_get_nl(hs[got]);
        if (!objs[got] || !objs[got]->is_signaled) break;
        kobj_ref(objs[got]);
    }
    if (got < n) {
        if (!objs[got]) {
            static u32 nw;
            if (nw++ < 20 || !(nw & 0xFFFF)) WARN("WaitSynchronization: invalid handle %x (%u times)\n", hs[got], nw);
        }
        r = R_BADHANDLE;
    } else
        r = wait_sync_nl(t, objs, n, timeout, out_idx);
    pm_unlock(&g_klock);
    for (u32 i = 0; i < got; i++) kobj_unref(objs[i]);
    return r;
}
