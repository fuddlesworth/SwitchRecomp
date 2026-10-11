/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* win32 backend: window, WGL, keyboard, XInput.
   F1 help  F3 screenshot  F4 fps limit  F11 fullscreen  Esc quit */
#include "core/main.h"
#include "core/options.h"
#include "gpu/renderer.h"
#include "hle/hid.h"
#include "kernel/kernel.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#include <windows.h>
#include <mmsystem.h> /* timeBeginPeriod: rt.h defines WIN32_LEAN_AND_MEAN, which leaves it out of windows.h */

static HWND g_hwnd;
static HDC g_hdc;
static HGLRC g_ctx, g_wctx;
static volatile int g_ww = 1280, g_wh = 720;
static u64 keys_held;
static int g_fullscreen = 0, g_want_fs = 1;
static WINDOWPLACEMENT g_wp = {sizeof(WINDOWPLACEMENT)};
static int g_seconds = 0;
static char g_shot[1024];
static u64 g_start;
static volatile int g_shot_req;
static HANDLE g_ready;

static int g_ls_mode = 0;
static char g_ls_path[1024];
void backend_ini(const char* k, const char* v) {
    if (!strcmp(k, "fullscreen"))
        g_want_fs = atoi(v);
    else if (!strcmp(k, "lossless_scaling"))
        g_ls_mode = atoi(v);
    else if (!strcmp(k, "lossless_path"))
        strncpy(g_ls_path, v, 1023);
    else if (!strcmp(k, "seconds"))
        g_seconds = atoi(v);
    else if (!strcmp(k, "shot"))
        strncpy(g_shot, v, 1023);
}
int video_has_gl(void) { return 1; }
void win_fatal_box(const char* msg) { MessageBoxA(g_hwnd, msg, "SwitchRecomp", MB_OK | MB_ICONERROR); }

/* ---------------------------------------------------------------- XInput, loaded dynamically */
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger, bRightTrigger;
    SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XGP;
typedef struct {
    DWORD dwPacketNumber;
    XGP Gamepad;
} XST;
typedef DWORD(WINAPI* XGetState)(DWORD, XST*);
static XGetState xget;
static void xinput_load(void) {
    const char* dl[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xget; i++) {
        HMODULE h = LoadLibraryA(dl[i]);
        if (h) xget = (XGetState)GetProcAddress(h, "XInputGetState");
    }
}

static void set_fullscreen(int on) {
    DWORD st = GetWindowLong(g_hwnd, GWL_STYLE);
    if (on && !g_fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(g_hwnd, &g_wp) &&
            GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(g_hwnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        ShowCursor(FALSE);
        g_fullscreen = 1;
    } else if (!on && g_fullscreen) {
        SetWindowLong(g_hwnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        ShowCursor(TRUE);
        g_fullscreen = 0;
    }
}

/* optional: launch the Lossless Scaling app next to the game (lossless_scaling=1).
   This path doesn't touch its dll, the app just captures our window. */
static int file_exists_(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int ls_find(char* out) {
    if (g_ls_path[0] && file_exists_(g_ls_path)) {
        strcpy(out, g_ls_path);
        return 1;
    }
    static char libs[10][600];
    int nl = 0;
    typedef LONG(WINAPI * RGV)(HKEY, LPCSTR, LPCSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    HMODULE a = LoadLibraryA("advapi32.dll");
    RGV rgv = a ? (RGV)GetProcAddress(a, "RegGetValueA") : NULL;
    char sp[600];
    DWORD sz = sizeof sp;
    if (rgv && rgv(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", 0x0000ffff, NULL, sp, &sz) == 0 && sp[0]) {
        strncpy(libs[nl], sp, 599);
        libs[nl][599] = 0;
        nl++;
    }
    strcpy(libs[nl++], "C:\\Program Files (x86)\\Steam");
    strcpy(libs[nl++], "C:\\Program Files\\Steam");
    {
        char vdf[700];
        snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", libs[0]);
        FILE* f = fopen(vdf, "r");
        if (f) {
            char ln[800];
            while (fgets(ln, sizeof ln, f) && nl < 10) {
                char* p = strstr(ln, "\"path\"");
                if (!p) continue;
                p = strchr(p + 6, '"');
                if (!p) continue;
                p++;
                char* e = strchr(p, '"');
                if (!e) continue;
                *e = 0;
                int k = 0;
                for (char* q = p; *q && k < 599; q++) {
                    if (q[0] == '\\' && q[1] == '\\') q++;
                    libs[nl][k++] = *q;
                }
                libs[nl][k] = 0;
                nl++;
            }
            fclose(f);
        }
    }
    for (int i = 0; i < nl; i++) {
        char c[800];
        snprintf(c, sizeof c, "%s\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe", libs[i]);
        for (char* q = c; *q; q++)
            if (*q == '/') *q = '\\';
        if (file_exists_(c)) {
            strcpy(out, c);
            return 1;
        }
    }
    return 0;
}
static void ls_launch(void) {
    if (!g_ls_mode) return;
    if (FindWindowA(NULL, "Lossless Scaling")) {
        LOG("lossless scaling: already running\n");
        return;
    }
    char p[1024];
    if (!ls_find(p)) {
        LOG("lossless scaling: not found (install it from Steam, or set lossless_path=C:\\...\\LosslessScaling.exe in the .ini)\n");
        return;
    }
    char dir[1024];
    strcpy(dir, p);
    char* sl = strrchr(dir, '\\');
    if (sl) *sl = 0;
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi;
    if (CreateProcessA(p, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        LOG("lossless scaling: started (%s)\n", p);
    } else
        LOG("lossless scaling: could not open %s (error %lu)\n", p, (unsigned long)GetLastError());
    SetForegroundWindow(g_hwnd);
}

/* window title update for lsfg on/off, posted from the GPU thread */
static char g_title[256];
void backend_lsfg_state(int on, int mult) {
    if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 7, (WPARAM)on, (LPARAM)mult);
}
/* F7 recording state in the title bar */
volatile int g_rec_toggle;
void backend_rec_state(int on) {
    if (g_hwnd) PostMessageA(g_hwnd, WM_APP + 8, (WPARAM)on, 0);
}
static const char* HELP =
    "SwitchRecomp - controls\n\n"
    "XInput (Xbox) controller: same as the Switch controller (A/B swapped by position).\n\n"
    "Keyboard:\n"
    "  Arrows = D-Pad     W A S D = Left stick    I J K L = Right stick\n"
    "  X / Space = A    Z = B    C = X    V = Y\n"
    "  Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -\n\n"
    "F1 = this help   F3 = screenshot   F4 = FPS limit 60 / unlimited   F5 = diagnostic dump (trace)\n"
    "F7 = record .mp4 video (press F7 again to stop; saved in the recordings folder)\n"
    "F8 = instant full-resolution screenshot (PNG in the captures folder)\n"
    "F11 / Alt+Enter = fullscreen   Esc = quit";

static u64 keymap(WPARAM k) {
    switch (k) {
        case 'X':
        case VK_SPACE: return 1ull << 0;
        case 'Z': return 1ull << 1;
        case 'C': return 1ull << 2;
        case 'V': return 1ull << 3;
        case 'Q': return 1ull << 6;
        case 'E': return 1ull << 7;
        case '1': return 1ull << 8;
        case '3': return 1ull << 9;
        case VK_RETURN: return 1ull << 10;
        case VK_BACK: return 1ull << 11;
        case VK_LEFT: return 1ull << 12;
        case VK_UP: return 1ull << 13;
        case VK_RIGHT: return 1ull << 14;
        case VK_DOWN: return 1ull << 15;
        /* stick keys, turned into axes later */
        case 'A': return 1ull << 40;
        case 'W': return 1ull << 41;
        case 'D': return 1ull << 42;
        case 'S': return 1ull << 43;
        case 'J': return 1ull << 44;
        case 'I': return 1ull << 45;
        case 'L': return 1ull << 46;
        case 'K': return 1ull << 47;
    }
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CLOSE:
            LOG("window: closed by the user or the system\n");
            g_quit = 1;
            return 0;
        case WM_QUERYENDSESSION: LOG("window: Windows is logging off / shutting down\n"); break;
        case WM_POWERBROADCAST:
            if (w == PBT_APMSUSPEND)
                LOG("window: the PC is going to sleep\n");
            else if (w == PBT_APMRESUMEAUTOMATIC)
                LOG("window: the PC resumed from sleep\n");
            break;
        case WM_SIZE:
            g_ww = LOWORD(l);
            g_wh = HIWORD(l);
            if (g_ww < 1) g_ww = 1;
            if (g_wh < 1) g_wh = 1;
            return 0;
        case WM_SYSKEYDOWN:
            if (w == VK_RETURN) {
                set_fullscreen(!g_fullscreen);
                return 0;
            }
            if (w == VK_F4) break; /* let Alt+F4 through */
        /* fallthrough */
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) {
                g_quit = 1;
                return 0;
            }
            if (w == VK_F11) {
                set_fullscreen(!g_fullscreen);
                return 0;
            }
            if (w == VK_F1) {
                int fs = g_fullscreen;
                if (fs) set_fullscreen(0);
                MessageBoxA(h, HELP, "SwitchRecomp", MB_OK | MB_ICONINFORMATION);
                if (fs) set_fullscreen(1);
                return 0;
            }
            if (w == VK_F3) {
                g_shot_req = 1;
                return 0;
            }
            if (w == VK_F7) {
                g_rec_toggle = 1;
                return 0;
            }
            if (w == VK_F8) {
                g_shot8_req = 1;
                return 0;
            }
            if (w == VK_F5) {
                CreateDirectoryA("captures", NULL);
                FILE* f = fopen("captures/threads.txt", "w"); /* written next to the trace */
                if (f) {
                    g_log_redirect = f;
                    kernel_dump_threads();
                    g_log_redirect = NULL;
                    fclose(f);
                }
                kernel_dump_threads();
                gpu_gl_dump_rts();
                return 0;
            }
            if (w == VK_F4) {
                g_opt_fps = g_opt_fps == 0 ? 1 : 0;
                char t[64];
                snprintf(t, sizeof t, "FPS limit: %s", g_opt_fps == 0 ? "unlimited" : "60");
                SetWindowTextA(h, t);
                {
                    char v[8];
                    snprintf(v, sizeof v, "%d", g_opt_fps);
                    ini_set("fps_limit", v);
                }
                return 0;
            }
            keys_held |= keymap(w);
            return 0;
        case WM_SYSKEYUP:
        case WM_KEYUP: keys_held &= ~keymap(w); return 0;
        case WM_KILLFOCUS: keys_held = 0; return 0;
        case WM_APP + 8: {
            char t[300];
            if (w)
                snprintf(t, sizeof t, "%s  |  RECORDING (F7 to stop)", g_title);
            else
                snprintf(t, sizeof t, "%s  |  Video saved in the recordings folder", g_title);
            SetWindowTextA(h, t);
            return 0;
        }
        case WM_APP + 7: {
            char t[160];
            if (w)
                snprintf(t, sizeof t, "%s  |  LSFG ON x%d (frame generation)", g_title, (int)l);
            else
                snprintf(t, sizeof t, "%s  |  LSFG standby", g_title);
            SetWindowTextA(h, t);
            return 0;
        }
        case WM_SETCURSOR:
            if (g_fullscreen && LOWORD(l) == HTCLIENT) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
    }
    return DefWindowProcA(h, m, w, l);
}

static char g_title[256];
/* window + message pump on their own thread so nothing stalls them */
static void win_thread(void* arg) {
    (void)arg;
    HINSTANCE hi = GetModuleHandle(NULL);
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "SwitchRecompWnd";
    RegisterClassA(&wc);
    RECT r = {0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("SwitchRecompWnd", g_title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                 PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    SetPixelFormat(g_hdc, ChoosePixelFormat(g_hdc, &pfd), &pfd);
    if (g_want_fs) set_fullscreen(1);
    ls_launch();
    SetEvent(g_ready);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    g_quit = 1;
}

void video_init(const char* title) {
    snprintf(g_title, sizeof g_title, "%s - SwitchRecomp  (F1 help)", title);
    xinput_load();
    timeBeginPeriod(1);
    g_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    _beginthread(win_thread, 1 << 20, NULL);
    WaitForSingleObject(g_ready, INFINITE);
    g_start = plat_ns();
    if (getenv("SWR_REC")) g_rec_toggle = 1;
    { /* under Proton the driver is Mesa; glthread has to be enabled from the Steam launch options */
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        if (nt && GetProcAddress(nt, "wine_get_version"))
            LOG("Proton/Wine detected: for more FPS put   mesa_glthread=true %%command%%   in the game's Steam launch options (moves the OpenGL driver work to another core)\n");
    }
    gpu_gl_start();
}

/* ---------------------------------------------------------------- GL, GPU thread */
typedef HGLRC(WINAPI* CCA)(HDC, HGLRC, const int*);
typedef BOOL(WINAPI* SWI)(int);
int backend_gl_create(void) {
    HGLRC tmp = wglCreateContext(g_hdc);
    if (!tmp) return 0;
    wglMakeCurrent(g_hdc, tmp);
    CCA cca = (CCA)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int vers[4][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}};
    for (int i = 0; i < 4 && !g_ctx && cca; i++) {
        int ca[] = {0x2091, vers[i][0], 0x2092, vers[i][1], 0x9126, 0x00000001, 0};
        g_ctx = cca(g_hdc, NULL, ca);
        if (g_ctx) g_wctx = cca(g_hdc, g_ctx, ca);
    } /* second shared context for the shader compile thread */
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    if (!g_ctx) {
        fatal("Your graphics card does not support OpenGL 4.3.\nPlease update your GPU driver.");
        return 0;
    }
    wglMakeCurrent(g_hdc, g_ctx);
    SWI si = (SWI)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(g_opt_vsync ? 1 : 0);
    return 1;
}
void* backend_gl_proc(const char* n) {
    void* p = (void*)wglGetProcAddress(n);
    if (!p || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) {
        static HMODULE gl;
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(gl, n);
    }
    return p;
}
void backend_gl_swap(void) { SwapBuffers(g_hdc); }
int backend_gl_worker_current(void) { return g_wctx && wglMakeCurrent(g_hdc, g_wctx); }
void backend_window_size(int* w, int* h) {
    *w = g_ww;
    *h = g_wh;
}
void video_present_rgba(const u32* px, int w, int h, int stride) { gpu_gl_present_cpu(px, w, h, stride); }

static i32 axis(u64 b, int neg, int pos) { return ((b >> pos) & 1 ? 32767 : 0) - ((b >> neg) & 1 ? 32767 : 0); }
static i32 dz(i32 v) { return (v > -7000 && v < 7000) ? 0 : v; }
void video_poll(void) {
    u64 k = keys_held, b = k & 0xFFFFull;
    i32 lx = axis(k, 40, 42), ly = axis(k, 43, 41), rx = axis(k, 44, 46), ry = axis(k, 47, 45);
    if (xget) {
        for (DWORD i = 0; i < 4; i++) {
            XST s;
            if (xget(i, &s)) continue;
            XGP* g = &s.Gamepad;
            WORD x = g->wButtons;
            /* map by position like a Switch pad: A right (xbox B), B bottom (xbox A), X top, Y left */
            if (x & 0x2000) b |= 1 << 0;
            if (x & 0x1000) b |= 1 << 1;
            if (x & 0x8000) b |= 1 << 2;
            if (x & 0x4000) b |= 1 << 3;
            if (x & 0x0040) b |= 1 << 4;
            if (x & 0x0080) b |= 1 << 5;
            if (x & 0x0100) b |= 1 << 6;
            if (x & 0x0200) b |= 1 << 7;
            if (g->bLeftTrigger > 60) b |= 1 << 8;
            if (g->bRightTrigger > 60) b |= 1 << 9;
            if (x & 0x0010) b |= 1 << 10;
            if (x & 0x0020) b |= 1 << 11;
            if (x & 0x0004) b |= 1 << 12;
            if (x & 0x0001) b |= 1 << 13;
            if (x & 0x0008) b |= 1 << 14;
            if (x & 0x0002) b |= 1 << 15;
            if (!lx && !ly) {
                lx = dz(g->sThumbLX);
                ly = dz(g->sThumbLY);
            }
            if (!rx && !ry) {
                rx = dz(g->sThumbRX);
                ry = dz(g->sThumbRY);
            }
            break;
        }
    }
    /* stick-as-button bits 16..23 */
    if (lx < -16000) b |= 1ull << 16;
    if (ly > 16000) b |= 1ull << 17;
    if (lx > 16000) b |= 1ull << 18;
    if (ly < -16000) b |= 1ull << 19;
    if (rx < -16000) b |= 1ull << 20;
    if (ry > 16000) b |= 1ull << 21;
    if (rx > 16000) b |= 1ull << 22;
    if (ry < -16000) b |= 1ull << 23;
    g_pad.buttons = b;
    g_pad.lx = lx;
    g_pad.ly = ly;
    g_pad.rx = rx;
    g_pad.ry = ry;
}

void frontend_frame(void) {
    if (g_shot_req) {
        g_shot_req = 0;
        char p[MAX_PATH];
        SYSTEMTIME t;
        GetLocalTime(&t);
        CreateDirectoryA("captures", NULL);
        snprintf(p, sizeof p, "captures/screenshot_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour,
                 t.wMinute, t.wSecond);
        gpu_gl_screenshot(p);
    }
    if (g_seconds && plat_ns() - g_start >= (u64)g_seconds * 1000000000ull) {
        u64 h = gpu_gl_screenshot(g_shot[0] ? g_shot : NULL);
        fprintf(stderr, "\nframes=%d hash=%016llx\n", g_frames, (unsigned long long)h);
        save_hints();
        { gpu_gl_rec_finish(); }
        fflush(NULL);
        ExitProcess(0);
    }
}

int backend_glq_ok(void) { return 1; } /* GL calls go through glq */
