/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the guest clock: real time, or slower than real time in smooth recording mode */
#include "core/clock.h"
#include "core/options.h"
#include "gpu/renderer.h"
#include "hle/vi.h"
#include "platform/plat.h"
#include <stdlib.h>

static u64 g_t0;
void clock_start(void) { g_t0 = plat_ns(); }
u64 g_t0_ns(void) { return g_t0; }
/* guest clock. Normally == real time. In smooth recording mode (F7) it runs slower than real time so that
   every frame spans ~1/recording_fps of guest time, which gives a steady video even when the PC can't keep up.
   It never stops, so guest timeouts still fire. vsync follows it too. */
volatile int g_vt_on, g_rec_active;
int g_opt_rec_smooth = 1, g_opt_rec_fps = 60;
static PMutex g_vt_m;
static int g_vt_mi;
static u64 g_vt_ar, g_vt_av, g_vt_lastfr;
static double g_vt_s = 1.0, g_vt_fr = 0; /* anchor + rate */
static volatile u64 g_vt_max;
static u64 guest_ns_raw(u64 r) { return g_vt_av + (u64)((double)(r - g_vt_ar) * g_vt_s); }
u64 guest_ns(void) {
    u64 r = plat_ns() - g_t0, v;
    if (!g_vt_mi) {
        v = r;
    } else {
        pm_lock(&g_vt_m);
        v = guest_ns_raw(r);
        pm_unlock(&g_vt_m);
    }
    u64 m = __atomic_load_n(&g_vt_max, __ATOMIC_RELAXED);
    while (v > m) {
        if (__atomic_compare_exchange_n(&g_vt_max, &m, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return v;
    }
    return m;
}
static void vt_rebase(u64 r, double s) {
    g_vt_av = guest_ns_raw(r);
    g_vt_ar = r;
    g_vt_s = s;
}
void vt_frame(void) {
    if (!g_vt_mi) return;
    u64 r = plat_ns() - g_t0;
    pm_lock(&g_vt_m);
    /* cost per frame = GPU thread busy time. don't use full frame time, the game's own waits
     would feed back and slow the clock further */
    {
        u64 b = g_renderer->busy_ns();
        static u64 lb;
        if (g_vt_lastfr) {
            double d = (double)(b - lb);
            if (d > 2e9) d = 2e9;
            /* attack fast, release slow */
            if (g_vt_fr <= 0 || d > g_vt_fr)
                g_vt_fr = d;
            else
                g_vt_fr = g_vt_fr * 0.92 + d * 0.08;
        }
        lb = b;
    }
    g_vt_lastfr = r;
    double s = 1.0;
    if (g_vt_on && g_vt_fr > 0) {
        s = (1e9 / (double)g_opt_rec_fps) / (g_vt_fr * 1.25);
        if (s > 1) s = 1;
        if (s < 0.02) s = 0.02;
    }
    {
        static double fs = -1;
        if (fs < 0) {
            const char* e = getenv("SWR_VT_SCALE");
            fs = e ? atof(e) : 0;
        }
        if (g_vt_on && fs > 0) s = fs;
    } /* test hook: force the clock rate */
    vt_rebase(r, s);
    pm_unlock(&g_vt_m);
}
void vt_set(int on) {
    if (!g_vt_mi) {
        pm_init(&g_vt_m);
        u64 r = plat_ns() - g_t0;
        g_vt_ar = r;
        g_vt_av = r;
        g_vt_s = 1.0;
        g_vt_mi = 1;
    }
    pm_lock(&g_vt_m);
    vt_rebase(plat_ns() - g_t0, 1.0);
    g_vt_fr = 0;
    g_vt_lastfr = 0;
    pm_unlock(&g_vt_m);
    g_vt_on = on && g_opt_rec_smooth;
}
/* vsync ticks on guest time */
void vsync_tick_vt(void) {
    static u64 next;
    if (!g_vt_on) {
        next = 0;
        vi_vsync_tick();
        return;
    }
    u64 v = guest_ns();
    if (!next) next = v;
    if (v >= next) {
        vi_vsync_tick();
        next += 16666667ull;
        if (v > next + 50000000ull) next = v;
    }
}
