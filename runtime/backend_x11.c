/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* X11 + GLX 4.3 backend. Also what the Xvfb tests run on (screenshots, scripted input). */
#include "rt.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/glx.h>

static Display* dpy;
static Window win;
static Atom wm_delete;
static int g_ww = 1280, g_wh = 720;
static int g_seconds = 0, g_maxframes = -1;
static char g_shot[1024];
static u64 g_start;
static u64 g_press = 0, g_press_at = 0, g_press_len = 300;
static char g_script[1024];
static u64 keys_held;
extern int gl_frontend_available;
void backend_ini(const char* k, const char* v) {
    if (!strcmp(k, "seconds"))
        g_seconds = atoi(v);
    else if (!strcmp(k, "frames"))
        g_maxframes = atoi(v);
    else if (!strcmp(k, "shot"))
        strncpy(g_shot, v, 1023);
    else if (!strcmp(k, "press"))
        g_press = strtoull(v, NULL, 16);
    else if (!strcmp(k, "press_at"))
        g_press_at = strtoull(v, NULL, 10);
    else if (!strcmp(k, "press_len"))
        g_press_len = strtoull(v, NULL, 10);
    else if (!strcmp(k, "script"))
        strncpy(g_script, v, 1023);
}
int video_has_gl(void) { return 1; }
void video_init(const char* title) {
    XInitThreads();
    /* let Mesa run its own glthread so driver work isn't on our GPU thread (Deck, AMD/Intel on Linux) */
    if (!getenv("SWR_NO_GLTHREAD")) setenv("mesa_glthread", "true", 0);
    dpy = XOpenDisplay(NULL);
    if (!dpy) fatal("No X11 display (DISPLAY).");
    int scr = DefaultScreen(dpy);
    static int att[] = {GLX_X_RENDERABLE,
                        True,
                        GLX_DRAWABLE_TYPE,
                        GLX_WINDOW_BIT,
                        GLX_RENDER_TYPE,
                        GLX_RGBA_BIT,
                        GLX_RED_SIZE,
                        8,
                        GLX_GREEN_SIZE,
                        8,
                        GLX_BLUE_SIZE,
                        8,
                        GLX_ALPHA_SIZE,
                        8,
                        GLX_DEPTH_SIZE,
                        24,
                        GLX_DOUBLEBUFFER,
                        True,
                        None};
    int n = 0;
    GLXFBConfig* fc = glXChooseFBConfig(dpy, scr, att, &n);
    if (!fc || !n) fatal("GLX: no color config");
    XVisualInfo* vi = glXGetVisualFromFBConfig(dpy, fc[0]);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    swa.event_mask =
        KeyPressMask | KeyReleaseMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, g_ww, g_wh, 0, vi->depth, InputOutput, vi->visual,
                        CWColormap | CWEventMask, &swa);
    XStoreName(dpy, win, title);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);
    XFlush(dpy);
    g_start = plat_ns();
    extern void gpu_gl_start(void);
    gpu_gl_start();
}
/* GPU thread */
static GLXContext ctx, wctx;
static GLXFBConfig wfc;
static int xerr_ignore(Display* d, XErrorEvent* e) {
    (void)d;
    (void)e;
    return 0;
}
int backend_gl_create(void) {
    int scr = DefaultScreen(dpy);
    static int att[] = {GLX_X_RENDERABLE,
                        True,
                        GLX_DRAWABLE_TYPE,
                        GLX_WINDOW_BIT,
                        GLX_RENDER_TYPE,
                        GLX_RGBA_BIT,
                        GLX_RED_SIZE,
                        8,
                        GLX_GREEN_SIZE,
                        8,
                        GLX_BLUE_SIZE,
                        8,
                        GLX_ALPHA_SIZE,
                        8,
                        GLX_DOUBLEBUFFER,
                        True,
                        None};
    int n = 0;
    GLXFBConfig* fc = glXChooseFBConfig(dpy, scr, att, &n);
    if (!fc || !n) return 0;
    typedef GLXContext (*CCA)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
    CCA cca = (CCA)glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
    static const int vers[4][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}};
    int (*old)(Display*, XErrorEvent*) = XSetErrorHandler(xerr_ignore);
    for (int i = 0; i < 4 && !ctx && cca; i++) {
        int ca[] = {0x2091, vers[i][0], 0x2092, vers[i][1], 0x9126, 0x00000001, None};
        ctx = cca(dpy, fc[0], NULL, True, ca);
        XSync(dpy, False);
        if (ctx) {
            wctx = cca(dpy, fc[0], ctx, True, ca);
            XSync(dpy, False);
        }
    }
    wfc = fc[0];
    XSetErrorHandler(old);
    if (!ctx) return 0;
    glXMakeCurrent(dpy, win, ctx);
    typedef int (*SI)(Display*, GLXDrawable, int);
    SI si = (SI)glXGetProcAddressARB((const GLubyte*)"glXSwapIntervalEXT");
    (void)si;
    return 1;
}
void* backend_gl_proc(const char* n) { return (void*)glXGetProcAddressARB((const GLubyte*)n); }
void backend_gl_swap(void) { glXSwapBuffers(dpy, win); }
int backend_gl_worker_current(void) {
    if (!wctx) return 0;
    int pa[] = {GLX_PBUFFER_WIDTH, 4, GLX_PBUFFER_HEIGHT, 4, None};
    int (*old)(Display*, XErrorEvent*) = XSetErrorHandler(xerr_ignore);
    GLXPbuffer pb = glXCreatePbuffer(dpy, wfc, pa);
    XSync(dpy, False);
    XSetErrorHandler(old);
    if (pb && glXMakeContextCurrent(dpy, pb, pb, wctx)) return 1;
    return glXMakeContextCurrent(dpy, None, None, wctx) ? 1 : 0;
}
void backend_window_size(int* w, int* h) {
    *w = g_ww;
    *h = g_wh;
}

void video_present_rgba(const u32* px, int w, int h, int stride) {
    extern void gpu_gl_present_cpu(const u32*, int, int, int);
    gpu_gl_present_cpu(px, w, h, stride);
}

/* keys -> HidNpadButton bits */
static u64 keymap(KeySym k) {
    switch (k) {
        case XK_x:
        case XK_space: return 1ull << 0;      /* A */
        case XK_z: return 1ull << 1;          /* B */
        case XK_s: return 1ull << 2;          /* X */
        case XK_a: return 1ull << 3;          /* Y */
        case XK_q: return 1ull << 6;          /* L */
        case XK_e: return 1ull << 7;          /* R */
        case XK_1: return 1ull << 8;          /* ZL */
        case XK_3: return 1ull << 9;          /* ZR */
        case XK_Return: return 1ull << 10;    /* + */
        case XK_BackSpace: return 1ull << 11; /* - */
        case XK_Left: return 1ull << 12;
        case XK_Up: return 1ull << 13;
        case XK_Right: return 1ull << 14;
        case XK_Down: return 1ull << 15;
    }
    return 0;
}
void video_poll(void) {
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == KeyPress) {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if (k == XK_Escape) g_quit = 1;
            keys_held |= keymap(k);
        } else if (e.type == KeyRelease) {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            keys_held &= ~keymap(k);
        } else if (e.type == ConfigureNotify) {
            g_ww = e.xconfigure.width;
            g_wh = e.xconfigure.height;
        } else if (e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete)
            g_quit = 1;
    }
    u64 el = (plat_ns() - g_start) / 1000000ull, b = keys_held;
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
}
void frontend_frame(void) {
    u64 el = plat_ns() - g_start;
    if ((g_maxframes >= 0 && g_frames >= g_maxframes) || (g_seconds && el >= (u64)g_seconds * 1000000000ull)) {
        extern u64 gpu_gl_screenshot(const char* path);
        u64 h = gpu_gl_screenshot(g_shot[0] ? g_shot : NULL);
        fprintf(stderr, "\nframes=%d hash=%016llx 1280x720\n", g_frames, (unsigned long long)h);
        extern void save_hints(void);
        save_hints();
        fflush(NULL);
        _exit(0);
    }
}

int backend_glq_ok(void) { return 1; } /* GL calls go through glq */
