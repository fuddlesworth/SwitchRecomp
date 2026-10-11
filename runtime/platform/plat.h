/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the platform layer (plat.c): threads, locks, time, virtual memory, files, libraries */
#ifndef SWR_PLAT_H
#define SWR_PLAT_H
#include "abi/cpu.h"
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct {
    SRWLOCK l;
} PMutex;
typedef struct {
    CONDITION_VARIABLE c;
} PCond;
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t l;
} PMutex;
typedef struct {
    pthread_cond_t c;
} PCond;
#endif
void pm_init(PMutex* m);
void pm_lock(PMutex* m);
void pm_unlock(PMutex* m);
void pc_init(PCond* c);
void pc_signal(PCond* c);
void pc_broadcast(PCond* c);
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns); /* 0 woken, 1 timed out */
typedef void (*PThreadFn)(void* arg);
int plat_thread(PThreadFn fn, void* arg, size_t stack);
void plat_thread_low_priority(void);
void plat_thread_exit(void);
void plat_sleep_ns(i64 ns);
void plat_yield(void);
u64 plat_ns(void);             /* monotonic */
u64 plat_thread_cpu_ns(void);  /* cpu time, this thread */
u64 plat_process_cpu_ns(void); /* cpu time, whole process */
i64 plat_unix_time(void);
i64 plat_tz_offset(void); /* local - UTC, seconds */
u8* plat_vm_reserve(u64 size);
int plat_ww_collect(u8* base, u64 size, void (*cb)(u64));
void plat_thread_high_priority(void);
extern int g_ww_ok;
/* written-page tracking by range: calls cb for each run of pages in [p, p+size) written since the range was last
   scanned, and starts tracking them again. -1 when the platform can't (then everything must be checked by content).
   Linux: userfaultfd write protection (async) and PAGEMAP_SCAN; Windows: GetWriteWatch, with SWR_WW set */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size));
int plat_vm_commit(u8* p, u64 size);
void plat_vm_decommit(u8* p, u64 size);
void* plat_dl_open(const char* path);
void* plat_dl_sym(void* h, const char* name);
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
int plat_pread(FILE* f, void* buf, u64 size, u64 off);
void plat_install_crash_handler(void);
int plat_ncpu(void);
extern __thread void* tls_thread;
void plat_prof_start(int boot); /* the sampling profiler (profile option), a no-op outside Windows */
#if defined(_WIN32)
void plat_prof_add_thread(HANDLE h);
#endif

#endif
