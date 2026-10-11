/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
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
/* Guards against a stale owner tag. Code built by old tcc emulated LDXR/STXR with a private spinlock, which
   isn't atomic vs the kernel's writes, so the game could clobber our release and leave a dead owner behind.
   Owner is bogus if the mutex word is 0, the thread exited, or it's itself waiting on this mutex / a condvar on it. */
static int g_heal_n;
static int owner_is_stale_nl(u32 tagv, u64 addr) {
    u32 o = tagv & ~HANDLE_WAIT;
    if (!o) return 1;
    if (o == H_CUR_THREAD) return 0; /* not sure, leave it */
    KObj* ob = h_get_nl(o);
    if (!ob || ob->type != KO_THREAD) return 0; /* same thread; never steal from a real owner */
    KThread* ot = (KThread*)ob;
    if (ot->state == 2) return 1;
    if ((ot->wait_kind == 2 || ot->wait_kind == 3) && ot->wait_addr == addr && wl_has(ot)) return 1;
    return 0;
}
static void heal_note(const char* what, u64 addr, u32 v) {
    if (g_heal_n++ < 20 || !(g_heal_n & 1023))
        WARN("kernel: mutex %llx repaired (%s, value %x) [%d times]\n", (unsigned long long)addr, what, v, g_heal_n);
}
u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag) {
    if (addr & 3) return R_BADADDR;
    pm_lock(&g_klock);
    u32 v = __atomic_load_n(U32(addr), __ATOMIC_SEQ_CST);
    if (v != (owner | HANDLE_WAIT)) {
        pm_unlock(&g_klock);
        return 0;
    }
    if (owner_is_stale_nl(v, addr)) {
        /* already released: hand it to us, keep the waiters bit if anyone else is queued */
        u32 nv = tag | (wl_count(2, addr) ? HANDLE_WAIT : 0);
        if (__atomic_compare_exchange_n(U32(addr), &v, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            heal_note("on request", addr, v);
        pm_unlock(&g_klock);
        return 0;
    } /* caller rereads the word and retries */
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
    pm_lock(&g_klock);
    unlock_nl(maddr);
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
    __atomic_store_n(U32(key), 1, __ATOMIC_SEQ_CST);
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
                    w->wait_kind = 2;
                    wl_add(w);
                    break;
                }
            }
        }
    }
    if (!wl_count(3, key)) __atomic_store_n(U32(key), 0, __ATOMIC_SEQ_CST);
    pm_unlock(&g_klock);
    return 0;
}

/* per-frame sweep for mutex waiters stuck behind a bogus owner, or whose waiters bit got cleared
   (the owner then unlocks without calling us) */
void kernel_heal_mutexes(void) {
    pm_lock(&g_klock);
    for (int guard = 0; guard < 64; guard++) {
        int changed = 0;
        for (KThread* w = g_mwait; w; w = w->next_waiter) {
            if (w->wait_kind != 2) continue;
            u64 a = w->wait_addr;
            u32 v = __atomic_load_n(U32(a), __ATOMIC_SEQ_CST);
            if (owner_is_stale_nl(v, a)) {
                KThread* n = wl_best(2, a);
                if (!n) continue;
                wl_del(n);
                int more = wl_count(2, a) > 0;
                wl_add(n);
                u32 nv = n->wait_tag | (more ? HANDLE_WAIT : 0);
                if (__atomic_compare_exchange_n(U32(a), &v, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    wl_del(n);
                    n->wait_kind = 0;
                    wake(n, 0);
                    heal_note("stale owner", a, v);
                    changed = 1;
                    break;
                }
            } else if (!(v & HANDLE_WAIT)) {
                if (__atomic_compare_exchange_n(U32(a), &v, v | HANDLE_WAIT, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                    heal_note("lost waiter flag", a, v);
            }
        }
        if (!changed) break;
    }
    pm_unlock(&g_klock);
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
        int total = wl_count(4, addr);
        i32 nv;
        if (count <= 0)
            nv = total > 0 ? value - 2 : value + 1;
        else if (total > 0) { /* same as hw kernel: look at up to count+1 waiters */
            int k = 0;
            for (int i = 1; i < total; i++) {
                if (k++ >= count) break;
            }
            nv = k == 0 ? value + 1 : k <= count ? value - 1 : value;
        } else
            nv = value + 1;
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
u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx) {
    if (n > 64) return R_OUTOFRANGE;
    KObj* objs[64];
    u32 hs[64];
    for (u32 i = 0; i < n; i++) hs[i] = gr32(hptr + i * 4);
    pm_lock(&g_klock);
    for (u32 i = 0; i < n; i++) {
        objs[i] = h_get_nl(hs[i]);
        if (!objs[i] || !objs[i]->is_signaled) {
            pm_unlock(&g_klock);
            if (!objs[i]) {
                static u32 nw;
                if (nw++ < 20 || !(nw & 0xFFFF)) WARN("WaitSynchronization: invalid handle %x (%u times)\n", hs[i], nw);
            }
            return R_BADHANDLE;
        }
    }
    if (t->cancel) {
        t->cancel = 0;
        pm_unlock(&g_klock);
        return R_CANCELLED;
    }
    u64 end = timeout < 0 ? 0 : plat_ns() + (u64)timeout;
    for (;;) {
        for (u32 i = 0; i < n; i++)
            if (objs[i]->is_signaled(objs[i])) {
                *out_idx = i;
                pm_unlock(&g_klock);
                return 0;
            }
        if (timeout == 0) {
            pm_unlock(&g_klock);
            return R_TIMEOUT;
        }
        i64 rem = -1;
        if (timeout > 0) {
            u64 now = plat_ns();
            if (now >= end) {
                pm_unlock(&g_klock);
                return R_TIMEOUT;
            }
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
        if (t->cancel) {
            t->cancel = 0;
            pm_unlock(&g_klock);
            return R_CANCELLED;
        }
    }
}
