/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#define _GNU_SOURCE
/* the platform layer on Linux and other POSIX systems: locks, threads, time, virtual memory and write tracking
   (userfaultfd), files, shared libraries, the crash handler */
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#ifdef __APPLE__
/* Darwin's <ucontext.h> stops the build unless _XOPEN_SOURCE asks for the deprecated context functions; the type the
   signal handler reads is in this one */
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <sched.h>
#include <errno.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
extern char** environ;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
/* zenity and kdialog are a desktop's: Android has no dialog to ask for text yet, and an iOS app runs no program */
int plat_text_input(char* out, size_t cap, const char* initial) {
    (void)out;
    (void)cap;
    (void)initial;
    return -1;
}
#else
/* Run a host text dialog without a shell: the initial text is game data, not a command. */
static int text_dialog_run(char* out, size_t cap, char* const argv[]) {
    int fd[2];
    if (pipe(fd)) return -1;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(fd[0]);
        close(fd[1]);
        return -1;
    }
    int err = posix_spawn_file_actions_addclose(&actions, fd[0]);
    if (!err) err = posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
    if (!err) err = posix_spawn_file_actions_addclose(&actions, fd[1]);
    pid_t pid = 0;
    if (!err) err = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fd[1]);
    if (err) {
        close(fd[0]);
        return err == ENOENT ? -2 : -1;
    }
    size_t n = 0;
    char c;
    int read_error = 0;
    for (;;) {
        ssize_t got = read(fd[0], &c, 1);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) read_error = 1;
        if (got != 1) break;
        if (c == '\n') break;
        if (n + 1 < cap)
            out[n++] = c;
        else
            read_error = 1;
    }
    close(fd[0]);
    out[n] = 0;
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status)) return -1;
    if (WEXITSTATUS(status) == 127) return -2;
    return read_error ? -1 : WEXITSTATUS(status) == 0 ? 0 : WEXITSTATUS(status) == 1 ? 1 : -1;
}
int plat_text_input(char* out, size_t cap, const char* initial) {
    if (!cap) return -1;
    if (!initial) initial = "";
#ifdef __APPLE__
    /* macOS's own dialog, through AppleScript. The text is the script's argument, never part of it: it is game data.
       Cancel exits with 1, as zenity's does */
    char* osascript[] = {"osascript",
                         "-e",
                         "on run argv",
                         "-e",
                         "text returned of (display dialog \"Enter text\" default answer (item 1 of argv) with title "
                         "\"ScarletNX\")",
                         "-e",
                         "end run",
                         "--",
                         (char*)initial,
                         NULL};
    int apple = text_dialog_run(out, cap, osascript);
    return apple == -2 ? -1 : apple;
#endif
    char* zenity[] = {"zenity",       "--entry", "--title=ScarletNX", "--text=Enter text", "--entry-text",
                      (char*)initial, NULL};
    char* kdialog[] = {"kdialog", "--title", "ScarletNX", "--inputbox", "Enter text", (char*)initial, NULL};
    const char* desktop = getenv("XDG_CURRENT_DESKTOP");
    int kde = desktop && (strstr(desktop, "KDE") || strstr(desktop, "Plasma"));
    int result = text_dialog_run(out, cap, kde ? kdialog : zenity);
    if (result == -2) result = text_dialog_run(out, cap, kde ? zenity : kdialog);
    return result == -2 ? -1 : result;
}
#endif
/* FreeBSD has no MAP_NORESERVE: it never reserves swap for a mapping up front, so there is nothing to opt out of */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
void pc_init(PCond* c) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#ifndef __APPLE__ /* Darwin has no clock to choose: pc_wait waits a relative time there */
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
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
#ifdef __APPLE__
    ts.tv_sec = ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;
    return pthread_cond_timedwait_relative_np(&c->c, &m->l, &ts) == ETIMEDOUT;
#endif
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 t = (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec + (u64)ns;
    ts.tv_sec = t / 1000000000ull;
    ts.tv_nsec = t % 1000000000ull;
    return pthread_cond_timedwait(&c->c, &m->l, &ts) == ETIMEDOUT;
}
typedef struct {
    PThreadFn fn;
    void* arg;
    size_t stack;
} TArg;
/* The signal stack is per thread, and only the main thread had one (plat_install_crash_handler): a guest thread that
   overflowed its stack died without a crash report, the handler having no stack to run on. A thread with a big stack
   now gets one in its own first frame. Small stacks (the 64 KiB audio clock) do without, it would take half of it */
#define ALTSTACK_MIN_STACK (256u << 10)
static void altstack_off(void) {
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, NULL);
}
__attribute__((noinline)) static void run_with_altstack(TArg a) {
    char alt[32768]; /* the crash report needs about 12 KiB */
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = alt;
    ss.ss_size = sizeof alt;
    sigaltstack(&ss, NULL);
    a.fn(a.arg);
    altstack_off(); /* alt goes out of scope, and the thread's exit code reuses that stack */
}
static void* tstart(void* p) {
    TArg a = *(TArg*)p;
    free(p);
    if (!a.stack || a.stack >= ALTSTACK_MIN_STACK)
        run_with_altstack(a);
    else
        a.fn(a.arg);
    return NULL;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    TArg* a = malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn;
    a->arg = arg;
    a->stack = stack;
    pthread_attr_t at;
    pthread_attr_init(&at);
    /* below the minimum, pthread_attr_setstacksize fails and the thread would get the default size instead */
    if (stack && stack < (size_t)PTHREAD_STACK_MIN) stack = (size_t)PTHREAD_STACK_MIN;
#ifdef __APPLE__
    /* and Darwin's takes only whole pages (16 KiB on Apple silicon) */
    if (stack) stack = (stack + (size_t)sysconf(_SC_PAGESIZE) - 1) & ~((size_t)sysconf(_SC_PAGESIZE) - 1);
#endif
    if (stack) pthread_attr_setstacksize(&at, stack);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int r = pthread_create(&t, &at, tstart, a);
    pthread_attr_destroy(&at);
    if (r) free(a);
    return r ? -1 : 0;
}
void plat_thread_exit(void) {
    altstack_off(); /* pthread_exit unwinds through run_with_altstack's frame, which holds the signal stack */
    pthread_exit(NULL);
}
void plat_exit_now(int code) {
    fflush(NULL);
    _exit(code);
}
void plat_sleep_ns(i64 ns) {
    if (ns <= 0) {
        sched_yield();
        return;
    }
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR);
}
void plat_yield(void) { sched_yield(); }
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
/* Written-page tracking for the GPU caches (plat_ww_scan): the reserved guest memory is registered with a
   userfaultfd in asynchronous write-protect mode, which needs no handler thread: a write to a protected page just
   unprotects it in the kernel, writes from the kernel (read() into guest memory) included. PAGEMAP_SCAN then lists
   the written pages of a range and protects them again. Only ranges a cache scans are ever protected, so the rest of
   guest memory costs nothing. Needs Linux 6.7; without it, the caches check contents as before */
static int g_uffd_pagemap = -1;
#if defined(__linux__) && defined(PAGEMAP_SCAN)
static void ww_init(u8* p, u64 size) {
    if (getenv("SNX_NO_WW")) return;
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
    /* guest memory is mapped and protected in the Switch's 4 KiB pages: with bigger host pages (16 KiB or 64 KiB on
       some arm64 systems) mprotect fails on most of them */
    long pg = sysconf(_SC_PAGESIZE);
    if (pg != 4096)
        fatal("This system uses %ld KiB memory pages. ScarletNX only runs on systems with 4 KiB pages.", pg / 1024);
    void* p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    ww_init(p, size);
    return (u8*)p;
}
int plat_vm_commit(u8* p, u64 size) { return mprotect(p, size, PROT_READ | PROT_WRITE); }
void plat_vm_decommit(u8* p, u64 size) {
#ifdef __APPLE__
    /* Darwin's MADV_DONTNEED keeps the pages' contents: a new mapping in their place is what gives zeroes back */
    mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#else
    madvise(p, size, MADV_DONTNEED);
    mprotect(p, size, PROT_NONE);
#endif
}
#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
/* Android's shared storage (/sdcard, where the app keeps games/) maps nothing executable: a module there runs from a
   copy in the app's cache */
void* plat_dl_open(const char* path) {
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    const char *cache = SDL_GetAndroidCachePath(), *name = strrchr(path, '/');
    char copy[1200];
    FILE *in, *out;
    char buffer[1 << 16];
    size_t n;
    int ok;
    if (h || !cache) return h;
    snprintf(copy, sizeof copy, "%s/%s", cache, name ? name + 1 : path);
    in = fopen(path, "rb");
    out = in ? fopen(copy, "wb") : NULL;
    ok = in && out;
    while (ok && (n = fread(buffer, 1, sizeof buffer, in)) > 0) ok = fwrite(buffer, 1, n, out) == n;
    ok = ok && !ferror(in);
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    return ok ? dlopen(copy, RTLD_NOW | RTLD_LOCAL) : NULL;
}
#else
void* plat_dl_open(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
#endif
void* plat_dl_sym(void* h, const char* n) { return dlsym(h, n); }
const char* plat_dl_error(void) {
    const char* e = dlerror();
    return e ? e : "no error reported";
}
int plat_is_dir(const char* p) {
    struct stat s;
    return !stat(p, &s) && S_ISDIR(s.st_mode);
}
typedef struct {
    DIR* d;
    char path[1024];
} PD_;
int plat_dir_open(PDir* d, const char* path) {
    if (strlen(path) >= sizeof((PD_*)0)->path) return -1; /* cut, plat_dir_next would stat files of another folder */
    PD_* p = malloc(sizeof *p);
    if (!p) return -1;
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
/* pread returns less than asked when interrupted or past 2 GiB in one call: loop to the end of the file */
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) {
    int fd = fileno(f);
    u64 got = 0;
    while (got < size) {
        ssize_t n = pread(fd, (u8*)buf + got, size - got, (off_t)(off + got));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) break;
        got += (u64)n;
    }
    return (i64)got;
}
static int file_map(PFileMap* m, const char* path, const u64* indexed_size) {
    m->data = NULL;
    m->size = 0;
    if (indexed_size && (*indexed_size > SIZE_MAX || *indexed_size > INT64_MAX)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    u64 size;
    if (indexed_size) {
        size = *indexed_size;
    } else {
        struct stat s;
        if (fstat(fd, &s) || !S_ISREG(s.st_mode) || s.st_size < 0) {
            close(fd);
            return -1;
        }
        size = (u64)s.st_size;
    }
    if (size > SIZE_MAX) {
        close(fd);
        return -1;
    }
    if (size) { /* mmap of length 0 fails */
        void* p = mmap(NULL, (size_t)size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            close(fd);
            return -1;
        }
        m->data = p;
        m->size = size;
    }
    close(fd); /* the mapping keeps the file */
    return 0;
}
int plat_file_map(PFileMap* m, const char* path) { return file_map(m, path, NULL); }
int plat_file_map_sized(PFileMap* m, const char* path, u64 size) { return file_map(m, path, &size); }
void plat_file_unmap(PFileMap* m) {
    if (m->data && !m->borrowed) munmap((void*)m->data, (size_t)m->size);
    m->data = NULL;
    m->size = 0;
    m->borrowed = 0;
}
int plat_replace_file(const char* tmp, const char* dst) {
    if (!rename(tmp, dst)) return 0;
    remove(tmp);
    return -1;
}
static void sighandler(int sig, siginfo_t* si, void* uc_) {
    /* A second fault while reporting the first ends the process. Another thread crashing meanwhile waits: the first
       report ends with _exit */
    static __thread volatile sig_atomic_t reporting;
    static int crashed;
    if (reporting) _exit(1);
    reporting = 1;
    if (__atomic_exchange_n(&crashed, 1, __ATOMIC_ACQ_REL))
        for (;;) pause();
    log_crash_mode(); /* the handler can't take the log's lock: the faulting code may hold it */
    void* pc = NULL;
    ucontext_t* uc = (ucontext_t*)uc_;
#if defined(__FreeBSD__) && defined(__x86_64__)
    pc = (void*)uc->uc_mcontext.mc_rip;
#elif defined(__FreeBSD__) && defined(__aarch64__)
    pc = (void*)uc->uc_mcontext.mc_gpregs.gp_elr;
#elif defined(__APPLE__) && defined(__aarch64__)
    pc = (void*)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
    pc = (void*)uc->uc_mcontext->__ss.__rip;
#elif defined(__x86_64__)
    pc = (void*)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (void*)uc->uc_mcontext.pc;
#else
    (void)uc;
#endif
    /* si_addr is the faulting address only for SIGSEGV and SIGBUS: for SIGILL and SIGFPE it's the instruction's */
    crash_report(pc, sig == SIGSEGV || sig == SIGBUS ? si->si_addr : NULL, sig);
    _exit(1);
}
/* abort() from a library (the video driver, or out of memory): say so in the log, as on Windows. Returning lets abort
   go on and end the process */
static void on_abort(int s) {
    log_crash_mode();
    logf_(0, "\n*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n", s);
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
    struct sigaction ab;
    memset(&ab, 0, sizeof ab);
    ab.sa_handler = on_abort;
    ab.sa_flags = SA_ONSTACK | SA_RESETHAND;
    sigaction(SIGABRT, &ab, NULL);
}
void plat_prof_start(void) {} /* the sampling profiler is Windows only */
