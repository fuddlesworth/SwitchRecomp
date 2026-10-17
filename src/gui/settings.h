/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings: the launcher's own two (libgccjit, gdbstub), then every setting of the game's .ini as the
   runtime lists them (scarletnx --settings, its tables), each a row of the form (editor.h), named and its values worded
   as the game's menu shows them. A setting is the .ini's, passed to no run, until one is picked. The picks are kept in
   a file, an .ini of the game's sections and keys, and go to every run as --key=value. A run takes a setting from what
   that run picks, then from the game's own file of picks (to come: today there is only the global one, every game's),
   then from the global file, and last from the game's .ini */
#ifndef SNX_GUI_SETTINGS_H
#define SNX_GUI_SETTINGS_H
#include <stddef.h>
#include "gui/theme.h"

#ifndef SNX_GUI_HAS_GCCJIT
#define SNX_GUI_HAS_GCCJIT 0
#endif
#define GUI_SETTINGS_OWN 2 /* the launcher's own rows, first: libgccjit and gdbstub */
/* a row that is no setting: a page's, which opens the section/page rows listed after it, and its All, first on that
   page, which steps them together */
enum { GUI_SETTING_PAGE = 1, GUI_SETTING_ALL };
#define GUI_SETTING_TEXT 192 /* a typed value's room: gamepad_map's whole map (pad_map.h) is 165 at most */
typedef struct {
    int word;                           /* a word row's: the word, 0 the .ini's */
    char text[GUI_SETTING_TEXT];        /* a typed row's: empty, the .ini's. A page's and an All's: the value shown */
    char start[GUI_SETTING_TEXT];       /* the value the menu starts with, which a file of picks leaves out */
    char option[GUI_SETTING_TEXT + 72]; /* a run's --key=value; a page's row: the section it is in */
    const char* key;                    /* the .ini's, the row's key being its name in the menus */
    const char** values;                /* a word row's values by word, NULL where the words are the values */
    const char* fallback; /* the runtime's default for the .ini, as the row words it: what it shows while not picked */
    int kind;             /* 0, GUI_SETTING_PAGE or GUI_SETTING_ALL */
    int first, count;     /* the rows a page opens, an All steps */
} GuiSetting;
typedef struct {
    GuiThemeKey* keys;  /* the rows: the launcher's, then the runtime's by section */
    GuiSetting* values; /* their values, which the keys' offsets are in */
    int count;          /* in one block with the list the keys' names and help point into */
} GuiSettings;
/* list's settings, a line each (section[/page], key, default, int | float | text, lowest, highest, help, name, the
   values value=word between '|', tabs between), after the launcher's own; another line is left out. A page's rows are
   listed after its section's others, one page a section. 0, else nonzero when out of memory */
int gui_settings_read(GuiSettings* settings, const char* list);
void gui_settings_free(GuiSettings* settings);
/* the row of key set to value, a word or a text as the row takes: 1 when it did */
int gui_settings_set(GuiSettings* settings, const char* key, const char* value);
/* the rows as they are, the launcher's own choices among them, as the values to start from */
void gui_settings_mark(GuiSettings* settings);
/* the picks of a file of them, global or a game's: each key's row set, the others left as they are */
void gui_settings_load(GuiSettings* settings, const char* path);
/* the rows that are not at their start in path, by section; 0, else nonzero */
int gui_settings_save(const GuiSettings* settings, const char* path);
/* what the file of picks at path holds that no row takes, which the next save leaves out: a key no row has, or a value
   its row does not take, as key=value, ", " between. How many */
int gui_settings_unread(GuiSettings* settings, const char* path, char* out, size_t size);
/* row back to its start (a page's or an All's: their rows), every row for -1 */
void gui_settings_reset(GuiSettings* settings, int row);
/* an All's row stepped (-1 or 1, 0 for a select) with every row it steps: 1, else 0 */
int gui_settings_step_all(GuiSettings* settings, int row, int step);
/* the Alls' values after a change: their rows' word when they share one */
void gui_settings_sync(GuiSettings* settings);
/* row's value as the Settings show it: a pick as it is, a row not picked its default and [default] ("Off
   [default]", "[default]" for an empty one), an All's [default] while none of its rows is picked */
void gui_settings_shown(const GuiSettings* settings, int row, char* out, size_t size);
/* the value picked for key, NULL for the .ini's or no such row */
const char* gui_settings_value(const GuiSettings* settings, const char* key);
/* argv's options for a run: --key=value for each setting picked; with launch the launcher's too, -S -s for the stub
   and --backend=gcc unless prepared (a game made already, recompiled only when another build made it, takes LLVM's).
   Their count, count + 1 at most */
size_t gui_settings_options(GuiSettings* settings, int launch, int prepared, const char** argv);
/* the backend a recompile is asked for: --backend=gcc while libgccjit is on, else NULL (LLVM, the driver's own) */
const char* gui_settings_backend(const GuiSettings* settings);
#endif
