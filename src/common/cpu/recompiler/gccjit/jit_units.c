/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "jit.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

/* every compile in this process and every fork: no unit's process starts while a thread is inside GCC */
static pthread_mutex_t compiling = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&compiling); }
static void unlock(void) { pthread_mutex_unlock(&compiling); }
#else
static void lock(void) {}
static void unlock(void) {}
#endif

const char* snx_jit_compile_to_file(SnxJit* jit, enum gcc_jit_output_kind kind, const char* path) {
    lock();
    gcc_jit_context_compile_to_file(jit->ctx, kind, path);
    unlock();
    return snx_jit_error(jit);
}

gcc_jit_result* snx_jit_compile(SnxJit* jit) {
    gcc_jit_result* result;
    lock();
    result = gcc_jit_context_compile(jit->ctx);
    unlock();
    return result;
}

static int compile_unit(const SnxJitUnits* u, size_t i, unsigned long* fallbacks, char* error, size_t size) {
    SnxJit* jit = snx_jit_new(u->opt);
    const char* message;
    char name[32];
    size_t j;
    if (!jit) {
        snprintf(error, size, "out of memory");
        return -1;
    }
    sprintf(name, "code_%03lu", (unsigned long)i);
    for (j = u->bounds[i]; j < u->bounds[i + 1]; ++j) snx_jit_guest_function(jit, u->codegen->functions[j].start, 1);
    for (j = u->bounds[i]; j < u->bounds[i + 1]; ++j) snx_jit_emit(jit, u->codegen, &u->codegen->functions[j]);
    if (u->keep_ir) {
        char *file = snx_format("%s.c", name), *path = snx_path_join(u->keep_ir, file);
        gcc_jit_context_dump_to_file(jit->ctx, path, 0);
        free(file);
        free(path);
    }
    message = snx_jit_compile_to_file(jit, GCC_JIT_OUTPUT_KIND_OBJECT_FILE, u->objects[i]);
    if (message) snprintf(error, size, "libgccjit (%s): %s", name, message);
    *fallbacks = jit->fallbacks;
    snx_jit_free(jit);
    return message ? -1 : 0;
}

#ifndef _WIN32
/* what a unit's process writes back before it ends */
typedef struct {
    unsigned long fallbacks;
    int failed;
    char error[1024];
} Report;

typedef struct {
    Report report;
    double start;
    size_t unit, got;
    pid_t pid; /* 0: free */
    int fd;
} Slot;

static void run_unit(const SnxJitUnits* u, size_t i, int fd) {
    Report r;
    const char* p = (const char*)&r;
    size_t left = sizeof(r);
    ssize_t n;
    memset(&r, 0, sizeof(r));
    r.failed = compile_unit(u, i, &r.fallbacks, r.error, sizeof(r.error)) != 0;
    while (left) {
        n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        p += n;
        left -= (size_t)n;
    }
    _exit(r.failed);
}

/* -1 with errno when the process can't start */
static int start(const SnxJitUnits* u, size_t i, Slot* s) {
    int fds[2], e;
    if (pipe(fds)) return -1;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    /* a process that leaves through exit() (GCC's fatal errors) would write what is buffered here again */
    fflush(NULL);
    lock();
    s->pid = fork();
    if (!s->pid) {
        unlock();
        close(fds[0]);
        setpgid(0, 0); /* with the compiler driver's processes it starts: stop() ends them all */
        run_unit(u, i, fds[1]);
    }
    e = errno;
    unlock();
    if (s->pid > 0) setpgid(s->pid, s->pid);
    close(fds[1]);
    if (s->pid < 0) {
        close(fds[0]);
        s->pid = 0;
        errno = e;
        return -1;
    }
    s->fd = fds[0];
    s->unit = i;
    s->got = 0;
    s->start = snx_seconds();
    return 0;
}

/* the process has reported, or ended without: 0 when its unit compiled */
static int finish(SnxJitUnits* u, Slot* s, char* error, size_t size) {
    int status = 0;
    pid_t waited;
    close(s->fd);
    do { waited = waitpid(s->pid, &status, 0); } while (waited < 0 && errno == EINTR);
    s->pid = 0;
    if (s->got == sizeof(s->report)) {
        if (!s->report.failed) {
            u->fallbacks += s->report.fallbacks;
            return 0;
        }
        s->report.error[sizeof(s->report.error) - 1] = 0;
        snprintf(error, size, "%s", s->report.error);
    } else if (waited > 0 && WIFSIGNALED(status))
        snprintf(error, size, "libgccjit (code_%03lu): its process was killed by signal %d", (unsigned long)s->unit,
                 WTERMSIG(status));
    else if (waited > 0)
        snprintf(error, size, "libgccjit (code_%03lu): its process exited with status %d without a report",
                 (unsigned long)s->unit, WEXITSTATUS(status));
    else
        snprintf(error, size, "libgccjit (code_%03lu): its process ended without a report", (unsigned long)s->unit);
    return -1;
}

/* the first failure ends the call: the units still compiling are stopped, and reaped as their pipes close */
static void stop(const Slot* slots, unsigned jobs) {
    unsigned k;
    for (k = 0; k < jobs; ++k)
        if (slots[k].pid && kill(-slots[k].pid, SIGKILL)) kill(slots[k].pid, SIGKILL);
}

static int compile_forked(SnxJitUnits* u, unsigned jobs) {
    Slot* slots;
    struct pollfd* polls;
    size_t next = 0, finished = 0, running = 0, unit, k;
    char error[sizeof(u->error)];
    double seconds;
    ssize_t n;
    int failed = 0;
    slots = (Slot*)calloc(jobs, sizeof(*slots) + sizeof(*polls));
    if (!slots) {
        snprintf(u->error, sizeof(u->error), "out of memory");
        return -1;
    }
    polls = (struct pollfd*)(slots + jobs);
    while (running || (next < u->count && !failed)) {
        for (k = 0; k < jobs && next < u->count && !failed; ++k) {
            if (slots[k].pid) continue;
            if (start(u, next, &slots[k])) {
                snprintf(u->error, sizeof(u->error), "libgccjit (code_%03lu): cannot start a process: %s",
                         (unsigned long)next, strerror(errno));
                failed = 1;
                stop(slots, jobs);
            } else {
                ++next;
                ++running;
            }
        }
        if (!running) continue;
        for (k = 0; k < jobs; ++k) {
            polls[k].fd = slots[k].pid ? slots[k].fd : -1;
            polls[k].events = POLLIN;
            polls[k].revents = 0;
        }
        /* no time limit: a slow unit is as valid as a quick one, and one whose process hangs without failing holds
           the call until it is ended (Ctrl+C) */
        if (poll(polls, jobs, -1) < 0) {
            if (errno == EINTR) continue;
            for (k = 0; k < jobs; ++k) polls[k].revents = POLLIN; /* reads block instead */
        }
        for (k = 0; k < jobs; ++k) {
            Slot* s = &slots[k];
            if (!s->pid || !polls[k].revents) continue;
            n = read(s->fd, (char*)&s->report + s->got, sizeof(s->report) - s->got);
            if (n < 0 && errno == EINTR) continue;
            if (n > 0) s->got += (size_t)n;
            if (n > 0 && s->got < sizeof(s->report)) continue;
            unit = s->unit;
            seconds = snx_seconds() - s->start;
            --running;
            if (finish(u, s, error, sizeof(error))) {
                if (!failed) {
                    snprintf(u->error, sizeof(u->error), "%s", error);
                    failed = 1;
                    stop(slots, jobs);
                }
            } else if (u->done)
                u->done(u, unit, ++finished, seconds);
        }
    }
    free(slots);
    return failed ? -1 : 0;
}
#endif

int snx_jit_compile_units(SnxJitUnits* u) {
    unsigned jobs = u->jobs ? u->jobs : snx_cpu_count();
    unsigned long fallbacks;
    double start;
    size_t i;
    u->fallbacks = 0;
    u->error[0] = 0;
    if (jobs > u->count) jobs = (unsigned)u->count;
#ifndef _WIN32
    if (jobs > 1) {
        snx_jit_host(); /* once here, not in every process */
        return compile_forked(u, jobs);
    }
#endif
    for (i = 0; i < u->count; ++i) {
        start = snx_seconds();
        if (compile_unit(u, i, &fallbacks, u->error, sizeof(u->error))) return -1;
        u->fallbacks += fallbacks;
        if (u->done) u->done(u, i, i + 1, snx_seconds() - start);
    }
    return 0;
}
