/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#define _GNU_SOURCE
/* what every platform shares: the guest thread pointer, logging and fatal errors. The rest of the platform layer is
   plat_posix.c or plat_win32.c (with profiler_win32.c) */
#include "platform/plat.h"
#include "core/main.h"
#include "platform/backend.h"
#include "platform/log.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

__thread void* tls_thread;
int g_verbose = 1;
static FILE* g_logf;
static int g_logf_failed; /* the log couldn't be created: said once on stderr instead of retried every line */
/* initialized statically: logf_ runs on several threads from the start, and a lazy pm_init could race */
static PMutex g_logm = PMUTEX_INIT;
static volatile int g_log_crash; /* log_crash_mode */

__thread FILE* tls_log_redirect; /* log.h */
void log_crash_mode(void) {
    g_log_crash = 1;
    tls_log_redirect = NULL;
}
/* a crash report line, written straight to the log and stderr (the log is flushed after every line, so the order
   holds). On Windows the report runs in an exception filter, not a signal handler, and stdio's locks are recursive */
static void log_raw(const char* buf) {
#if defined(_WIN32)
    if (g_logf) {
        fputs(buf, g_logf);
        fflush(g_logf);
    }
    fputs(buf, stderr);
#else
    size_t L = strlen(buf);
    if (g_logf && write(fileno(g_logf), buf, L) < 0) {}
    if (write(2, buf, L) < 0) {}
#endif
}
void logf_(int lvl, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* a cut line keeps its newline, or the next line would be glued to it */
    if (n >= (int)sizeof buf) buf[sizeof buf - 2] = '\n';
    if (g_log_crash) {
        log_raw(buf);
        return;
    }
    {
        FILE* rf = tls_log_redirect;
        if (rf) {
            fputs(buf, rf);
            return;
        }
    }
    static u64 t0;
    u64 now = plat_ns();
    if (!t0) t0 = now;
    char tb[24];
    snprintf(tb, sizeof tb, "[%7.2f] ", (double)(now - t0) / 1e9);
    pm_lock(&g_logm);
    /* until main knows the game's name (a fatal error about the command line) only stderr: the log would be created
       in the current folder */
    if (!g_logf && !g_logf_failed && g_exename[0]) {
        char p[1200];
        snprintf(p, sizeof p, "%s%s.log", g_exedir, g_exename);
        g_logf = fopen(p, "w");
        if (!g_logf) {
            g_logf_failed = 1;
            fprintf(stderr, "Could not create the log %s, so it only goes to the console.\n", p);
        }
    }
    if (g_logf) {
        static int bol = 1;
        if (bol) fputs(tb, g_logf);
        fputs(buf, g_logf);
        size_t L = strlen(buf);
        bol = L && buf[L - 1] == '\n';
        fflush(g_logf);
    }
    if (lvl <= 1 || g_verbose >= 2) { fputs(buf, stderr); }
    pm_unlock(&g_logm);
}
void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tls_log_redirect = NULL; /* a failure during the threads.txt dump belongs in the log */
    logf_(0, "FATAL: %s\n", buf);
#if !defined(_WIN32)
    /* from the crash handler, neither a box nor fflush: they could wait on a lock (malloc's, stdio's) the crashed code
       holds. The log is written already */
    if (g_log_crash) _exit(1);
#endif
    /* the box on every platform: started from a desktop entry, a player sees neither the console nor the log */
    win_fatal_box(buf);
    plat_exit_now(1);
}
