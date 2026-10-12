/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* threads: creating, starting, the thread a host thread runs, exiting */
#include "kernel/kernel_int.h"
#include "gpu/renderer.h"
/* ============================================================ threads */
KThread* g_threads[256];
int g_nthreads;
KThread* kthread_cur(void) { return (KThread*)tls_thread; }
static void kthread_set(void* t) { tls_thread = t; }
static int th_sig(KObj* o) { return ((KThread*)o)->state == 2; }
static void th_destroy(KObj* o) { (void)o; /* thread objects are never freed */ }
static u64 alloc_tls(void) {
    u64 a = kmem_alloc(0x1000, MS_TLS, PERM_RW);
    memset(GP(a), 0, 0x1000);
    return a;
}

KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core) {
    KThread* t = calloc(1, sizeof *t);
    t->o.type = KO_THREAD;
    t->o.is_signaled = th_sig;
    t->o.destroy = th_destroy;
    t->o.name = "thread";
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->prio = prio;
    t->core = core < 0 ? 0 : core;
    t->ideal_core = t->core;
    t->on_core = -1;
    t->affinity = 1ull << t->core;
    t->tls = alloc_tls();
    pc_init(&t->cv);
    pm_lock(&g_klock);
    t->id = g_next_tid++;
    /* the table is for the dumps and the thread counts: once full, an exited thread's slot is reused */
    if (g_nthreads < 256)
        g_threads[g_nthreads++] = t;
    else {
        int i = 0;
        while (i < 256 && g_threads[i]->state != 2) i++;
        if (i < 256)
            g_threads[i] = t;
        else
            WARN_LIMIT(1, "kernel: more than 256 live threads, the dumps leave the newest out\n");
    }
    pm_unlock(&g_klock);
    CPU* c = &t->cpu;
    memset(c, 0, sizeof *c);
    c->mem = g_mem;
    c->disp = g_disp;
    c->code_lo = g_code_lo;
    c->code_hi = g_code_hi;
    c->rt = &g_rtapi;
    c->thread = t;
    c->tpidrro = t->tls;
    c->x[0] = arg;
    c->x[31] = stack_top;
    c->pc = entry;
    return t;
}

/* A console core runs one thread at a time, and games build on it: Tears of the Kingdom keeps per-core state (command
   memory cursors, heap caches) without locks, indexed by svcGetCurrentProcessorNumber, and every thread created on the
   default core shares core 0. Guest threads run on host threads in parallel, so a thread holds its core while it runs
   and lets go only where a console thread is off the CPU too: when it blocks in the kernel, sleeps or yields, makes an
   IPC call, or waits in WFE. A ticket per core keeps the order fair. */
static PMutex g_core_lk;
static PCond g_core_cv;
static u64 g_core_next[4], g_core_serving[4];
void core_enter(KThread* t) {
    if (t->on_core >= 0) return;
    int k = t->core & 3;
    pm_lock(&g_core_lk);
    u64 my = g_core_next[k]++;
    while (g_core_serving[k] != my) pc_wait(&g_core_cv, &g_core_lk, -1);
    t->on_core = k;
    pm_unlock(&g_core_lk);
}
void core_leave(KThread* t) {
    if (!t || t->on_core < 0) return;
    pm_lock(&g_core_lk);
    g_core_serving[t->on_core]++;
    t->on_core = -1;
    pc_broadcast(&g_core_cv);
    pm_unlock(&g_core_lk);
}
static void thread_body(void* p) {
    KThread* t = p;
    kthread_set(t);
    core_enter(t);
    CPU* c = &t->cpu;
    snx_fpcr_apply(c);
    LOGV("[thread %llu] starts at %llx\n", (unsigned long long)t->id, (unsigned long long)c->pc);
    for (;;) {
        if (UNLIKELY(c->pc == 0)) { /* entry function returned */
            svc_exit_thread(t);
        }
        GuestFn f = snx_lookup(c, c->pc);
        f(c, c->pc);
    }
}
int kthread_live_count(void) {
    int live = 0;
    pm_lock(&g_klock);
    for (int i = 0; i < g_nthreads; i++)
        if (g_threads[i]->state != 2) live++;
    pm_unlock(&g_klock);
    return live;
}
void kthread_start(KThread* t) {
    t->state = 1;
    if (plat_thread(thread_body, t, (size_t)64 << 20)) fatal("could not create a system thread");
}
void svc_exit_thread(KThread* t) {
    core_leave(t);
    pm_lock(&g_klock);
    t->state = 2;
    kobj_signal_nl(&t->o);
    pm_unlock(&g_klock);
    LOGV("[thread %llu] exits\n", (unsigned long long)t->id);
    plat_thread_exit();
}

void kernel_init(void) {
    pm_init(&g_klock);
    pm_init(&g_core_lk);
    pc_init(&g_core_cv);
}

/* main thread: stack size from npdm, x0=0, x1=own handle */
KThread* g_main;
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core) {
    stack_size = (stack_size + 0xFFF) & ~0xFFFull;
    u64 st = mem_find_free(STACK_BASE, STACK_BASE + STACK_SIZE, stack_size + 0x2000, 0x1000);
    if (!st || mem_map(st + 0x1000, stack_size, MS_STACK, PERM_RW))
        fatal("Could not map the game's main thread stack (%llu KB)", (unsigned long long)(stack_size >> 10));
    st += 0x1000; /* a free page on each side */
    KThread* t = kthread_create(entry, 0, st + stack_size, prio, core);
    g_main = t;
    strcpy(t->name, "main");
    u32 h = h_new(&t->o);
    t->cpu.x[1] = h;
    kthread_start(t);
}

/* svcExitProcess. exit() would run the atexit handlers and tear the runtime down while the other guest threads still
   run in it, so the process ends at once, the way main.c ends it when the window closes (with the hints and the
   renderer's pipeline cache saved first, as there) */
void process_exit(int code) {
    save_hints();
    if (g_renderer && g_renderer->save) g_renderer->save();
    LOG("The game exited (code %d)\n", code);
    plat_exit_now(code);
}
