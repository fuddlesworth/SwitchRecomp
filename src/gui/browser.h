/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_BROWSER_H
#define SNX_GUI_BROWSER_H
#include <stdbool.h>
#include <stddef.h>
typedef struct {
    char** names; /* sorted basenames, not display paths */
    size_t count;
} GuiGames;
/* Immediate executable files only. Never recurse into data/save directories. */
bool gui_games_scan(GuiGames* games, const char* directory, char* error, size_t size);
void gui_games_free(GuiGames* games);
/* An NRO, XCI or NSP (or a decrypted one) by its name: a dump the driver prepares, not a game ready to run */
bool gui_is_dump(const char* name);
#endif
