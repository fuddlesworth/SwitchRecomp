/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The theme editor, a screen of its own the menu's Theme editor item opens: a row for the folder the theme is saved
   in, a row per key of theme.ini (gui_theme_keys) and Save. main.c draws it, with a preview of the menu or a run in
   the theme as it is; this holds what it edits and what each key does to it, without SDL. */
#ifndef SNX_GUI_EDITOR_H
#define SNX_GUI_EDITOR_H
#include <stdbool.h>
#include <stddef.h>
#include "model.h"
#include "theme.h"

typedef struct {
    GuiTheme theme;
    GuiThemeFile file; /* the extras of the file it started from, written back with it */
    char id[64];       /* the theme's folder in themes/ */
    int row, top;      /* the focused row, and the first one shown */
    bool typing;       /* a value being typed for the focused row, in text */
    char text[80];
    bool dirty, leaving; /* changed since it was saved; ESC pressed once over changes */
    int preview;         /* GUI_SCREEN_MENU or GUI_SCREEN_COMPILING */
    char note[160];      /* what the last key did, when it did not do what it was asked */
} GuiEditor;
/* the rows: the folder, each key, and Save */
#define GUI_EDITOR_FOLDER 0
int gui_editor_rows(void);
/* the key of a row, NULL for the folder and Save */
const GuiThemeKey* gui_editor_key_of(int row);
/* edits theme, its file's extras taken over (file may be NULL), saved as themes/id */
void gui_editor_start(GuiEditor* editor, const GuiTheme* theme, GuiThemeFile* file, const char* id);
void gui_editor_stop(GuiEditor* editor);
/* what a key asks of the screen: nothing more, to leave it, to save */
typedef enum { GUI_EDITOR_STAY, GUI_EDITOR_LEAVE, GUI_EDITOR_SAVE } GuiEditorDo;
/* Up/Down and the pages move; Left/Right change a word, a flag or a number; Select changes a word or a flag, starts
   typing the others and takes what was typed; Cancel stops typing, or leaves (asking once over changes) */
GuiEditorDo gui_editor_key(GuiEditor* editor, GuiKey key);
/* while typing: text added, or the last character taken off */
void gui_editor_type(GuiEditor* editor, const char* text);
void gui_editor_erase(GuiEditor* editor);
/* a row's label and value as the screen shows them */
void gui_editor_label(int row, char* out, size_t size);
void gui_editor_value(const GuiEditor* editor, int row, char* out, size_t size);
/* themes_dir/id/theme.ini written: 0, else nonzero with the note saying why */
int gui_editor_save(GuiEditor* editor, const char* themes_dir);
/* rows rows on screen: top follows the focus */
void gui_editor_scroll(GuiEditor* editor, int rows);
#endif
