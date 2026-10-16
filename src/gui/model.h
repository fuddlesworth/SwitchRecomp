/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_MODEL_H
#define SNX_GUI_MODEL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "font.h" /* gui_glyph */

#ifndef SNX_GUI_HAS_GCCJIT
#define SNX_GUI_HAS_GCCJIT 0
#endif

enum {
    GUI_SWITCH,
    GUI_LINUX,
    GUI_3DS,
    GUI_KEYS,
    GUI_FULLSCREEN,
    GUI_TOUCH,    /* what the game's touch screen takes */
    GUI_CONTROLS, /* the on-screen controls over the game */
    GUI_GCCJIT,
    GUI_GDBSTUB,
    GUI_THEME,
    GUI_THEME_EDIT, /* the theme editor: its keys, a preview, and Save */
    GUI_TEXT,       /* the menu's letters */
    GUI_LOG,        /* the logs in one file, for a bug report: a phone has no terminal to copy them from */
    GUI_EXIT,
    GUI_ITEM_COUNT
};
typedef enum { GUI_UP, GUI_DOWN, GUI_LEFT, GUI_RIGHT, GUI_SELECT, GUI_CANCEL, GUI_PAGE_UP, GUI_PAGE_DOWN } GuiKey;
#define GUI_TEXT_MAX 4
enum { GUI_TOUCH_FINGERS, GUI_TOUCH_MOUSE, GUI_TOUCH_OFF }; /* GuiMenu.touch: fingers, the mouse too, nothing */
typedef enum { GUI_LOAD_NONE, GUI_LOAD_OPTIONS, GUI_LOAD_DROP, GUI_LOAD_GAMES, GUI_LOAD_KEYS } GuiLoadView;
enum { GUI_LOAD_DRAG, GUI_LOAD_DIRECTORY };
typedef struct {
    int selected, load_selected; /* keyboard/mouse focus, not activation */
    int active, load_active;     /* last explicitly activated items; active owns the submenu */
    bool activated, load_activated, focus_load;
    GuiLoadView view;
    bool running, fullscreen, gccjit, gdbstub, quit;
    bool controls;  /* the on-screen controls, on by default on Android */
    int touch;      /* GUI_TOUCH_*, fingers the default */
    int text_size;  /* the font's scale, 1 to GUI_TEXT_MAX; 0 fits the screen */
    int theme_next; /* the GUI moves to the next theme and clears it */
    int theme_edit; /* the GUI opens the theme editor and clears it */
    int share_log;  /* the GUI shares the logs and clears it */
    /* the recent games above the items: how many, the focused one + 1 (0: an item has the focus), and the one
       to run + 1, which the GUI runs and clears */
    int recent, recent_focus, recent_open;
} GuiMenu;
void gui_menu_key(GuiMenu* menu, GuiKey key);
/* The question a running run's abort asks first, or leaving the menu on a computer: shown or not, which one, and the
   choice in focus, the one that stays first */
enum { GUI_CONFIRM_KEEP, GUI_CONFIRM_ABORT };
typedef struct {
    bool shown;
    bool exit; /* leaving the app, not aborting a run */
    int selected;
} GuiConfirm;
/* A key on the question: the arrows move between its choices, Select takes the one in focus and Cancel keeps going,
   both closing it. True when the run is to be aborted */
bool gui_confirm_key(GuiConfirm* confirm, GuiKey key);
const char* gui_setting_description(const GuiMenu* menu);
/* Append up to GUI_LAUNCH_MAX Switch options; prepared games do not run a compiler. repo, when not NULL, is the folder
   the driver keeps work/ and games/ in: the menu's own, so the two list the same games */
#define GUI_LAUNCH_MAX 8
size_t gui_launch_options(const GuiMenu* menu, bool prepared, const char* repo, const char** argv);
/* the game's --touch=N and --controls=N for the menu's choices */
const char* gui_touch_option(const GuiMenu* menu);
const char* gui_controls_option(const GuiMenu* menu);
/* the footer's line: RAM in MB or GB, the CPU as a share of all cores and the cores in use (cpu_percent counts one
   core as 100%) */
void gui_usage_text(char* out, size_t size, bool available, uint64_t memory_bytes, double cpu_percent, int cores);
/* how many columns a line over columns too many has slid at ms, a column every step_ms: it waits at each end */
int gui_slide(int over, uint64_t ms, uint32_t step_ms);

/* The runner's banner, "ScarletNX - <name>" at the start of a line: the recompile is over, the game is loading */
typedef struct {
    size_t matched;
    int seen;
} GuiBanner;
void gui_banner_reset(GuiBanner* banner);
int gui_banner_feed(GuiBanner* banner, const char* data, size_t size);

typedef struct {
    size_t count, page, page_size, selected; /* focus: selected == 0 is Back on every page */
    size_t active;                           /* absolute file index, preserved across page changes and resizing */
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
/* SDL_DROP_FILE is already a path. DROP_TEXT/clipboard may contain a quoted path
   or a local file:// URI. Returns owned UTF-8, never shell-expanded. */
char* gui_drop_path(const char* text, bool text_drop);
/* The path of a file a picker chose: on Android a document of the device's storage, or a download it names by path,
   NULL for any other (an app's, a cloud's); elsewhere the path or file:// URI it gave. Owned. */
char* gui_document_path(const char* uri);
#endif
