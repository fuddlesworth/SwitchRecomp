/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* theme.ini written from a theme reads back to that theme (gui_theme_write), over any base: the built-ins, every
   format's fixtures (tests/gui_themes), ErET's loading layout and the keys the GUI does not read; and the key table's
   values: what each type takes and leaves */
#include "common.h"
#include "theme.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* every key the same in both, with the format: 0 and the first that differs printed */
static int same_theme(const GuiTheme* a, const GuiTheme* b, const char* what) {
    const GuiThemeKey* k;
    char x[80], y[80];
    if (a->format != b->format) {
        printf("FAIL %s: format %lu, written and read %lu\n", what, (unsigned long)a->format, (unsigned long)b->format);
        return 0;
    }
    for (k = gui_theme_keys; k->section; ++k) {
        gui_theme_get(a, k, x, sizeof x);
        gui_theme_get(b, k, y, sizeof y);
        if (strcmp(x, y)) {
            printf("FAIL %s: [%s] %s is \"%s\", written and read \"%s\"\n", what, k->section, k->key, x, y);
            return 0;
        }
    }
    return 1;
}
/* text as a file of folder id reads, written and read again: the same theme, and the written text read again writes
   the same text */
static int round_trip(const char* text, size_t size, const char* id, const char* what, char** written) {
    GuiTheme read, again;
    GuiThemeFile file, file_again;
    char* text_again;
    int ok;
    assert(gui_theme_parse(&read, id, text, size) && gui_theme_file_read(&file, text, size));
    *written = gui_theme_write(&read, &file);
    assert(*written && gui_theme_parse(&again, id, *written, strlen(*written)));
    assert(gui_theme_file_read(&file_again, *written, strlen(*written)));
    text_again = gui_theme_write(&again, &file_again);
    ok = same_theme(&read, &again, what);
    if (ok && strcmp(*written, text_again)) {
        printf("FAIL %s: written twice, two texts\n", what);
        ok = 0;
    }
    free(text_again);
    gui_theme_file_free(&file);
    gui_theme_file_free(&file_again);
    return ok;
}

int main(int argc, char** argv) {
    const char* fixtures = argc > 1 ? argv[1] : SNX_GUI_THEME_FIXTURES "/1";
    static const char* const builtins[] = {"default", "yorha"};
    char** names = NULL;
    size_t count = 0, i;
    int fails = 0;
    char* written;
    GuiTheme t;
    const GuiThemeKey* k;
    char value[80];

    /* the built-ins, as their own files say them */
    for (i = 0; i < 2; ++i) {
        const GuiTheme* b = gui_theme_builtin(builtins[i]);
        GuiThemeFile file = {"", "", 0, 0, 0, NULL};
        snprintf(file.base, sizeof file.base, "%s", builtins[i]);
        written = gui_theme_write(b, &file);
        assert(written && gui_theme_parse(&t, builtins[i], written, strlen(written)));
        fails += !same_theme(b, &t, builtins[i]);
        assert(strstr(written, "base = ") && strstr(written, builtins[i]));
        free(written);
    }
    /* every fixture of format 1: what the GUI reads from it, written, reads the same */
    assert(!snx_list_dir(fixtures, &names, &count) && count);
    for (i = 0; i < count; ++i) {
        char* path;
        unsigned char* text;
        size_t size;
        size_t n = strlen(names[i]);
        if (n < 5 || strcmp(names[i] + n - 4, ".ini")) continue;
        path = snx_path_join(fixtures, names[i]);
        text = snx_read_file(path, &size);
        assert(text);
        names[i][n - 4] = 0;
        fails += !round_trip((const char*)text, size, names[i], names[i], &written);
        if (!strcmp(names[i], "site-eret-loading"))
            assert(strstr(written, "layout = loading") && !strstr(written, "base = "));
        free(written);
        free(text);
        free(path);
    }
    snx_free_strings(names, count);

    /* the keys a file has that the GUI does not read stay, by section, and ErET's places are written with the theme's
       values; quotes keep the spaces at the ends of a value, and a value that is itself quoted */
    {
        static const char text[] =
            "[theme]\nbase = yorha\nwho = me\n[run]\nring = left\n[header]\nring_side = right\n"
            "[loading]\ntitle = BOOT\n[colors]\ntexture = true\nlogo = big\n[body]\nmarker = \" > \"\n"
            "[head]\nmenu = \"\"quoted\"\"\n[extra]\na = 1\nb = \" two \"\n";
        fails += !round_trip(text, sizeof text - 1, "mine", "extras", &written);
        assert(strstr(written, "[theme]\nwho = me\n[colors]\nlogo = big\n[extra]\na = 1\nb = \" two \"\n"));
        assert(strstr(written, "[header]\nring_side = right\n") && strstr(written, "[loading]\ntitle = BOOT\n"));
        assert(strstr(written, "marker = \" > \"\n") && strstr(written, "menu = \"\"quoted\"\"\n"));
        free(written);
    }

    /* what each type takes; a value it does not take leaves the field */
    t = *gui_theme_builtin("default");
    k = gui_theme_key("colors", "page");
    assert(k && gui_theme_set(&t, k, "#a0B1c2") && !gui_theme_set(&t, k, "#a0B1c") && !gui_theme_set(&t, k, "a0B1c2f"));
    assert(!gui_theme_set(&t, k, "#GG0000"));
    gui_theme_get(&t, k, value, sizeof value);
    assert(!strcmp(value, "#A0B1C2"));
    k = gui_theme_key("head", "rule");
    assert(gui_theme_set(&t, k, "false") && !gui_theme_set(&t, k, "yes") && !gui_theme_set(&t, k, "1"));
    k = gui_theme_key("transitions", "blink_ms");
    assert(gui_theme_set(&t, k, "10000") && !gui_theme_set(&t, k, "10001") && !gui_theme_set(&t, k, "-1"));
    assert(!gui_theme_set(&t, k, "") && !gui_theme_set(&t, k, "5x"));
    gui_theme_get(&t, k, value, sizeof value);
    assert(!strcmp(value, "10000"));
    k = gui_theme_key("splash", "chance");
    assert(gui_theme_set(&t, k, "500") && t.splash_chance == 100);
    k = gui_theme_key("run", "ring");
    assert(gui_theme_set(&t, k, "right-near-text") && t.ring == GUI_RING_NEAR_TEXT && !gui_theme_set(&t, k, "up"));
    k = gui_theme_key("body", "marker");
    assert(gui_theme_set(&t, k, "123456789012345") && !gui_theme_set(&t, k, "1234567890123456"));
    assert(!gui_theme_set(&t, k, "a\nb") && !strcmp(t.marker, "123456789012345"));
    assert(!gui_theme_key("colors", "texture") && !gui_theme_key("theme", "format") && gui_theme_key("theme", "name"));
    for (k = gui_theme_keys; k->section; ++k) assert(*gui_theme_section_about(k->section) && *k->about);

    if (fails) return 1;
    puts("gui_theme_write passed");
    return 0;
}
