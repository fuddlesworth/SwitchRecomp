/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#define _GNU_SOURCE
/* the platform layer on Linux and other POSIX systems: locks, threads, time, virtual memory and write tracking
   (userfaultfd), files, shared libraries, the crash handler */
#include "core/main.h"
#include "gpu/renderer.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sched.h>
#include <errno.h>
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
void pc_init(PCond* c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
}
void pc_signal(PCond* c) { pthread_cond_signal(&c->c); }
void pc_broadcast(PCond* c) { pthread_cond_broadcast(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    if (ns < 0) {
        pthread_cond_wait(&c->c, &m->l);
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 t = (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec + (u64)ns;
    ts.tv_sec = t / 1000000000ull;
    ts.tv_nsec = t % 1000000000ull;
    return pthread_cond_timedwait(&c->c, &m->l, &ts) == ETIMEDOUT;
}
typedef struct {
    PThreadFn fn;
    void* arg;
} TArg;
static void* tstart(void* p) {
    TArg a = *(TArg*)p;
    free(p);
    a.fn(a.arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    TArg* a = malloc(sizeof *a);
    a->fn = fn;
    a->arg = arg;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, stack);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int r = pthread_create(&t, &at, tstart, a);
    pthread_attr_destroy(&at);
    return r ? -1 : 0;
}
void plat_thread_exit(void) { pthread_exit(NULL); }
void plat_sleep_ns(i64 ns) {
    if (ns <= 0) {
        sched_yield();
        return;
    }
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
}
void plat_yield(void) { sched_yield(); }
void plat_thread_low_priority(void) {}
void plat_thread_high_priority(void) {}
u64 plat_thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
u64 plat_process_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
u64 plat_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
i64 plat_unix_time(void) { return (i64)time(NULL); }
i64 plat_tz_offset(void) {
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return (i64)lt.tm_gmtoff;
}
int g_ww_ok;
int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)) {
    (void)base;
    (void)size;
    (void)cb;
    return -1;
}
/* Written-page tracking for the GPU caches (plat_ww_scan): the reserved guest memory is registered with a
   userfaultfd in asynchronous write-protect mode, which needs no handler thread: a write to a protected page just
   unprotects it in the kernel, writes from the kernel (read() into guest memory) included. PAGEMAP_SCAN then lists
   the written pages of a range and protects them again. Only ranges a cache scans are ever protected, so the rest of
   guest memory costs nothing. Needs Linux 6.7; without it, the caches check contents as before */
static int g_uffd_pagemap = -1;
#if defined(__linux__) && !defined(__ANDROID__) && defined(PAGEMAP_SCAN)
static void ww_init(u8* p, u64 size) {
    if (getenv("SWR_NO_WW")) return;
    int u = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (u < 0) return;
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    struct uffdio_register reg = {.range = {(u64)(uintptr_t)p, size}, .mode = UFFDIO_REGISTER_MODE_WP};
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0 || ioctl(u, UFFDIO_API, &api) || ioctl(u, UFFDIO_REGISTER, &reg)) {
        if (pm >= 0) close(pm);
        close(u);
        return;
    }
    g_uffd_pagemap = pm; /* the userfaultfd stays open for the registration to last */
}
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    if (g_uffd_pagemap < 0) return -1;
    struct page_region r[64];
    u64 start = (u64)(uintptr_t)p & ~4095ull, end = ((u64)(uintptr_t)p + size + 4095) & ~4095ull;
    while (start < end) {
        struct pm_scan_arg a = {.size = sizeof a,
                                .flags = PM_SCAN_WP_MATCHING,
                                .start = start,
                                .end = end,
                                .vec = (u64)(uintptr_t)r,
                                .vec_len = 64,
                                .category_mask = PAGE_IS_WRITTEN,
                                .return_mask = PAGE_IS_WRITTEN};
        long n = ioctl(g_uffd_pagemap, PAGEMAP_SCAN, &a);
        if (n < 0) return -1;
        for (long i = 0; i < n; i++) cb((u8*)(uintptr_t)r[i].start, r[i].end - r[i].start);
        if (a.walk_end >= end || !a.walk_end) break; /* walk_end: where a full vec stopped the scan */
        start = a.walk_end;
    }
    return 0;
}
#else
static void ww_init(u8* p, u64 size) {
    (void)p;
    (void)size;
}
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    (void)p;
    (void)size;
    (void)cb;
    return -1;
}
#endif
u8* plat_vm_reserve(u64 size) {
    void* p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    ww_init(p, size);
    return (u8*)p;
}
int plat_vm_commit(u8* p, u64 size) { return mprotect(p, size, PROT_READ | PROT_WRITE); }
void plat_vm_decommit(u8* p, u64 size) {
    madvise(p, size, MADV_DONTNEED);
    mprotect(p, size, PROT_NONE);
}
void* plat_dl_open(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void* plat_dl_sym(void* h, const char* n) { return dlsym(h, n); }
int plat_mkdir(const char* p) { return mkdir(p, 0755); }
int plat_is_dir(const char* p) {
    struct stat s;
    return !stat(p, &s) && S_ISDIR(s.st_mode);
}
typedef struct {
    DIR* d;
    char path[1024];
} PD_;
int plat_dir_open(PDir* d, const char* path) {
    PD_* p = malloc(sizeof *p);
    p->d = opendir(path);
    if (!p->d) {
        free(p);
        return -1;
    }
    strncpy(p->path, path, 1023);
    p->path[1023] = 0;
    d->h = p;
    return 0;
}
int plat_dir_next(PDir* d) {
    PD_* p = d->h;
    struct dirent* e;
    while ((e = readdir(p->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        strncpy(d->name, e->d_name, 511);
        d->name[511] = 0;
        char f[1600];
        snprintf(f, sizeof f, "%s/%s", p->path, e->d_name);
        struct stat s;
        if (stat(f, &s)) continue;
        d->is_dir = S_ISDIR(s.st_mode);
        d->size = (u64)s.st_size;
        return 1;
    }
    return 0;
}
void plat_dir_close(PDir* d) {
    PD_* p = d->h;
    closedir(p->d);
    free(p);
}
u64 plat_file_size(const char* p) {
    struct stat s;
    if (stat(p, &s)) return 0;
    return (u64)s.st_size;
}
int plat_pread(FILE* f, void* buf, u64 size, u64 off) { return (int)pread(fileno(f), buf, size, (off_t)off); }
int plat_ncpu(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
static void sighandler(int sig, siginfo_t* si, void* uc_) {
    void* pc = NULL;
#if defined(__x86_64__)
#include <ucontext.h>
    ucontext_t* uc = (ucontext_t*)uc_;
    pc = (void*)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
#include <ucontext.h>
    ucontext_t* uc = (ucontext_t*)uc_;
    pc = (void*)uc->uc_mcontext.pc;
#else
    (void)uc_;
#endif
    crash_report(pc, si->si_addr, sig);
    _exit(1);
}
void plat_install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = sighandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    static char altstack[65536];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
void plat_prof_start(int boot) { (void)boot; } /* the sampling profiler is Windows only */
