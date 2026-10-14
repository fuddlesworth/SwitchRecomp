/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_MODEL_H
#define SNX_GUI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SNX_GUI_HAS_GCCJIT
#define SNX_GUI_HAS_GCCJIT 0
#endif

enum { GUI_SWITCH, GUI_LINUX, GUI_KEYS, GUI_FULLSCREEN, GUI_GCCJIT, GUI_GDBSTUB, GUI_THEME, GUI_EXIT, GUI_ITEM_COUNT };
typedef enum { GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_SELECT, GUI_CANCEL, GUI_PAGE_UP, GUI_PAGE_DOWN } GuiKey;
typedef enum { GUI_LOAD_NONE, GUI_LOAD_OPTIONS, GUI_LOAD_DROP, GUI_LOAD_GAMES, GUI_LOAD_KEYS } GuiLoadView;
enum { GUI_LOAD_DRAG, GUI_LOAD_DIRECTORY };
typedef struct {
    int selected, load_selected; /* keyboard/mouse focus, not activation */
    int active, load_active;     /* last explicitly activated items; active owns the submenu */
    bool activated, load_activated, focus_load;
    GuiLoadView view;
    bool running, fullscreen, gccjit, gdbstub, quit;
    int theme_next; /* the GUI moves to the next theme and clears it */
    /* the recent games above the items: how many, the focused one + 1 (0: an item has the focus), and the one
       to run + 1, which the GUI runs and clears */
    int recent, recent_focus, recent_open;
} GuiMenu;
void gui_menu_key(GuiMenu* menu, GuiKey key);
const char* gui_setting_description(const GuiMenu* menu);
/* Append up to four Switch options; prepared games do not run a compiler. */
size_t gui_launch_options(const GuiMenu* menu, bool prepared, const char** argv);

/* The runner's banner, "ScarletNX - <name>" at the start of a line: the recompile is over, the game is loading */
typedef struct {
    size_t matched;
    int seen;
} GuiBanner;
void gui_banner_reset(GuiBanner* banner);
int gui_banner_feed(GuiBanner* banner, const char* data, size_t size);

typedef struct {
    size_t count, page, page_size, selected; /* focus: selected == 0 is Back on every page */
    size_t active; /* absolute file index, preserved across page changes and resizing */
    bool activated;
} GuiBrowser;
typedef enum { GUI_BROWSER_NONE, GUI_BROWSER_BACK, GUI_BROWSER_OPEN } GuiBrowserAction;
size_t gui_browser_pages(const GuiBrowser* browser);
size_t gui_browser_items(const GuiBrowser* browser);
void gui_browser_resize(GuiBrowser* browser, size_t page_size);
GuiBrowserAction gui_browser_key(GuiBrowser* browser, GuiKey key);

#define GUI_MAX_COLS 240
#define GUI_MAX_ROWS 100
typedef struct {
    unsigned char cells[GUI_MAX_ROWS][GUI_MAX_COLS];
    int cols, rows, row, col, escape, utf8_left;
    uint32_t codepoint, utf8_min;
} GuiConsole;
void gui_console_init(GuiConsole* console, int cols, int rows);
void gui_console_resize(GuiConsole* console, int cols, int rows);
void gui_console_feed(GuiConsole* console, const char* data, size_t size);
/* The VGA BIOS font uses CP437. Unrepresentable Unicode becomes '?'. */
unsigned char gui_glyph(uint32_t codepoint);
/* SDL_DROP_FILE is already a path. DROP_TEXT/clipboard may contain a quoted path
   or a local file:// URI. Returns owned UTF-8, never shell-expanded. */
char* gui_drop_path(const char* text, bool text_drop);
/* The path of a file a picker chose: on Android a document of the device's storage, or a download it names by path,
   NULL for any other (an app's, a cloud's); elsewhere the path or file:// URI it gave. Owned. */
char* gui_document_path(const char* uri);
#endif
