/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_BROWSER_H
#define SNX_GUI_BROWSER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    char** names; /* basenames: a game's folder, or an older version's file */
    char** shown; /* what each is shown as, sorted in each folder: the name in its control.nacp, else names' */
    char** paths; /* where each is */
    size_t count;
} GuiGames;
/* what a folder's games are: ready to run (executables; on Android a game's code or a dump), CIA files */
enum { GUI_GAMES_RUN = 1, GUI_GAMES_CIA = 2 };
/* A folder's games of kinds added after those listed: its own files, and each folder of it that holds its game
   (<name>/<name>), named by the folder; nothing deeper. False, error saying why, when it cannot be read */
bool gui_games_add(GuiGames* games, const char* directory, int kinds, char* error, size_t size);
/* and with depth, the games of its other folders too, down that many levels; a hidden folder left alone. The folder's
   own error alone is said */
bool gui_games_add_deep(GuiGames* games, const char* directory, int kinds, int depth, char* error, size_t size);
/* whether directory holds a folder that is no game and not hidden: one whose games a deep add would list */
bool gui_has_folders(const char* directory, int kinds);
/* games/'s alone: its executable files and game folders, or with cia CIA files (case-insensitive extension, no execute
   bit needed). Never recurse into data/save directories. */
bool gui_games_scan(GuiGames* games, const char* directory, bool cia, char* error, size_t size);
void gui_games_free(GuiGames* games);
/* The bytes of the files under path, in its folders too, links not followed: 0 for no such folder, or "". It stops,
   with what it counted, once *stop is set by another thread */
uint64_t gui_tree_size(const char* path, const int* stop);
/* An NRO, XCI or NSP (or a decrypted one) by its name: a dump the driver prepares, not a game ready to run */
bool gui_is_dump(const char* name);
/* A game's program in a folder of its own beside its data (<name>/<name>, .exe on Windows, and <name>/<name>_data/),
   as the recompile lays games out: the driver plays it, and recompiles it first when another build made it. A game
   an older version laid out beside the others is not one */
bool gui_game_folder(const char* path);
/* A 3DS CIA by its name */
bool gui_is_cia(const char* name);
#endif
