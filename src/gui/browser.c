/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "browser.h"
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static bool suffix(const char* name, const char* extension) {
    size_t n = strlen(name), e = strlen(extension);
    if (n < e) return false;
    for (size_t i = 0; i < e; ++i)
        if (tolower((unsigned char)name[n - e + i]) != extension[i]) return false;
    return true;
}
void gui_games_free(GuiGames* games) {
    snx_free_strings(games->names, games->count);
    games->names = NULL;
    games->count = 0;
}
bool gui_games_scan(GuiGames* games, const char* directory, char* error, size_t size) {
    gui_games_free(games);
    if (size) error[0] = 0;
    char** names = NULL;
    size_t count = 0;
    if (snx_list_dir(directory, &names, &count)) {
        snprintf(error, size, "Cannot open games/. Create it in the displayed working directory.");
        return false;
    }
    games->names = calloc(count ? count : 1, sizeof(*games->names));
    if (!games->names) {
        snx_free_strings(names, count);
        snprintf(error, size, "Not enough memory to list games/.");
        return false;
    }
    /* snx_list_dir already sorts names. Libraries often carry executable bits,
       but they are not game launchers. Do not show them or hidden files. */
    for (size_t i = 0; i < count; ++i) {
        char* name = names[i];
        if (name[0] == '.') continue;
#ifdef __ANDROID__
        /* the shared storage runs nothing, so nothing there is executable: a game is <name> beside <name>_code.so */
        if (!suffix(name, "_code.so")) continue;
        name[strlen(name) - 8] = 0;
        games->names[games->count++] = names[i];
        names[i] = NULL;
        continue;
#endif
#ifdef _WIN32
        if (!suffix(name, ".exe")) continue;
#else
        if (suffix(name, ".so") || strstr(name, ".so.") || suffix(name, ".dll") || suffix(name, ".dylib")) continue;
#endif
        char* path = snx_path_join(directory, name);
#ifndef _WIN32
        if (access(path, X_OK)) {
            free(path);
            continue;
        }
#endif
        char* executable = snx_find_tool(path); /* regular file, with UTF-8 paths on Windows */
        free(path);
        if (!executable) continue;
        free(executable);
        games->names[games->count++] = names[i];
        names[i] = NULL;
    }
    snx_free_strings(names, count);
    return true;
}
