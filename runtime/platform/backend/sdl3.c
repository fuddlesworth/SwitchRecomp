/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* SDL3 backend, the window backend on Linux and Windows: the window and its Vulkan surface, keyboard and gamepads. Runs
   natively on Wayland and on X11 (SDL_VIDEO_DRIVER=x11 picks X11), and takes the unattended-run options (seconds,
   frames, shot, press, script). */
#include "core/main.h"
#include "core/options.h"
#include "core/rtapi.h"
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
#include "gpu/renderer.h"
#include "gpu/vk/vk.h"
#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL.h>
#include <time.h>

static SDL_Window* g_win;
static volatile int g_ww = 1280, g_wh = 720;
static int g_seconds = 0, g_maxframes = -1, g_shot_every = 0;
static char g_shot[1024];
static u64 g_start;
static u64 g_press = 0, g_press_at = 0, g_press_len = 300;
static char g_script[1024];
static u64 keys_held;
static int g_fullscreen;
static volatile int g_shot_req;
static SDL_Gamepad* g_pads[4];

void backend_ini(const char* k, const char* v) {
    if (!strcmp(k, "seconds"))
        g_seconds = atoi(v);
    else if (!strcmp(k, "frames"))
        g_maxframes = atoi(v);
    else if (!strcmp(k, "shot"))
        strncpy(g_shot, v, 1023);
    else if (!strcmp(k, "shot_every"))
        g_shot_every = atoi(v);
    else if (!strcmp(k, "press"))
        g_press = strtoull(v, NULL, 16);
    else if (!strcmp(k, "press_at"))
        g_press_at = strtoull(v, NULL, 10);
    else if (!strcmp(k, "press_len"))
        g_press_len = strtoull(v, NULL, 10);
    else if (!strcmp(k, "script"))
        strncpy(g_script, v, 1023);
}

/* the window and its size in pixels */
static int open_window(const char* title) {
    g_win = SDL_CreateWindow(title, g_ww, g_wh, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_win) {
        LOG("video: SDL could not open a Vulkan window: %s\n", SDL_GetError());
        return 0;
    }
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    if (w > 0 && h > 0) {
        g_ww = w;
        g_wh = h;
    }
    return 1;
}

void video_init(const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL could not start its video: %s", SDL_GetError());
    LOG("video: SDL %d.%d.%d on %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
        SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    g_start = plat_ns();
    if (!open_window(title)) fatal("SDL could not open a window: %s", SDL_GetError());
    if (!g_renderer->start())
        fatal("Vulkan could not start (see the log). SwitchRecomp needs a GPU and driver with Vulkan 1.3.");
    LOG("renderer: Vulkan\n");
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
void backend_window_size(int* w, int* h) {
    *w = g_ww;
    *h = g_wh;
}
void video_present_rgba(const u32* px, int w, int h, int stride) {
    /* the renderer presents from the game's buffers (present_buffer), so a frame made on the CPU isn't shown */
    (void)px;
    (void)w;
    (void)h;
    (void)stride;
}

static const char* HELP = "Controller: like a Switch controller, buttons by position.\n\n"
                          "Keyboard:\n"
                          "  Arrows = D-Pad     W A S D = left stick    I J K L = right stick\n"
                          "  X / Space = A    Z = B    C = X    V = Y\n"
                          "  Q = L   E = R   1 = ZL   3 = ZR   Enter = +   Backspace = -\n\n"
                          "F1 = this help   F3 = screenshot   F4 = FPS limit 60 / unlimited   F5 = thread dump\n"
                          "F11 / Alt+Enter = fullscreen   Esc = quit";

/* keys -> HidNpadButton bits; 40..47 are the stick keys, turned into axes in video_poll */
static u64 keymap(SDL_Keycode k) {
    switch (k) {
        case SDLK_X:
        case SDLK_SPACE: return 1ull << 0; /* A */
        case SDLK_Z: return 1ull << 1;     /* B */
        case SDLK_C: return 1ull << 2;     /* X */
        case SDLK_V: return 1ull << 3;     /* Y */
        case SDLK_Q: return 1ull << 6;     /* L */
        case SDLK_E: return 1ull << 7;     /* R */
        case SDLK_1: return 1ull << 8;     /* ZL */
        case SDLK_3: return 1ull << 9;     /* ZR */
        case SDLK_RETURN: return 1ull << 10;
        case SDLK_BACKSPACE: return 1ull << 11;
        case SDLK_LEFT: return 1ull << 12;
        case SDLK_UP: return 1ull << 13;
        case SDLK_RIGHT: return 1ull << 14;
        case SDLK_DOWN: return 1ull << 15;
        case SDLK_A: return 1ull << 40;
        case SDLK_W: return 1ull << 41;
        case SDLK_D: return 1ull << 42;
        case SDLK_S: return 1ull << 43;
        case SDLK_J: return 1ull << 44;
        case SDLK_I: return 1ull << 45;
        case SDLK_L: return 1ull << 46;
        case SDLK_K: return 1ull << 47;
    }
    return 0;
}

static void set_fullscreen(int on) {
    g_fullscreen = on;
    SDL_SetWindowFullscreen(g_win, on);
}
#ifdef _WIN32
/* plat.c's fatal error box */
void win_fatal_box(const char* msg) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SwitchRecomp", msg, g_win); }
#endif

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
            g_opt_fps = g_opt_fps == 0 ? 1 : 0;
            ini_set("fps_limit", g_opt_fps ? "1" : "0");
            LOG("FPS limit: %s\n", g_opt_fps ? "60" : "unlimited");
            break;
        }
        case SDLK_F5: kernel_dump_threads(); break;
        case SDLK_F11: set_fullscreen(!g_fullscreen); break;
    }
}

static void pad_open(SDL_JoystickID id) {
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
}

static i32 axis(u64 b, int neg, int pos) { return ((b >> pos) & 1 ? 32767 : 0) - ((b >> neg) & 1 ? 32767 : 0); }
static i32 dz(i32 v) { return (v > -7000 && v < 7000) ? 0 : v; }
/* SDL's Y axes point down, the Switch's up */
static i32 up(i32 v) { return v == -32768 ? 32767 : -v; }

void video_poll(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT: g_quit = 1; break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                if (e.window.windowID == SDL_GetWindowID(g_win) && e.window.data1 > 0 && e.window.data2 > 0) {
                    g_ww = e.window.data1;
                    g_wh = e.window.data2;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_ESCAPE) {
                    g_quit = 1;
                    break;
                }
                if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
                    if (!e.key.repeat) set_fullscreen(!g_fullscreen);
                    break;
                }
                if (!e.key.repeat) hotkey(e.key.key);
                keys_held |= keymap(e.key.key);
                break;
            case SDL_EVENT_KEY_UP: keys_held &= ~keymap(e.key.key); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: keys_held = 0; break;
            case SDL_EVENT_GAMEPAD_ADDED: pad_open(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: pad_close(e.gdevice.which); break;
        }
    }
    u64 k = keys_held, b = k & 0xFFFFull;
    i32 lx = axis(k, 40, 42), ly = axis(k, 43, 41), rx = axis(k, 44, 46), ry = axis(k, 47, 45);
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
            lx = dz(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
            ly = dz(up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY)));
        }
        if (!rx && !ry) {
            rx = dz(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
            ry = dz(up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY)));
        }
    }
    u64 el = (plat_ns() - g_start) / 1000000ull;
    if (g_press && el >= g_press_at && el < g_press_at + g_press_len) b |= g_press;
    if (g_script[0]) { /* "ms:hex,ms:hex,..." */
        const char* p = g_script;
        while (*p) {
            u64 t = strtoull(p, (char**)&p, 10);
            if (*p != ':') break;
            p++;
            u64 m = strtoull(p, (char**)&p, 16);
            if (el >= t && el < t + g_press_len) b |= m;
            if (*p == ',') p++;
        }
    }
    g_pad.buttons = b;
    g_pad.lx = lx;
    g_pad.ly = ly;
    g_pad.rx = rx;
    g_pad.ry = ry;
}

void frontend_frame(void) {
    if (g_shot_req) {
        g_shot_req = 0;
        char p[1200];
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
    { /* shot_every=N with shot=PATH: PATH_0000, PATH_0001... every N frames, to follow a scripted run */
        static int last = -1, n;
        if (g_shot_every > 0 && g_shot[0] && g_frames != last && g_frames % g_shot_every == 0) {
            last = g_frames;
            char p[1100];
            snprintf(p, sizeof p, "%s_%04d", g_shot, n++);
            g_renderer->screenshot(p);
        }
    }
    u64 el = plat_ns() - g_start;
    if ((g_maxframes >= 0 && g_frames >= g_maxframes) || (g_seconds && el >= (u64)g_seconds * 1000000000ull)) {
        u64 h = g_renderer->screenshot(g_shot[0] ? g_shot : NULL);
        fprintf(stderr, "\nframes=%d hash=%016llx %dx%d\n", g_frames, (unsigned long long)h, g_ww, g_wh);
        save_hints();
        fflush(NULL);
        _exit(0);
    }
}
