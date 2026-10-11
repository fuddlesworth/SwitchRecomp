/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#define _GNU_SOURCE
/* platform glue: threads, timers, VM, files. win32 + posix */
#include "platform/plat.h"
#include "core/loader.h"
#include "core/main.h"
#include "gpu/gl/overlay.h"
#include "gpu/renderer.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#include <stdarg.h>
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
    if (lvl <= 1 || g_verbose >= 2) {
        fputs(buf, stderr);
#if defined(__ANDROID__)
        android_log(buf);
#endif
    }
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
#if defined(__ANDROID__)
    {
        char m[600];
        snprintf(m, sizeof m, "Error: %.500s\n(see %s.log)", buf, g_exename);
        overlay_message(m);
        static u32 black[64 * 36];
        for (int i = 0; i < 150; i++) {
            gpu_gl_present_cpu(black, 64, 36, 64 * 4);
            plat_sleep_ns(200000000);
        }
    }
#endif
    fflush(NULL);
    _exit(1);
}

#if defined(_WIN32)
/* ------------------------------------------------------------------ Windows */
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
/* ------------------------------------------------------------------ sampling profiler (profile=1)
   ~1 ms tick: suspend each thread, grab rip, map it to a game function / exe offset / module.
   profile.txt gets rewritten every 10 s. */
static HANDLE g_pth[256];
static volatile LONG g_npth;
void plat_prof_add_thread(HANDLE h) {
    HANDLE d = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    LONG i = InterlockedIncrement(&g_npth) - 1;
    if (i < 256)
        g_pth[i] = d;
    else
        CloseHandle(d);
}
typedef struct {
    u64 host;
    u32 idx;
} PEnt;
static PEnt* g_pe;
static u32 g_npe;
static u64 g_plo, g_phi;
static u32* g_pcnt;
#define PMOD 32
static struct {
    u64 base;
    char name[64];
    u32 n;
} g_pmod[PMOD];
static int g_npmod;
#define PEXE 4096
static struct {
    u32 off;
    u32 n;
} g_pexe[PEXE];
static u64 g_ptot, g_pguest, g_pexe_n;
static int pe_cmp(const void* a, const void* b) {
    u64 x = ((const PEnt*)a)->host, y = ((const PEnt*)b)->host;
    return x < y ? -1 : x > y;
}
static void prof_note(u64 rip) {
    g_ptot++;
    if (rip >= g_plo && rip < g_phi + 0x4000) {
        u32 lo = 0, hi = g_npe;
        while (hi - lo > 1) {
            u32 m = (lo + hi) / 2;
            if (g_pe[m].host <= rip)
                lo = m;
            else
                hi = m;
        }
        g_pcnt[lo]++;
        g_pguest++;
        return;
    }
    HMODULE hm = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)rip, &hm) ||
        !hm) {
        hm = NULL;
    }
    u64 base = (u64)(uintptr_t)hm;
    if (hm == GetModuleHandleA(NULL)) {
        u32 off = (u32)((rip - base) & ~0x3Full);
        u32 k = (off * 2654435761u) >> 20;
        for (int t = 0; t < PEXE; t++) {
            u32 s = (k + t) & (PEXE - 1);
            if (!g_pexe[s].n || g_pexe[s].off == off) {
                g_pexe[s].off = off;
                g_pexe[s].n++;
                break;
            }
        }
        g_pexe_n++;
    }
    int i;
    for (i = 0; i < g_npmod; i++)
        if (g_pmod[i].base == base) break;
    if (i == g_npmod && g_npmod < PMOD) {
        g_pmod[i].base = base;
        g_pmod[i].n = 0;
        if (!hm)
            strcpy(g_pmod[i].name, "(no module: generated/JIT code)");
        else {
            char pth[MAX_PATH];
            GetModuleFileNameA(hm, pth, MAX_PATH);
            const char* s = strrchr(pth, '\\');
            snprintf(g_pmod[i].name, 64, "%s", s ? s + 1 : pth);
        }
        g_npmod++;
    }
    if (i < g_npmod) g_pmod[i].n++;
}
static const char* g_pfile = "profile.txt";
static int g_pboot;
static void prof_write(void) {
    FILE* f = fopen(g_pfile, "w");
    if (!f) return;
    /* idle threads sit in ntdll/kernelbase/win32u, so percentages are of non-idle samples */
    u64 idle = 0;
    for (int i = 0; i < g_npmod; i++) {
        const char* n = g_pmod[i].name;
        if (!_stricmp(n, "ntdll.dll") || !_stricmp(n, "kernelbase.dll") || !_stricmp(n, "win32u.dll") ||
            !_stricmp(n, "kernel32.dll"))
            idle += g_pmod[i].n;
    }
    u64 act = g_ptot > idle ? g_ptot - idle : 1;
    fprintf(f, "SwitchRecomp profile: %llu samples, %llu active (excluding waits). %% of active time\n\n",
            (unsigned long long)g_ptot, (unsigned long long)act);
    fprintf(f, "modules:\n");
    for (int i = 0; i < g_npmod; i++) {
        const char* n = g_pmod[i].name;
        int w = !_stricmp(n, "ntdll.dll") || !_stricmp(n, "kernelbase.dll") || !_stricmp(n, "win32u.dll") ||
                !_stricmp(n, "kernel32.dll");
        if (w)
            fprintf(f, "   (waits) %s: %llu samples\n", n, (unsigned long long)g_pmod[i].n);
        else
            fprintf(f, "  %6.2f%%  %s\n", 100.0 * g_pmod[i].n / act, n);
    }
    fprintf(f, "  %6.2f%%  game code (functions below)\n", 100.0 * g_pguest / act);
    fprintf(f, "\ngame functions (guest address):\n");
    u64 top[6];
    int ntop = 0;
    for (int r = 0; r < 60; r++) {
        u32 best = ~0u, bn = 0;
        for (u32 i = 0; i < g_npe; i++)
            if (!(g_pcnt[i] & 0x80000000u) && g_pcnt[i] > bn) {
                bn = g_pcnt[i];
                best = i;
            }
        if (best == ~0u || !bn || bn * 2000ull < act) break;
        fprintf(f, "  %6.2f%%  %llx\n", 100.0 * bn / act, (unsigned long long)g_mod->entry_addr[g_pe[best].idx]);
        if (r < 6) top[ntop++] = g_mod->entry_addr[g_pe[best].idx];
        g_pcnt[best] |= 0x80000000u;
    }
    for (u32 i = 0; i < g_npe; i++) g_pcnt[i] &= 0x7FFFFFFFu;
    /* raw A64 words of the top functions, for disassembling later */
    fprintf(f, "\ncode of the hottest game functions (first 96 instructions):\n");
    for (int t = 0; t < ntop; t++) {
        fprintf(f, "  %llx:", (unsigned long long)top[t]);
        const u32* w = (const u32*)GP(top[t]);
        for (int k = 0; k < 96; k++) {
            if (k % 16 == 0) fprintf(f, "\n   ");
            fprintf(f, " %08x", w[k]);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\nruntime (offset in the exe):\n");
    for (int r = 0; r < 40; r++) {
        int best = -1;
        u32 bn = 0;
        for (int s = 0; s < PEXE; s++)
            if (g_pexe[s].n > bn && !(g_pexe[s].n & 0x80000000u)) {
                bn = g_pexe[s].n;
                best = s;
            }
        if (best < 0 || bn * 2000ull < act) break;
        fprintf(f, "  %6.2f%%  +%x\n", 100.0 * bn / act, g_pexe[best].off);
        g_pexe[best].n |= 0x80000000u;
    }
    for (int s = 0; s < PEXE; s++) g_pexe[s].n &= 0x7FFFFFFFu;
    fclose(f);
}
static unsigned __stdcall prof_thread(void* a) {
    (void)a;
    u64 last = GetTickCount64();
    DWORD me = GetCurrentThreadId();
    u64 start = last;
    for (;;) {
        Sleep(g_pboot ? 2 : 1);
        LONG n = g_npth;
        if (n > 256) n = 256;
        for (LONG i = 0; i < n; i++) {
            HANDLE h = g_pth[i];
            if (!h || GetThreadId(h) == me) continue;
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT cx;
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &cx)) {
                ResumeThread(h);
                prof_note(cx.Rip);
            } else
                ResumeThread(h);
        }
        if (GetTickCount64() - last >= (g_pboot ? 5000u : 10000u)) {
            last = GetTickCount64();
            prof_write();
        }
        if (g_pboot && GetTickCount64() - start >= 90000) {
            prof_write();
            break;
        }
    }
    return 0;
}
void plat_prof_start(int boot) {
    if (!g_mod || !g_mod->nentries) return;
    g_pboot = boot;
    if (boot) g_pfile = "startup.txt";
    g_npe = g_mod->nentries;
    g_pe = malloc(sizeof(PEnt) * g_npe);
    g_pcnt = calloc(g_npe, 4);
    g_plo = ~0ull;
    g_phi = 0;
    for (u32 i = 0; i < g_npe; i++) {
        g_pe[i].host = (u64)(uintptr_t)g_mod->entry_fn[i];
        g_pe[i].idx = i;
        if (g_pe[i].host < g_plo) g_plo = g_pe[i].host;
        if (g_pe[i].host > g_phi) g_phi = g_pe[i].host;
    }
    qsort(g_pe, g_npe, sizeof(PEnt), pe_cmp);
    {
        HANDLE d = NULL;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &d, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        plat_prof_add_thread(d);
        CloseHandle(d);
    }
    uintptr_t h = _beginthreadex(NULL, 0, prof_thread, NULL, 0, NULL);
    if (h) {
        SetThreadPriority((HANDLE)h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle((HANDLE)h);
    }
}
#else
/* ------------------------------------------------------------------ POSIX */
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
#endif
#ifndef _WIN32
void plat_prof_start(int boot) { (void)boot; }
#endif
