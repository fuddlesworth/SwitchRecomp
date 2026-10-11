/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the settings in <Game>.ini: loading, applying, and writing back what changes */
#include "core/options.h"
#include "core/loader.h"
#include "core/main.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int g_opt_diag = 0;
int g_opt_docked = 0, g_opt_lang = 1, g_opt_fps = 1, g_opt_drs = 0;
float g_opt_resf = 1.0f;
int g_opt_shadows = 1;
int g_opt_prof = 0;
/* legacy (spanish) key names -> current ones */
static const char* const INI_ALIAS[][2] = {{"modo_tv", "docked"},
                                           {"idioma", "language"},
                                           {"resolucion", "resolution"},
                                           {"resolucion_dinamica_del_juego", "game_dynamic_resolution"},
                                           {"sombras", "shadows"},
                                           {"perfil", "profile"},
                                           {NULL, NULL}};
/* options that went with the OpenGL renderer (renderer choice, its dynamic resolution, LSFG, the F7 recorder) and
   older ones: dropped on load, so the rewritten .ini loses them */
static const char* const INI_REMOVED[] = {"vsync",
                                          "renderer",
                                          "dynamic_resolution",
                                          "dynamic_resolution_min",
                                          "resolucion_dinamica",
                                          "resolucion_minima",
                                          "low_priority_shader_compiler",
                                          "compilador_prioridad_baja",
                                          "forced_vsync",
                                          "vsync_forzado",
                                          "smooth_recording",
                                          "grabacion_fluida",
                                          "recording_fps",
                                          "grabacion_fps",
                                          "lsfg",
                                          "lsfg_dll",
                                          "lsfg_target",
                                          "lsfg_objetivo",
                                          "lsfg_multiplier",
                                          "lsfg_multiplicador",
                                          "lsfg_flow",
                                          "lsfg_flujo",
                                          "lsfg_resolution",
                                          "lsfg_resolucion",
                                          "lossless_scaling",
                                          "lossless_path",
                                          "lossless_ruta",
                                          "touch_controls",
                                          "controles",
                                          "touch_controls_opacity",
                                          "controles_opacidad",
                                          NULL};
static int ini_removed(const char* k) {
    for (int i = 0; INI_REMOVED[i]; i++)
        if (!strcmp(k, INI_REMOVED[i])) return 1;
    return 0;
}
static const char* ini_norm(const char* k) {
    for (int i = 0; INI_ALIAS[i][0]; i++)
        if (!strcmp(k, INI_ALIAS[i][0])) return INI_ALIAS[i][1];
    return k;
}
/* everything we read, so the rewrite keeps unknown keys */
#define INI_MAXK 128
static char g_ini_k[INI_MAXK][64], g_ini_v[INI_MAXK][1024];
static int g_ini_n;
static void ini_store(const char* k, const char* v) {
    for (int i = 0; i < g_ini_n; i++)
        if (!strcmp(g_ini_k[i], k)) {
            strncpy(g_ini_v[i], v, 1023);
            return;
        }
    if (g_ini_n < INI_MAXK) {
        strncpy(g_ini_k[g_ini_n], k, 63);
        strncpy(g_ini_v[g_ini_n], v, 1023);
        g_ini_n++;
    }
}
static const char* ini_get(const char* k, const char* def) {
    for (int i = 0; i < g_ini_n; i++)
        if (!strcmp(g_ini_k[i], k)) return g_ini_v[i];
    return def;
}
static void ini_apply(const char* k, const char* v) {
    if (!strcmp(k, "docked"))
        g_opt_docked = atoi(v);
    else if (!strcmp(k, "language"))
        g_opt_lang = atoi(v);
    else if (!strcmp(k, "log"))
        g_verbose = atoi(v);
    else if (!strcmp(k, "diag"))
        g_opt_diag = atoi(v);
    else if (!strcmp(k, "fps_limit"))
        g_opt_fps = atoi(v);
    else if (!strcmp(k, "resolution")) {
        g_opt_resf = (float)atof(v);
        if (g_opt_resf < 0.25f) g_opt_resf = 0.25f;
        if (g_opt_resf > 4) g_opt_resf = 4;
    } else if (!strcmp(k, "shadows"))
        g_opt_shadows = atoi(v);
    else if (!strcmp(k, "profile"))
        g_opt_prof = atoi(v);
    else if (!strcmp(k, "game_dynamic_resolution"))
        g_opt_drs = atoi(v); /* 1 = let the game's own DRS work */
    else if (!strcmp(k, "memory_gb")) {
        g_opt_mem_gb = atoi(v);
    } else if (!strcmp(k, "heap_max_mb")) {
        g_heap_max = (u64)atoi(v) << 20;
    }
    backend_ini(k, v);
}
void ini_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char ln[1200];
    while (fgets(ln, sizeof ln, f)) {
        char* eq = strchr(ln, '=');
        if (!eq || ln[0] == ';' || ln[0] == '#' || ln[0] == '[') continue;
        *eq = 0;
        char* v = eq + 1;
        char* nl = strpbrk(v, "\r\n");
        if (nl) *nl = 0;
        char* k = ln;
        while (*k == ' ' || *k == '\t') k++;
        char* ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = 0;
        while (*v == ' ' || *v == '\t') v++;
        {
            char* ve = v + strlen(v);
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
        }
        if (ini_removed(k)) continue;
        const char* kn = ini_norm(k);
        ini_store(kn, v);
        ini_apply(kn, v);
    }
    fclose(f);
}
/* rewrite the ini on every start: sorted, all keys, with help text */
typedef struct {
    const char* key;
    const char* def;
    const char* help;
} IniOpt;
typedef struct {
    const char* title;
    const IniOpt* o;
} IniSec;
static const IniOpt INI_DISPLAY[] = {
    {"docked", "0", "1 = TV mode (1920x1080 / 1600x900 games), 0 = handheld mode (1280x720)"},
    {"fullscreen", "1", "1 = start in fullscreen (F11 or Alt+Enter toggles it in game)"},
    {"language", "1",
     "Game language: 0 Japanese, 1 English (US), 2 French, 3 German, 4 Italian, 5 Spanish,\n;   6 Chinese, 7 Korean, 8 Dutch, 9 Portuguese, 10 Russian, 11 Taiwanese, 12 English (UK),\n;   13 French (Canada), 14 Spanish (Latin America)"},
    {NULL, NULL, NULL}};
static const IniOpt INI_GRAPHICS[] = {
    {"resolution", "1", "Internal 3D resolution: 1 = native, 2 = double, 0.75 / 0.5 = faster (applies at start)"},
    {"game_dynamic_resolution", "0",
     "1 = let the game lower its own resolution like on the console (0 = always full resolution)"},
    {"shadows", "1", "0 = disable shadow maps (more FPS)"},
    {NULL, NULL, NULL}};
static const IniOpt INI_PERF[] = {
    {"fps_limit", "1",
     "1 = 60 FPS (the speed of a Switch), 0 = unlimited (the game runs faster). Values under 60 are treated as 60: the game advances one step per image, so 30 meant half speed. F4 changes it in game"},
    {"memory_gb", "0",
     "Memory the game sees: 0 = automatic (3.2 GB like the console, more for games that need it,\n;   e.g. Tears of the Kingdom), 4 = console (3.2 GB), 6 or 8 = extended (like yuzu's extended memory layout)"},
    {NULL, NULL, NULL}};
static const IniOpt INI_DBG[] = {{"log", "1", "Log detail: 1 = normal, 2 = everything (slow)"},
                                 {"diag", "0", "1 = automatic diagnostic dumps (F5 always works)"},
                                 {"profile", "0", "1 = write profile.txt every 10 seconds (CPU profiler)"},
                                 {NULL, NULL, NULL}};
static const IniSec INI_SECS[] = {
    {"Display", INI_DISPLAY}, {"Graphics", INI_GRAPHICS}, {"Performance", INI_PERF}, {"Debug", INI_DBG}, {NULL, NULL}};
static char g_ini_path[1400];
static u64 g_ini_mtime;
static u64 file_mtime(const char* p) {
    struct stat st;
    if (stat(p, &st)) return 0;
    return (u64)st.st_mtime;
}
static int ini_known(const char* k) {
    for (int s = 0; INI_SECS[s].title; s++)
        for (const IniOpt* o = INI_SECS[s].o; o->key; o++)
            if (!strcmp(o->key, k)) return 1;
    return 0;
}
static void ini_write(void) {
    if (!g_ini_path[0]) return;
    char tmp[1450];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_ini_path);
    FILE* f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "; =====================================================================\n");
    fprintf(f, ";  %s - SwitchRecomp settings\n", g_mod && g_mod->title ? g_mod->title : g_exename);
    fprintf(f, ";  Edit a value and save: most settings apply live while the game runs\n");
    fprintf(f, ";  (docked, language, fullscreen and frame generation apply on restart).\n");
    fprintf(f, ";  Keys in game: F1 help, F3 screenshot, F4 FPS limit, F5 trace,\n");
    fprintf(f, ";  F7 record video, F8 full-res screenshot, F11 fullscreen.\n");
    fprintf(f, "; =====================================================================\n");
    for (int s = 0; INI_SECS[s].title; s++) {
        fprintf(f, "\n[%s]\n", INI_SECS[s].title);
        for (const IniOpt* o = INI_SECS[s].o; o->key; o++)
            fprintf(f, "; %s\n%s=%s\n", o->help, o->key, ini_get(o->key, o->def));
    }
    int other = 0;
    for (int i = 0; i < g_ini_n; i++)
        if (!ini_known(g_ini_k[i])) {
            if (!other) {
                fprintf(f, "\n[Other]\n");
                other = 1;
            }
            fprintf(f, "%s=%s\n", g_ini_k[i], g_ini_v[i]);
        }
    fclose(f);
    remove(g_ini_path);
    if (rename(tmp, g_ini_path)) {
        remove(tmp);
        return;
    }
    g_ini_mtime = file_mtime(g_ini_path);
}
/* in-game toggles (F4) get persisted */
static volatile int g_ini_dirty;
void ini_set(const char* k, const char* v) {
    ini_store(k, v);
    g_ini_dirty = 1;
}
void ini_open(const char* path) {
    strncpy(g_ini_path, path, sizeof g_ini_path - 1);
    ini_load(path);
    ini_write();
}
/* ini edited while running: reload and apply what we can live */
void ini_poll(void) {
    if (!g_ini_path[0]) return;
    if (g_ini_dirty) {
        g_ini_dirty = 0;
        ini_write();
        return;
    }
    u64 m = file_mtime(g_ini_path);
    if (!m || m == g_ini_mtime) return;
    g_ini_mtime = m;
    g_ini_n = 0;
    ini_load(g_ini_path);
    LOG("settings: %s reloaded\n", g_ini_path);
    ini_write();
}
