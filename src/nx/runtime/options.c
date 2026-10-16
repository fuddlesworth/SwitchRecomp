/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the settings in <Game>.ini: loading, applying, and writing back what changes */
#include "runtime/options.h"
#include "runtime/loader.h"
#include "runtime/main.h"
#include "hle/kernel/kernel.h"
#include "memory/mem.h"
#include "runtime/ini.h"
#include "utilities/common.h"
#include "utilities/ini_lines.h"
#include "runtime/platform/backend.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include "runtime/platform/vpad.h"
#include "hle/services/services.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_opt_diag = 0;
int g_opt_auto_stub_ipc = 1;
int g_opt_docked = 0, g_opt_lang = 1, g_opt_fps = SNX_FPS_GAME, g_opt_fullscreen = 0;
float g_opt_resf = 1.0f;
int g_opt_shadows = 1;
int g_opt_vsync = 1, g_opt_aniso = 0;
int g_opt_prof = 0;
int g_opt_network = 0;
/* the variables above hold until ini_open, which applies the file's values, or the tables' defaults for keys it
   lacks (the logging before it needs g_verbose's) */
/* options that went with the OpenGL renderer (renderer choice, its dynamic resolution, LSFG, the F7 recorder) and
   older ones: dropped on load, so the rewritten .ini loses them. game_dynamic_resolution worked through GPU timings
   the OpenGL renderer's thread measured. vsync is a setting again (the Vulkan swapchain's present mode) */
static const char* const INI_REMOVED[] = {"audio_device", /* Playback now always follows the system default. */
                                          "renderer",
                                          "dynamic_resolution",
                                          "dynamic_resolution_min",
                                          "game_dynamic_resolution",
                                          "low_priority_shader_compiler",
                                          "forced_vsync",
                                          "smooth_recording",
                                          "recording_fps",
                                          "lsfg",
                                          "lsfg_dll",
                                          "lsfg_target",
                                          "lsfg_multiplier",
                                          "lsfg_flow",
                                          "lsfg_resolution",
                                          "lossless_scaling",
                                          "lossless_path",
                                          "touch_controls",
                                          "touch_controls_opacity",
                                          NULL};
static int ini_removed(const char* k) {
    for (int i = 0; INI_REMOVED[i]; i++)
        if (!strcmp(k, INI_REMOVED[i])) return 1;
    return 0;
}
/* everything we read, so the rewrite keeps unknown keys. A value is under INI_LINE bytes */
#define INI_MAXK 128
#define INI_LINE 2048
static char g_ini_k[INI_MAXK][64], g_ini_v[INI_MAXK][INI_LINE];
static int g_ini_n;
static void ini_store(const char* k, const char* v) {
    for (int i = 0; i < g_ini_n; i++)
        if (!strcmp(g_ini_k[i], k)) {
            snprintf(g_ini_v[i], sizeof g_ini_v[i], "%s", v);
            return;
        }
    if (g_ini_n == INI_MAXK) {
        WARN_LIMIT(1, "settings: more than %d keys in the .ini, %s and later ones are not kept\n", INI_MAXK, k);
        return;
    }
    snprintf(g_ini_k[g_ini_n], sizeof g_ini_k[g_ini_n], "%s", k);
    snprintf(g_ini_v[g_ini_n], sizeof g_ini_v[g_ini_n], "%s", v);
    g_ini_n++;
}
static const char* ini_get(const char* k, const char* def) {
    for (int i = 0; i < g_ini_n; i++)
        if (!strcmp(g_ini_k[i], k)) return g_ini_v[i];
    return def;
}
static int g_ini_started; /* the game runs: a reload leaves the INI_STARTUP settings alone */
/* the command line's --key=value: they win over the file, at start and on every reload, and the rewritten file keeps
   its own values */
#define INI_MAXO 64
static char g_ovr_k[INI_MAXO][64];
static const char* g_ovr_v[INI_MAXO];
static int g_ovr_n;
void ini_override(const char* k, const char* v) {
    int i = 0;
    while (i < g_ovr_n && strcmp(g_ovr_k[i], k)) i++;
    if (i == INI_MAXO) return;
    snprintf(g_ovr_k[i], sizeof g_ovr_k[i], "%s", k);
    g_ovr_v[i] = v;
    if (i == g_ovr_n) g_ovr_n++;
}
static int ini_overridden(const char* k) {
    for (int i = 0; i < g_ovr_n; i++)
        if (!strcmp(g_ovr_k[i], k)) return 1;
    return 0;
}
/* v is the word w, in any case */
static int word_is(const char* v, const char* w) {
    for (; *w; v++, w++)
        if ((*v | 0x20) != *w) return 0;
    return !*v;
}
/* fps_limit: game, 0, or a number. 1 and negative numbers ran at the console's 60 FPS before the swap interval was
   read: they are the game's pace now */
static void fps_limit_set(const IniOpt* o, const char* v) {
    char* end;
    double d = strtod(v, &end);
    int fps;
    (void)o;
    if (word_is(v, "game"))
        fps = SNX_FPS_GAME;
    else if (end != v && d == d)
        fps = d < 0 || (d >= 1 && d < 2) ? SNX_FPS_GAME : d < 1 ? 0 : d < 10 ? 10 : d > 1000 ? 1000 : (int)d;
    else
        return;
    __atomic_store_n(&g_opt_fps, fps, __ATOMIC_RELAXED); /* present_pace reads it on the game's threads */
}
void fps_limit_text(int fps, char* out, size_t n) {
    if (fps == SNX_FPS_GAME)
        snprintf(out, n, "game");
    else
        snprintf(out, n, "%d", fps);
}
/* a language the settings service doesn't know is English (US), as set_language_code makes it */
static void language_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    if (g_opt_lang < 0 || g_opt_lang >= set_language_count()) g_opt_lang = 1;
}
/* heap_max_mb, in MB: 0 or less is no cap but the heap region's */
static void heap_max_set(const IniOpt* o, const char* v) {
    char* end;
    double mb = strtod(v, &end);
    (void)o;
    if (end == v || mb != mb) return;
    g_heap_max = mb < 1 ? 0 : mb < (double)(HEAP_SIZE >> 20) ? (u64)mb << 20 : HEAP_SIZE;
}
/* the settings this file reads; the platform's (backend.h, vpad.h) are written with them */
static const IniOpt INI_DISPLAY[] = {
    {"docked", "0", "1 = TV mode (1920x1080 / 1600x900 games), 0 = handheld mode (1280x720)", INI_INT, INI_STARTUP,
     &g_opt_docked, 0, 1, NULL},
    {"fullscreen", "0",
     "1 = start in fullscreen (not in timed test runs). -f or --fullscreen=0 sets it for one run.\n"
     ";   F11 or Alt+Enter switches while playing without changing this setting",
     INI_INT, INI_STARTUP, &g_opt_fullscreen, 0, 1, NULL},
    {"language", "1",
     "Game language: 0 Japanese, 1 English (US), 2 French, 3 German, 4 Italian, 5 Spanish,\n;   6 Chinese, 7 Korean, "
     "8 Dutch, 9 Portuguese, 10 Russian, 11 Taiwanese, 12 English (UK),\n;   13 French (Canada), 14 Spanish (Latin "
     "America), 15 Chinese (Simplified), 16 Chinese (Traditional),\n;   17 Portuguese (Brazil)",
     INI_INT, INI_STARTUP, &g_opt_lang, INT_MIN, INT_MAX, language_set},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};
static const IniOpt INI_GRAPHICS[] = {
    {"resolution", "1", "Internal 3D resolution: 1 = native, 2 = double, 0.75 / 0.5 = faster", INI_FLOAT, INI_STARTUP,
     &g_opt_resf, 0.25, 4, NULL},
    {"shadows", "1", "0 = disable shadow maps (more FPS)", INI_INT, 0, &g_opt_shadows, 0, 1, NULL},
    {"vsync", "1",
     "1 = images are shown only on a refresh of the display, so nothing tears. 0 = no wait for the refresh: the\n"
     ";   newest image is shown on the next one where the display can do that, else at once (which may tear).\n"
     ";   A display that can do neither waits for the refresh as 1 does",
     INI_INT, 0, &g_opt_vsync, 0, 1, NULL},
    {"anisotropy", "0",
     "Texture filtering at an angle: 0 = as the game asks. 2 to 16 = at least that much (sharper floors and roads\n"
     ";   in 3D games, a little more GPU work)",
     INI_INT, INI_STARTUP, &g_opt_aniso, 0, 16, NULL},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};
static const IniOpt INI_PERF[] = {
    {"fps_limit", "game",
     "game = the pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30.\n"
     ";   0 = unlimited (a window shows at most one image per refresh unless images tear).\n"
     ";   10 to 1000 = that many FPS.\n"
     ";   A game that advances once per image changes speed with it: 30 runs a 60 FPS game at half speed.\n"
     ";   F4 turns the limit off and on",
     INI_TEXT, 0, NULL, 0, 0, fps_limit_set},
    {"memory_gb", "0",
     "Memory the game sees: 0 or 4 = 3.2 GB like the console, 6 or 8 = more (like yuzu's extended memory layout).\n;"
     "   Games that run out of memory on 3.2 GB, like Tears of the Kingdom, need 6",
     INI_INT, INI_STARTUP, &g_opt_mem_gb, 0, 8, NULL},
    {"heap_max_mb", "0", "The most heap the game can allocate, in MB: 0 = as much as the memory allows", INI_TEXT,
     INI_STARTUP, NULL, 0, 0, heap_max_set},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};
static const IniOpt INI_NETWORK[] = {
    {"network", "0",
     "1 = host networking. Known Nintendo service domains are blocked by the resolver, not by an IP firewall.\n;   0 = "
     "report offline and restrict outbound destinations to loopback. This is not a network sandbox",
     INI_INT, 0, &g_opt_network, 0, 1, NULL},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};
static const IniOpt INI_DBG[] = {
    {"log", "1", "Log detail: 1 = normal, 2 = everything (slow)", INI_INT, 0, &g_verbose, 0, 2, NULL},
    {"auto_stub_ipc", "1",
     "1 = a service command the runtime doesn't know answers success (logged as [undeclared]), 0 = it fails with "
     "UnknownCommandId, as on a console",
     INI_INT, 0, &g_opt_auto_stub_ipc, 0, 1, NULL},
    {"diag", "0",
     "1 = save the data of each new compute shader to captures/compute, and every new shader's\n;   snx-shaderc corpus entry to captures/shaders, for debugging",
     INI_INT, 0, &g_opt_diag, 0, 1, NULL},
    {"profile", "0", "1 = write profile.txt every 10 seconds (CPU profiler, Windows only)", INI_INT, INI_STARTUP,
     &g_opt_prof, 0, 1, NULL},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL}};
/* a table of the file's sections, and what reads its keys: NULL for this file's own (ini_read), backend_ini for the
   platform's, which takes the lock the on-screen controls need */
typedef struct {
    const IniOpt* opts;
    int (*apply)(const char* k, const char* v);
} IniTable;
#define INI_SEC_TABLES 2
typedef struct {
    const char* title;
    IniTable t[INI_SEC_TABLES];
} IniSec;
static const IniSec INI_SECS[] = {{"Display", {{INI_DISPLAY, NULL}}},
                                  {"Graphics", {{INI_GRAPHICS, NULL}}},
                                  {"Performance", {{INI_PERF, NULL}}},
                                  {"Input", {{g_backend_ini, backend_ini}, {g_vpad_ini, backend_ini}}},
                                  {"Network", {{INI_NETWORK, NULL}}},
                                  {"Debug", {{INI_DBG, NULL}}},
                                  {NULL, {{NULL, NULL}}}};
/* k's row in the file's sections, and its table */
static const IniOpt* ini_known(const char* k, const IniTable** table) {
    for (int s = 0; INI_SECS[s].title; s++)
        for (int t = 0; t < INI_SEC_TABLES && INI_SECS[s].t[t].opts; t++) {
            const IniOpt* o = ini_find(INI_SECS[s].t[t].opts, k);
            if (o) {
                if (table) *table = &INI_SECS[s].t[t];
                return o;
            }
        }
    return NULL;
}
/* 1 when some table knows k (the unattended-run options go to backend_ini too) */
static int ini_apply(const char* k, const char* v) {
    const IniTable* t = NULL;
    const IniOpt* o = ini_known(k, &t);
    if (!o) return backend_ini(k, v);
    if (g_ini_started && (o->flags & INI_STARTUP)) return 1;
    if (t->apply) return t->apply(k, v);
    ini_read(o, v);
    return 1;
}
/* a known key missing from the file (deleted while the game runs, or no file yet) goes back to its default */
static void ini_defaults(void) {
    for (int s = 0; INI_SECS[s].title; s++)
        for (int t = 0; t < INI_SEC_TABLES && INI_SECS[s].t[t].opts; t++)
            for (const IniOpt* o = INI_SECS[s].t[t].opts; o->key; o++)
                if (o->def && !ini_get(o->key, NULL) && !ini_overridden(o->key)) ini_apply(o->key, o->def);
}
static char g_ini_path[1400];
static u64 g_ini_hash; /* the file as last read or written: another hash means someone edited it */
/* the file's contents' hash: 0, -1 when it can't be read (errno says why) */
static int file_hash(const char* p, u64* h) {
    unsigned char buf[4096];
    size_t n;
    FILE* f = fopen(p, "rb");
    if (!f) return -1;
    *h = 14695981039346656037ull;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; i++) *h = (*h ^ buf[i]) * 1099511628211ull;
    fclose(f);
    return 0;
}
/* the file's keys into the store, applied: 0, -1 when it can't be read (errno says why) */
static int ini_load(const char* path) {
    size_t size;
    char* text = (char*)snx_read_file(path, &size);
    SnxIni ini;
    char *k, *v;
    if (!text) return -1;
    snx_ini_start(&ini, text); /* sections only arrange the file: a key means the same in any */
    while (snx_ini_next(&ini, &k, &v)) {
        if (strlen(v) >= INI_LINE) { /* longer than the store keeps: dropped, not cut */
            WARN_LIMIT(1,
                       "settings: %s's %s is longer than %d bytes: it is not read, and the rewritten file drops it\n",
                       path, k, INI_LINE - 1);
            continue;
        }
        if (ini_removed(k)) continue;
        ini_store(k, v);
        if (!ini_overridden(k)) ini_apply(k, v);
    }
    free(text);
    return 0;
}
/* the startup-only keys, wrapped to the header's box */
static void write_startup_keys(FILE* f) {
    int col = fprintf(f, ";  These apply when the game starts again: "), n = 0;
    for (int s = 0; INI_SECS[s].title; s++)
        for (int t = 0; t < INI_SEC_TABLES && INI_SECS[s].t[t].opts; t++)
            for (const IniOpt* o = INI_SECS[s].t[t].opts; o->key; o++) {
                if (!(o->flags & INI_STARTUP)) continue;
                if (n && col + (int)strlen(o->key) + 2 > 70)
                    col = fprintf(f, ",\n;  %s", o->key) - 2;
                else
                    col += fprintf(f, "%s%s", n ? ", " : "", o->key);
                n++;
            }
    fprintf(f, ".\n");
}
/* rewrite the ini: the known keys by section with their help, then the others. 0 when it replaced the file */
static int ini_write(void) {
    char tmp[1450];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_ini_path);
    FILE* f = fopen(tmp, "w");
    if (!f) {
        WARN_LIMIT(1, "settings: %s could not be written, so changes made in the game are not saved\n", tmp);
        return -1;
    }
    fprintf(f, "; =====================================================================\n");
    fprintf(f, "; ScarletNX settings for %s\n", g_mod && g_mod->title ? g_mod->title : g_exename);
    fprintf(f, ";  Edit a value and save: most settings apply while the game runs.\n");
    write_startup_keys(f);
    fprintf(f, ";  Keys in game: F1 help, F2 the game's menu, F3 screenshot,\n");
    fprintf(f, ";  F4 FPS limit, F5 thread dump, F11 or Alt+Enter fullscreen.\n");
    fprintf(f, "; =====================================================================\n");
    for (int s = 0; INI_SECS[s].title; s++) {
        fprintf(f, "\n[%s]\n", INI_SECS[s].title);
        for (int t = 0; t < INI_SEC_TABLES && INI_SECS[s].t[t].opts; t++)
            for (const IniOpt* o = INI_SECS[s].t[t].opts; o->key; o++) {
                const char* v = ini_get(o->key, o->def);
                if (o->help) fprintf(f, "; %s\n", o->help);
                if (v) fprintf(f, "%s=%s\n", o->key, v);
            }
    }
    int other = 0;
    for (int i = 0; i < g_ini_n; i++)
        if (!ini_known(g_ini_k[i], NULL)) {
            if (!other) {
                fprintf(f, "\n[Other]\n");
                other = 1;
            }
            fprintf(f, "%s=%s\n", g_ini_k[i], g_ini_v[i]);
        }
    /* the old file stays until the new one replaces it: a failure or a kill in between keeps the player's settings.
       plat_replace_file removes tmp when it fails */
    if (fclose(f) || plat_replace_file(tmp, g_ini_path)) { /* not all of it was written (disk full), or not moved */
        remove(tmp);
        WARN_LIMIT(1, "settings: %s could not be replaced, so changes made in the game are not saved\n", g_ini_path);
        return -1;
    }
    file_hash(g_ini_path, &g_ini_hash);
    return 0;
}
/* in-game toggles (F4) get persisted. Kept until written, so a reload of a file edited meanwhile doesn't lose them */
#define INI_MAXS 32 /* the on-screen controls' reset sets one key per control */
static char g_set_k[INI_MAXS][64], g_set_v[INI_MAXS][64];
static int g_set_n;
void ini_set(const char* k, const char* v) {
    if (ini_overridden(k)) return; /* this run's value is the command line's: the file keeps its own */
    ini_store(k, v);
    int i = 0;
    while (i < g_set_n && strcmp(g_set_k[i], k)) i++;
    if (i == INI_MAXS) return;
    snprintf(g_set_k[i], sizeof g_set_k[i], "%s", k);
    snprintf(g_set_v[i], sizeof g_set_v[i], "%s", v);
    if (i == g_set_n) g_set_n++;
}
void ini_open(const char* path) {
    snprintf(g_ini_path, sizeof g_ini_path, "%s", path);
    int missing = ini_load(path) && errno == ENOENT, unreadable = !missing && file_hash(path, &g_ini_hash);
    ini_defaults();
    for (int i = 0; i < g_ovr_n; i++)
        if (!ini_apply(g_ovr_k[i], g_ovr_v[i]) && strcmp(g_ovr_k[i], "game"))
            WARN("settings: --%s is not a setting, so it does nothing\n", g_ovr_k[i]);
    if (unreadable) /* it would be replaced by defaults: left as it is */
        WARN("settings: %s could not be read, so the defaults apply and it is left as it is\n", path);
    else
        ini_write();
    g_ini_started = 1;
}
/* ini edited while running: reloaded and applied what we can live, once it has stopped changing (an editor saving
   it may be caught halfway, by a poll a second apart). The last poll (final, at exit) doesn't wait when a toggle made
   in the game is waiting to be saved: it reloads what is there and saves the toggle with it */
void ini_poll(int final) {
    static u64 seen; /* an edit's hash, seen once: it is read when the next poll finds it unchanged */
    u64 h;
    int unread, n;
    if (!g_ini_path[0]) return;
    unread = file_hash(g_ini_path, &h);
    if (unread && errno != ENOENT) return; /* there but unreadable: kept as it is, with what changed in the game */
    if (!unread && h != g_ini_hash) {
        if (h != seen && (!final || !g_set_n)) { /* not settled: the next poll reads it, or at exit the next start */
            seen = h;
            return;
        }
        n = g_ini_n;
        g_ini_n = 0;
        if (ini_load(g_ini_path)) { /* gone or unreadable since it was hashed: kept as it was, read again next poll */
            g_ini_n = n;
            if (!final || errno != ENOENT) return; /* gone at exit: the toggles are saved in a new file */
        } else {
            ini_defaults();
            for (int i = 0; i < g_set_n; i++) { /* a toggle made since the last write wins over the file */
                ini_store(g_set_k[i], g_set_v[i]);
                ini_apply(g_set_k[i], g_set_v[i]);
            }
            g_ini_hash = h;
            LOG("settings: %s reloaded\n", g_ini_path);
        }
    } else if (!g_set_n)
        return;
    if (!ini_write()) g_set_n = 0;
}
