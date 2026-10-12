/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the platform layer: threads, locks, time, virtual memory, files, libraries (plat_posix.c or plat_win32.c), logging
   and fatal errors (plat.c) */
#ifndef SWR_PLAT_H
#define SWR_PLAT_H
#include "abi/cpu.h"
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX /* windows.h's min and max macros would break any later min( or max( in the runtime */
#endif
#include <windows.h>
typedef struct {
    SRWLOCK l;
} PMutex;
#define PMUTEX_INIT {SRWLOCK_INIT}
typedef struct {
    CONDITION_VARIABLE c;
} PCond;
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t l;
} PMutex;
#define PMUTEX_INIT {PTHREAD_MUTEX_INITIALIZER}
typedef struct {
    pthread_cond_t c;
} PCond;
#endif
void pm_init(PMutex* m); /* or PMUTEX_INIT, for a static lock that must work before any init runs */
void pm_lock(PMutex* m);
void pm_unlock(PMutex* m);
void pc_init(PCond* c);
void pc_signal(PCond* c);
void pc_broadcast(PCond* c);
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns); /* 0 woken, 1 timed out */
typedef void (*PThreadFn)(void* arg);
int plat_thread(PThreadFn fn, void* arg, size_t stack);
void plat_thread_exit(void);
/* ends the process now, with code: stdio flushed, no atexit handlers, the other threads not joined */
void plat_exit_now(int code) __attribute__((noreturn));
void plat_sleep_ns(i64 ns);
void plat_yield(void);
u64 plat_ns(void); /* monotonic */
i64 plat_unix_time(void);
i64 plat_tz_offset(void); /* local - UTC, seconds */
u8* plat_vm_reserve(u64 size);
/* written-page tracking by range: calls cb for each run of pages in [p, p+size) written since the range was last
   scanned, and starts tracking them again. -1 when the platform can't (then everything must be checked by content).
   Linux: userfaultfd write protection (async) and PAGEMAP_SCAN; Windows: GetWriteWatch, with SWR_WW set */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size));
int plat_vm_commit(u8* p, u64 size);
void plat_vm_decommit(u8* p, u64 size);
void* plat_dl_open(const char* path);
void* plat_dl_sym(void* h, const char* name);
const char* plat_dl_error(void); /* why the last plat_dl_open or plat_dl_sym failed */
int plat_mkdir(const char* path);
int plat_is_dir(const char* path);
typedef struct {
    void* h;
    int first;
    char name[512];
    int is_dir;
    u64 size;
} PDir;
int plat_dir_open(PDir* d, const char* path);
int plat_dir_next(PDir* d);
void plat_dir_close(PDir* d);
u64 plat_file_size(const char* path);
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off); /* bytes read (fewer at the end of the file), -1 on error */
/* a whole file mapped read-only. An empty file maps to data NULL with size 0 (and succeeds) */
typedef struct {
    const u8* data;
    u64 size;
} PFileMap;
int plat_file_map(PFileMap* m, const char* path); /* 0, or -1 when the file can't be opened or mapped */
void plat_file_unmap(PFileMap* m);
/* replaces dst with tmp in one step (rename), so a reader never sees dst missing or half written. On failure dst is
   kept and tmp removed */
int plat_replace_file(const char* tmp, const char* dst);
void plat_install_crash_handler(void);
extern __thread void* tls_thread;
void plat_prof_start(void); /* the sampling profiler (profile option), a no-op outside Windows */
#if defined(_WIN32)
void plat_prof_add_thread(HANDLE h);
#endif

#endif
