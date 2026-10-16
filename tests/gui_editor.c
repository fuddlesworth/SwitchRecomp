/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The theme editor's rows and keys (src/gui/editor.c): what each key does to the theme, typing a value, leaving over
   changes, and Save writing a theme.ini that reads back to the theme edited */
#include "common.h"
#include "editor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GuiEditor e;
/* the row of section/key */
static int row_of(const char* section, const char* key) {
    int r;
    for (r = 0; r < gui_editor_rows(); ++r) {
        const GuiThemeKey* k = gui_editor_key_of(r);
        if (k && !strcmp(k->section, section) && !strcmp(k->key, key)) return r;
    }
    assert(!"no such row");
    return -1;
}
static const char* value_of(int row) {
    static char v[96];
    gui_editor_value(&e, row, v, sizeof v);
    return v;
}
static void type(const char* text) {
    while (e.text[0]) gui_editor_erase(&e);
    gui_editor_type(&e, text);
}

int main(void) {
    const GuiTheme* def = gui_theme_builtin("default");
    int keys = 0, r;
    char label[64], *dir, *path, *text;
    size_t size;
    GuiTheme back;
    while (gui_theme_keys[keys].section) ++keys;

    /* a row for the folder, one per key, and Save */
    assert(gui_editor_rows() == keys + 2 && !gui_editor_key_of(GUI_EDITOR_FOLDER) && !gui_editor_key_of(keys + 1));
    assert(gui_editor_key_of(1) == &gui_theme_keys[0] && gui_editor_key_of(keys) == &gui_theme_keys[keys - 1]);
    gui_editor_label(row_of("colors", "page"), label, sizeof label);
    assert(!strncmp(label, "colors", 6) && strstr(label, "page"));
    gui_editor_label(row_of("colors", "ink"), label, sizeof label);
    assert(label[0] == ' ' && strstr(label, "ink"));
    gui_editor_label(keys + 1, label, sizeof label);
    assert(!strcmp(label, "Save"));

    /* a built-in is edited as a new theme that starts from it */
    gui_editor_start(&e, def, NULL, "mytheme");
    assert(!strcmp(e.id, "mytheme") && !strcmp(e.file.base, "default") && e.row == GUI_EDITOR_FOLDER && !e.dirty);
    assert(!strcmp(value_of(keys + 1), "themes/mytheme/theme.ini"));

    /* moving: the ends hold, the pages go ten */
    assert(gui_editor_key(&e, GUI_UP) == GUI_EDITOR_STAY && e.row == 0);
    gui_editor_key(&e, GUI_PAGE_DOWN);
    assert(e.row == 10);
    for (r = 0; r < 20; ++r) gui_editor_key(&e, GUI_PAGE_DOWN);
    assert(e.row == keys + 1);
    gui_editor_key(&e, GUI_PAGE_UP);
    assert(e.row == keys + 1 - 10 && !e.dirty);

    /* words go round, flags turn, numbers step and stay in their range */
    e.row = row_of("head", "style");
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.head == GUI_STYLE_BAR && e.dirty);
    gui_editor_key(&e, GUI_RIGHT);
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.head == GUI_STYLE_PLAIN);
    gui_editor_key(&e, GUI_LEFT);
    assert(e.theme.head == GUI_STYLE_NONE);
    gui_editor_key(&e, GUI_SELECT); /* Select changes a word too, without typing */
    assert(e.theme.head == GUI_STYLE_PLAIN && !e.typing);
    e.row = row_of("head", "rule");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.theme.head_rule == !def->head_rule);
    e.row = row_of("transitions", "blink_ms");
    e.theme.blink_ms = 5;
    gui_editor_key(&e, GUI_LEFT);
    assert(e.theme.blink_ms == 0);
    gui_editor_key(&e, GUI_RIGHT);
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.blink_ms == 20);
    e.theme.blink_ms = 9995;
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.blink_ms == 10000);
    e.row = row_of("splash", "chance");
    e.theme.splash_chance = 100;
    gui_editor_key(&e, GUI_RIGHT);
    assert(e.theme.splash_chance == 100);
    gui_editor_key(&e, GUI_LEFT);
    assert(e.theme.splash_chance == 99);
    /* a colour is typed: Left and Right only say so */
    e.row = row_of("colors", "page");
    back = e.theme;
    gui_editor_key(&e, GUI_RIGHT);
    assert(!memcmp(&back.page, &e.theme.page, sizeof back.page) && strstr(e.note, "#RRGGBB"));

    /* typing: it starts from the value, what the row does not take keeps it typing, Cancel drops it */
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && !strcmp(e.text, value_of(e.row)));
    gui_editor_erase(&e);
    assert(strlen(e.text) == 6);
    type("#12");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && strstr(e.note, "#RRGGBB"));
    assert(gui_editor_key(&e, GUI_UP) == GUI_EDITOR_STAY && e.typing); /* the keys type, they do not move */
    type("#1a2B3c");
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.typing && e.theme.page.r == 0x1A && e.theme.page.g == 0x2B && e.theme.page.b == 0x3C);
    assert(!strcmp(value_of(e.row), "#1A2B3C"));
    e.row = row_of("body", "marker");
    gui_editor_key(&e, GUI_SELECT);
    type("12345678901234567");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && strstr(e.note, "15 bytes"));
    type(" > ");
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.typing && !strcmp(e.theme.marker, " > "));
    e.row = row_of("head", "menu");
    gui_editor_key(&e, GUI_SELECT);
    type("NIGHT");
    gui_editor_key(&e, GUI_CANCEL);
    assert(!e.typing && strcmp(e.theme.title[GUI_SCREEN_MENU], "NIGHT"));
    /* text as typed: added up to the room, a line's end refused, a UTF-8 character erased whole */
    gui_editor_key(&e, GUI_SELECT);
    type("ab\xC3\xA9");
    gui_editor_type(&e, "x\ny");
    assert(!strcmp(e.text, "ab\xC3\xA9"));
    gui_editor_erase(&e);
    assert(!strcmp(e.text, "ab"));
    for (r = 0; r < 100; ++r) gui_editor_type(&e, "z");
    assert(strlen(e.text) == sizeof e.text - 1);
    gui_editor_key(&e, GUI_CANCEL);
    /* the folder: a name every system takes */
    e.row = GUI_EDITOR_FOLDER;
    gui_editor_key(&e, GUI_SELECT);
    type(".hidden");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing && strstr(e.note, "folder"));
    type("a/b");
    gui_editor_key(&e, GUI_SELECT);
    assert(e.typing);
    type("Night ink_2");
    gui_editor_key(&e, GUI_SELECT);
    assert(!e.typing && !strcmp(e.id, "Night ink_2"));

    /* leaving over changes asks once; a key in between asks again */
    assert(gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_STAY && e.leaving && strstr(e.note, "ESC again"));
    gui_editor_key(&e, GUI_DOWN);
    assert(!e.leaving && gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_STAY);
    assert(gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_LEAVE);

    /* Save: the row says so, and the file reads back to the theme edited */
    e.row = keys + 1;
    assert(gui_editor_key(&e, GUI_SELECT) == GUI_EDITOR_SAVE);
    dir = snx_temp_dir(NULL);
    assert(dir && !gui_editor_save(&e, dir) && !e.dirty && strstr(e.note, "Saved in"));
    path = snx_format("%s/Night ink_2/theme.ini", dir);
    text = (char*)snx_read_file(path, &size);
    assert(text && gui_theme_parse(&back, "Night ink_2", text, size));
    for (r = 1; r <= keys; ++r) {
        char a[96], b[96];
        gui_theme_get(&e.theme, gui_editor_key_of(r), a, sizeof a);
        gui_theme_get(&back, gui_editor_key_of(r), b, sizeof b);
        assert(!strcmp(a, b));
    }
    assert(strstr(text, "base = default"));
    free(text);
    free(path);
    assert(gui_editor_key(&e, GUI_CANCEL) == GUI_EDITOR_LEAVE); /* nothing left to save: no question */
    assert(gui_editor_save(&e, NULL) && strstr(e.note, "Not saved"));
    snx_remove_tree(dir);
    free(dir);

    /* the rows shown follow the focus */
    e.top = 0;
    e.row = 30;
    gui_editor_scroll(&e, 10);
    assert(e.top == 21);
    e.row = 3;
    gui_editor_scroll(&e, 10);
    assert(e.top == 3);
    e.row = keys + 1;
    gui_editor_scroll(&e, 1000);
    assert(e.top == 0);
    gui_editor_stop(&e);
    puts("gui_editor passed");
    return 0;
}
