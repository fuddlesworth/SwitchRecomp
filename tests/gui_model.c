/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "model.h"
#include "browser.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int feed(GuiBanner* banner, const char* text) { return gui_banner_feed(banner, text, strlen(text)); }

int main(void) {
    GuiMenu menu = {0};
    GuiMenu themed, ring;
    GuiBanner banner;
    GuiConfirm confirm = {0};
    gui_menu_key(&menu, GUI_LEFT);
    assert(menu.selected == GUI_EXIT);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_SWITCH && !menu.activated);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_OPTIONS && !menu.running);
    assert(menu.activated && menu.active == GUI_SWITCH && menu.focus_load && !menu.load_activated);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_DROP && menu.load_activated && menu.load_active == GUI_LOAD_DRAG);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_OPTIONS);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_SWITCH && menu.load_selected == GUI_LOAD_DIRECTORY);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_GAMES);
    menu.view = GUI_LOAD_OPTIONS; /* Back from the browser */
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_3DS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.active == GUI_3DS && menu.view == GUI_LOAD_OPTIONS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_DROP && menu.active == GUI_3DS);
    gui_menu_key(&menu, GUI_CANCEL);
    gui_menu_key(&menu, GUI_CANCEL);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_KEYS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.view == GUI_LOAD_KEYS && !menu.running);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.selected == GUI_KEYS && menu.view == GUI_LOAD_KEYS);
    gui_menu_key(&menu, GUI_CANCEL);
    assert(menu.view == GUI_LOAD_NONE && !menu.quit);
    gui_menu_key(&menu, GUI_RIGHT);
    assert(menu.selected == GUI_FULLSCREEN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.fullscreen && menu.view == GUI_LOAD_NONE);
    gui_menu_key(&menu, GUI_SELECT);
    assert(!menu.fullscreen);
    menu.running = true;
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_FULLSCREEN);
    menu.running = false;
    /* Touch goes round fingers, the mouse too, nothing, and back */
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_TOUCH && menu.touch == GUI_TOUCH_FINGERS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.touch == GUI_TOUCH_MOUSE && !strcmp(gui_touch_option(&menu), "--touch=2"));
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.touch == GUI_TOUCH_OFF && !strcmp(gui_touch_option(&menu), "--touch=0"));
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.touch == GUI_TOUCH_FINGERS && !strcmp(gui_touch_option(&menu), "--touch=1"));
    /* the on-screen controls, off and on: what the game gets */
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_CONTROLS && !menu.controls && !strcmp(gui_controls_option(&menu), "--controls=0"));
    assert(!strcmp(gui_setting_description(&menu), "A pad on the screen. The game's menu edits it"));
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.controls && !strcmp(gui_controls_option(&menu), "--controls=1") && menu.view == GUI_LOAD_NONE);
    gui_menu_key(&menu, GUI_SELECT);
    assert(!menu.controls);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_GCCJIT && menu.active == GUI_CONTROLS);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.active == GUI_GCCJIT);
    assert(menu.gccjit == !!SNX_GUI_HAS_GCCJIT && menu.view == GUI_LOAD_NONE);
    assert(!strcmp(gui_setting_description(&menu), SNX_GUI_HAS_GCCJIT ? "libgccjit will be used instead of llvm"
                                                                      : "Program was not compiled with libgccjit"));
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_GDBSTUB);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.gdbstub && menu.view == GUI_LOAD_NONE);
    gui_menu_key(&menu, GUI_SELECT);
    assert(!menu.gdbstub);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.selected == GUI_THEME && menu.theme_next && !menu.quit);
    menu.theme_next = false; /* the GUI's to clear */
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.selected == GUI_THEME_EDIT && menu.theme_edit && !menu.theme_next && menu.view == GUI_LOAD_NONE);
    menu.theme_edit = 0; /* the GUI's to clear */
    /* Text size goes round 1x to the largest, then back to fitting the screen */
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_TEXT && !menu.text_size);
    for (int size = 1; size <= GUI_TEXT_MAX; ++size) {
        gui_menu_key(&menu, GUI_SELECT);
        assert(menu.text_size == size);
    }
    gui_menu_key(&menu, GUI_SELECT);
    assert(!menu.text_size && !menu.quit);
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.selected == GUI_LOG && menu.share_log && !menu.quit);
    menu.share_log = 0; /* the GUI's to clear */
    gui_menu_key(&menu, GUI_DOWN);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.quit);

    menu = (GuiMenu){0};
    gui_menu_key(&menu, GUI_SELECT);
    menu.selected = GUI_LINUX; /* Mouse focus outside the open submenu. */
    menu.focus_load = false;
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_3DS && menu.active == GUI_SWITCH && menu.view == GUI_LOAD_OPTIONS);
    gui_menu_key(&menu, GUI_DOWN);
    assert(menu.selected == GUI_KEYS && menu.active == GUI_SWITCH);
    gui_menu_key(&menu, GUI_SELECT);
    assert(menu.active == GUI_KEYS && menu.view == GUI_LOAD_KEYS);

    for (int bits = 0; bits < 48; ++bits) {
        static const char* const touch[] = {"--touch=1", "--touch=2", "--touch=0"};
        menu = (GuiMenu){.selected = GUI_SWITCH,
                         .fullscreen = !!(bits & 1),
                         .gccjit = !!(bits & 2),
                         .gdbstub = !!(bits & 4),
                         .controls = !!(bits & 8),
                         .touch = bits >> 4};
        for (int prepared = 0; prepared < 2; ++prepared) {
            const char* args[GUI_LAUNCH_MAX];
            size_t n = gui_launch_options(&menu, prepared, "data folder", args), i = 0;
            assert(n <= GUI_LAUNCH_MAX && (bits != 47 || prepared || n == 7 + !!SNX_GUI_HAS_GCCJIT)); /* all of them */
            assert(i < n && !strcmp(args[i++], menu.fullscreen ? "-f" : "--fullscreen=0"));
            assert(i < n && !strcmp(args[i++], touch[menu.touch]));
            assert(i < n && !strcmp(args[i++], menu.controls ? "--controls=1" : "--controls=0"));
            if (menu.gccjit && SNX_GUI_HAS_GCCJIT && !prepared) assert(i < n && !strcmp(args[i++], "--backend=gcc"));
            if (menu.gdbstub) {
                assert(i < n && !strcmp(args[i++], "-S"));
                assert(i < n && !strcmp(args[i++], "-s"));
            }
            /* the driver works in the menu's data folder; a prepared game runs no driver */
            if (!prepared) {
                assert(i < n && !strcmp(args[i++], "--repo"));
                assert(i < n && !strcmp(args[i++], "data folder"));
            }
            assert(i == n);
            assert(gui_launch_options(&menu, prepared, NULL, args) == n - (prepared ? 0 : 2));
        }
        menu.selected = GUI_LINUX;
        const char* args[GUI_LAUNCH_MAX];
        assert(gui_launch_options(&menu, false, "data folder", args) == 0);
        menu.selected = GUI_3DS;
        assert(gui_launch_options(&menu, false, "data folder", args) == 0);
        assert(gui_launch_options(&menu, true, "data folder", args) == 0);
    }

    /* the footer's use, as a player reads it: MB under a GB, the CPU a share of every core, never over 100% */
    {
        char use[128];
        gui_usage_text(use, sizeof use, true, 812ull << 20, 52, 4);
        assert(!strcmp(use, "GUI + children: RAM 812 MB  CPU 13% (0.5 cores)"));
        gui_usage_text(use, sizeof use, true, 1404ull << 20, 216, 8); /* a game compiling on 8 cores */
        assert(!strcmp(use, "GUI + children: RAM 1.4 GB  CPU 27% (2.2 cores)"));
        gui_usage_text(use, sizeof use, true, 1, 830, 8);
        assert(!strcmp(use, "GUI + children: RAM 0 MB  CPU 100% (8.0 cores)"));
        gui_usage_text(use, sizeof use, true, 0, 150, 0); /* SDL knows no cores: one */
        assert(!strcmp(use, "GUI + children: RAM 0 MB  CPU 100% (1.0 cores)"));
        gui_usage_text(use, sizeof use, false, 1, 1, 1);
        assert(!strcmp(use, "GUI + children: resource usage unavailable"));
    }
    /* a line 5 columns too long, a column every 100 ms: rests 8 steps, slides, rests at its end, slides back */
    assert(!gui_slide(0, 900, 100) && !gui_slide(5, 900, 0) && !gui_slide(-3, 900, 100));
    assert(!gui_slide(5, 0, 100) && !gui_slide(5, 799, 100) && gui_slide(5, 900, 100) == 1);
    assert(gui_slide(5, 1300, 100) == 5 && gui_slide(5, 2000, 100) == 5 && gui_slide(5, 2200, 100) == 4);
    assert(gui_slide(5, 2500, 100) == 1 && !gui_slide(5, 2600, 100) && gui_slide(5, 2600 + 900, 100) == 1);

    GuiBrowser browser = {.count = 23};
    gui_browser_resize(&browser, 10);
    assert(gui_browser_pages(&browser) == 3 && gui_browser_items(&browser) == 10);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_BACK);
    gui_browser_key(&browser, GUI_UP);
    assert(browser.selected == 10 && !browser.activated);
    gui_browser_key(&browser, GUI_RIGHT);
    assert(browser.page == 1 && browser.selected == 10);
    gui_browser_key(&browser, GUI_PAGE_DOWN);
    assert(browser.page == 2 && browser.selected == 3 && gui_browser_items(&browser) == 3);
    gui_browser_key(&browser, GUI_RIGHT); /* no wrapping past the last page */
    assert(browser.page == 2);
    assert(gui_browser_key(&browser, GUI_SELECT) == GUI_BROWSER_OPEN);
    assert(browser.activated && browser.active == 22);
    gui_browser_resize(&browser, 7);
    assert(browser.page * browser.page_size + browser.selected - 1 == 22);
    gui_browser_key(&browser, GUI_PAGE_UP);
    gui_browser_key(&browser, GUI_LEFT);
    assert(browser.page == 1);
    assert(gui_browser_key(&browser, GUI_CANCEL) == GUI_BROWSER_BACK);
    browser = (GuiBrowser){0};
    gui_browser_resize(&browser, 0);
    gui_browser_key(&browser, GUI_DOWN);
    gui_browser_key(&browser, GUI_PAGE_DOWN);
    assert(browser.page == 0 && browser.selected == 0 && gui_browser_pages(&browser) == 1);

    GuiConsole screen;
    gui_console_init(&screen, 5, 3);
    gui_console_feed(&screen, "abcdefghijklmnoP", 16);
    assert(!memcmp(screen.cells[0], "fghij", 5));
    assert(!memcmp(screen.cells[1], "klmno", 5));
    assert(screen.cells[2][0] == 'P');
    gui_console_feed(&screen, "\rX\n", 3);
    assert(screen.cells[1][0] == 'X' && screen.col == 0);
    gui_console_init(&screen, 10, 2);
    gui_console_feed(&screen, "\033[3", 3);
    gui_console_feed(&screen, "1mred\033[0m\033]0;title\007!", strlen("1mred\033[0m\033]0;title\007!"));
    assert(!memcmp(screen.cells[0], "red!", 4));
    gui_console_feed(&screen, "\xc3", 1);
    gui_console_feed(&screen, "\xa9", 1);
    assert(screen.cells[0][4] == gui_glyph(0xe9));
    for (int i = 0; i < 1000000; ++i) gui_console_feed(&screen, "z", 1);
    assert(screen.row < screen.rows && screen.col <= screen.cols);
    gui_console_resize(&screen, 3, 1);
    assert(screen.row == 0 && screen.cols == 3 && screen.col <= 3);
    gui_console_resize(&screen, 9999, 9999);
    assert(screen.rows == GUI_MAX_ROWS && screen.cols == GUI_MAX_COLS);

    char* path = gui_drop_path("  \"/tmp/a b;$(not-a-shell).nro\"\r\n", true);
    assert(path && !strcmp(path, "/tmp/a b;$(not-a-shell).nro"));
    free(path);
    path = gui_drop_path("file://localhost/tmp/a%20b.nro", true);
    assert(path && !strcmp(path, "/tmp/a b.nro"));
    free(path);
    path = gui_drop_path(" /a real trailing space ", false);
    assert(path && !strcmp(path, " /a real trailing space "));
    free(path);
    assert(!gui_drop_path("file://remote/tmp/a", true));
    assert(!gui_drop_path("file:///tmp/%00.nro", true));
    assert(!gui_drop_path("file:///tmp/%zz", true));
    assert(!gui_drop_path("one\ntwo", true));
    assert(!gui_drop_path("", true));
    /* what a picker gives: the Android one's documents of the device's storage and its cards, a download by path; no
       other of its documents has a path */
    path =
        gui_document_path("content://com.android.externalstorage.documents/document/primary%3ADownload%2FGame%20X.xci");
    assert(path && !strcmp(path, "/storage/emulated/0/Download/Game X.xci"));
    free(path);
    path = gui_document_path("content://com.android.externalstorage.documents/document/1A2B-3C4D%3Agames%2Fa.nsp");
    assert(path && !strcmp(path, "/storage/1A2B-3C4D/games/a.nsp"));
    free(path);
    path = gui_document_path("content://com.android.providers.downloads.documents/document/"
                             "raw%3A%2Fstorage%2Femulated%2F0%2FDownload%2Fprod.keys");
    assert(path && !strcmp(path, "/storage/emulated/0/Download/prod.keys"));
    free(path);
    assert(!gui_document_path("content://com.android.providers.downloads.documents/document/msf%3A1234"));
    assert(!gui_document_path("content://com.android.providers.media.documents/document/document%3A12"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary%3A%zz"));
    assert(!gui_document_path("content://com.android.externalstorage.documents/document/primary"));
    path = gui_document_path("file:///tmp/a%20b.nro");
    assert(path && !strcmp(path, "/tmp/a b.nro"));
    free(path);
    path = gui_document_path("/tmp/ a.nro ");
    assert(path && !strcmp(path, "/tmp/ a.nro "));
    free(path);
    /* the dumps the Android games/ list offers, to prepare first */
    assert(gui_is_dump("a.NRO") && gui_is_dump("b.xci") && gui_is_dump("c.nsp") && gui_is_dump("d.dxci"));
    assert(!gui_is_dump("e_code.so") && !gui_is_dump("nro") && !gui_is_dump("f.nro.ini"));
    path = gui_document_path("C:\\Games\\a.nro"); /* Windows' */
    assert(path && !strcmp(path, "C:\\Games\\a.nro"));
    free(path);
    /* the theme item asks the GUI for the next theme */
    memset(&themed, 0, sizeof themed);
    themed.selected = GUI_THEME;
    gui_menu_key(&themed, GUI_SELECT);
    assert(themed.theme_next && themed.view == GUI_LOAD_NONE && !themed.quit);
    assert(strstr(gui_setting_description(&themed), "themes/"));

    /* two recent games and the items make one ring; selecting a game asks the GUI to run it, closing a submenu */
    memset(&ring, 0, sizeof ring);
    ring.recent = 2;
    gui_menu_key(&ring, GUI_UP);
    assert(ring.recent_focus == 2 && ring.selected == GUI_SWITCH);
    gui_menu_key(&ring, GUI_LEFT);
    assert(ring.recent_focus == 1);
    gui_menu_key(&ring, GUI_UP);
    assert(!ring.recent_focus && ring.selected == GUI_EXIT);
    gui_menu_key(&ring, GUI_DOWN);
    assert(ring.recent_focus == 1);
    gui_menu_key(&ring, GUI_RIGHT);
    assert(ring.recent_focus == 2);
    gui_menu_key(&ring, GUI_DOWN);
    assert(!ring.recent_focus && ring.selected == GUI_SWITCH);
    ring.recent_focus = 2;
    ring.view = GUI_LOAD_OPTIONS;
    gui_menu_key(&ring, GUI_SELECT);
    assert(ring.recent_open == 2 && ring.view == GUI_LOAD_NONE && !ring.activated && !ring.focus_load);
    ring.recent = 0;
    ring.recent_focus = 0;
    ring.selected = GUI_SWITCH;
    gui_menu_key(&ring, GUI_UP); /* none: the items alone, as before */
    assert(ring.selected == GUI_EXIT && !ring.recent_focus);

    /* the question before an abort: keys do nothing while it is not shown; the arrows move between its choices, Select
       takes the one in focus and Cancel keeps going, both closing it */
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !gui_confirm_key(&confirm, GUI_DOWN));
    assert(!confirm.shown && confirm.selected == GUI_CONFIRM_KEEP);
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown); /* Keep going is the first choice */
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_DOWN) && confirm.selected == GUI_CONFIRM_ABORT && confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_PAGE_DOWN) && confirm.selected == GUI_CONFIRM_ABORT && confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_CANCEL) && !confirm.shown); /* Back again, on Abort too, keeps going */
    confirm.shown = true;
    assert(!gui_confirm_key(&confirm, GUI_UP) && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_RIGHT) && confirm.selected == GUI_CONFIRM_ABORT);
    assert(!gui_confirm_key(&confirm, GUI_LEFT) && confirm.selected == GUI_CONFIRM_KEEP);
    assert(!gui_confirm_key(&confirm, GUI_UP) && confirm.selected == GUI_CONFIRM_ABORT);
    assert(gui_confirm_key(&confirm, GUI_SELECT) && !confirm.shown);
    assert(!gui_confirm_key(&confirm, GUI_SELECT)); /* answered: nothing more to abort */

    /* the runner's banner, at the start of a line, split across reads or not */
    gui_banner_reset(&banner);
    assert(!feed(&banner, "Recompiling x\nnot ScarletNX - x\nScarletNX"));
    assert(feed(&banner, " - x\n") && banner.seen);
    gui_banner_reset(&banner);
    assert(feed(&banner, "ScarletNX - first line"));
    gui_banner_reset(&banner);
    assert(!feed(&banner, "ScarletNX -x\n\nScarletNX  - x") && !banner.seen);
    assert(feed(&banner, "\n\nScarletNX - y"));
    puts("GUI menu, bounded console, UTF-8 and file drops passed");
    return 0;
}
