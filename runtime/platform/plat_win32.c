/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the platform layer on Windows: locks, threads, time, virtual memory and write watching, files, DLLs, the crash
   handler */
#include "core/main.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>
#include <io.h> /* _get_osfhandle */
void pm_init(PMutex* m) { InitializeSRWLock(&m->l); }
void pm_lock(PMutex* m) { AcquireSRWLockExclusive(&m->l); }
void pm_unlock(PMutex* m) { ReleaseSRWLockExclusive(&m->l); }
void pc_init(PCond* c) { InitializeConditionVariable(&c->c); }
void pc_signal(PCond* c) { WakeConditionVariable(&c->c); }
void pc_broadcast(PCond* c) { WakeAllConditionVariable(&c->c); }
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    DWORD ms = ns < 0 ? INFINITE : (DWORD)((ns + 999999) / 1000000);
    if (!SleepConditionVariableSRW(&c->c, &m->l, ms, 0)) return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
    return 0;
}
typedef struct {
    PThreadFn fn;
    void* arg;
} TArg;
static unsigned __stdcall tstart(void* p) {
    TArg a = *(TArg*)p;
    free(p);
    a.fn(a.arg);
    return 0;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    TArg* a = malloc(sizeof *a);
    a->fn = fn;
    a->arg = arg;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack, tstart, a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) return -1;
    { plat_prof_add_thread((HANDLE)h); }
    CloseHandle((HANDLE)h);
    return 0;
}
void plat_thread_exit(void) { _endthreadex(0); }
static LONG g_hires;
void plat_sleep_ns(i64 ns) {
    if (!g_hires) {
        typedef UINT(WINAPI * TBP)(UINT);
        HMODULE w = LoadLibraryA("winmm.dll");
        if (w) {
            TBP f = (TBP)GetProcAddress(w, "timeBeginPeriod");
            if (f) f(1);
        }
        g_hires = 1;
    }
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    /* use the high-res waitable timer when the OS has it */
    static __thread HANDLE tmr;
    if (!tmr) {
        tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!tmr) tmr = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (tmr) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);
        if (!due.QuadPart) due.QuadPart = -1;
        SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(tmr, INFINITE);
    } else
        Sleep((DWORD)(ns / 1000000));
}
void plat_yield(void) { SwitchToThread(); }
void plat_thread_low_priority(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
void plat_thread_high_priority(void) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); }
u64 plat_thread_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return ((((u64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((u64)u.dwHighDateTime << 32) | u.dwLowDateTime)) *
           100;
}
u64 plat_process_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return ((((u64)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((u64)u.dwHighDateTime << 32) | u.dwLowDateTime)) *
           100;
}
u64 plat_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (u64)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
i64 plat_unix_time(void) { return (i64)time(NULL); }
/* UTC offset in seconds, DST included */
i64 plat_tz_offset(void) {
    TIME_ZONE_INFORMATION z;
    DWORD r = GetTimeZoneInformation(&z);
    if (r == TIME_ZONE_ID_INVALID) return 0;
    long b = z.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? z.DaylightBias : r == TIME_ZONE_ID_STANDARD ? z.StandardBias : 0);
    return -(i64)b * 60;
}
int g_ww_ok; /* MEM_WRITE_WATCH: Windows tracks written pages, so unchanged buffers don't need a reread */
u8* plat_vm_reserve(u64 size) {
    void* p = !getenv("SWR_WW") ? NULL : VirtualAlloc(NULL, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (p) {
        g_ww_ok = 1;
        return (u8*)p;
    }
    return (u8*)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}
int plat_ww_collect(u8* base, u64 size, void (*cb)(u64)) {
    static PVOID a[16384];
    if (!g_ww_ok) return -1;
    for (int it = 0; it < 64; it++) {
        ULONG_PTR n = 16384;
        ULONG gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, (SIZE_T)size, a, &n, &gran)) return -1;
        for (ULONG_PTR i = 0; i < n; i++) cb((u64)((u8*)a[i] - base));
        if (n < 16384) break;
    }
    return 0;
}
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    static PVOID a[4096];
    if (!g_ww_ok) return -1;
    for (int it = 0; it < 64; it++) {
        ULONG_PTR n = 4096;
        ULONG gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, p, (SIZE_T)size, a, &n, &gran)) return -1;
        for (ULONG_PTR i = 0; i < n; i++) cb((u8*)a[i], gran);
        if (n < 4096) break;
    }
    return 0;
}
int plat_vm_commit(u8* p, u64 size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1; }
void plat_vm_decommit(u8* p, u64 size) { VirtualFree(p, size, MEM_DECOMMIT); }
void* plat_dl_open(const char* path) { return (void*)LoadLibraryA(path); }
void* plat_dl_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
int plat_mkdir(const char* p) { return CreateDirectoryA(p, NULL) ? 0 : -1; }
int plat_is_dir(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int plat_dir_open(PDir* d, const char* path) {
    char p[1100];
    snprintf(p, sizeof p, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    d->h = h;
    d->first = 1;
    strncpy(d->name, fd.cFileName, 511);
    d->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    d->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    return 0;
}
int plat_dir_next(PDir* d) {
    for (;;) {
        if (d->first) {
            d->first = 0;
        } else {
            WIN32_FIND_DATAA fd;
            if (!FindNextFileA((HANDLE)d->h, &fd)) return 0;
            strncpy(d->name, fd.cFileName, 511);
            d->is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            d->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
        if (strcmp(d->name, ".") && strcmp(d->name, "..")) return 1;
    }
}
void plat_dir_close(PDir* d) { FindClose((HANDLE)d->h); }
u64 plat_file_size(const char* p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return 0;
    return ((u64)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
int plat_pread(FILE* f, void* buf, u64 size, u64 off) {
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)size, &got, &ov)) return -1;
    return (int)got;
}
int plat_ncpu(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    crash_report((void*)e->ContextRecord->Rip, (void*)e->ExceptionRecord->ExceptionInformation[1],
                 (int)e->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}
static LONG WINAPI veh_fatal(EXCEPTION_POINTERS* e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_STACK_OVERFLOW || c == 0xC0000409 /* STACK_BUFFER_OVERRUN / fastfail */ ||
        c == 0xC0000374 /* heap corruption */) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) {
            logf_(0, "\n*** Shutdown: exception %08lx at %p (stack/heap) ***\n", (unsigned long)c,
                  (void*)e->ContextRecord->Rip);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#include <signal.h>
static void on_abort(int s) {
    logf_(0, "\n*** Shutdown: abort() (signal %d), probably the video driver or out of memory ***\n", s);
}
static void on_exit_log(void) { logf_(0, "program shutdown\n"); }
static void inv_param(const wchar_t* e, const wchar_t* f, const wchar_t* fi, unsigned l, uintptr_t r) {
    (void)e;
    (void)f;
    (void)fi;
    (void)l;
    (void)r;
    logf_(0, "warning: invalid parameter in the C library\n");
}
void plat_install_crash_handler(void) {
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(0, veh_fatal);
    signal(SIGABRT, on_abort);
    atexit(on_exit_log);
    _set_invalid_parameter_handler(inv_param);
    /* keep the display/system awake while playing */
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
}
