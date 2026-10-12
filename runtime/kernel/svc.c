/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the SVC dispatch: each supervisor call the game makes, by number */
#include "kernel/kernel_int.h"
static void snx_svc_(CPU* c, u32 imm);
/* svcCreateEvent's writable end: SignalEvent and ClearEvent take it, WaitSynchronization doesn't */
typedef struct {
    KObj o;
    KEvent* ev;
} KWEvent;
static void wev_destroy(KObj* o) {
    kobj_unref(&((KWEvent*)o)->ev->o);
    free(o->waiters);
    free(o);
}
/* ============================================================ pausing threads (svcSetThreadActivity) */
/* The console stops a paused thread wherever it is. Recompiled code only enters the kernel through SVCs, so a thread
   stops on its way into one (before the SVC does anything) or out of it (before the game sees its results). A thread
   blocked inside one stays blocked, and if its wait ends while it is paused it doesn't go back to the game until it is
   resumed, as on the console. in_svc tells the pausing thread the target runs no game code: it is set before the
   target reads suspended, and cleared before it reads it again on the way out, so with both sides sequentially
   consistent either the target sees the pause or the pausing thread sees it inside the SVC */
static void park_nl(KThread* t, int at) {
    if (!t->suspended) return;
    t->pause_at = at;
    for (int i = 0; i < t->o.nwait; i++) /* the threads waiting for it to stop (wait_stopped_nl) */
        if (t->o.waiters[i]->wait_kind == 5) wake(t->o.waiters[i], 0);
    core_leave(t); /* snx_svc takes the core back on the way out */
    while (t->suspended) pc_wait(&t->cv, &g_klock, -1);
    t->pause_at = 0;
}
static void svc_enter(KThread* t) {
    __atomic_store_n(&t->in_svc, 1, __ATOMIC_SEQ_CST);
    if (UNLIKELY(__atomic_load_n(&t->suspended, __ATOMIC_SEQ_CST))) {
        pm_lock(&g_klock);
        park_nl(t, 1);
        pm_unlock(&g_klock);
    }
}
/* on the way out the thread takes its core back (if the SVC let go of it) while still inside the SVC, where
   svcSetThreadActivity doesn't wait for it: a thread pausing it from the same core would otherwise wait for it to run */
static void svc_leave(KThread* t) {
    for (;;) {
        core_enter(t);
        core_preempt(t);
        __atomic_store_n(&t->in_svc, 0, __ATOMIC_SEQ_CST);
        if (LIKELY(!__atomic_load_n(&t->suspended, __ATOMIC_SEQ_CST))) return;
        pm_lock(&g_klock);
        __atomic_store_n(&t->in_svc, 1, __ATOMIC_SEQ_CST);
        park_nl(t, 2);
        pm_unlock(&g_klock);
    }
}
/* SetThreadActivity returns once the thread no longer runs (the console waits until no core runs it), and
   GetThreadContext3 doesn't read registers the thread is still changing: both wait here, in n's waiter list, until n is
   inside an SVC, has exited, or was resumed meanwhile */
static void wait_stopped_nl(KThread* self, KThread* n) {
    while (n->suspended && n->state != 2 && !__atomic_load_n(&n->in_svc, __ATOMIC_SEQ_CST)) {
        self->woken = 0;
        self->wait_kind = 5;
        self->wait_name = "a thread to pause";
        self->wait_timeout = -1;
        waiter_add(&n->o, self);
        sleep_nl(self, -1);
        waiter_del(&n->o, self);
        self->wait_kind = 0;
    }
}
/* svcGetThreadContext3's ThreadContext, as the console's GetUserContext fills it. A thread paused before or after an
   SVC is in the game's code (at the svc instruction, or past it), so every register is its own. One blocked inside an
   SVC shows what the kernel keeps of it: x19-x28 and v8-v15, with pc at the svc instruction */
static void thread_context_nl(const KThread* n, u8* p) {
    const CPU* c = &n->cpu;
    int inside = n->pause_at == 0;
    u64 pc = n->pause_at == 2 ? c->pc : c->pc - 4; /* the generated code sets pc past the svc before calling it */
    u32 pstate = get_nzcv(c);
    memset(p, 0, 0x320);
    if (n->state == 2) return; /* an exited thread has no context: zeros, as on console */
    if (inside)
        memcpy(p + 19 * 8, &c->x[19], 10 * 8);
    else
        memcpy(p, c->x, 29 * 8);
    memcpy(p + 0xE8, &c->x[29], 3 * 8); /* fp, lr, sp */
    memcpy(p + 0x100, &pc, 8);
    memcpy(p + 0x108, &pstate, 4);
    if (inside)
        memcpy(p + 0x110 + 8 * 16, &c->v[8], 8 * 16);
    else
        memcpy(p + 0x110, c->v, 32 * 16);
    memcpy(p + 0x310, &c->fpcr, 4);
    memcpy(p + 0x314, &c->fpsr, 4);
    memcpy(p + 0x318, &c->tpidr, 8);
}

void snx_svc(CPU* c, u32 imm) {
    u64 a0 = c->x[0], a1 = c->x[1], a2 = c->x[2];
    u64 a3 = c->x[3];
    KThread* t = kthread_cur();
    svc_enter(t);
    snx_svc_(c, imm);
    if (imm != 0x1E && imm != 0x0B && imm != 0x10) {
        u32 i = __atomic_fetch_add(&g_svc_ring_n, 1, __ATOMIC_RELAXED) & 63;
        SvcRec* r = &g_svc_ring[i];
        r->imm = imm;
        r->lr = c->x[30];
        r->a[0] = a0;
        r->a[1] = a1;
        r->a[2] = a2;
        r->a[3] = a3;
        r->res = (u32)c->x[0];
        r->o1 = c->x[1];
    }
    static u32 nbad;
    /* GetSystemTick, SleepThread and GetCurrentProcessorNumber don't return a result in x0 */
    if ((u32)c->x[0] == R_BADHANDLE && imm != 0x16 && imm != 0x1E && imm != 0x0B && imm != 0x10 &&
        (nbad++ < 20 || !(nbad & 0xFFFF)))
        WARN("SVC 0x%x with invalid handle (args %llx %llx %llx) pc=%llx\n", imm, (unsigned long long)a0,
             (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)c->pc);
    svc_leave(t);
}
static void snx_svc_(CPU* c, u32 imm) {
    KThread* t = kthread_cur();
    u64* x = c->x;
    switch (imm) {
        case 0x01:
            x[0] = svc_set_heap_size(x[1]);
            if (!x[0]) x[1] = HEAP_BASE;
            return;
        case 0x02:
            mem_set_state(x[0], x[1], 0xFFFFFFFFu, (u32)x[2]);
            x[0] = 0;
            return;
        case 0x03:
            mem_set_attr(x[0], x[1], (u32)x[2], (u32)x[3]);
            x[0] = 0;
            return;
        case 0x04: x[0] = svc_map_memory(x[0], x[1], x[2]); return;
        case 0x05: x[0] = svc_unmap_memory(x[0], x[1], x[2]); return;
        case 0x06:
            x[0] = svc_query(x[0], x[2]);
            x[1] = 0;
            return;
        case 0x07: process_exit(0); return;
        case 0x08: {
            int prio = (int)(u32)x[4], core = (int)(i32)(u32)x[5];
            if (core == -2) core = g_default_core;
            /* the console's checks: a core and a priority the npdm gives the process */
            if (core < 0 || core >= 64 || !((g_core_mask >> core) & 1)) {
                x[0] = R_BADCORE;
                return;
            }
            if (prio < 0 || prio >= 64 || !((g_prio_mask >> prio) & 1)) {
                x[0] = R_BADPRIO;
                return;
            }
            KThread* n = kthread_create(x[1], x[2], x[3], prio, core);
            u32 h = h_new(&n->o);
            LOGV("CreateThread entry=%llx stack=%llx prio=%d core=%d -> %x\n", (unsigned long long)x[1],
                 (unsigned long long)n->stack_top, prio, core, h);
            x[0] = 0;
            x[1] = h;
            return;
        }
        case 0x09: {
            KThread* n = (KThread*)h_get_type((u32)x[0], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            if (n->state == 0) kthread_start(n);
            x[0] = 0;
            return;
        }
        case 0x0A: svc_exit_thread(t); return;
        case 0x0B: {
            i64 ns = (i64)x[0];
            t->sleep_lr = x[30];
            t->sleep_n++;
            memcpy(t->sleep_x, x, sizeof t->sleep_x);
            if (ns <= 0) {
                /* a yield (0, -1, -2). On the console it returns at once when nothing else waits for the core, and
                   Tears of the Kingdom's frame tasks yield between every work item. sched_yield can give up the
                   rest of a time slice to any thread sharing the host CPU, about 2 ms each here, which made its
                   loading take minutes. A yield here hands the guest core to a waiting thread, if one waits, and
                   returns once it has the core back. Every 64th yield of a thread is still a host one, so a spin loop
                   can't starve a thread on its host CPU */
                core_yield(t); /* threads waiting for this core go first */
                if (!(++t->yields & 63)) plat_yield();
            } else {
                core_leave(t);
                plat_sleep_ns(ns);
            }
            return;
        }
        case 0x0C: {
            KThread* n = (KThread*)h_get_type((u32)x[1], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            x[0] = 0;
            x[1] = (u32)n->prio;
            return;
        }
        case 0x0D: {
            KThread* n = (KThread*)h_get_type((u32)x[0], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            u32 prio = (u32)x[1];
            if (prio >= 64 || !((g_prio_mask >> prio) & 1)) {
                x[0] = R_BADPRIO;
                return;
            }
            pm_lock(&g_klock); /* the waiter lists pick by priority under it */
            core_set_prio(n, (int)prio);
            pm_unlock(&g_klock);
            x[0] = 0;
            return;
        }
        case 0x0E: {
            KThread* n = (KThread*)h_get_type((u32)x[2], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            x[0] = 0;
            x[1] = (u32)n->ideal_core;
            x[2] = n->affinity;
            return;
        }
        case 0x0F: { /* SetThreadCoreMask: core -1 = don't care, -2 = the process's default, -3 = keep the ideal core */
            KThread* n = (KThread*)h_get_type((u32)x[0], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            i32 core = (i32)(u32)x[1];
            u64 mask = x[2];
            /* the console's checks: a mask within the process's cores, and a core in it */
            if (core == -2) {
                core = g_default_core;
                mask = 1ull << core;
            } else if ((mask | g_core_mask) != g_core_mask) {
                x[0] = R_BADCORE;
                return;
            } else if (!mask) {
                x[0] = R_BADCOMBO;
                return;
            } else if (core >= NCORES || core < -3) {
                x[0] = R_BADCORE;
                return;
            }
            pm_lock(&g_klock); /* n may be another thread, changing its own mask */
            if (core == -3) core = n->ideal_core;
            if (core >= 0 && !((mask >> core) & 1)) {
                pm_unlock(&g_klock);
                x[0] = R_BADCOMBO;
                return;
            }
            n->ideal_core = core;
            n->affinity = mask;
            /* the thread moves to its ideal core, or with no ideal core and a mask that leaves its core out, to the
               highest core the mask allows. Tears of the Kingdom moves its render workers with (-1, mask 4) and keeps
               per-core command memory, indexed by svcGetCurrentProcessorNumber: a worker still reporting core 0 there
               records over the commands of the thread on core 0 */
            if (core >= 0)
                n->core = core;
            else if (!((mask >> n->core) & 1))
                n->core = 63 - __builtin_clzll(mask);
            pm_unlock(&g_klock);
            if (n == t && t->on_core >= 0 && t->on_core != n->core) core_leave(t); /* moves on the way out */
            x[0] = 0;
            return;
        }
        case 0x10: /* the core it holds: after a new core mask it moves there only when it next lets go */
            x[0] = (u32)(t ? (t->on_core >= 0 ? t->on_core : t->core) : 0);
            return;
        case 0x11: { /* SignalEvent: the writable end only */
            KWEvent* w = (KWEvent*)h_get_ref((u32)x[0], KO_WEVENT);
            if (!w) {
                x[0] = R_BADHANDLE;
                return;
            }
            kevent_signal(w->ev);
            kobj_unref(&w->o);
            x[0] = 0;
            return;
        }
        case 0x12:   /* ClearEvent: either end */
        case 0x17: { /* ResetSignal: the readable end, and an error when it wasn't signaled */
            KObj* o = h_get_ref((u32)x[0], 0);
            KEvent* e = !o                                    ? NULL
                        : o->type == KO_EVENT                 ? (KEvent*)o
                        : o->type == KO_WEVENT && imm == 0x12 ? ((KWEvent*)o)->ev
                                                              : NULL;
            if (e) {
                pm_lock(&g_klock);
                int was = e->signaled;
                e->signaled = 0;
                pm_unlock(&g_klock);
                x[0] = (imm == 0x17 && !was) ? R_BADSTATE : 0;
            } else
                x[0] = R_BADHANDLE;
            kobj_unref(o);
            return;
        }
        case 0x13: x[0] = svc_map_shm((u32)x[0], x[1], x[2], (u32)x[3]); return;
        case 0x14: x[0] = svc_unmap_shm((u32)x[0], x[1], x[2]); return;
        case 0x15: { /* CreateTransferMemory: the source stays locked, with the permission given, until it's closed */
            if (x[1] & 0xFFF) {
                x[0] = R_BADADDR;
                return;
            }
            if (!x[2] || (x[2] & 0xFFF)) {
                x[0] = R_BADSIZE;
                return;
            }
            KTmem* m = calloc(1, sizeof *m);
            m->o.type = KO_TMEM;
            m->o.destroy = tmem_destroy;
            m->o.name = "tmem";
            m->addr = x[1];
            m->size = x[2];
            m->perm = (u32)x[3];
            mem_set_attr(m->addr, m->size, 1, 1);
            mem_set_state(m->addr, m->size, 0xFFFFFFFFu, m->perm & PERM_RW);
            x[0] = 0;
            x[1] = h_new(&m->o);
            return;
        }
        case 0x16: x[0] = (u32)h_close((u32)x[0]); return; /* pseudo handles too are InvalidHandle, as on console */
        case 0x18: {
            u32 idx = 0;
            u32 r = svc_wait_sync(t, x[1], (u32)x[2], (i64)x[3], &idx);
            x[0] = r;
            x[1] = idx;
            return;
        }
        case 0x19: {
            KThread* n = (KThread*)h_get_type((u32)x[0], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            pm_lock(&g_klock);
            if (n->wait_kind == 1) wake(n, R_CANCELLED);
            n->cancel = 1;
            pm_unlock(&g_klock);
            x[0] = 0;
            return;
        }
        case 0x1A: x[0] = svc_arbitrate_lock(t, (u32)x[0], x[1], (u32)x[2]); return;
        case 0x1B: x[0] = svc_arbitrate_unlock(x[0]); return;
        case 0x1C: x[0] = svc_wait_cv(t, x[0], x[1], (u32)x[2], (i64)x[3]); return;
        case 0x1D: x[0] = svc_signal_cv(x[0], (i32)(u32)x[1]); return;
        case 0x1E: x[0] = c->rt->ticks(); return;
        case 0x1F: {
            char name[16] = {0};
            memcpy(name, GP(x[1]), 12);
            if (!strcmp(name, "sm:")) {
                SvcObj* o = svc_open("sm:");
                x[0] = 0;
                x[1] = session_new(o);
                return;
            }
            WARN("ConnectToNamedPort(%s) unknown\n", name);
            x[0] = R_NOTFOUND;
            return;
        }
        case 0x21: /* the caller waits for the reply off its core */
            core_leave(t);
            ipc_handle(t, (u32)x[0], t->tls);
            return;
        case 0x22:
            core_leave(t);
            ipc_handle(t, (u32)x[2], x[0]);
            return;
        case 0x24:
            x[0] = 0;
            x[1] = 0x51;
            return;
        case 0x25: {
            KThread* n = (KThread*)h_get_type((u32)x[1], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            x[0] = 0;
            x[1] = n->id;
            return;
        }
        case 0x26: {
            u32 reason = (u32)x[0];
            if (reason & 0x80000000u) {
                LOGV("svcBreak (notice) %x\n", reason);
                x[0] = 0;
                return;
            }
            WARN("\n*** The game called svcBreak (reason=%x, info=%llx size=%llx) ***\n", reason,
                 (unsigned long long)x[1], (unsigned long long)x[2]);
            if (x[1] && x[2] >= 4 && x[2] < 0x1000) {
                u8 tmp[64];
                memcpy(tmp, GP(x[1]), x[2] < 64 ? x[2] : 64);
                WARN("  data: ");
                for (u64 i = 0; i < (x[2] < 64 ? x[2] : 64); i++) WARN("%02x", tmp[i]);
                WARN("\n");
            }
            kernel_dump_regs(c);
            /* strings pointed to by regs, often the abort message */
            for (int r = 0; r < 31; r++) {
                u64 a = c->x[r];
                if (a < 0x10000 || a >= AS_SIZE - 256) continue;
                MemBlock b;
                mem_query(a, &b);
                if (b.state == MS_FREE || b.state == MS_INACCESSIBLE) continue;
                u64 room = b.addr + b.size - a; /* don't read past the mapping */
                int max = room < 200 ? (int)room : 200;
                const char* p = (const char*)GP(a);
                int n = 0;
                while (n < max && p[n] >= 32 && p[n] < 127) n++;
                if (n >= 4 && (n == max || p[n] == 0)) WARN("  x%d -> \"%.*s\"\n", r, n, p);
            }
            fatal("The game stopped (svcBreak). See the .log for details.");
            return;
        }
        case 0x27: {
            u64 n = x[1];
            if (n > 4096) n = 4096;
            char buf[4100];
            memcpy(buf, GP(x[0]), n);
            buf[n] = 0;
            LOG("[game] %s%s", buf, (n && buf[n - 1] == '\n') ? "" : "\n");
            x[0] = 0;
            return;
        }
        case 0x29: {
            u64 o = 0;
            x[0] = svc_get_info((u32)x[1], (u32)x[2], x[3], &o);
            x[1] = o;
            return;
        }
        case 0x2A:
        case 0x2B:
        case 0x5D:
        case 0x5E:
        case 0x5F:
        case 0x36: x[0] = 0; return;
        case 0x2C: x[0] = svc_map_physical(x[0], x[1]); return;
        case 0x2D: x[0] = svc_unmap_physical(x[0], x[1]); return;
        case 0x2E: x[0] = R_NOTFOUND; return;
        case 0x2F: x[0] = R_NOTFOUND; return;
        case 0x30:
        case 0x37: {
            u32 which = (u32)x[2];
            x[0] = 0;
            x[1] = which == 0 ? mem_total() : which == 1 ? THREAD_MAX : which == 2 ? 800 : which == 3 ? 256 : 64;
            return;
        }
        case 0x31: {
            u32 which = (u32)x[2];
            x[0] = 0;
            x[1] = which == 0 ? used_memory() : which == 1 ? (u64)kthread_live_count() : 16;
            return;
        }
        case 0x32: { /* SetThreadActivity: 1 pauses another thread, 0 resumes it (the console's checks, in its order) */
            u32 act = (u32)x[1];
            if (act > 1) {
                x[0] = R_BADENUM;
                return;
            }
            KThread* n = (KThread*)h_get_type((u32)x[0], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            if (n == t) {
                x[0] = R_BUSY;
                return;
            }
            pm_lock(&g_klock);
            if (n->state != 1 || n->suspended == (int)act) /* not started or exited, or already paused/running */
                x[0] = R_BADSTATE;
            else {
                __atomic_store_n(&n->suspended, (int)act, __ATOMIC_SEQ_CST);
                if (act)
                    wait_stopped_nl(t, n);
                else
                    pc_signal(&n->cv); /* out of park_nl. A thread blocked in a wait just checks it again */
                x[0] = 0;
            }
            pm_unlock(&g_klock);
            return;
        }
        case 0x33: { /* GetThreadContext3: only of a paused thread, whose registers hold still */
            KThread* n = (KThread*)h_get_type((u32)x[1], KO_THREAD);
            if (!n) {
                x[0] = R_BADHANDLE;
                return;
            }
            if (n == t) {
                x[0] = R_BUSY;
                return;
            }
            u8 ctx[0x320];
            pm_lock(&g_klock);
            if (n->suspended) wait_stopped_nl(t, n); /* paused by another thread whose SetThreadActivity still waits */
            u32 r = n->suspended ? 0 : R_BADSTATE;
            if (!r) thread_context_nl(n, ctx);
            pm_unlock(&g_klock);
            if (!r) memcpy(GP(x[0]), ctx, sizeof ctx);
            x[0] = r;
            return;
        }
        case 0x34: x[0] = svc_wait_addr(t, x[0], (u32)x[1], (i32)(u32)x[2], (i64)x[3]); return;
        case 0x35: x[0] = svc_signal_addr(x[0], (u32)x[1], (i32)(u32)x[2], (i32)(u32)x[3]); return;
        case 0x45: {
            KEvent* e = kevent_new("user event");
            KWEvent* we = calloc(1, sizeof *we);
            we->o.type = KO_WEVENT;
            we->o.destroy = wev_destroy;
            we->o.name = "user event (writable)";
            we->ev = e;
            kobj_ref(&e->o);
            x[0] = 0;
            x[1] = h_new(&we->o);
            x[2] = h_new(&e->o);
            return;
        }
        case 0x4B: {
            x[0] = R_NOTIMPL;
            return;
        }
        case 0x6F: { /* GetSystemInfo(type, handle, subtype). newer SDKs abort on boot if this returns 0 */
            u32 ty = (u32)x[1];
            u64 sub = x[3], o = 0;
            if (ty == 0)
                o = sub == 0   ? mem_total()
                    : sub == 1 ? 0x1FB00000ull
                    : sub == 2 ? 0x29D00000ull
                               : 0x6000000ull; /* pool physical size */
            else if (ty == 1)
                o = sub == 0 ? used_memory() : 0x1000000ull; /* pool used */
            else if (ty == 2)
                o = sub == 0 ? 1 : 0x50; /* initial process id range */
            x[0] = 0;
            x[1] = o;
            return;
        }
        default:
            WARN("SVC 0x%x not implemented (pc=%llx)\n", imm, (unsigned long long)c->pc);
            x[0] = R_NOTIMPL;
            return;
    }
}
