/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* threads: creating, starting, the thread a host thread runs, exiting */
#include "kernel/kernel_int.h"
/* ============================================================ threads */
KThread* g_threads[256];
int g_nthreads;
#if defined(__ANDROID__)
static pthread_key_t g_tkey;
static pthread_once_t g_tonce = PTHREAD_ONCE_INIT;
static void tkey_init(void) { pthread_key_create(&g_tkey, NULL); }
KThread* kthread_cur(void) {
    pthread_once(&g_tonce, tkey_init);
    return (KThread*)pthread_getspecific(g_tkey);
}
static void kthread_set(void* t) {
    pthread_once(&g_tonce, tkey_init);
    pthread_setspecific(g_tkey, t);
}
#else
KThread* kthread_cur(void) { return (KThread*)tls_thread; }
static void kthread_set(void* t) { tls_thread = t; }
#endif
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
    t->affinity = 1ull << t->core;
    t->tls = alloc_tls();
    pc_init(&t->cv);
    pm_lock(&g_klock);
    t->id = g_next_tid++;
    if (g_nthreads < 256) g_threads[g_nthreads++] = t;
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

static void thread_body(void* p) {
    KThread* t = p;
    kthread_set(t);
    CPU* c = &t->cpu;
    swr_fpcr_apply(c);
    LOGV("[thread %llu] starts at %llx\n", (unsigned long long)t->id, (unsigned long long)c->pc);
    for (;;) {
        if (UNLIKELY(c->pc == 0)) { /* entry function returned */
            svc_exit_thread(t);
        }
        GuestFn f = swr_lookup(c, c->pc);
        f(c, c->pc);
    }
}
void kthread_start(KThread* t) {
    t->state = 1;
    if (plat_thread(thread_body, t, (size_t)64 << 20)) fatal("could not create a system thread");
}
void svc_exit_thread(KThread* t) {
    pm_lock(&g_klock);
    t->state = 2;
    kobj_signal_nl(&t->o);
    pm_unlock(&g_klock);
    LOGV("[thread %llu] exits\n", (unsigned long long)t->id);
    plat_thread_exit();
}

void kernel_init(void) { pm_init(&g_klock); }

/* main thread: stack size from npdm, x0=0, x1=own handle */
KThread* g_main;
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core) {
    stack_size = (stack_size + 0xFFF) & ~0xFFFull;
    u64 st = mem_find_free(STACK_BASE, STACK_BASE + STACK_SIZE, stack_size + 0x2000, 0x1000) + 0x1000;
    mem_map(st, stack_size, MS_STACK, PERM_RW);
    KThread* t = kthread_create(entry, 0, st + stack_size, prio, core);
    t->is_main = 1;
    g_main = t;
    strcpy(t->name, "main");
    u32 h = h_new(&t->o);
    t->cpu.x[1] = h;
    kthread_start(t);
}

void process_exit(int code) {
    g_exiting = 1;
    LOG("The game exited (code %d)\n", code);
    fflush(NULL);
    exit(code);
}
