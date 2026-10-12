/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the unattended-run options, shared by the SDL3 and headless backends: they had a copy each, and the copies had
   drifted apart (headless ignored script and press_len, SDL3 ignored shot_every) */
#include "platform/unattended.h"
#include "core/main.h"
#include "core/rtapi.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_seconds = 0, g_maxframes = -1, g_shot_every = 0;
static const char* g_shot;
static int g_shots, g_shot_bucket = -1;
static u64 g_start;
static u64 g_press = 0, g_press_at = 0, g_press_len = 300;
/* script: "ms:hex,ms:hex,...", parsed once when the option is set */
typedef struct {
    u64 t, mask;
} ScriptPress;
static ScriptPress* g_script;
static int g_nscript;

static void script_parse(const char* v) {
    free(g_script);
    g_script = NULL;
    g_nscript = 0;
    int cap = 0;
    const char* p = v;
    while (*p) {
        char* e;
        u64 t = strtoull(p, &e, 10);
        if (e == p || *e != ':') break;
        const char* m0 = e + 1;
        u64 m = strtoull(m0, &e, 16);
        if (e == m0 || (*e && *e != ',')) break;
        if (g_nscript == cap) {
            cap = cap ? cap * 2 : 16;
            ScriptPress* n = realloc(g_script, sizeof *n * (size_t)cap);
            if (!n) break;
            g_script = n;
        }
        g_script[g_nscript++] = (ScriptPress){t, m};
        p = *e ? e + 1 : e;
    }
    if (*p) WARN("script: \"%s\" is not ms:hex, so it and the presses after it are ignored\n", p);
}

int unattended_ini(const char* k, const char* v) {
    if (!strcmp(k, "seconds"))
        g_seconds = atoi(v);
    else if (!strcmp(k, "frames"))
        g_maxframes = atoi(v);
    else if (!strcmp(k, "shot")) {
        /* the old string is kept: headless takes its numbered shots on the presenting thread */
        char* s = strdup(v);
        if (s) g_shot = s[0] ? s : NULL;
    } else if (!strcmp(k, "shot_every"))
        g_shot_every = atoi(v);
    else if (!strcmp(k, "press"))
        g_press = strtoull(v, NULL, 16);
    else if (!strcmp(k, "press_at"))
        g_press_at = strtoull(v, NULL, 10);
    else if (!strcmp(k, "press_len"))
        g_press_len = strtoull(v, NULL, 10);
    else if (!strcmp(k, "script"))
        script_parse(v);
    else
        return 0;
    return 1;
}
void unattended_start(void) { g_start = plat_ns(); }
int unattended_active(void) { return g_seconds || g_maxframes >= 0; }
u64 unattended_buttons(void) {
    u64 el = (plat_ns() - g_start) / 1000000ull, b = 0;
    if (g_press && el >= g_press_at && el < g_press_at + g_press_len) b |= g_press;
    for (int i = 0; i < g_nscript; i++)
        if (el >= g_script[i].t && el < g_script[i].t + g_press_len) b |= g_script[i].mask;
    return b;
}
const char* unattended_shot(void) { return g_shot; }
int unattended_periodic_shot(char* path, size_t n) {
    if (!g_shot_every || !g_shot) return 0;
    /* by bucket rather than g_frames % shot_every: the SDL3 backend asks once per tick, and frames can go by two
       at a time */
    int b = g_frames / g_shot_every;
    if (b == g_shot_bucket) return 0;
    g_shot_bucket = b;
    snprintf(path, n, "%s_%04d", g_shot, g_shots++);
    return 1;
}
int unattended_done(void) {
    u64 el = plat_ns() - g_start;
    return (g_maxframes >= 0 && g_frames >= g_maxframes) || (g_seconds && el >= (u64)g_seconds * 1000000000ull);
}
void unattended_exit(u64 hash, int w, int h) {
    if (w > 0 && h > 0)
        fprintf(stderr, "\nframes=%d hash=%016llx %dx%d\n", g_frames, (unsigned long long)hash, w, h);
    else
        fprintf(stderr, "\nframes=%d hash=%016llx\n", g_frames, (unsigned long long)hash);
    save_hints();
    plat_exit_now(0);
}

static i32 axis(u64 b, int neg, int pos) { return ((b >> pos) & 1 ? 32767 : 0) - ((b >> neg) & 1 ? 32767 : 0); }
void input_key_sticks(u64 k, i32 st[4]) {
    st[0] = axis(k, 40, 42);
    st[1] = axis(k, 43, 41);
    st[2] = axis(k, 44, 46);
    st[3] = axis(k, 47, 45);
}
void input_stick_clamp(i32* x, i32* y) {
    i32 cx = *x < -32767 ? -32767 : *x, cy = *y < -32767 ? -32767 : *y;
    double r = sqrt((double)cx * cx + (double)cy * cy);
    if (r > 32767.0) {
        cx = (i32)(cx * 32767.0 / r);
        cy = (i32)(cy * 32767.0 / r);
    }
    *x = cx;
    *y = cy;
}
