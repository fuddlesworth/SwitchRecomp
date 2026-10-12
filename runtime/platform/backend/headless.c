/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* headless backend for the test runner, writes screenshots as PPM. Takes the unattended-run options
   (platform/unattended.c) */
#include "core/main.h"
#include "hle/hid.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "platform/unattended.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static u32* g_last;
static int g_lw, g_lh;
static PMutex g_hl;
void backend_ini(const char* k, const char* v) { unattended_ini(k, v); }
void video_init(const char* t) {
    (void)t;
    pm_init(&g_hl);
    unattended_start();
}
static void save_ppm(const char* path) {
    if (!g_last) return;
    FILE* f = fopen(path, "wb");
    if (!f) {
        WARN("screenshot: could not write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", g_lw, g_lh);
    for (int i = 0; i < g_lw * g_lh; i++) {
        u32 p = g_last[i];
        u8 rgb[3] = {(u8)p, (u8)(p >> 8), (u8)(p >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}
void video_present_rgba(const u32* px, int w, int h, int stride) {
    pm_lock(&g_hl);
    if (g_lw != w || g_lh != h) {
        free(g_last);
        g_last = malloc((size_t)w * h * 4);
        g_lw = g_last ? w : 0;
        g_lh = g_last ? h : 0;
    }
    if (g_last) {
        for (int y = 0; y < h; y++) memcpy(g_last + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 4);
        char p[1100];
        if (unattended_periodic_shot(p, sizeof p - 4)) {
            strcat(p, ".ppm");
            save_ppm(p);
        }
    }
    pm_unlock(&g_hl);
}
void video_poll(void) {
    /* the scripted presses, stick keys (40..47) as axes like the SDL3 backend's keyboard */
    u64 s = unattended_buttons();
    i32 st[4];
    input_key_sticks(s, st);
    input_stick_clamp(&st[0], &st[1]);
    input_stick_clamp(&st[2], &st[3]);
    g_pad.buttons = s & 0xFFFFFFull;
    g_pad.lx = st[0];
    g_pad.ly = st[1];
    g_pad.rx = st[2];
    g_pad.ry = st[3];
}
void frontend_frame(void) {
    if (unattended_done()) {
        pm_lock(&g_hl);
        const char* shot = unattended_shot();
        if (shot) {
            char p[1100];
            snprintf(p, sizeof p, "%s.ppm", shot);
            save_ppm(p);
        }
        u64 h = 1469598103934665603ull;
        if (g_last)
            for (int i = 0; i < g_lw * g_lh; i++) {
                h ^= g_last[i];
                h *= 1099511628211ull;
            }
        int w = g_lw, hh = g_lh;
        pm_unlock(&g_hl);
        unattended_exit(h, w, hh);
    }
}
/* plat.c's fatal error box: Windows only, and not in an unattended run, where nobody would close it */
void win_fatal_box(const char* msg) {
#if defined(_WIN32)
    if (!unattended_active()) MessageBoxA(NULL, msg, "SwitchRecomp", MB_OK | MB_ICONERROR);
#else
    (void)msg;
#endif
}
