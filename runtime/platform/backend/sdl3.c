/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* SDL3 backend, the window backend on Linux and Windows: the window and its Vulkan surface, keyboard and gamepads. Runs
   natively on Wayland and on X11 (SDL_VIDEO_DRIVER=x11 picks X11), and takes the unattended-run options (seconds,
   frames, shot, shot_every, press, press_at, press_len, script: platform/unattended.c). */
#include "core/main.h"
#include "core/options.h"
#include "hle/hid.h"
#include "kernel/kernel.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "platform/unattended.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/renderer.h"
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL.h>
#include <time.h>

static SDL_Window* g_win;
/* the window's size in pixels, width | height << 32: the renderer reads it on its own thread, and one atomic keeps
   a width from one resize from pairing with the height of another */
static _Atomic u64 g_wsize = 1280 | (720ull << 32);
static u64 keys_held;
static int g_fullscreen;
static int g_fullscreen_ini; /* the .ini's fullscreen, applied once the window is open */
static void set_fullscreen(int on);
static int g_shot_req;
static SDL_Gamepad* g_pads[4];

void backend_ini(const char* k, const char* v) {
    if (!strcmp(k, "fullscreen"))
        g_fullscreen_ini = atoi(v);
    else
        unattended_ini(k, v);
}

static void wsize_set(int w, int h) {
    if (w > 0 && h > 0) g_wsize = (u64)(u32)w | (u64)(u32)h << 32;
}
void backend_window_size(int* w, int* h) {
    u64 s = g_wsize;
    *w = (int)(u32)s;
    *h = (int)(u32)(s >> 32);
}

void video_init(const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL could not start its video: %s", SDL_GetError());
    LOG("video: SDL %d.%d.%d on %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    unattended_start();
    int w, h;
    backend_window_size(&w, &h);
    g_win = SDL_CreateWindow(title, w, h, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_win) fatal("SDL could not open a Vulkan window: %s", SDL_GetError());
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    wsize_set(w, h);
    if (!g_renderer->start())
        fatal("Vulkan could not start (see the log). SwitchRecomp needs a GPU and driver with Vulkan 1.3.");
    LOG("renderer: Vulkan\n");
    /* not for an unattended run: nobody is at the screen, and it would cover the desktop of whoever is */
    if (g_fullscreen_ini && !unattended_active()) set_fullscreen(1);
#ifdef _WIN32
    /* keep the display and the system awake while playing (it was set with the crash handler, headless runs too) */
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
}

void* backend_vk_loader(void) { return (void*)SDL_Vulkan_GetVkGetInstanceProcAddr(); }
const char* const* backend_vk_extensions(u32* n) {
    Uint32 c = 0;
    const char* const* e = SDL_Vulkan_GetInstanceExtensions(&c);
    *n = c;
    return e;
}
int backend_vk_surface(VkInstance instance, VkSurfaceKHR* surface) {
    if (SDL_Vulkan_CreateSurface(g_win, instance, NULL, surface)) return 1;
    LOG("video: no Vulkan surface: %s\n", SDL_GetError());
    return 0;
}
void video_present_rgba(const u32* px, int w, int h, int stride) {
    /* the renderer presents from the game's buffers (present_buffer), so a frame made on the CPU isn't shown */
    (void)px;
    (void)w;
    (void)h;
    (void)stride;
}

static const char* HELP = "Controller: like a Switch controller, buttons by position.\n\n"
                          "Keyboard (keys by position, as on a US layout):\n"
                          "  Arrows = D-Pad     W A S D = left stick    I J K L = right stick\n"
                          "  X / Space = A    Z = B    C = X    V = Y\n"
                          "  Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -\n\n"
                          "F1 = this help   F3 = screenshot   F4 = FPS limit on / unlimited   F5 = thread dump\n"
                          "F11 / Alt+Enter = fullscreen   Esc = quit";

/* keys -> HidNpadButton bits; 40..47 are the stick keys, turned into axes in video_poll. By scancode, the key's
   position: the keycode depends on the layout and on the modifiers, so Shift pressed while 1 was held made its release
   a different key, and ZL stayed held */
static u64 keymap(SDL_Scancode k) {
    switch (k) {
        case SDL_SCANCODE_X:
        case SDL_SCANCODE_SPACE: return 1ull << 0; /* A */
        case SDL_SCANCODE_Z: return 1ull << 1;     /* B */
        case SDL_SCANCODE_C: return 1ull << 2;     /* X */
        case SDL_SCANCODE_V: return 1ull << 3;     /* Y */
        case SDL_SCANCODE_Q: return 1ull << 6;     /* L */
        case SDL_SCANCODE_E: return 1ull << 7;     /* R */
        case SDL_SCANCODE_1: return 1ull << 8;     /* ZL */
        case SDL_SCANCODE_3: return 1ull << 9;     /* ZR */
        case SDL_SCANCODE_RETURN: return 1ull << 10;
        case SDL_SCANCODE_BACKSPACE: return 1ull << 11;
        case SDL_SCANCODE_LEFT: return 1ull << 12;
        case SDL_SCANCODE_UP: return 1ull << 13;
        case SDL_SCANCODE_RIGHT: return 1ull << 14;
        case SDL_SCANCODE_DOWN: return 1ull << 15;
        case SDL_SCANCODE_A: return 1ull << 40;
        case SDL_SCANCODE_W: return 1ull << 41;
        case SDL_SCANCODE_D: return 1ull << 42;
        case SDL_SCANCODE_S: return 1ull << 43;
        case SDL_SCANCODE_J: return 1ull << 44;
        case SDL_SCANCODE_I: return 1ull << 45;
        case SDL_SCANCODE_L: return 1ull << 46;
        case SDL_SCANCODE_K: return 1ull << 47;
        default: break;
    }
    return 0;
}

/* g_fullscreen follows the window: also set from the fullscreen events, for a change the window manager made */
static void set_fullscreen(int on) {
    if (SDL_SetWindowFullscreen(g_win, on))
        g_fullscreen = on;
    else
        LOG("video: fullscreen %s failed: %s\n", on ? "on" : "off", SDL_GetError());
}
/* plat.c's fatal error box. SDL's box only works on the main thread (with the window as its parent). From another
   thread, Windows' own box (it takes any thread), and none on Linux: the error is in the log and on stderr. None
   either for an unattended run, where nobody would close it */
void win_fatal_box(const char* msg) {
    if (unattended_active()) return;
    if (SDL_IsMainThread()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SwitchRecomp", msg, g_win);
        return;
    }
#ifdef _WIN32
    wchar_t w[4096]; /* on the stack: this also runs in the crash report, where the heap may be what broke */
    if (MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 4096))
        MessageBoxW(NULL, w, L"SwitchRecomp", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    else
        MessageBoxA(NULL, msg, "SwitchRecomp", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

static void hotkey(SDL_Keycode k) {
    switch (k) {
        case SDLK_F1: {
            int fs = g_fullscreen;
            if (fs) set_fullscreen(0);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "SwitchRecomp", HELP, g_win);
            if (fs) set_fullscreen(1);
            break;
        }
        case SDLK_F3: g_shot_req = 1; break;
        case SDLK_F4: {
            /* off and back to the limit the player set (fps_limit=120 stays 120), 60 when it started off */
            static int was = 1;
            if (g_opt_fps) {
                was = g_opt_fps;
                g_opt_fps = 0;
            } else
                g_opt_fps = was;
            char v[16];
            snprintf(v, sizeof v, "%d", g_opt_fps);
            ini_set("fps_limit", v);
            if (g_opt_fps)
                LOG("FPS limit: %d\n", g_opt_fps >= 60 && g_opt_fps <= 240 ? g_opt_fps : 60);
            else
                LOG("FPS limit: unlimited\n");
            break;
        }
        case SDLK_F5: kernel_dump_threads(); break;
        case SDLK_F11: set_fullscreen(!g_fullscreen); break;
    }
}

static int pad_is_open(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) return 1;
    return 0;
}
static void pad_open(SDL_JoystickID id) {
    if (pad_is_open(id)) return;
    for (int i = 0; i < 4; i++)
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            if (g_pads[i]) LOG("input: gamepad %s\n", SDL_GetGamepadName(g_pads[i]));
            return;
        }
}
static void pad_close(SDL_JoystickID id) {
    for (int i = 0; i < 4; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
        }
    /* a fifth pad connected while four were open takes the free place */
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (int i = 0; i < n; i++) pad_open(ids[i]);
    SDL_free(ids);
}

/* the dead zone as a circle, so a stick pushed along one axis keeps the small part of the other */
static void dz(i32* x, i32* y) {
    if ((i64)*x * *x + (i64)*y * *y < 7000ll * 7000) *x = *y = 0;
}
/* SDL's Y axes point down, the Switch's up */
static i32 up(i32 v) { return v == -32768 ? 32767 : -v; }

void video_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = 1; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (e.window.windowID == SDL_GetWindowID(g_win)) wsize_set(e.window.data1, e.window.data2);
                break;
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: g_fullscreen = 1; break;
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: g_fullscreen = 0; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) {
                    g_quit = 1;
                    break;
                }
                if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                    if (!e.key.repeat) set_fullscreen(!g_fullscreen);
                    break;
                }
                /* not with Ctrl, Alt or the system key: Alt+F4 also arrives as F4 on Windows */
                if (!e.key.repeat && !(e.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) hotkey(e.key.key);
                keys_held |= keymap(e.key.scancode);
                break;
            case SDL_EVENT_KEY_UP: keys_held &= ~keymap(e.key.scancode); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: keys_held = 0; break;
            case SDL_EVENT_GAMEPAD_ADDED: pad_open(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pad_close(e.gdevice.which); break;
        }
    }
    /* the scripted presses act like keys, stick keys included. Bits 16..23 (the stick directions as buttons) can be
       scripted too, but the stick keys (40..47) are no npad buttons */
    u64 s = unattended_buttons(), k = keys_held | s, b = (k & 0xFFFFull) | (s & 0xFF0000ull);
    i32 st[4];
    input_key_sticks(k, st);
    i32 lx = st[0], ly = st[1], rx = st[2], ry = st[3];
    /* every pad and the keyboard drive the one controller the game sees (hid.c has a single PadState) */
    for (int i = 0; i < 4; i++) {
        SDL_Gamepad* g = g_pads[i];
        if (!g) continue;
        /* by position like a Switch pad: A right, B bottom, X top, Y left */
        static const struct {
            SDL_GamepadButton btn;
            int bit;
        } map[] = {{SDL_GAMEPAD_BUTTON_EAST, 0},          {SDL_GAMEPAD_BUTTON_SOUTH, 1},
                   {SDL_GAMEPAD_BUTTON_NORTH, 2},         {SDL_GAMEPAD_BUTTON_WEST, 3},
                   {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5},
                   {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7},
                   {SDL_GAMEPAD_BUTTON_START, 10},        {SDL_GAMEPAD_BUTTON_BACK, 11},
                   {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12},    {SDL_GAMEPAD_BUTTON_DPAD_UP, 13},
                   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14},   {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15}};
        for (size_t m = 0; m < sizeof map / sizeof map[0]; m++)
            if (SDL_GetGamepadButton(g, map[m].btn)) b |= 1ull << map[m].bit;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8000) b |= 1ull << 8;
        if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8000) b |= 1ull << 9;
        if (!lx && !ly) {
            lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
            ly = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
            dz(&lx, &ly);
        }
        if (!rx && !ry) {
            rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
            ry = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
            dz(&rx, &ry);
        }
    }
    input_stick_clamp(&lx, &ly);
    input_stick_clamp(&rx, &ry);
    g_pad.buttons = b;
    g_pad.lx = lx;
    g_pad.ly = ly;
    g_pad.rx = rx;
    g_pad.ry = ry;
}

void frontend_frame(void) {
    char p[1200];
    if (g_shot_req) {
        g_shot_req = 0;
        time_t now = time(NULL);
        struct tm t;
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
        snprintf(p, sizeof p, "%scaptures", g_exedir);
        plat_mkdir(p);
        snprintf(p, sizeof p, "%scaptures/screenshot_%04d%02d%02d_%02d%02d%02d", g_exedir, t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
        g_renderer->screenshot(p);
    }
    if (unattended_periodic_shot(p, sizeof p)) g_renderer->screenshot(p);
    if (unattended_done()) {
        /* the size of the image the renderer saved isn't known here: the window's would be a different one */
        unattended_exit(g_renderer->screenshot(unattended_shot()), 0, 0);
    }
}
