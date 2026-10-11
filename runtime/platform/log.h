/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* logging (plat.c) */
#ifndef SWR_LOG_H
#define SWR_LOG_H
#include <stdio.h>

extern int g_verbose;
void logf_(int lvl, const char* fmt, ...);
#define LOG(...) logf_(1, __VA_ARGS__)
#define LOGV(...)                                                                                                      \
    do {                                                                                                               \
        if (g_verbose >= 2) logf_(2, __VA_ARGS__);                                                                     \
    } while (0)
#define WARN(...) logf_(0, __VA_ARGS__)
void fatal(const char* fmt, ...);
extern FILE* volatile g_log_redirect; /* the periodic threads.txt dump writes here instead of the log */

#endif
