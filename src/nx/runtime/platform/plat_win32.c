/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the platform layer on Windows: locks, threads, time, virtual memory and write watching, files, DLLs, the crash
   handler */
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>
#include <io.h> /* _get_osfhandle */
#include <fcntl.h>
typedef struct {
    char* out;
    size_t cap;
    HWND edit;
    int result;
} TextDialog;
static LRESULT CALLBACK text_dialog_proc(HWND win, UINT msg, WPARAM wp, LPARAM lp) {
    TextDialog* d = (TextDialog*)GetWindowLongPtrW(win, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(win, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
        return TRUE;
    }
    if (msg == WM_COMMAND && d) {
        int id = LOWORD(wp);
        if (id == IDOK) {
            int len = GetWindowTextLengthW(d->edit);
            WCHAR* wide = malloc(((size_t)len + 1) * sizeof *wide);
            if (wide) {
                GetWindowTextW(d->edit, wide, len + 1);
                int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
                if (bytes > 0 && (size_t)bytes > d->cap) {
                    free(wide);
                    MessageBoxW(win, L"Text is too long.", L"ScarletNX", MB_OK | MB_ICONWARNING);
                    SetFocus(d->edit);
                    return 0;
                }
                if (bytes > 0 && WideCharToMultiByte(CP_UTF8, 0, wide, -1, d->out, bytes, NULL, NULL) == bytes)
                    d->result = 0;
                free(wide);
            }
            DestroyWindow(win);
            return 0;
        }
        if (id == IDCANCEL) {
            d->result = 1;
            DestroyWindow(win);
            return 0;
        }
    }
    if (msg == WM_CLOSE) {
        if (d) d->result = 1;
        DestroyWindow(win);
        return 0;
    }
    return DefWindowProcW(win, msg, wp, lp);
}
int plat_text_input(char* out, size_t cap, const char* initial) {
    if (!cap) return -1;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW cls = {.lpfnWndProc = text_dialog_proc,
                     .hInstance = inst,
                     .lpszClassName = L"ScarletNXTextInput",
                     .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                     .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1)};
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
    TextDialog d = {.out = out, .cap = cap, .result = -1};
    int x = (GetSystemMetrics(SM_CXSCREEN) - 360) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - 140) / 2;
    HWND win = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls.lpszClassName, L"ScarletNX text entry",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, 360, 140, NULL, NULL, inst, &d);
    if (!win) return -1;
    CreateWindowExW(0, L"STATIC", L"Enter text:", WS_CHILD | WS_VISIBLE, 12, 12, 324, 18, win, NULL, inst, NULL);
    d.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12,
                             34, 324, 24, win, (HMENU)100, inst, NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 176, 70, 76,
                              25, win, (HMENU)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 260, 70, 76, 25, win,
                                  (HMENU)IDCANCEL, inst, NULL);
    if (!d.edit || !ok || !cancel) {
        DestroyWindow(win);
        return -1;
    }
    if (initial) {
        int n = MultiByteToWideChar(CP_UTF8, 0, initial, -1, NULL, 0);
        WCHAR* wide = n > 0 ? malloc((size_t)n * sizeof *wide) : NULL;
        if (wide) {
            if (MultiByteToWideChar(CP_UTF8, 0, initial, -1, wide, n)) SetWindowTextW(d.edit, wide);
            free(wide);
        }
    }
    ShowWindow(win, SW_SHOW);
    SetFocus(d.edit);
    MSG msg;
    while (IsWindow(win)) {
        BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0) {
            if (!got) PostQuitMessage((int)msg.wParam);
            DestroyWindow(win);
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.hwnd == d.edit && msg.wParam == VK_RETURN) {
            SendMessageW(win, WM_COMMAND, IDOK, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return d.result;
}
void pc_init(PCond* c) { InitializeConditionVariable(&c->c); }
void pc_signal(PCond* c) { WakeConditionVariable(&c->c); }
void pc_broadcast(PCond* c) { WakeAllConditionVariable(&c->c); }
static volatile LONG g_hires;
/* 1 ms timer resolution (the default is 15.6 ms), set before the first sleep or timed wait, whichever comes first */
static void hires_init(void) {
    if (g_hires || InterlockedExchange(&g_hires, 1)) return;
    typedef UINT(WINAPI * TBP)(UINT);
    HMODULE w = LoadLibraryA("winmm.dll");
    if (w) {
        TBP f = (TBP)GetProcAddress(w, "timeBeginPeriod");
        if (f) f(1);
    }
}
int pc_wait(PCond* c, PMutex* m, i64 ns) {
    hires_init();
    /* rounded up to whole ms, and kept below INFINITE: ns + 999999 overflowed near INT64_MAX, and the DWORD cut long
       timeouts short */
    DWORD ms = INFINITE;
    if (ns >= 0) {
        u64 r = ((u64)ns + 999999) / 1000000;
        ms = r >= INFINITE ? INFINITE - 1 : (DWORD)r;
    }
    if (!SleepConditionVariableSRW(&c->c, &m->l, ms, 0)) return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
    return 0;
}
typedef struct {
    PThreadFn fn;
    void* arg;
    size_t stack;
} TArg;
static __thread HANDLE g_sleep_tmr; /* plat_sleep_ns's timer, closed when the thread ends */
static void sleep_timer_close(void) {
    if (g_sleep_tmr) CloseHandle(g_sleep_tmr);
    g_sleep_tmr = NULL;
}
/* stack left for the crash report when a thread overflows its stack: by default the exception filter gets little
   more than the guard page, and the report overflowed again */
static void stack_guarantee(void) {
    ULONG g = 32768;
    SetThreadStackGuarantee(&g);
}
static unsigned __stdcall tstart(void* p) {
    TArg a = *(TArg*)p;
    free(p);
    if (!a.stack || a.stack >= (256u << 10)) stack_guarantee(); /* not on small stacks (the 64 KiB audio clock) */
    a.fn(a.arg);
    sleep_timer_close();
    return 0;
}
int plat_thread(PThreadFn fn, void* arg, size_t stack) {
    TArg* a = malloc(sizeof *a);
    if (!a) return -1;
    a->fn = fn;
    a->arg = arg;
    a->stack = stack;
    uintptr_t h = _beginthreadex(NULL, (unsigned)stack, tstart, a, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) {
        free(a);
        return -1;
    }
    /* only for the profiler: each thread's handle is kept for sampling */
    if (g_opt_prof) plat_prof_add_thread((HANDLE)h);
    CloseHandle((HANDLE)h);
    return 0;
}
void plat_thread_exit(void) {
    sleep_timer_close();
    _endthreadex(0);
}
/* TerminateProcess rather than _exit: ExitProcess stops the other threads wherever they are and then runs every DLL's
   detach code, which can wait on a lock one of them held (the video driver's) */
void plat_exit_now(int code) {
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
    _exit(code);
}
void plat_sleep_ns(i64 ns) {
    hires_init();
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    /* use the high-res waitable timer when the OS has it */
    if (!g_sleep_tmr) {
        g_sleep_tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!g_sleep_tmr) g_sleep_tmr = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (g_sleep_tmr) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100);
        if (!due.QuadPart) due.QuadPart = -1;
        SetWaitableTimer(g_sleep_tmr, &due, 0, NULL, NULL, FALSE);
        WaitForSingleObject(g_sleep_tmr, INFINITE);
    } else
        Sleep((DWORD)(ns / 1000000));
}
void plat_yield(void) { SwitchToThread(); }
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
/* MEM_WRITE_WATCH: Windows tracks written pages, so unchanged buffers don't need a reread. On unless SNX_NO_WW is set,
   as on Linux: without it every cache checks its memory by content each time, and Super Mario Odyssey's GPU thread
   spent a third of its time hashing render targets */
static int g_ww_ok;
u8* plat_vm_reserve(u64 size) {
    void* p = getenv("SNX_NO_WW") ? NULL : VirtualAlloc(NULL, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    if (p) {
        g_ww_ok = 1;
        return (u8*)p;
    }
    return (u8*)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
}
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size)) {
    /* on the stack, so two threads can scan at once. Each call reports and resets up to 1024 pages, so a range is
       done in size / 4 MiB calls: running out of calls (pages written again meanwhile) leaves pages unreported, and
       the caller then has to check by content */
    PVOID a[1024];
    if (!g_ww_ok) return -1;
    for (u64 it = 0, max = size / (4096ull * 1024) + 2; it < max; it++) {
        ULONG_PTR n = 1024;
        ULONG gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, p, (SIZE_T)size, a, &n, &gran)) return -1;
        for (ULONG_PTR i = 0; i < n; i++) cb((u8*)a[i], gran);
        if (n < 1024) return 0;
    }
    return -1;
}
/* MEM_COMMIT charges the whole range to the commit limit (page file included) up front */
int plat_vm_commit(u8* p, u64 size) {
    if (VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE)) return 0;
    WARN("memory: could not commit %llu KB (error %lu): out of memory or page file\n", (unsigned long long)(size >> 10),
         (unsigned long)GetLastError());
    return -1;
}
void plat_vm_decommit(u8* p, u64 size) { VirtualFree(p, size, MEM_DECOMMIT); }
void* plat_dl_open(const char* path) { return (void*)LoadLibraryA(path); }
void* plat_dl_sym(void* h, const char* n) { return (void*)GetProcAddress((HMODULE)h, n); }
const char* plat_dl_error(void) {
    static __thread char buf[300];
    DWORD e = GetLastError();
    char m[256];
    if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, m, sizeof m, NULL))
        m[0] = 0;
    size_t L = strlen(m);
    while (L && (m[L - 1] == '\n' || m[L - 1] == '\r' || m[L - 1] == ' ')) m[--L] = 0;
    snprintf(buf, sizeof buf, "%s (error %lu)", m[0] ? m : "unknown error", (unsigned long)e);
    return buf;
}
int plat_is_dir(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
int plat_dir_open(PDir* d, const char* path) {
    char p[1100];
    if ((size_t)snprintf(p, sizeof p, "%s\\*", path) >= sizeof p) return -1; /* cut, it would list another folder */
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
/* ReadFile takes a DWORD and fails with ERROR_HANDLE_EOF at the end of the file: read in 1 GiB pieces, and the end of
   the file is a short read like pread's, not an error */
i64 plat_pread(FILE* f, void* buf, u64 size, u64 off) {
    HANDLE h = (HANDLE)_get_osfhandle(_fileno(f));
    u64 got = 0;
    while (got < size) {
        u64 left = size - got, at = off + got;
        DWORD want = left > (1u << 30) ? (1u << 30) : (DWORD)left, n = 0;
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        if (!ReadFile(h, (u8*)buf + got, want, &n, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return -1;
        }
        if (!n) break;
        got += n;
    }
    return (i64)got;
}
static int file_map(PFileMap* m, const char* path, const u64* indexed_size) {
    m->data = NULL;
    m->size = 0;
    if (indexed_size && (*indexed_size > SIZE_MAX || *indexed_size > INT64_MAX)) return -1;
    wchar_t* w = snx_host_wpath(path);
    if (!w) return -1;
    /* a directory doesn't open without FILE_FLAG_BACKUP_SEMANTICS, so this is a regular file */
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (f == INVALID_HANDLE_VALUE) return -1;
    u64 size;
    if (indexed_size) {
        size = *indexed_size;
    } else {
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 0) {
            CloseHandle(f);
            return -1;
        }
        size = (u64)sz.QuadPart;
    }
    if (size > SIZE_MAX) {
        CloseHandle(f);
        return -1;
    }
    if (size) { /* a mapping of an empty file fails */
        HANDLE mp = CreateFileMappingW(f, NULL, PAGE_READONLY, (DWORD)(size >> 32), (DWORD)size, NULL);
        void* p = mp ? MapViewOfFile(mp, FILE_MAP_READ, 0, 0, (SIZE_T)size) : NULL;
        if (mp) CloseHandle(mp); /* the view keeps the mapping and the file */
        if (!p) {
            CloseHandle(f);
            return -1;
        }
        m->data = p;
        m->size = size;
    }
    CloseHandle(f);
    return 0;
}
int plat_file_map(PFileMap* m, const char* path) { return file_map(m, path, NULL); }
int plat_file_map_sized(PFileMap* m, const char* path, u64 size) { return file_map(m, path, &size); }
void plat_file_unmap(PFileMap* m) {
    if (m->data && !m->borrowed) UnmapViewOfFile(m->data);
    m->data = NULL;
    m->size = 0;
    m->borrowed = 0;
}
/* rename fails on Windows when dst exists, and a remove first loses dst if the process dies in between */
int plat_replace_file(const char* tmp, const char* dst) {
    wchar_t *wt = snx_host_wpath(tmp), *wd = snx_host_wpath(dst);
    int ok = wt && wd && MoveFileExW(wt, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) remove(tmp);
    free(wt);
    free(wd);
    return ok ? 0 : -1;
}
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    log_crash_mode(); /* the faulting code may hold the log's lock */
    EXCEPTION_RECORD* r = e->ExceptionRecord;
    /* only an access violation or in-page error has the address it touched (ExceptionInformation[1]) */
    int access = r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR;
    void* fault = access && r->NumberParameters >= 2 ? (void*)r->ExceptionInformation[1] : NULL;
    crash_report((void*)e->ContextRecord->Rip, fault, (int)r->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}
static LONG WINAPI veh_fatal(EXCEPTION_POINTERS* e) {
    DWORD c = e->ExceptionRecord->ExceptionCode;
    /* __fastfail never reaches a handler, so 0xC0000409 only shows up here when something raises it the old way */
    if (c == EXCEPTION_STACK_OVERFLOW || c == 0xC0000409 /* STACK_BUFFER_OVERRUN */ ||
        c == 0xC0000374 /* heap corruption */) {
        static volatile LONG once;
        if (!InterlockedExchange(&once, 1)) {
            log_crash_mode();
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
    stack_guarantee(); /* the main thread's; plat_thread's threads set their own */
}
