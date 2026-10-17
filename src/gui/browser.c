/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/browser.h"
#include "common/utilities/common.h"
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
bool gui_is_cia(const char* name) { return suffix(name, ".cia"); }
bool gui_is_dump(const char* name) {
    return suffix(name, ".nro") || suffix(name, ".xci") || suffix(name, ".nsp") || suffix(name, ".dxci") ||
           suffix(name, ".dnsp");
}
bool gui_game_folder(const char* path) {
    char *folder = snx_dirname(path), *name, *data;
    const char *own = path + strlen(path), *leaf = folder + strlen(folder);
    bool found;
    while (own > path && own[-1] != '/' && own[-1] != '\\') --own;
    while (leaf > folder && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
    name = snx_strdup(own);
#ifdef _WIN32
    if (suffix(name, ".exe")) name[strlen(name) - 4] = 0;
#endif
    data = snx_format("%s/%s_data", folder, name);
    found = *name && !strcmp(leaf, name) && snx_is_file(path) && snx_is_dir(data);
    free(data);
    free(name);
    free(folder);
    return found;
}
void gui_games_free(GuiGames* games) {
    snx_free_strings(games->names, games->count);
    snx_free_strings(games->shown, games->count);
    snx_free_strings(games->paths, games->count);
    games->names = games->shown = games->paths = NULL;
    games->count = 0;
}
uint64_t gui_tree_size(const char* path, const int* stop) {
    char** names = NULL;
    size_t count = 0, i;
    uint64_t total = 0;
    if (!*path || snx_list_dir(path, &names, &count)) return 0; /* "" is none: Windows would list the working folder */
    for (i = 0; i < count && !__atomic_load_n(stop, __ATOMIC_RELAXED); ++i) {
        char* entry = snx_path_join(path, names[i]);
        if (entry && !snx_is_link(entry))
            total += snx_is_dir(entry) ? gui_tree_size(entry, stop) : snx_file_size(entry);
        free(entry);
    }
    snx_free_strings(names, count);
    return total;
}
/* whether a folder's entry is a game of kinds: a CIA, or one ready to run. Libraries often carry executable bits but
   are not games, and hidden files are not shown. On Android the shared storage runs nothing: a game is <name> beside
   <name>_code.so (name is cut there), and a dump put there, to be prepared, shows with its own name */
static bool playable(const char* directory, char* name, int kinds) {
    char* path;
    bool listed;
    if (name[0] == '.') return false;
    if (suffix(name, ".cia")) {
        if (!(kinds & GUI_GAMES_CIA)) return false;
        path = snx_path_join(directory, name);
        listed = path && snx_is_file(path);
        free(path);
        return listed;
    }
    if (!(kinds & GUI_GAMES_RUN)) return false;
#if defined(__ANDROID__) || defined(SNX_IOS)
    { /* a game's code or a dump, a file: a folder named so is neither. An app's games have no runtime copy */
        bool code = suffix(name, "_code.so");
        if (!code && !gui_is_dump(name)) return false;
        path = snx_path_join(directory, name);
        listed = path && snx_is_file(path);
        free(path);
        if (listed && code) name[strlen(name) - 8] = 0;
        return listed;
    }
#else
#ifdef _WIN32
    if (!suffix(name, ".exe")) return false;
#else
    if (suffix(name, ".so") || strstr(name, ".so.") || suffix(name, ".dll") || suffix(name, ".dylib")) return false;
#endif
    path = snx_path_join(directory, name);
#ifndef _WIN32
    if (!path || access(path, X_OK)) {
        free(path);
        return false;
    }
#endif
    {
        char* executable = path ? snx_find_tool(path) : NULL; /* regular file, with UTF-8 paths on Windows */
        free(path);
        listed = executable != NULL;
        free(executable);
    }
    return listed;
#endif
}
/* a game in a folder of its own, <name>/<name> (<name>.exe on Windows, <name>_code.so on Android): where it is, else
   NULL. Any other folder (an older version's <name>_data/, the player's) is no game */
static char* folder_game(const char* directory, const char* name, int kinds) {
    char *folder, *own = NULL, *path = NULL;
    if (!(kinds & GUI_GAMES_RUN) || name[0] == '.') return NULL;
    folder = snx_path_join(directory, name);
    if (folder && snx_is_dir(folder) && !snx_is_link(folder)) {
#if defined(__ANDROID__) || defined(SNX_IOS)
        own = snx_format("%s_code.so", name);
#elif defined(_WIN32)
        own = snx_format("%s.exe", name);
#else
        own = snx_strdup(name);
#endif
        if (own && playable(folder, own, GUI_GAMES_RUN)) path = snx_path_join(folder, own);
    }
    free(own);
    free(folder);
    return path;
}
/* what the game at path, named name, is shown as: the name in its <name>_data/control.nacp, which the recompile
   copies there, else name */
static char* shown_name(const char* path, const char* name) {
    char *folder = snx_dirname(path), *stem = snx_strdup(name), *nacp = NULL, *shown = NULL, text[0x200];
    unsigned char* bytes;
    size_t size = 0;
    if (stem && suffix(stem, ".exe")) stem[strlen(stem) - 4] = 0;
    if (folder && stem) nacp = snx_format("%s/%s_data/control.nacp", folder, stem);
    bytes = nacp && snx_is_file(nacp) ? (unsigned char*)snx_read_file(nacp, &size) : NULL;
    if (bytes && snx_nacp_name(bytes, size, text, sizeof text)) shown = snx_strdup(text);
    free(bytes);
    free(nacp);
    free(stem);
    free(folder);
    return shown ? shown : snx_strdup(name);
}
static int before(const char* a, const char* b) {
    for (; *a && tolower((unsigned char)*a) == tolower((unsigned char)*b); ++a, ++b) {}
    return tolower((unsigned char)*a) < tolower((unsigned char)*b);
}
/* directory's games into games, sorted from first on by what they are shown as; with depth, its folders' games too, down
   that many levels, a folder that is a game or hidden left alone */
static bool add_games(GuiGames* games, const char* directory, int kinds, size_t first, int depth, char* error,
                      size_t size) {
    char **names = NULL, **more;
    size_t count = 0, i;
    if (snx_list_dir(directory, &names, &count)) {
        snprintf(error, size, "Cannot open %s.", directory);
        return false;
    }
    size_t j;
    more = (char**)realloc(games->names, (games->count + count + 1) * sizeof *more);
    if (more) games->names = more;
    more = more ? (char**)realloc(games->shown, (games->count + count + 1) * sizeof *more) : NULL;
    if (more) games->shown = more;
    more = more ? (char**)realloc(games->paths, (games->count + count + 1) * sizeof *more) : NULL;
    if (more) games->paths = more;
    if (!more) {
        snx_free_strings(names, count);
        snprintf(error, size, "Not enough memory to list %s.", directory);
        return false;
    }
    for (i = 0; i < count; ++i) {
        char* path = folder_game(directory, names[i], kinds);
        char* shown;
        if (!path && playable(directory, names[i], kinds)) path = snx_path_join(directory, names[i]);
        if (!path && depth > 0 && names[i][0] != '.') { /* a folder of games, not one */
            char* sub = snx_path_join(directory, names[i]);
            if (sub && snx_is_dir(sub)) add_games(games, sub, kinds, first, depth - 1, error, 0);
            free(sub);
            continue;
        }
        if (!path) continue;
        shown = shown_name(path, names[i]);
        if (!shown) {
            free(path);
            continue;
        }
        /* in among the folder's ones by what they are shown as, a game's folder being its title ID */
        for (j = games->count; j > first && before(shown, games->shown[j - 1]); --j) {
            games->names[j] = games->names[j - 1];
            games->shown[j] = games->shown[j - 1];
            games->paths[j] = games->paths[j - 1];
        }
        games->names[j] = names[i];
        games->shown[j] = shown;
        games->paths[j] = path;
        games->count++;
        names[i] = NULL;
    }
    snx_free_strings(names, count);
    return true;
}
bool gui_games_add(GuiGames* games, const char* directory, int kinds, char* error, size_t size) {
    return gui_games_add_deep(games, directory, kinds, 0, error, size);
}
bool gui_games_add_deep(GuiGames* games, const char* directory, int kinds, int depth, char* error, size_t size) {
    if (size) error[0] = 0;
    return add_games(games, directory, kinds, games->count, depth, error, size);
}
bool gui_has_folders(const char* directory, int kinds) {
    char** names = NULL;
    size_t count = 0, i;
    bool found = false;
    if (snx_list_dir(directory, &names, &count)) return false;
    for (i = 0; i < count && !found; ++i) {
        char *sub, *game;
        if (names[i][0] == '.') continue;
        sub = snx_path_join(directory, names[i]);
        game = folder_game(directory, names[i], kinds);
        found = sub && snx_is_dir(sub) && !game;
        free(game);
        free(sub);
    }
    snx_free_strings(names, count);
    return found;
}
bool gui_games_scan(GuiGames* games, const char* directory, bool cia, char* error, size_t size) {
    gui_games_free(games);
    if (gui_games_add(games, directory, cia ? GUI_GAMES_CIA : GUI_GAMES_RUN, error, size)) return true;
    if (!strncmp(error, "Cannot open", 11))
        snprintf(error, size, "Cannot open games/. Create it in the displayed working directory.");
    return false;
}
