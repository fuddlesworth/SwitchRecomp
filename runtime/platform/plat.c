/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#define _GNU_SOURCE
/* what every platform shares: the guest thread pointer, logging and fatal errors. The rest of the platform layer is
   plat_posix.c or plat_win32.c (with profiler_win32.c) */
#include "platform/plat.h"
#include "core/main.h"
#include "gpu/renderer.h"
#include "platform/backend.h"
#include "platform/log.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#include <time.h>

__thread void* tls_thread;
int g_verbose = 1;
static FILE* g_logf;
static PMutex g_logm;
static int g_logm_ok;

FILE* volatile g_log_redirect; /* redirect for the periodic threads.txt dump so it doesn't end up in the log */
void logf_(int lvl, const char* fmt, ...) {
    if (!g_logm_ok) {
        pm_init(&g_logm);
        g_logm_ok = 1;
    }
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    {
        FILE* rf = g_log_redirect;
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
    if (!g_logf) {
        char p[1200];
        snprintf(p, sizeof p, "%s%s.log", g_exedir, g_exename[0] ? g_exename : "switchrecomp");
        g_logf = fopen(p, "w");
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
    logf_(0, "FATAL: %s\n", buf);
#if defined(_WIN32)
    { win_fatal_box(buf); }
#endif
    fflush(NULL);
    _exit(1);
}
