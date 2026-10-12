/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the settings in <Game>.ini: loading, applying, and writing back what changes */
#include "core/options.h"
#include "core/loader.h"
#include "core/main.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int g_opt_diag = 0;
int g_opt_docked = 0, g_opt_lang = 1, g_opt_fps = 1;
float g_opt_resf = 1.0f;
int g_opt_shadows = 1;
int g_opt_prof = 0;
/* legacy (spanish) key names -> current ones */
static const char* const INI_ALIAS[][2] = {{"modo_tv", "docked"},  {"idioma", "language"}, {"resolucion", "resolution"},
                                           {"sombras", "shadows"}, {"perfil", "profile"},  {NULL, NULL}};
/* options that went with the OpenGL renderer (renderer choice, its dynamic resolution, LSFG, the F7 recorder) and
   older ones: dropped on load, so the rewritten .ini loses them. game_dynamic_resolution worked through GPU timings
   the OpenGL renderer's thread measured */
static const char* const INI_REMOVED[] = {"vsync",
                                          "renderer",
                                          "dynamic_resolution",
                                          "dynamic_resolution_min",
                                          "game_dynamic_resolution",
                                          "resolucion_dinamica_del_juego",
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
/* what only applies when the game starts: the services, the window and the renderer read them once */
static const char* const INI_RESTART[] = {"docked",    "language",    "fullscreen", "resolution",
                                          "memory_gb", "heap_max_mb", "profile",    NULL};
static int g_ini_started; /* the game runs: a reload leaves INI_RESTART's settings alone */
static int ini_restart_only(const char* k) {
    for (int i = 0; INI_RESTART[i]; i++)
        if (!strcmp(k, INI_RESTART[i])) return 1;
    return 0;
}
/* the command line's --key=value: they win over the file, at start and on every reload, and the rewritten file keeps
   its own values */
#define INI_MAXO 64
static char g_ovr_k[INI_MAXO][64];
static const char* g_ovr_v[INI_MAXO];
static int g_ovr_n;
void ini_override(const char* k, const char* v) {
    k = ini_norm(k);
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
static void ini_apply(const char* k, const char* v) {
    if (g_ini_started && ini_restart_only(k)) return;
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
    else if (!strcmp(k, "memory_gb")) {
        g_opt_mem_gb = atoi(v);
    } else if (!strcmp(k, "heap_max_mb")) { /* 0 or less: no cap but the heap region's */
        long long mb = atoll(v);
        g_heap_max = mb <= 0 ? 0 : (u64)mb < (HEAP_SIZE >> 20) ? (u64)mb << 20 : HEAP_SIZE;
    }
    backend_ini(k, v);
}
/* rewrite the ini on every start: the known keys by section with their help, then the others */
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
    {"shadows", "1", "0 = disable shadow maps (more FPS)"},
    {NULL, NULL, NULL}};
static const IniOpt INI_PERF[] = {
    {"fps_limit", "1",
     "1 = 60 FPS (the speed of a Switch), 0 = unlimited (the game runs faster). Values under 60 are treated as 60: the game advances one step per image, so 30 meant half speed. F4 changes it in game"},
    {"memory_gb", "0",
     "Memory the game sees: 0 or 4 = 3.2 GB like the console, 6 or 8 = more (like yuzu's extended memory layout).\n;   Games that run out of memory on 3.2 GB, like Tears of the Kingdom, need 6"},
    {NULL, NULL, NULL}};
static const IniOpt INI_DBG[] = {
    {"log", "1", "Log detail: 1 = normal, 2 = everything (slow)"},
    {"diag", "0", "1 = save the data of each new compute shader to captures/compute, for debugging"},
    {"profile", "0", "1 = write profile.txt every 10 seconds (CPU profiler, Windows only)"},
    {NULL, NULL, NULL}};
static const IniSec INI_SECS[] = {
    {"Display", INI_DISPLAY}, {"Graphics", INI_GRAPHICS}, {"Performance", INI_PERF}, {"Debug", INI_DBG}, {NULL, NULL}};
static void ini_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char ln[1200];
    while (fgets(ln, sizeof ln, f)) {
        char* k = ln;
        while (*k == ' ' || *k == '\t') k++;
        char* eq = strchr(k, '=');
        if (!eq || *k == ';' || *k == '#' || *k == '[') continue;
        *eq = 0;
        char* v = eq + 1;
        char* nl = strpbrk(v, "\r\n");
        if (nl) *nl = 0;
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
        if (!ini_overridden(kn)) ini_apply(kn, v);
    }
    fclose(f);
    /* a known key missing from the file (deleted while the game runs) goes back to its default */
    for (int s = 0; INI_SECS[s].title; s++)
        for (const IniOpt* o = INI_SECS[s].o; o->key; o++)
            if (!ini_get(o->key, NULL) && !ini_overridden(o->key)) ini_apply(o->key, o->def);
}
static char g_ini_path[1400];
/* the file as last read or written: a change of either means someone edited it */
static u64 g_ini_mtime, g_ini_size;
static int file_changed(const char* p, int remember) {
    struct stat st;
    if (stat(p, &st)) return 0;
    int ch = (u64)st.st_mtime != g_ini_mtime || (u64)st.st_size != g_ini_size;
    if (remember) {
        g_ini_mtime = (u64)st.st_mtime;
        g_ini_size = (u64)st.st_size;
    }
    return ch;
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
    fprintf(f, "; SwitchRecomp settings for %s\n", g_mod && g_mod->title ? g_mod->title : g_exename);
    fprintf(f, ";  Edit a value and save: most settings apply while the game runs.\n");
    fprintf(f, ";  docked, language, fullscreen, resolution, memory_gb and profile\n");
    fprintf(f, ";  apply when the game starts again.\n");
    fprintf(f, ";  Keys in game: F1 help, F3 screenshot, F4 FPS limit, F5 thread dump,\n");
    fprintf(f, ";  F11 or Alt+Enter fullscreen.\n");
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
    /* the old file stays until the new one replaces it: a failure or a kill in between keeps the player's settings */
    if (fclose(f)) { /* not all of it was written (disk full) */
        remove(tmp);
        return;
    }
    if (plat_replace_file(tmp, g_ini_path)) return;
    file_changed(g_ini_path, 1);
}
/* in-game toggles (F4) get persisted. Kept until written, so a reload of a file edited meanwhile doesn't lose them */
#define INI_MAXS 8
static char g_set_k[INI_MAXS][64], g_set_v[INI_MAXS][64];
static int g_set_n;
void ini_set(const char* k, const char* v) {
    ini_store(k, v);
    int i = 0;
    while (i < g_set_n && strcmp(g_set_k[i], k)) i++;
    if (i == INI_MAXS) return;
    snprintf(g_set_k[i], sizeof g_set_k[i], "%s", k);
    snprintf(g_set_v[i], sizeof g_set_v[i], "%s", v);
    if (i == g_set_n) g_set_n++;
}
void ini_open(const char* path) {
    strncpy(g_ini_path, path, sizeof g_ini_path - 1);
    ini_load(path);
    for (int i = 0; i < g_ovr_n; i++) ini_apply(g_ovr_k[i], g_ovr_v[i]);
    ini_write();
    g_ini_started = 1;
}
/* ini edited while running: reload and apply what we can live */
void ini_poll(void) {
    if (!g_ini_path[0]) return;
    if (file_changed(g_ini_path, 1)) {
        g_ini_n = 0;
        ini_load(g_ini_path);
        for (int i = 0; i < g_set_n; i++) { /* a toggle made since the last write wins over the file */
            ini_store(g_set_k[i], g_set_v[i]);
            ini_apply(g_set_k[i], g_set_v[i]);
        }
        LOG("settings: %s reloaded\n", g_ini_path);
    } else if (!g_set_n)
        return;
    g_set_n = 0;
    ini_write();
}
