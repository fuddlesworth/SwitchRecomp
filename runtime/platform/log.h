/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* logging (plat.c) */
#ifndef SWR_LOG_H
#define SWR_LOG_H
#include <stdio.h>

extern int g_verbose;
/* the format attributes let the compiler check every call's arguments against its format */
void logf_(int lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOG(...) logf_(1, __VA_ARGS__)
#define LOGV(...)                                                                                                      \
    do {                                                                                                               \
        if (g_verbose >= 2) logf_(2, __VA_ARGS__);                                                                     \
    } while (0)
#define WARN(...) logf_(0, __VA_ARGS__)
/* LOG, at most n times from this call site, for messages that would otherwise repeat every frame */
#define LOG_LIMIT(n, ...)                                                                                              \
    do {                                                                                                               \
        static int log_limit_said_;                                                                                    \
        if (log_limit_said_ < (n) && __atomic_fetch_add(&log_limit_said_, 1, __ATOMIC_RELAXED) < (n))                  \
            LOG(__VA_ARGS__);                                                                                          \
    } while (0)
/* WARN, at most n times from this call site */
#define WARN_LIMIT(n, ...)                                                                                             \
    do {                                                                                                               \
        static int warn_limit_said_;                                                                                   \
        if (warn_limit_said_ < (n) && __atomic_fetch_add(&warn_limit_said_, 1, __ATOMIC_RELAXED) < (n))                \
            WARN(__VA_ARGS__);                                                                                         \
    } while (0)
void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
/* this thread's log lines go here instead of the log (the periodic threads.txt dump). Per thread, so the other
   threads keep logging to the log while the main thread writes a dump, and never into a file being closed */
extern __thread FILE* tls_log_redirect;
/* the crash handler is running: log without locks or stdio from here on (a fault inside the log's lock or malloc
   would otherwise deadlock the report), and drop this thread's redirect so the report reaches the log */
void log_crash_mode(void);

#endif
