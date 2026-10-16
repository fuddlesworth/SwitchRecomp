/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "editor.h"
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int key_count(void) {
    static int n = -1;
    if (n < 0)
        for (n = 0; gui_theme_keys[n].section; ++n) {}
    return n;
}
int gui_editor_rows(void) { return key_count() + 2; }
const GuiThemeKey* gui_editor_key_of(int row) {
    return row > GUI_EDITOR_FOLDER && row <= key_count() ? &gui_theme_keys[row - 1] : NULL;
}
static int is_save(int row) { return row == gui_editor_rows() - 1; }

void gui_editor_start(GuiEditor* editor, const GuiTheme* theme, GuiThemeFile* file, const char* id) {
    memset(editor, 0, sizeof *editor);
    editor->theme = *theme;
    if (file) {
        editor->file = *file;
        file->others = NULL; /* the editor's now */
    } else
        snprintf(editor->file.base, sizeof editor->file.base, "%s", gui_theme_builtin(theme->id) ? theme->id : "");
    snprintf(editor->id, sizeof editor->id, "%s", id);
    editor->preview = GUI_SCREEN_MENU;
}
void gui_editor_stop(GuiEditor* editor) { gui_theme_file_free(&editor->file); }

/* a folder of themes/ on every system: letters, digits, space, - _ and ., not starting with . */
static int folder_name(const char* s) {
    const char* c;
    if (!*s || *s == '.' || strlen(s) >= sizeof(((GuiEditor*)0)->id)) return 0;
    for (c = s; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == ' ' ||
              *c == '-' || *c == '_' || *c == '.'))
            return 0;
    return 1;
}
/* a word, a flag or a number moved by step: a word goes round, a number stays in 0 to its max */
static void change(GuiEditor* editor, const GuiThemeKey* key, int step) {
    char* field = (char*)&editor->theme + key->at;
    if (key->type == GUI_KEY_FLAG)
        *(int*)field = !*(int*)field;
    else if (key->type == GUI_KEY_WORD) {
        int n = 0;
        while (key->words[n]) ++n;
        *(int*)field = (*(int*)field + n + step) % n;
    } else if (key->type == GUI_KEY_NUMBER) {
        long v = (long)*(uint32_t*)field + step * (key->max >= 1000 ? 10 : 1); /* ms by tens */
        *(uint32_t*)field = (uint32_t)(v<0 ? 0 : v>(long) key->max ? (long)key->max : v);
    } else {
        snprintf(editor->note, sizeof editor->note, "Select to type it: %s",
                 key->type == GUI_KEY_COLOUR ? "#RRGGBB" : "its text");
        return;
    }
    editor->dirty = true;
}
/* what was typed into the focused row: kept, or the note says what the row takes */
static void take(GuiEditor* editor) {
    const GuiThemeKey* key = gui_editor_key_of(editor->row);
    if (!key) {
        if (!folder_name(editor->text)) {
            snprintf(editor->note, sizeof editor->note,
                     "A folder name: letters, digits, spaces, - _ and ., not starting with .");
            return;
        }
        snprintf(editor->id, sizeof editor->id, "%s", editor->text);
    } else if (!gui_theme_set(&editor->theme, key, editor->text)) {
        if (key->type == GUI_KEY_TEXT)
            snprintf(editor->note, sizeof editor->note, "At most %u bytes", (unsigned)key->size - 1);
        else
            snprintf(editor->note, sizeof editor->note, "%s",
                     key->type == GUI_KEY_COLOUR ? "A colour: #RRGGBB, as #1A2B3C" : "A whole number, 0 to 10000");
        return;
    }
    editor->typing = false;
    editor->dirty = true;
}
GuiEditorDo gui_editor_key(GuiEditor* editor, GuiKey key) {
    const GuiThemeKey* k = gui_editor_key_of(editor->row);
    int rows = gui_editor_rows(), leaving = editor->leaving;
    editor->leaving = false;
    editor->note[0] = 0;
    if (editor->typing) {
        if (key == GUI_SELECT)
            take(editor);
        else if (key == GUI_CANCEL)
            editor->typing = false;
        return GUI_EDITOR_STAY;
    }
    switch (key) {
        case GUI_UP: editor->row = editor->row ? editor->row - 1 : 0; break;
        case GUI_DOWN: editor->row = editor->row + 1 < rows ? editor->row + 1 : rows - 1; break;
        case GUI_PAGE_UP: editor->row = editor->row > 10 ? editor->row - 10 : 0; break;
        case GUI_PAGE_DOWN: editor->row = editor->row + 10 < rows ? editor->row + 10 : rows - 1; break;
        case GUI_LEFT:
        case GUI_RIGHT:
            if (k) change(editor, k, key == GUI_RIGHT ? 1 : -1);
            break;
        case GUI_SELECT:
            if (is_save(editor->row)) return GUI_EDITOR_SAVE;
            if (k && (k->type == GUI_KEY_FLAG || k->type == GUI_KEY_WORD)) {
                change(editor, k, 1);
                break;
            }
            gui_editor_value(editor, editor->row, editor->text, sizeof editor->text);
            editor->typing = true;
            break;
        case GUI_CANCEL:
            if (!editor->dirty || leaving) return GUI_EDITOR_LEAVE;
            editor->leaving = true;
            snprintf(editor->note, sizeof editor->note, "Not saved: ESC again leaves without it, Save keeps it");
            break;
    }
    return GUI_EDITOR_STAY;
}
void gui_editor_type(GuiEditor* editor, const char* text) {
    size_t n = strlen(editor->text), add = strlen(text);
    if (!editor->typing || strchr(text, '\n')) return;
    if (n + add >= sizeof editor->text) add = sizeof editor->text - 1 - n;
    memcpy(editor->text + n, text, add);
    editor->text[n + add] = 0;
}
void gui_editor_erase(GuiEditor* editor) {
    size_t n = strlen(editor->text);
    while (n && ((unsigned char)editor->text[n - 1] & 0xC0) == 0x80) --n; /* a UTF-8 character's continuation */
    if (n) --n;
    editor->text[n] = 0;
}

void gui_editor_label(int row, char* out, size_t size) {
    const GuiThemeKey* k = gui_editor_key_of(row);
    int first = k && (row == 1 || strcmp(gui_editor_key_of(row - 1)->section, k->section));
    if (!k)
        snprintf(out, size, "%s", row == GUI_EDITOR_FOLDER ? "Folder" : "Save");
    else
        snprintf(out, size, "%-11s %s", first ? k->section : "", k->key);
}
void gui_editor_value(const GuiEditor* editor, int row, char* out, size_t size) {
    const GuiThemeKey* k = gui_editor_key_of(row);
    if (k)
        gui_theme_get(&editor->theme, k, out, size);
    else if (row == GUI_EDITOR_FOLDER)
        snprintf(out, size, "%s", editor->id);
    else
        snprintf(out, size, "themes/%s/theme.ini", editor->id);
}

int gui_editor_save(GuiEditor* editor, const char* themes_dir) {
    char* folder = themes_dir ? snx_path_join(themes_dir, editor->id) : NULL;
    char* path = folder ? snx_path_join(folder, "theme.ini") : NULL;
    char* text = gui_theme_write(&editor->theme, &editor->file);
    int failed = !path || !text || snx_mkdirs(folder) || snx_write_file(path, text, strlen(text));
    if (failed)
        snprintf(editor->note, sizeof editor->note, "Not saved: %s could not be written",
                 path ? path : "the themes folder");
    else {
        snprintf(editor->note, sizeof editor->note, "Saved in %s", path);
        editor->dirty = false;
    }
    free(text);
    free(path);
    free(folder);
    return failed;
}

void gui_editor_scroll(GuiEditor* editor, int rows) {
    if (rows < 1) rows = 1;
    if (editor->row < editor->top) editor->top = editor->row;
    if (editor->row >= editor->top + rows) editor->top = editor->row - rows + 1;
    if (editor->top > gui_editor_rows() - rows) editor->top = gui_editor_rows() - rows;
    if (editor->top < 0) editor->top = 0;
}
