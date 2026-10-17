/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings (src/gui/settings.c): the runtime's list read into the form's rows, a setting's pick and the
   options a run gets. With a runtime's path, its own --settings list too */
#include "common/utilities/common.h"
#include "gui/editor.h"
#include "gui/settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* as scarletnx --settings prints them, and a line of something else, an older runtime's of seven fields, values with
   one missing its word, a Windows line end and no last one */
static const char list[] =
    "Display\tdocked\t1\tint\t0\t1\t1 = TV mode\t\t0=0|1=1\n"
    "Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\r\n"
    "Display\tlanguage\t1\tint\t-2.14748e+09\t2.14748e+09\tGame language: 0 Japanese\t\t\n"
    "Graphics\tresolution\t1\tfloat\t0.25\t4\tInternal 3D resolution\t\t\n"
    "Graphics\tanisotropy\t0\tint\t0\t16\tTexture filtering at an angle\t\t"
    "0=0|1=1|2=2|3=3|4=4|5=5|6=6|7=7|8=8|9=9|10=10|11=11|12=12|13=13|14=14|15=15|16=16\n"
    "Graphics\told\t0\tint\t0\t1\tAn older runtime's line\n"
    "Graphics\tbroken\t0\tint\t0\t1\tA value without its word\t\t0|1=On\n"
    "settings: not a setting\n"
    "On-screen controls\tcontrols\t0\tint\t0\t1\tThe on-screen controls\tShow controls\t0=Off|1=On\n"
    "On-screen controls\ttouch\t2\tint\t0\t2\tThe game's touch screen\tTouch screen\t0=Off|1=Fingers|2=Mouse too\n"
    "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\tA control drawn\tA\t0=Hidden|1=Shown\n"
    "On-screen controls/Buttons\tcontrols_b_shown\t\tint\t0\t1\tA control drawn\tB\t0=Hidden|1=Shown\n"
    "Controllers\tgamepad_rumble\t100\tint\t0\t100\tHow strongly\tVibration\t0=Off|50=50%|100=100%\n"
    "Debug\tlog\t1\tint\t0\t2\tLog detail\t\t0=0|1=1|2=2";

/* a row by its .ini key, or by the name it shows */
static int at(const GuiSettings* s, const char* name) {
    int i;
    for (i = 0; i < s->count; ++i)
        if ((s->values[i].key && !strcmp(s->values[i].key, name)) || !strcmp(s->keys[i].key, name)) return i;
    return -1;
}
static const GuiThemeKey* key(const GuiSettings* s, const char* name) {
    int i = at(s, name);
    return i < 0 ? NULL : &s->keys[i];
}
static int words(const GuiThemeKey* k) {
    int n = 0;
    while (k->words[n]) ++n;
    return n;
}
static const char* shown(const GuiSettings* s, const char* name) {
    static char value[64];
    gui_theme_get(s->values, key(s, name), value, sizeof value);
    return value;
}
/* as the Settings screen shows it */
static const char* on_screen(const GuiSettings* s, const char* name) {
    static char value[96];
    gui_settings_shown(s, at(s, name), value, sizeof value);
    return value;
}

int main(int argc, char** argv) {
    GuiSettings s = {0};
    GuiEditor form;
    const char* options[64];
    int fullscreen, touch, gcc, stub, prepared, launch, page, all;
    assert(!gui_settings_read(&s, list));

    /* the launcher's two, then a row a setting in the list's order, the other line left out; a page's row and its
       All before its settings */
    assert(s.count == GUI_SETTINGS_OWN + 15 && !strcmp(s.keys[0].key, "libgccjit") &&
           !strcmp(s.keys[1].key, "gdbstub"));
    assert(!strcmp(s.keys[0].section, "Launch") && words(&s.keys[0]) == 1 + SNX_GUI_HAS_GCCJIT);
    assert(!strcmp(s.keys[0].about, SNX_GUI_HAS_GCCJIT ? "libgccjit will be used instead of llvm"
                                                       : "Program was not compiled with libgccjit"));
    assert(!strcmp(s.keys[1].about, "Enables gdbstub on port 1234") && words(&s.keys[1]) == 2);
    assert(!strcmp(s.keys[2].section, "Display") && !strcmp(s.keys[2].key, "docked"));
    assert(!strcmp(s.keys[2].about, "1 = TV mode") && !strcmp(key(&s, "fullscreen")->about, "1 = start in fullscreen"));
    assert(!strcmp(s.keys[s.count - 1].key, "log") && !strcmp(s.keys[s.count - 1].about, "Log detail"));
    /* a setting with values goes round .ini and their words, named as the game's menu names it; the others are
       typed, an older runtime's and one with a value missing its word among them */
    assert(!strcmp(key(&s, "controls")->key, "Show controls") && key(&s, "controls")->type == GUI_KEY_WORD);
    assert(words(key(&s, "touch")) == 4 && !strcmp(key(&s, "touch")->key, "Touch screen"));
    assert(!strcmp(key(&s, "touch")->words[0], ".ini") && !strcmp(key(&s, "touch")->words[3], "Mouse too"));
    assert(words(key(&s, "anisotropy")) == 18 && !strcmp(key(&s, "anisotropy")->words[17], "16"));
    assert(key(&s, "language")->type == GUI_KEY_TEXT && key(&s, "resolution")->type == GUI_KEY_TEXT);
    assert(key(&s, "old")->type == GUI_KEY_TEXT && key(&s, "broken")->type == GUI_KEY_TEXT);
    assert(!strcmp(shown(&s, "touch"), ".ini") && !strcmp(shown(&s, "log"), ".ini"));
    /* the page: its row in its section opens the rows after its All, which steps them */
    page = at(&s, "Buttons");
    all = page + 1;
    assert(page == at(&s, "touch") + 1 && !strcmp(s.keys[page].section, "On-screen controls"));
    assert(s.keys[page].type == GUI_KEY_ACTION && s.values[page].kind == GUI_SETTING_PAGE);
    assert(s.values[page].first == all && s.values[page].count == 3 && !strcmp(shown(&s, "Buttons"), ">"));
    assert(!strcmp(s.keys[all].key, "All") && s.values[all].kind == GUI_SETTING_ALL && s.values[all].first == all + 1);
    assert(s.values[all].count == 2 && !strcmp(s.keys[all].section, "On-screen controls/Buttons"));
    assert(!strcmp(key(&s, "controls_b_shown")->key, "B") && at(&s, "controls_b_shown") == all + 2);
    assert(!strcmp(s.keys[all + 2].section, "On-screen controls/Buttons") && !strcmp(shown(&s, "All"), ".ini"));

    /* a pick: a value of its row or a text, never a word; nothing picked is the .ini's */
    assert(!gui_settings_value(&s, "touch") && !gui_settings_value(&s, "language") && !gui_settings_value(&s, "x"));
    assert(gui_settings_set(&s, "touch", "1") && !strcmp(gui_settings_value(&s, "touch"), "1"));
    assert(!strcmp(shown(&s, "touch"), "Fingers"));
    assert(!gui_settings_set(&s, "touch", "3") && !gui_settings_set(&s, "touch", "Mouse too"));
    assert(!strcmp(gui_settings_value(&s, "touch"), "1") && !gui_settings_set(&s, "Touch screen", "2"));
    /* on the screen: a pick as it is; a row not picked the runtime's default in its words, a text as it is, an empty
       one bare, then [default]; an All while none of its rows is; the launcher's own and a page's as they are */
    assert(!strcmp(on_screen(&s, "touch"), "Fingers") && !strcmp(on_screen(&s, "controls"), "Off [default]"));
    assert(!strcmp(on_screen(&s, "gamepad_rumble"), "100% [default]") && !strcmp(on_screen(&s, "log"), "1 [default]"));
    assert(!strcmp(on_screen(&s, "resolution"), "1 [default]") &&
           !strcmp(on_screen(&s, "controls_a_shown"), "[default]"));
    assert(!strcmp(on_screen(&s, "All"), "[default]") && !strcmp(on_screen(&s, "gdbstub"), "off"));
    assert(!strcmp(on_screen(&s, "Buttons"), ">"));
    assert(gui_settings_set(&s, "language", "3") && !strcmp(gui_settings_value(&s, "language"), "3"));
    assert(gui_settings_set(&s, "old", "0.5 0.5 100 1") && gui_settings_set(&s, "language", ""));
    assert(!gui_settings_value(&s, "language") && !gui_settings_set(&s, "nothing", "1"));
    assert(!gui_settings_set(&s, "Buttons", ">") && !gui_settings_set(&s, "All", ".ini"));
    assert(gui_settings_options(&s, 1, 0, options) == 2 && !strcmp(options[0], "--old=0.5 0.5 100 1"));
    assert(!strcmp(options[1], "--touch=1"));
    assert(gui_settings_set(&s, "touch", ".ini") && gui_settings_set(&s, "old", ""));

    /* All: every row of its page to the first one's next word, either way; Mixed when they differ */
    assert(!gui_settings_step_all(&s, page, 1) && !gui_settings_step_all(&s, at(&s, "touch"), 1));
    assert(gui_settings_step_all(&s, all, 1) && !strcmp(shown(&s, "All"), "Hidden"));
    assert(!strcmp(gui_settings_value(&s, "controls_a_shown"), "0") &&
           !strcmp(gui_settings_value(&s, "controls_b_shown"), "0"));
    assert(gui_settings_set(&s, "controls_b_shown", "1"));
    gui_settings_sync(&s);
    assert(!strcmp(shown(&s, "All"), "Mixed") && !strcmp(on_screen(&s, "All"), "Mixed"));
    assert(gui_settings_step_all(&s, all, -1) && !strcmp(shown(&s, "All"), ".ini"));
    assert(gui_settings_step_all(&s, all, 0) && gui_settings_step_all(&s, all, 0) &&
           !strcmp(shown(&s, "All"), "Shown"));
    assert(gui_settings_options(&s, 0, 0, options) == 2 && !strcmp(options[0], "--controls_a_shown=1"));
    /* the page's row, or its All, back to where they started: its rows */
    gui_settings_mark(&s);
    gui_settings_step_all(&s, all, 1);
    gui_settings_reset(&s, page);
    assert(!strcmp(shown(&s, "All"), "Shown") && !strcmp(gui_settings_value(&s, "controls_b_shown"), "1"));
    gui_settings_free(&s);
    assert(!gui_settings_read(&s, list));

    /* the form edits them: Left and Right go round a row's words, .ini among them */
    gui_editor_form(&form, s.keys, s.count, s.values, NULL);
    form.row = at(&s, "fullscreen");
    gui_editor_key(&form, GUI_RIGHT);
    assert(!strcmp(gui_settings_value(&s, "fullscreen"), "0"));
    gui_editor_key(&form, GUI_RIGHT);
    gui_editor_key(&form, GUI_RIGHT);
    assert(!gui_settings_value(&s, "fullscreen"));
    gui_editor_key(&form, GUI_LEFT);
    assert(!strcmp(gui_settings_value(&s, "fullscreen"), "1"));
    gui_editor_stop(&form);

    /* a run's options: --key=value for each pick in the list's order; the launcher's own with launch, the compiler
       only for what is not a game from games/ already */
    for (fullscreen = 0; fullscreen < 3; ++fullscreen)
        for (touch = 0; touch < 4; ++touch)
            for (gcc = 0; gcc < 2; ++gcc)
                for (stub = 0; stub < 2; ++stub)
                    for (prepared = 0; prepared < 2; ++prepared)
                        for (launch = 0; launch < 2; ++launch) {
                            static const char* const picks[] = {".ini", "0", "1", "2"};
                            size_t n, i = 0;
                            char want[32];
                            assert(gui_settings_set(&s, "fullscreen", picks[fullscreen]));
                            assert(gui_settings_set(&s, "touch", picks[touch]));
                            assert(gui_settings_set(&s, "libgccjit", gcc ? "on" : "off") ==
                                   (!gcc || SNX_GUI_HAS_GCCJIT));
                            assert(gui_settings_set(&s, "gdbstub", stub ? "on" : "off"));
                            /* what R's recompile takes, alone */
                            assert(gcc && SNX_GUI_HAS_GCCJIT ? !strcmp(gui_settings_backend(&s), "--backend=gcc")
                                                             : !gui_settings_backend(&s));
                            n = gui_settings_options(&s, launch, prepared, options);
                            assert(n <= (size_t)s.count + 1);
                            if (fullscreen) {
                                snprintf(want, sizeof want, "--fullscreen=%s", picks[fullscreen]);
                                assert(i < n && !strcmp(options[i++], want));
                            }
                            if (touch) {
                                snprintf(want, sizeof want, "--touch=%s", picks[touch]);
                                assert(i < n && !strcmp(options[i++], want));
                            }
                            if (launch && gcc && SNX_GUI_HAS_GCCJIT && !prepared)
                                assert(i < n && !strcmp(options[i++], "--backend=gcc"));
                            if (launch && stub) {
                                assert(i < n && !strcmp(options[i++], "-S"));
                                assert(i < n && !strcmp(options[i++], "-s"));
                            }
                            assert(i == n);
                        }
    gui_settings_free(&s);

    /* kept in a file of the game's sections and keys: what changed from where the rows started, read back over a
       new start; a pick back at its start leaves it */
    {
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "config/settings.ini"), *text;
        size_t size;
        assert(!gui_settings_read(&s, list) && gui_settings_set(&s, "fullscreen", "0"));
        gui_settings_mark(&s); /* the launcher's own choice, fullscreen 0, is where it starts */
        assert(gui_settings_set(&s, "gdbstub", "on") && gui_settings_set(&s, "touch", "2"));
        assert(gui_settings_set(&s, "language", "3") && gui_settings_set(&s, "fullscreen", ".ini"));
        assert(gui_settings_set(&s, "log", "2") && gui_settings_set(&s, "log", ".ini"));
        assert(gui_settings_set(&s, "controls_b_shown", "0"));
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text &&
               strstr(text, "\n[Launch]\ngdbstub=on\n[Display]\nfullscreen=.ini\nlanguage=3\n"
                            "[On-screen controls]\ntouch=2\n[On-screen controls/Buttons]\ncontrols_b_shown=0\n"));
        assert(!strstr(text, "log=") && !strstr(text, "docked") && !strstr(text, "libgccjit") && !strstr(text, "All"));
        free(text);
        gui_settings_free(&s);
        assert(!gui_settings_read(&s, list) && gui_settings_set(&s, "fullscreen", "0"));
        gui_settings_mark(&s);
        gui_settings_load(&s, path);
        assert(!strcmp(gui_settings_value(&s, "gdbstub"), "on") && !strcmp(gui_settings_value(&s, "language"), "3"));
        assert(!gui_settings_value(&s, "fullscreen") && !strcmp(gui_settings_value(&s, "touch"), "2"));
        assert(!gui_settings_value(&s, "log") && !strcmp(gui_settings_value(&s, "controls_b_shown"), "0"));
        assert(!strcmp(shown(&s, "All"), "Mixed") && gui_settings_options(&s, 1, 0, options) == 5);
        /* a row back to where it started, then every one */
        gui_settings_reset(&s, at(&s, "language"));
        assert(!gui_settings_value(&s, "language") && !strcmp(gui_settings_value(&s, "touch"), "2"));
        gui_settings_reset(&s, -1);
        assert(!strcmp(gui_settings_value(&s, "fullscreen"), "0") && !gui_settings_value(&s, "touch"));
        assert(!gui_settings_value(&s, "gdbstub") && gui_settings_options(&s, 1, 0, options) == 1);
        assert(!strcmp(shown(&s, "All"), ".ini"));
        gui_settings_load(&s, path);                        /* and the file's picks again */
        gui_settings_load(&s, "/nonexistent/settings.ini"); /* none: nothing changes */
        assert(!strcmp(gui_settings_value(&s, "language"), "3"));
        /* what the file holds that no row takes, a key gone from the runtime or a value its row does not have, named
           without changing the rows; the next save leaves it out */
        {
            char unread[256];
            assert(!gui_settings_unread(&s, path, unread, sizeof unread) && !unread[0]);
            assert(!gui_settings_unread(&s, NULL, unread, sizeof unread) && !unread[0]);
            text = (char*)snx_read_file(path, &size);
            assert(text);
            {
                char* more = snx_format("%s[Display]\nrenderer=opengl\ntouch=9\nlanguage=4\n", text);
                assert(!snx_write_file(path, more, strlen(more)));
                free(more);
            }
            free(text);
            assert(gui_settings_unread(&s, path, unread, sizeof unread) == 2);
            assert(!strcmp(unread, "renderer=opengl, touch=9"));
            assert(!strcmp(gui_settings_value(&s, "language"), "3") && !strcmp(gui_settings_value(&s, "touch"), "2"));
            assert(gui_settings_unread(&s, path, unread, 10) == 2 && !strcmp(unread, "renderer="));
            assert(!gui_settings_save(&s, path) && !gui_settings_unread(&s, path, unread, sizeof unread));
        }
        {
            char *plain = snx_path_join(dir, "a file"), *under = snx_path_join(plain, "settings.ini");
            assert(!snx_write_file(plain, "", 0) && gui_settings_save(&s, under)); /* a folder it cannot make */
            free(under);
            free(plain);
        }
        gui_settings_free(&s);
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    assert(!gui_settings_read(&s, "") && s.count == GUI_SETTINGS_OWN); /* no runtime: the launcher's alone */
    assert(gui_settings_options(&s, 1, 0, options) == 0 && !gui_settings_set(&s, "touch", "1"));
    gui_settings_free(&s);

    /* the runtime's own list: every line a row, and a page's two; the game's menu's settings named and worded as it
       shows them, but a control's place, its own */
    if (argc > 1) {
        const char* run[] = {argv[1], "--settings", NULL};
        char* output = NULL;
        const char *c, *next, *last = NULL;
        int lines = 0, pages = 0;
        assert(!snx_run(run, NULL, NULL, &output) && output);
        for (c = output; *c; c = next, ++lines) {
            size_t section = strcspn(c, "\t\n");
            /* a page: a run of lines whose section has a '/', a row and an All more */
            if (memchr(c, '/', section) && !(last && strcspn(last, "\t") == section && !strncmp(c, last, section)))
                ++pages;
            last = c;
            next = c + strcspn(c, "\n");
            if (*next) ++next;
        }
        assert(pages > 0 && !gui_settings_read(&s, output) && s.count == GUI_SETTINGS_OWN + lines + 2 * pages);
        /* each player's own, on Controllers' Player page: a word row whose default is every player's */
        assert(words(key(&s, "gamepad_p1_swap_ab")) == 4 && !strcmp(key(&s, "gamepad_p8_swap_ab")->words[1], "As All"));
        assert(key(&s, "gamepad_p3_device")->type == GUI_KEY_TEXT &&
               !strcmp(key(&s, "gamepad_p3_device")->section, "Controllers/Player"));
        assert(!strcmp(s.keys[GUI_SETTINGS_OWN].section, "Display") && !strcmp(s.keys[s.count - 1].section, "Debug"));
        assert(words(key(&s, "fullscreen")) == 3 && words(key(&s, "touch")) == 4 && words(key(&s, "controls")) == 3);
        assert(!strcmp(key(&s, "controls")->key, "Show controls") && !strcmp(key(&s, "touch")->words[3], "Mouse too"));
        assert(!strcmp(key(&s, "gamepad_rumble")->words[1], "Off") && !key(&s, "controls_a"));
        assert(gui_settings_set(&s, "fullscreen", "0") && gui_settings_set(&s, "touch", "1"));
        assert(gui_settings_set(&s, "controls", "0") && key(&s, "resolution")->type == GUI_KEY_TEXT);
        page = at(&s, "Buttons");
        assert(page > 0 && s.values[page].count == 1 + 17 && !strcmp(s.keys[page + 2].key, "L"));
        gui_settings_free(&s);
        free(output);
    }
    puts("GUI settings: the runtime's list, picks and a run's options passed");
    return 0;
}
