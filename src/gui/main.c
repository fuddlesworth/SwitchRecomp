/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#include "common/utilities/ini_lines.h"
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
#include "common/utilities/common.h"
#include "gui/browser.h"
#include "gui/editor.h"
#include "common/utilities/font.h"
#include "gui/gui.h"
#include "gui/model.h"
#include "gui/place.h"
#include "gui/gui_process.h"
#include "gui/steps.h"
#include "gui/theme.h"
#include "common/utilities/window_icon.h"
#include "common/utilities/build_info.h"
#include "gui/recent.h"
#include "gui/clean.h"
#include "gui/settings.h"
#include "gui/splash.h"
#include "common/utilities/pad_sdl.h"
#include "common/utilities/pad_screen.h"
#ifdef SDL_PLATFORM_ANDROID
#include "gui/android.h"
#endif
#ifdef SDL_PLATFORM_APPLE
#include "gui/apple.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
/* the checkout the drivers were built from and its build tree, where an older version kept games/ and work/ */
#ifdef SNX_SOURCE_DIR
#define GUI_CHECKOUT SNX_SOURCE_DIR
#define GUI_BUILD    SNX_BUILD_DIR
#else
#define GUI_CHECKOUT NULL
#define GUI_BUILD    NULL
#endif
/* the space folders take, counted on a thread of its own: they may hold many files, on a slow card. The thread's until
   done is set; counted says how many have their bytes. One block: this, each one's bytes, its path's pointer, the
   paths */
#define GUI_SPACE_DIRS 7 /* the data folder's games/, work/, themes/; the XDG folders or the menu's; a picked work/ */
typedef struct {
    int count, counted, done, stop;
    bool games;    /* a file is a game an older version laid out beside the others: its _code.so, _data/... count */
    uint64_t used; /* all of them */
    uint64_t* bytes;
    char** dirs; /* "" none */
} GuiSpace;
/* the editor's screens: the Settings' groups, a group's settings, the App's rows, its game folders, the theme editor,
   the Controllers' button map, the pad drawn (pad_screen.h), and the App's folders of data, games and extractions */
enum {
    GUI_FORM_SETTINGS,
    GUI_FORM_GROUP,
    GUI_FORM_APP,
    GUI_FORM_FOLDERS,
    GUI_FORM_THEME,
    GUI_FORM_PAD,
    GUI_FORM_PLACES
};
/* Settings > App > Folders' rows, in the order of their keys in app.ini's [folders] */
enum { GUI_PLACE_DATA, GUI_PLACE_GAMES, GUI_PLACE_WORK, GUI_PLACES };
#define GUI_GROUPS_MAX   32
#define GUI_FOLDERS_MAX  16    /* the player's game folders, past games/ */
#define GUI_FOLDER_DEPTH 4     /* how deep the games of a folder kept with its subfolders are looked for */
#define GUI_FOLDER_DEEP  "/**" /* after such a folder's path, in the file of folders */
#define GUI_VIEW_ROWS    24    /* a group's rows shown as a view of them */
#define SNX_GUI_PLAYERS  8     /* Controllers' players, as the runtime's gamepad_p<N>_ keys */
/* the App screen's values, as its rows show them, and the game folders' (games/ first) */
typedef struct {
    char theme[64], text[16], folders[32], places[32];
    char folder[GUI_FOLDERS_MAX + 1][16];
    char place[GUI_PLACES][64];
} GuiAppForm;
typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* font;
    GuiProcess* process;
    GuiMenu menu;
    GuiBrowser browser;
    GuiGames games;
    GuiConsole console;
    GuiUsage usage;
    GuiTheme theme;
    GuiThemes themes;
    GuiSteps steps;
    GuiRecent recent;
    GuiSettings settings; /* read the first time a run or the Settings need them */
    char* settings_file;  /* the picks kept for every game (settings_path); NULL keeps none */
    char* app_file;       /* Settings > App's picks (app.ini), beside it; NULL keeps none */
    GuiSpace* space;      /* ScarletNX's folders, counted when the Settings open; its thread counter until joined */
    SDL_Thread* counter;
    GuiSpace* left_space; /* what older builds left, each one's, for their box */
    SDL_Thread* left_counter;
    uint64_t free_bytes; /* on the data folder's disk, when the count started */
    char* left;          /* what older builds left where this one no longer looks, a line each, lefts of them */
    int lefts;
    bool left_shown;   /* in a box as the menu opens, until a key, a button or a finger closes it */
    Uint64 left_at;    /* when they were looked for */
    char* recent_file; /* where the recent games are kept; NULL keeps none */
    char* run_log;     /* where a run's output goes as it comes, the last run's; NULL keeps none */
    FILE* run_out;
    int recent_named;           /* the run's own name went to its recent game */
    bool recent_unsaved;        /* the list could not be written the last time it changed: the message says so */
    Uint64 clock;               /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    char* self;    /* the program that runs Switch inputs: the runtime the menu is part of, NULL to look for one */
    char* drivers; /* the folder the other drivers are looked for in first */
    char *cwd, *file, *games_dir, *keys_path;
    char* data_dir;       /* games/, work/ and themes/: the driver's too, which gets it as --repo (snx_data_dir) */
    char* checkout;       /* a build tree's menu's checkout (snx_checkout), where an older version kept games/ */
    char* themes_dirs[2]; /* themes/ in the data folder, then beside the GUI */
    char message[512];
    Uint64 keys_done; /* the keys screen's outcome shown since, 0 for none: then the menu by itself */
    bool keys_ok;
    int cols, rows, scale;
    int auto_scale; /* Text size Auto's scale, chosen when the window opens: a resize keeps the letters as they were */
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
    GuiConfirm confirm; /* the question before a run is aborted, when ask_abort says to ask it */
    bool ask_abort;     /* ESC asks first: on a phone it is Back, an edge swipe easily made by mistake */
    int native;
    bool recompile;           /* R on Games: the run recompiles its game (-r) and plays nothing */
    Uint64 finger;            /* the finger on the screen (finger_id), 0 for none */
    float finger_x, finger_y; /* where it went down, 0 to 1 across the window */
    float finger_mark;        /* the height a drag last moved the focus at, a row at a time */
    bool pen_held;            /* a pen lifted: its hover leaves the focus where the lift put it until it moves away */
    float pen_x, pen_y;       /* where it lifted, 0 to 1 across the window */
    bool hovering;            /* a mouse or a pen over the window, at hover_x, hover_y of the render: a key of the foot
                                 under it that a click would press is drawn lit */
    float hover_x, hover_y;
    Uint32 picked;                    /* the event a file from the system's picker comes back as */
    SDL_Texture* splash;              /* the boot's splash, its drawing splash_side pixels square, until splash_until */
    const SnxSplashSvg* splash_svg;   /* the boot's pick: Wimsy, or one of dist/splash's */
    SnxSplashDrawing* splash_drawing; /* it, read once; NULL: the icon shows */
    int splash_side;
    Uint64 splash_until;
    GuiRgb splash_page;
    bool editing; /* the editor's screen (form says which) instead of the menu's */
    GuiEditor editor;
    /* a press the colour picker took, until it lifts: its bar's channel + 1, or GUI_PICK_ROWS + 1 off the bars */
    int pick_press;
    int form;        /* GUI_FORM_* */
    int group;       /* the group open, or the one the Settings come back to */
    int first, page; /* a group's: the settings' row of its first, and of the page's row it opened (-1 none) */
    /* a group shown as a view of its rows, Controllers by player: each row's setting, and the player shown (0 every
       player's, 1 to SNX_GUI_PLAYERS) */
    GuiThemeKey view_keys[GUI_VIEW_ROWS];
    int view_rows[GUI_VIEW_ROWS], view_count, player;
    GuiThemeKey peek_keys[GUI_VIEW_ROWS]; /* and the focused group's, in the groups' screen */
    int peek_rows[GUI_VIEW_ROWS];
    int pad_map, pad_swap, pad_player; /* the button map's settings: its map's row and its swap's, for which player */
    int device_listen; /* a player's Controller row waiting for a press on the controller it takes, + 1; 0 for none */
    SDL_Gamepad* listened[16]; /* the controllers open meanwhile: a press comes only from an open one */
    int listened_count;
    bool theme_from_app;       /* the theme editor goes back to the App's rows */
    SnxPadScreen pad;          /* the button map's, on the row gamepad_map, A and B as gamepad_swap_ab says */
    SnxPadDrawing pad_drawing; /* as last drawn: what a click or a tap lands on */
    SDL_Gamepad* gamepad;      /* the pad it shows, the first one plugged in, while it is open */
    bool pads_read;            /* SDL's gamepads started for it */
    unsigned pad_pushed;       /* the triggers and the left stick's directions past half way, a bit each */
    /* the Settings' groups: App, then a group per section of the settings, its first row and rows; a row each */
    int groups, group_first[GUI_GROUPS_MAX], group_rows[GUI_GROUPS_MAX];
    GuiThemeKey group_keys[GUI_GROUPS_MAX];
    char group_about[GUI_GROUPS_MAX][128];
    GuiAppForm app_form;
    bool clean_armed;   /* App's Clean old settings selected once, saying what it would do: again does it */
    GuiCleaned cleaned; /* what it did, in a box over the screen while cleaned_shown, until a key closes it */
    bool cleaned_shown;
    char* folders[GUI_FOLDERS_MAX];     /* the player's game folders, listed after games/ */
    bool folders_deep[GUI_FOLDERS_MAX]; /* each with its subfolders' games too */
    int folder_count;
    char* folder_asked; /* a folder picked with folders in it, asked whether their games are listed too */
    char* folders_file; /* where they are kept; NULL keeps none */
    GuiThemeKey folder_keys[GUI_FOLDERS_MAX + 1];
    int folder_armed;       /* a folder's row Select was pressed on once, + 1: again takes it off */
    uint64_t games_stamp;   /* what the game folders held when last counted (games_stamp) */
    Uint64 games_checked;   /* when they were last looked at */
    bool recent_all;        /* every recent game shows, not GUI_RECENT_SHOWN and a row for the rest */
    Uint32 picked_folder;   /* the event a folder from the system's picker comes back as */
    SDL_Texture* preview;   /* the editor's: the menu or a run drawn in its theme, at 1x */
    GuiSteps preview_steps; /* the run it shows, over again every GUI_PREVIEW_MS */
    GuiConsole preview_console;

    /* Settings > App > Folders */
    char* home_dir;          /* the data folder snx_data_dir picks: the menu's own files go by it, [folders] or not */
    char* place[GUI_PLACES]; /* app.ini's [folders]: data, games and work picked in Folders; NULL the default */
    char* work_dir;          /* the extractions: [folders] work, else the data folder's work/ in its cache */
    GuiThemeKey place_keys[GUI_PLACES];
    char place_about[GUI_PLACES][384];
    char place_line[GUI_PLACES][4200]; /* the folder in use, whole, under the rows */
    int place_row;                     /* the row the folder picker was opened for, -1 none */
    char* own_dir;      /* Android: the app's own storage; a folder outside it is asked about (GUI_ASK_OUTSIDE) */
    char* outside_path; /* the folder asked about, for the row outside_place */
    int outside_place;
    char* room_file; /* a run asked about (GUI_ASK_ROOM): its file, its load and whether it was prepared */
    int room_item;
    bool room_prepared, room_recompile;
    bool room_ok;       /* asked and answered: the run goes */
    char room_line[96]; /* the question: what it takes, what is free */
    uint64_t room_free; /* fixed free space for tests; 0 asks the disk */

    Uint64 preview_since;
#ifdef SDL_PLATFORM_ANDROID
    bool hidden;     /* behind other apps: nothing is drawn, a run is still read */
    bool play_later; /* a game made ready while hidden, which opens once the menu shows */
    char told[340];  /* the run's notification: its step's line, its percent, when; 0 before its first */
    int told_percent;
    Uint64 told_ms;
#endif
} App;

static const char* const items[] = {"Load Switch NSP/XCI/NRO",
                                    "Load Linux ARM ELF",
                                    "Load 3DS CIA",
                                    "Setup device keys",
                                    "Settings",
                                    "Share log",
#ifdef SDL_PLATFORM_ANDROID
                                    "Open data folder",
#endif
                                    "Exit"};
#define GUI_RECENT_SHOWN 3 /* the recent games Games shows before a row that shows the rest */
/* what the screens say to press: a phone has a finger, the back button and the system's file picker, nothing to drop;
   a computer has the picker too (on Linux the desktop's portal, else zenity), on a screen of its own as the keys' */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU       "Tap: select. Back: go back."
#define SAY_GAMES      "Tap: open. Swipe across: turn the page. Recompile: tap it. Back: go back."
#define SAY_KEYS_HOW   "Back: go back."
#define SAY_KEYS_WAIT  "Pick prod.keys"
#define SAY_PICK_BOX   "tap the box to open the file picker"
#define SAY_FILE_WAIT  "Pick a file"
#define SAY_DONE       "Tap to return to menu"
#define SAY_ABORT      "Tap here to abort and return to menu"
#define SAY_NO_GAMES   "No games in games/: put a game's .nro, .xci or .nsp there."
#define SAY_COUNT      "game(s)"
#define SAY_LOG_FAILED "The log could not be saved in the data folder."
#define SAY_EDITOR     "Tap a key to change, type or pick it. Back: go back."
#define SAY_PICKER     "Drag a bar, or tap #RRGGBB to type it. Tap outside: keep. Back: put back."
#define SAY_SETTINGS   SAY_EDITOR
#define SAY_GROUPS     "Tap a group to open it. Back: go back."
#define SAY_APP        "Tap a row to change or open it. Back: go back."
#define SAY_FOLDERS    "Add a folder: tap it. Tap a folder twice to take it off. Back: go back."
#define SAY_PLACES     "Tap a folder to pick another. Back: go back."
#define SAY_PAD        "Tap a control; tap it again for a press. Back: go back."
#else
#define SAY_MENU       "Arrows/mouse: move. Space/Enter/click: select."
#define SAY_GAMES      "Up/Down: select. Enter/Space: open. R: recompile. Left/Right/PgUp/PgDown: page."
#define SAY_PICK_BOX   "click the box (or Enter) to open the file picker"
#define SAY_KEYS_HOW   "Ctrl+V: paste a path, or drop the file on the window. ESC: back."
#define SAY_KEYS_WAIT  "Pick or drop prod.keys"
#define SAY_FILE_WAIT  "Pick or drop a file"
#define SAY_DONE       "Press [ESC] to return to menu"
#define SAY_ABORT      "Press [ESC] to abort and return to menu"
#define SAY_NO_GAMES   "No games in games/ or the folders of Settings > App."
#define SAY_COUNT      "game(s)"
#define SAY_LOG_FAILED "The log could not be saved: check the folder's permissions."
#define SAY_EDITOR     "Up/Down: key. Left/Right: change. Enter: edit. Ctrl+S: save. ESC: back."
#define SAY_PICKER     "Up/Down: row. Left/Right: 1. PgUp/PgDn: 16. Enter: keep. ESC: put back."
#define SAY_SETTINGS   "Up/Down: setting. Left/Right: change. Enter: type. Del: default. ESC: back."
#define SAY_GROUPS     "Up/Down: group. Enter: open. ESC: back."
#define SAY_APP        "Up/Down: row. Left/Right: change. Enter: change or open. ESC: back."
#define SAY_FOLDERS    "Add a folder: pick, drop or paste one. Del: take one off. ESC: back."
#define SAY_PLACES     "Up/Down: folder. Enter: pick another. Del: the default. ESC: back."
#define SAY_PAD        "Arrows/pad: move, change. Enter/A: select. Del: the .ini's. ESC: back."
#endif
static SDL_Color rgb(GuiRgb c) {
    SDL_Color out = {c.r, c.g, c.b, 255};
    return out;
}
static Uint64 now_ms(const App* app) { return app->clock ? app->clock : SDL_GetTicks(); }
#define GUI_KEYS_OK_MS     3000  /* how long the keys screen shows they were installed, then the menu */
#define GUI_KEYS_FAILED_MS 10000 /* and why they were not, the menu's line saying it after */
/* the keys screen's outcome, gone back to the menu once shown long enough; the seconds it still shows, 0 for none */
static int keys_left(App* app) {
    Uint64 shown = app->keys_ok ? GUI_KEYS_OK_MS : GUI_KEYS_FAILED_MS, now = now_ms(app);
    if (!app->keys_done || app->menu.view != GUI_LOAD_KEYS) {
        app->keys_done = 0;
        return 0;
    }
    if (now - app->keys_done >= shown) {
        app->keys_done = 0;
        app->menu.view = GUI_LOAD_NONE;
        return 0;
    }
    return (int)((shown - (now - app->keys_done) + 999) / 1000);
}
static void glyph(App* app, float x, float y, unsigned char c);
static SDL_Texture* create_font(SDL_Renderer* renderer) {
    SDL_Surface* surface = SDL_CreateSurface(128, 256, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return NULL;
    for (int glyph = 0; glyph < 256; ++glyph)
        for (int y = 0; y < 16; ++y) {
            Uint32* line = (Uint32*)((char*)surface->pixels + ((glyph / 16) * 16 + y) * surface->pitch);
            for (int x = 0; x < 8; ++x)
                line[(glyph % 16) * 8 + x] = (gui_vga_font[glyph * 16 + y] & (0x80 >> x)) ? 0xffffffffu : 0;
        }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }
    return texture;
}
/* the font's glyph c, its corner at x, y in pixels */
static void glyph(App* app, float x, float y, unsigned char c) {
    SDL_FRect source = {(float)(c % 16 * 8), (float)(c / 16 * 16), 8, 16};
    SDL_FRect destination = {x, y, 8.0f * (float)app->scale, 16.0f * (float)app->scale};
    SDL_RenderTexture(app->renderer, app->font, &source, &destination);
}
/* text from a pixel position, chars at most, alpha its opacity, cut at the last column; where it ends */
static float text_px(App* app, float x, float y, const char* value, size_t chars, SDL_Color color, float alpha) {
    size_t i;
    float w = 8.0f * (float)app->scale, limit = (float)(app->cols - 1) * w;
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(app->font, (Uint8)(alpha * 255));
    for (i = 0; *value && i < chars; ++i, x += w) {
        unsigned char c = gui_glyph(SDL_StepUTF8(&value, NULL));
        if (x + w <= limit) glyph(app, x, y, c);
    }
    SDL_SetTextureAlphaMod(app->font, 255);
    return x;
}
/* text at a cell, a line too long for its row sliding as the theme says */
static void text(App* app, int x, int y, const char* value, SDL_Color color) {
    int skip = gui_slide((int)SDL_utf8strlen(value) - (app->cols - 1 - x), now_ms(app), app->theme.scroll_ms);
    while (skip-- > 0 && *value) SDL_StepUTF8(&value, NULL);
    text_px(app, (float)(x * 8 * app->scale), (float)(y * 16 * app->scale), value, SIZE_MAX, color, 1);
}
static int cell_h(const App* app) { return 16 * app->scale; }
static int cell_w(const App* app) { return 8 * app->scale; }
static void prompt_in(App* app, GuiRect r, int y, GuiPromptLine line, const char* value, SDL_Color color);
/* a line of the loading screen that waits on the user, on row y as the theme's prompt places it */
static void prompt(App* app, GuiPromptLine line, int x, int y, const char* value, SDL_Color color) {
    GuiRect r = gui_rect(x * cell_w(app), y * cell_h(app), (app->cols - 1 - x) * cell_w(app), cell_h(app));
    prompt_in(app, r, r.y, line, value, color);
}
/* the part of the window no system bar or camera covers, in the renderer's pixels: everything is drawn in it */
static void set_area(App* app) {
    SDL_Rect r;
    int ww, wh, ow, oh;
    SDL_GetWindowSize(app->window, &ww, &wh);
    SDL_GetRenderOutputSize(app->renderer, &ow, &oh);
    if (ww > 0 && wh > 0 && SDL_GetWindowSafeArea(app->window, &r)) {
        r.x = r.x * ow / ww;
        r.w = r.w * ow / ww;
        r.y = r.y * oh / wh;
        r.h = r.h * oh / wh;
        SDL_SetRenderViewport(app->renderer, &r);
    } else
        SDL_SetRenderViewport(app->renderer, NULL);
}
static void fill(App* app, float x, float y, float w, float h, GuiRgb c, Uint8 alpha) {
    SDL_FRect r = {x, y, w, h};
    SDL_SetRenderDrawBlendMode(app->renderer, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, alpha);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
static void area(const App* app, int* width, int* height) {
    SDL_Rect r;
    SDL_GetRenderViewport(app->renderer, &r);
    *width = r.w;
    *height = r.h;
}
/* rows of the whole width, as a band behind text: on to the window's edges, over the camera's cutout beside the
   text and what is left below the last row */
static void band(App* app, int row, int rows, GuiRgb c) {
    SDL_Rect safe;
    int width, height, bottom = row + rows == app->rows && app->rows < GUI_MAX_ROWS;
    float top;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    top = row ? (float)(safe.y + row * 16 * app->scale) : 0;
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, top, (float)width, (bottom ? (float)height : (float)(safe.y + (row + rows) * 16 * app->scale)) - top,
         c, 255);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* a line between two rows, across the text area */
static void rule(App* app, int row, GuiRgb c) {
    float s = (float)app->scale;
    fill(app, 8 * s, (float)(row * 16) * s - s, (float)((app->cols - 2) * 8) * s, s, c, 255);
}
static void event(App* app, const SDL_Event* e);
/* ~/.switch/prod.keys (%USERPROFILE%\.switch on Windows), where hactool and other tools keep them */
static char* home_keys_path(void) {
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home || !home[0]) home = getenv("HOME");
#else
    const char* home = getenv("HOME");
#endif
    if (!home || !home[0]) return NULL;
    char* directory = snx_path_join(home, ".switch");
    char* path = directory ? snx_path_join(directory, "prod.keys") : NULL;
    free(directory);
    return path;
}
#ifdef SNX_XDG
/* the menu whose folder is the user's data folder (snx_data_dir), not a portable copy's */
static bool installed(const App* app) {
    char* user = snx_user_data_dir();
    bool same = user && app->data_dir && !strcmp(user, app->data_dir);
    free(user);
    return same;
}
#endif
/* the folder of kind for the menu's own files (settings.ini, app.ini and folders.txt in config, recent.txt and
   last-run.log in state), made: its data folder's (snx_app_dir: the user's, a portable copy's), else the user's. On
   Android the app's own storage, which needs no permission. Made with make; NULL when none can be */
static char* menu_folder(const App* app, int kind, bool make) {
    char* folder;
#ifdef SDL_PLATFORM_ANDROID
    char* sdl = kind == SNX_USER_CACHE ? NULL : SDL_GetPrefPath("scarletnx", "gui");
    (void)app;
    folder = sdl ? snx_strdup(sdl) : NULL;
    SDL_free(sdl);
#else
    folder = snx_menu_dir(app->home_dir ? app->home_dir : app->data_dir, kind);
#endif
    if (folder && make && snx_mkdirs(folder)) {
        free(folder);
        folder = NULL;
    }
    return folder;
}
/* where the keys are installed: the user's data folder, which the driver reads first, so nothing is written in the
   home folder itself (XDG); else ~/.switch */
static char* device_keys_path(const App* app) {
#ifdef SNX_XDG
    if (installed(app)) return snx_path_join(app->data_dir, "prod.keys");
#else
    (void)app;
#endif
    return home_keys_path();
}
/* the prod.keys the driver reads: those installed here, else with the user's data folder the ones in ~/.switch */
static char* keys_in_use(const App* app) {
    if (app->keys_path && snx_is_file(app->keys_path)) return snx_strdup(app->keys_path);
#ifdef SNX_XDG
    char* home = installed(app) ? home_keys_path() : NULL;
    if (home && snx_is_file(home)) return home;
    free(home);
#endif
    return NULL;
}
/* the footer from row: the usage, then the working folder, which games/ and themes/ are found in. The theme's bar style
   puts them on a band of ink over rows rows, its plain one in soft and ink; a rule above when it says so */
/* what ScarletNX's folders take, counted on a thread, and what is left on their disk; "" before the first count */
static void disk_text(const App* app, char* out, size_t size) {
    char used[32], left[32];
    out[0] = 0;
    if (!app->space) return;
    gui_size_text(left, sizeof left, app->free_bytes);
    if (__atomic_load_n(&app->space->done, __ATOMIC_ACQUIRE)) {
        gui_size_text(used, sizeof used, app->space->used);
        snprintf(out, size, "Disk: %s, %s free", used, left);
    } else
        snprintf(out, size, "Disk: counting, %s free", left);
}
static void footer(App* app, int row, int rows, GuiRgb page, GuiRgb ink, GuiRgb soft) {
    char usage[256], disk[96];
    int bar = app->theme.footer == GUI_STYLE_BAR;
    size_t n;
    if (bar) band(app, row, rows, ink);
    if (app->theme.footer_rule) rule(app, row, soft);
    gui_usage_text(usage, sizeof(usage), app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    n = strlen(usage);
    if (disk[0]) snprintf(usage + n, sizeof usage - n, "  %s", disk);
    text(app, 1, row, usage, rgb(bar ? page : soft));
    text(app, 1, row + 1, "CWD:", rgb(bar ? page : soft));
    text(app, 6, row + 1, app->cwd ? app->cwd : "(unavailable)", rgb(bar ? page : ink));
}
/* the screen a file is picked or dropped on: the keys', and a load option's */
static int pick_screen(const App* app) { return app->menu.view == GUI_LOAD_KEYS || app->menu.view == GUI_LOAD_DROP; }
/* the menu's screen, a run or not */
static GuiScreen menu_screen(const App* app) {
    return app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
           : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                             : GUI_SCREEN_MENU;
}
static GuiScreen screen(const App* app) {
    if (app->menu.running || app->finished)
        return (app->native && !gui_steps_last(&app->steps, "RECOMPILE")) || gui_steps_last(&app->steps, "LAUNCH")
                   ? GUI_SCREEN_LOADING
                   : GUI_SCREEN_COMPILING;
    return menu_screen(app);
}
/* the theme step themes after or before the one shown, round the list */
static void next_theme(App* app, int step) {
    size_t i;
    const char* const* dirs = (const char* const*)app->themes_dirs;
    gui_themes_scan(&app->themes, dirs, 2);
    i = 0;
    while (i < app->themes.count && strcmp(app->themes.ids[i], app->theme.id)) ++i;
    if (app->themes.count)
        gui_theme_load(&app->theme, dirs, 2, app->themes.ids[(i + app->themes.count + step) % app->themes.count]);
}
/* app.ini's [folders] keys, in the order of Settings > App > Folders' rows */
static const char* const place_names[GUI_PLACES] = {"data", "games", "work"};
/* a path from the root: '/' first, or on Windows a drive's (C:\ or C:/) or a share's (\\host) */
static bool full_path(const char* path) {
#ifdef _WIN32
    if (SDL_isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return true;
    return path[0] == '\\' && path[1] == '\\';
#else
    return path[0] == '/';
#endif
}
/* Settings > App's picks, kept at once in app_file for the next start. The App's note, or the menu's message, says when
   they could not be; 0, else nonzero */
static int app_save(App* app) {
    char *text, *dir, *staged;
    size_t size = 256;
    int n, failed, i;
    if (!app->app_file) return 0;
    for (i = 0; i < GUI_PLACES; ++i) size += 16 + (app->place[i] ? strlen(app->place[i]) : 0);
    text = (char*)snx_alloc(size);
    n = snprintf(text, size, "; Settings > App in the launcher, as they were left\n[app]\ntheme = %s\n", app->theme.id);
    if (app->menu.text_size)
        n += snprintf(text + n, size - (size_t)n, "text_size = %d\n", app->menu.text_size);
    else
        n += snprintf(text + n, size - (size_t)n, "text_size = auto\n");
    /* Folders' section once one is picked; an empty key is the default */
    if (app->place[GUI_PLACE_DATA] || app->place[GUI_PLACE_GAMES] || app->place[GUI_PLACE_WORK]) {
        n += snprintf(text + n, size - (size_t)n, "[folders]\n");
        for (i = 0; i < GUI_PLACES; ++i)
            n += snprintf(text + n, size - (size_t)n, "%s = %s\n", place_names[i], app->place[i] ? app->place[i] : "");
    }
    /* staged beside it, then put in place: a crash mid-write leaves the old picks */
    dir = snx_dirname(app->app_file);
    staged = snx_format("%s.new", app->app_file);
    failed = !dir || !staged || snx_mkdirs(dir) || snx_write_file(staged, text, (size_t)n) ||
             snx_replace(staged, app->app_file);
    if (failed && staged) remove(staged);
    if (failed && app->editing)
        snprintf(app->editor.note, sizeof app->editor.note, "Not kept for the next start: %s could not be written",
                 app->app_file);
    else if (failed)
        snprintf(app->message, sizeof app->message, "Not kept for the next start: %s could not be written",
                 app->app_file);
    free(dir);
    free(staged);
    free(text);
    return failed;
}
static void layout(App* app);
static void places_apply(App* app);
/* what app_save kept, read: applied as the menu opens (apply), else only looked at. A value the menu does not know, or a
   theme no longer there, leaves what the menu has, named in bad as key = value; how many */
static int app_read(App* app, bool apply, char* bad, size_t size) {
    size_t bytes = 0, i;
    char *text = app->app_file ? (char*)snx_read_file(app->app_file, &bytes) : NULL, *key, *value;
    char* places[GUI_PLACES] = {NULL, NULL, NULL};
    const char* const* dirs = (const char* const*)app->themes_dirs;
    int scale = app->menu.text_size, n = 0, count = 0, moved = 0;
    SnxIni ini;
    if (size) bad[0] = 0;
    if (!text) return 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        char* end = NULL;
        long number = strtol(value, &end, 10);
        int place = GUI_PLACES;
        if (!strcmp(ini.section, "folders"))
            for (place = 0; place < GUI_PLACES && strcmp(key, place_names[place]); ++place) {}
        if (place < GUI_PLACES && (!*value || full_path(value))) { /* nothing is the default */
            free(places[place]);
            places[place] = *value ? snx_strdup(value) : NULL;
            continue;
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "theme")) {
            gui_themes_scan(&app->themes, dirs, 2);
            for (i = 0; i < app->themes.count && strcmp(app->themes.ids[i], value); ++i) {}
            if (i < app->themes.count) {
                if (apply) gui_theme_load(&app->theme, dirs, 2, value);
                continue;
            }
        } else if (!strcmp(ini.section, "app") && !strcmp(key, "text_size")) {
            if (!strcmp(value, "auto")) {
                scale = 0;
                continue;
            }
            if (*value && !*end && number >= 1 && number <= GUI_TEXT_MAX) {
                scale = (int)number;
                continue;
            }
        }
        ++count;
        if (n) n += snprintf(bad + n, size - (size_t)n, ", ");
        if (n < (int)size && strcmp(ini.section, "app")) /* a key outside [app] says where it is */
            n += snprintf(bad + n, size - (size_t)n, "[%s] ", ini.section);
        if (n < (int)size) n += snprintf(bad + n, size - (size_t)n, "%s = %s", key, value);
        if (n >= (int)size) n = (int)size - 1;
    }
    free(text);
    if (apply && scale != app->menu.text_size) {
        app->menu.text_size = scale;
        layout(app);
    }
    for (i = 0; i < GUI_PLACES; ++i) {
        if (apply && (places[i] ? !app->place[i] || strcmp(places[i], app->place[i]) : app->place[i] != NULL)) {
            free(app->place[i]);
            app->place[i] = places[i];
            places[i] = NULL;
            moved = 1;
        }
        free(places[i]);
    }
    if (moved) places_apply(app);
    return count;
}
/* app.ini as the menu opens, what it does not take named in the message */
static void app_load(App* app) {
    char bad[384];
    app_read(app, true, bad, sizeof bad);
    if (bad[0]) {
        size_t m = strlen(app->message);
        snprintf(app->message + m, sizeof app->message - m, "%sNot used from %s: %s", m ? " " : "", app->app_file, bad);
    }
}
/* a list that could not be kept: the message says so, after what it says already, until it is */
static void recent_note(App* app) {
    char note[sizeof app->message], *at;
    size_t n = strlen(app->message);
    if (!app->recent_file) return;
    snprintf(note, sizeof note, "Recent games not kept: %s could not be written.", app->recent_file);
    at = strstr(app->message, note);
    if (app->recent_unsaved && !at)
        snprintf(app->message + n, sizeof app->message - n, "%s%s", n ? " " : "", note);
    else if (!app->recent_unsaved && at) { /* with the space that set it apart */
        size_t cut = strlen(note);
        if (at > app->message)
            --at, ++cut;
        else if (at[cut] == ' ')
            ++cut;
        memmove(at, at + cut, strlen(at + cut) + 1);
    }
}
/* the recent games shown, and whether a row under them shows the rest */
static int recent_shown(const App* app) {
    return app->recent_all ? app->recent.count : SDL_min(app->recent.count, GUI_RECENT_SHOWN);
}
static int recent_more(const App* app) { return recent_shown(app) < app->recent.count; }
/* the rows over Games' games: Add a folder, then the recent games and the row that shows the rest */
static size_t games_head(const App* app) { return 1 + (size_t)(recent_shown(app) + recent_more(app)); }
/* a recent row: a game, or under the ones shown the row that shows the rest */
static void recent_label(const App* app, int index, char* label, size_t size) {
    if (index < recent_shown(app))
        snprintf(label, size, "%s%s", app->theme.marker, app->recent.game[index].name);
    else
        snprintf(label, size, "%s...show more", app->theme.marker);
}
/* the list as the menu and the file have it */
static void recent_changed(App* app) {
    app->browser.head = games_head(app);
    gui_browser_resize(&app->browser, app->browser.page_size);
    if (app->recent_file) app->recent_unsaved = gui_recent_save(&app->recent, app->recent_file) != 0;
    recent_note(app);
}
static void recent_open(App* app, char* file) {
    free(app->recent_file);
    app->recent_file = file;
    gui_recent_load(&app->recent, file);
    app->browser.head = games_head(app);
}
static void menu_label(const App* app, int item, char* label, size_t size) {
    snprintf(label, size, "%s%s", app->theme.marker, items[item]);
}
/* still the menu, fading out, while a run starts */
static int entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
/* the rows Auto keeps at the least: a phone held wide is short, and its menu needs room for its items */
#ifdef SDL_PLATFORM_ANDROID
#define GUI_AUTO_ROWS 20
#else
#define GUI_AUTO_ROWS 16
#endif
/* Text size Auto's scale: twice the display's own (Android's dpi / 160), so a computer at 100% gets 2x and a phone's
   letters come out as big to the eye, as long as rows rows fit in a window height pixels high */
static int pick_auto_scale(float display_scale, int height, int rows) {
    int scale = SDL_max(1, (int)(2.0f * display_scale));
    while (scale > 1 && height / (16 * scale) < rows) --scale;
    return scale;
}
/* a list of boxes in a column of the screen: its box, the cursor's margin before the boxes and the arrows' column after
   them when entries are hidden, each box's height and the pitch from one to the next, the first shown and how many */
typedef struct {
    GuiRect at;
    int count, focus, margin, arrows, item, pitch, first, shown;
} GuiList;
/* what a screen has besides its title, list and foot: the status box (unless the footer's style is none), the tabs
   (a run has none) */
enum { GUI_SHOWS_STATUS = 1, GUI_SHOWS_TABS = 2 };
static void place_screen(App* app, GuiPlace* p, const char* keys, int list_cols, int shows);
static const char* shown_keys(const App* app, char* out, size_t size);
/* a screen's keys, Tab's after them */
static const char* screen_keys(const App* app, const char* keys, char* out, size_t size) {
#ifdef SDL_PLATFORM_ANDROID
    (void)app;
    (void)out;
    (void)size;
    return keys; /* a tap on a tab */
#else
    (void)app;
    snprintf(out, size, "%s Tab: screen.", keys);
    return out;
#endif
}
static void games_place(App* app, GuiPlace* p) {
    size_t i;
    int longest = (int)strlen("Recent games");
    char keys[256];
    for (i = 0; i < app->games.count; ++i) longest = SDL_max(longest, (int)SDL_utf8strlen(app->games.shown[i]));
    for (i = 0; i < (size_t)recent_shown(app); ++i)
        longest = SDL_max(longest, (int)SDL_utf8strlen(app->recent.game[i].name));
    place_screen(app, p, shown_keys(app, keys, sizeof keys),
                 longest + 2 + (int)SDL_utf8strlen(app->theme.marker) + (int)SDL_utf8strlen(app->theme.cursor),
                 GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
}
/* a list entry's box: its text under a frame's width, the frame at its foot over the font's last row, which is empty,
   so many fit; its text's row in it */
static int item_h(const App* app) { return cell_h(app) + app->scale; }
static void list_place(const App* app, GuiList* l, GuiRect at, int count, int focus) {
    int s = app->scale;
    l->at = at;
    l->count = count;
    l->focus = focus;
    l->margin = app->theme.cursor[0] ? ((int)SDL_utf8strlen(app->theme.cursor) * cell_w(app)) : 0;
    l->item = item_h(app);
    l->pitch = gui_place_pitch(at.h, count, l->item + 2 * s, l->item + 14 * s);
    l->shown = gui_place_list(at.h, l->item, l->pitch, count, focus, &l->first);
    l->arrows = l->shown < count ? cell_w(app) : 0;
}
/* Add a folder's box over the list, and the wider gap under it */
static int games_add_h(const App* app) { return item_h(app) + 24 * app->scale; } /* wider than the list's gaps */
/* the list's entries before the page's games: "Recent games" and the recent rows, none without recent games */
static int games_lead(const App* app) { return app->browser.head > 1 ? (int)app->browser.head : 0; }
/* the games a page of the boxed list holds: those that fit under Add a folder and the recent games */
static size_t games_page(App* app) {
    GuiPlace p;
    GuiList l;
    games_place(app, &p);
    list_place(app, &l, gui_rect(p.list.x, p.list.y + games_add_h(app), p.list.w, p.list.h - games_add_h(app)), 1 << 16,
               -1);
    return (size_t)SDL_max(1, l.shown - games_lead(app));
}
/* a box head's height: its label, two pixels of the text above and below */
static int head_h(const App* app) { return cell_h(app) + 4 * app->scale; }
/* r without a column at either side: where a box's text goes */
static GuiRect inner(const App* app, GuiRect r) { return gui_rect(r.x + cell_w(app), r.y, r.w - 2 * cell_w(app), r.h); }
/* the output box's cells under its head */
static void run_console(App* app, const GuiPlace* p, int* cols, int* rows) {
    GuiRect r = inner(app, p->list);
    *cols = SDL_max(1, r.w / cell_w(app));
    *rows = SDL_max(1, (p->list.h - head_h(app) - 4 * app->scale) / cell_h(app));
}
/* ---- a run of the console layout: the driver's output in the wide box, beside it what runs, its file and how it
   ended, the status under them; the file in the tips, the ESC line where the keys go, as the theme's prompt frames it */
static void run_place(App* app, GuiPlace* p) {
    int width, height;
    area(app, &width, &height);
    place_screen(app, p, SAY_ABORT, width / cell_w(app), GUI_SHOWS_STATUS); /* its longest ESC line */
}
static void layout(App* app) {
    int run;
    int width, height, whole_width, whole_height;
    set_area(app);
    area(app, &width, &height);
    /* Auto's letters are the opening window's, measured whole: a phone's system bars may still show over it then, and
       the safe area be short. The window then keeps room for 16 rows of them */
    if (!app->auto_scale && SDL_GetWindowSizeInPixels(app->window, &whole_width, &whole_height) && whole_height > 0) {
        app->auto_scale = pick_auto_scale(SDL_GetWindowDisplayScale(app->window), whole_height, GUI_AUTO_ROWS);
        SDL_SetWindowMinimumSize(
            app->window, 800,
            SDL_max(480, (int)SDL_ceilf(256.0f * (float)app->auto_scale / SDL_GetWindowPixelDensity(app->window))));
    }
    app->scale = app->menu.text_size ? app->menu.text_size : app->auto_scale;
    /* as long as 16 rows still fit: Auto's always do on a computer, a phone's not while Fullscreen is off */
    while (app->scale > 1 && height / (16 * app->scale) < 16) --app->scale;
    if (app->scale < 1) app->scale = 1;
    app->cols = SDL_min(width / (8 * app->scale), GUI_MAX_COLS);
    app->rows = SDL_min(height / (16 * app->scale), GUI_MAX_ROWS);
    run = (app->menu.running || app->finished) && !entering(app);
    gui_browser_resize(&app->browser,
                       app->menu.view == GUI_LOAD_GAMES ? games_page(app) : (size_t)SDL_max(1, app->rows - 10));
    if (run && app->theme.run != GUI_RUN_STEPS) { /* the run's output box */
        GuiPlace p;
        int cols, rows;
        run_place(app, &p);
        run_console(app, &p, &cols, &rows);
        gui_console_resize(&app->console, cols, rows);
    }
}
/* an arc of a circle, from and sweep in degrees, thickness pixels inward */
static void arc(App* app, float cx, float cy, float r, float from, float sweep, float thickness, GuiRgb c) {
    float t;
    SDL_FPoint p[49];
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    for (t = 0; t < thickness; t += 1.0f) {
        int i;
        for (i = 0; i <= 48; ++i) {
            float a = (from + sweep * (float)i / 48) * SDL_PI_F / 180;
            p[i].x = cx + (r - t) * SDL_cosf(a);
            p[i].y = cy + (r - t) * SDL_sinf(a);
        }
        SDL_RenderLines(app->renderer, p, 49);
    }
}
/* the ring that turns while a run goes: a thin outer ring, a thicker inner one and a bar going round it */
static void ring(App* app, float cx, float cy) {
    float r = 7.0f * (float)app->scale, inner = r * 0.62f, thick = SDL_max(1.0f, 1.5f * (float)app->scale);
    float angle = app->finished ? 0 : (float)((now_ms(app) / 4) % 360);
    arc(app, cx, cy, r, 0, 360, SDL_max(1.0f, 0.5f * (float)app->scale), app->theme.run_colors.dim);
    arc(app, cx, cy, inner, 0, 360, thick, app->theme.run_colors.soft);
    arc(app, cx, cy, inner, angle, 70, thick, app->theme.run_colors.ink);
}
/* "", ".", "..", "..." round and round; "..." once the run is over */
static const char* dots(const App* app) {
    static const char* const turn[] = {"", ".", "..", "..."};
    return app->finished ? "..." : turn[(now_ms(app) / 400) % 4];
}
static void launch(App* app, char* absolute, bool prepared);
/* the game's name as the run knows it: the runner's banner, else the file's own, in capitals */
static void run_name(const App* app, char* out, size_t size) {
    const char* q;
    size_t n;
    const char* dot;
    const GuiStep *launch = gui_steps_last(&app->steps, "LAUNCH"), *made = gui_steps_last(&app->steps, "RECOMPILE");
    /* a game made already is named by its title ID: its recompile's line says what it is shown as */
    const char* name = launch ? launch->detail : app->native && made ? made->detail : app->file ? app->file : "";
    for (q = name; *q; ++q)
        if (*q == '/' || *q == '\\') name = q + 1;
    n = 0;
    dot = launch || (app->native && made) ? NULL : strrchr(name, '.');
    for (q = name; *q && q != dot && n + 1 < size; ++q) out[n++] = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
    out[n] = 0;
}
/* a step's line: its label, its detail in capitals and its status */
static size_t step_runs(const GuiStep* s, int how, Uint64 now, char run[3][112], size_t length[3]) {
    char* q;
    size_t total;
    int r;
    snprintf(run[0], 112, "%s:", s->label);
    snprintf(run[1], 112, " %s", s->detail);
    for (q = run[1]; *q; ++q)
        if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    if (s->state == GUI_STEP_FAILED)
        snprintf(run[2], 112, "...FAILED");
    else if (s->state == GUI_STEP_DONE)
        snprintf(run[2], 112, "...OK");
    else {
        char progress[64];
        gui_step_progress(s, how, now, progress, sizeof progress);
        snprintf(run[2], 112, "%s%s", progress[0] ? " " : "", progress);
    }
    total = 0;
    for (r = 0; r < 3; ++r) total += length[r] = SDL_utf8strlen(run[r]);
    return total;
}
/* a row cut to the columns it has: a step's detail, keeping its label and state, or a line, ending in "..." */
static void clip_row(char run[3][112], size_t length[3], int note, int room) {
    int cut;
    size_t marker, other, left, i, at;
    const char* end;
    size_t total = length[0] + length[1] + length[2];
    if (room < 8 || total <= (size_t)room) return;
    cut = note ? 0 : 1;
    marker = note ? 3 : 0;
    other = total - length[cut];
    left = (size_t)room > other + marker ? (size_t)room - other - marker : 0;
    end = run[cut];
    for (i = 0; i < left; ++i) SDL_StepUTF8(&end, NULL);
    at = (size_t)(end - run[cut]);
    snprintf(run[cut] + at, 112 - at, "%s", note ? "..." : "");
    length[cut] = left + marker;
}
/* how far each of rows rows has typed at now, total[r] characters that came at shown[r]: async, each from when it came;
   sync, one after the other. A slow row types a character every typing_ms, any other the whole row in typing_ms, and
   one above first, off the screen, at once. Returns the row being typed, -1 for none */
static int type_rows(const GuiTheme* theme, int rows, int first, const size_t* total, const Uint64* shown,
                     const int* slow, Uint64 now, size_t* typed) {
    int r, typing = -1;
    Uint64 free_at = 0;
    for (r = 0; r < rows; ++r) {
        Uint64 start, span = slow[r] ? (Uint64)theme->typing_ms * total[r] : theme->typing_ms;
        typed[r] = total[r];
        if (r < first || theme->line_in != GUI_LINE_TYPING || !theme->typing_ms) continue;
        start = theme->typing_sync ? SDL_max(shown[r], free_at) : shown[r];
        free_at = start + span;
        if (now < start)
            typed[r] = 0;
        else if (now - start < span)
            typed[r] = (size_t)((now - start) * total[r] / span);
        if (now >= start && typed[r] < total[r]) typing = r;
    }
    return typing;
}
static void foot(App* app, const GuiPlace* p, const char* tips, SDL_Color color, const char* keys);
static int hint_pick(const char* key, int col);
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
static void render_steps(App* app) {
    /* the rows: each step, then every line it kept when detailed, folded once it is done when collapse says so;
       each ends before the right edge, with room for a step's dots and the cursor */
    enum { ROWS = GUI_STEPS_MAX + GUI_NOTES_MAX, SEGMENTS = 40 };
    static char runs[ROWS][3][112];
    static size_t lengths[ROWS][3], typed[ROWS], totals[ROWS];
    static const GuiStep* owner[ROWS];
    static int is_note[ROWS], slow[ROWS];
    static Uint64 shown[ROWS];
    static const GuiRgb white = {255, 255, 255};
    const GuiTheme* theme = &app->theme;
    const GuiPalette* colors = &theme->run_colors;
    /* since: a line that came while the menu faded out comes in with the run */
    Uint64 now = now_ms(app), since = app->fade_ms;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    int width, height, name_col, col, y, i, r, rows = 0, last_step = -1, typing, top = 3, bottom, fit, first, cursor;
    int foot = theme->footer != GUI_STYLE_NONE ? 2 : 0; /* the footer's rows, over the ESC line */
    const char* title;
    char name[96], head[128];
    SDL_Color ink, soft, failure;
    area(app, &width, &height);
    if (theme->texture)
        for (y = 3; y < height; y += 4) fill(app, 0, (float)y, (float)width, 1, white, 14);
    ink = rgb(colors->ink);
    soft = rgb(colors->soft);
    failure = rgb(colors->failure);

    run_name(app, name, sizeof name);
    col = theme->ring == GUI_RING_LEFT ? 4 : 1;
    title = theme->title[screen(app)];
    name_col = col + (int)SDL_utf8strlen(title) + (title[0] ? 1 : 0);
    if (theme->ring == GUI_RING_LEFT) ring(app, cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_RIGHT) ring(app, (float)width - cw * 2, ch * 1.5f);
    if (theme->ring == GUI_RING_NEAR_TEXT) /* after the name and the dots' whole width, so it stays put */
        ring(app, cw * (float)(name_col + (int)SDL_utf8strlen(name) + 4) + 7.0f * (float)app->scale, ch * 1.5f);
    text(app, col, 1, title, ink);
    snprintf(head, sizeof head, "%s%s", name, dots(app));
    text(app, name_col, 1, head, soft);
    if (app->finished) text(app, 1, 2, app->message, app->abnormal_exit ? failure : soft);
    for (i = 0; i < app->steps.count; ++i) {
        int n;
        const GuiNote* notes[GUI_NOTES_MAX];
        int k;
        const GuiStep* s = &app->steps.step[i];
        owner[rows] = s;
        is_note[rows] = 0;
        shown[rows] = SDL_max(s->shown_ms, since);
        step_runs(s, theme->progress, now, runs[rows], lengths[rows]);
        clip_row(runs[rows], lengths[rows], 0, app->cols - 6);
        last_step = rows++;
        if (!theme->detailed || (theme->collapse && s->state == GUI_STEP_DONE)) continue;

        n = gui_step_notes(&app->steps, s, notes, GUI_NOTES_MAX);
        for (k = 0; k < n; ++k, ++rows) {
            owner[rows] = s;
            is_note[rows] = 1;
            shown[rows] = SDL_max(notes[k]->shown_ms, since);
            snprintf(runs[rows][0], sizeof runs[rows][0], "%s", notes[k]->text);
            runs[rows][1][0] = runs[rows][2][0] = 0;
            lengths[rows][0] = SDL_utf8strlen(runs[rows][0]);
            lengths[rows][1] = lengths[rows][2] = 0;
            clip_row(runs[rows], lengths[rows], 1, app->cols - 5);
        }
    }
    bottom = (theme->bar ? app->rows - 5 : app->rows - 2) - foot;
    fit = SDL_max(1, bottom - top);
    first = SDL_max(0, rows - fit);
    /* the newest row of a step still running types readably; a done step's, and one a newer row of its step passed,
       at once but for a glimpse: a fast log keeps the cursor on its latest lines */
    for (r = 0; r < rows; ++r) {
        totals[r] = lengths[r][0] + lengths[r][1] + lengths[r][2];
        slow[r] = owner[r]->state == GUI_STEP_ACTIVE && (r + 1 == rows || owner[r + 1] != owner[r]);
    }
    typing = type_rows(theme, rows, first, totals, shown, slow, now, typed);
    cursor = typing >= 0 ? typing : last_step;
    for (r = first; r < rows; ++r) {
        int working, blink, k;
        size_t left;
        const GuiStep* s = owner[r];
        int last = s == &app->steps.step[app->steps.count - 1];
        int failed = s->state == GUI_STEP_FAILED, active = s->state == GUI_STEP_ACTIVE;
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        Uint64 age = now > shown[r] ? now - shown[r] : 0;
        int moving = theme->line_in == GUI_LINE_SLIDE || theme->line_in == GUI_LINE_FADE;
        float p = moving && theme->line_in_ms ? SDL_min(1.0f, (float)age / (float)theme->line_in_ms) : 1.0f;
        float x = cw * (is_note[r] ? 3 : 1) -
                  (theme->line_in == GUI_LINE_SLIDE ? 16.0f * (float)app->scale * (1 - p) * (1 - p) : 0);
        float y = ch * (float)(top + r - first);
        SDL_Color color[3];
        color[0] = failed ? failure : last ? ink : soft;
        color[1] = failed ? failure : last || active ? ink : soft;
        color[2] = failed ? failure : ink;
        if (is_note[r]) color[0] = soft;
        left = typed[r];
        for (k = 0; k < 3; ++k) {
            size_t drawn = SDL_min(left, lengths[r][k]);
            x = text_px(app, x, y, runs[r][k], drawn, color[k], p);
            left -= drawn;
        }
        if (!is_note[r] && typed[r] == total && active) x = text_px(app, x, y, dots(app), 3, ink, p);
        blink = !theme->blink_ms || (now / theme->blink_ms) % 2 == 0;
        working = typing == r || (typing < 0 && active);
        if (r == cursor && working && blink) text_px(app, x, y, "_", 1, ink, p);
    }
    if (theme->bar) {
        int k, percent = gui_steps_percent(&app->steps);
        int filled = percent < 0 ? 0 : (percent * SEGMENTS + 50) / 100;
        float span = (float)(app->cols - 8) * cw, gap = 2.0f * (float)app->scale;
        float seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS, sy = ch * (float)(app->rows - foot - 3) + ch * 0.3f;
        char number[16];
        fill(app, cw, ch * (float)(app->rows - foot - 4) + ch / 2, (float)(app->cols - 2) * cw, (float)app->scale,
             colors->dim, 255);
        for (k = 0; k < SEGMENTS; ++k) {
            SDL_FRect r = {cw + (float)k * (seg + gap), sy, seg, ch * 0.4f};
            if (k < filled)
                fill(app, r.x, r.y, r.w, r.h, colors->ink, 255);
            else {
                SDL_SetRenderDrawColor(app->renderer, colors->dim.r, colors->dim.g, colors->dim.b, 255);
                SDL_RenderRect(app->renderer, &r);
            }
        }
        if (percent < 0)
            snprintf(number, sizeof number, " --%%");
        else
            snprintf(number, sizeof number, "%3d%%", percent);
        text(app, app->cols - 6, app->rows - foot - 3, number, ink);
        text(app, 1, app->rows - foot - 2, name, ink);
    }
    if (foot) footer(app, app->rows - 3, 2, colors->page, colors->ink, colors->soft);
    if (app->finished)
        prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, app->rows - 1, SAY_DONE, soft);
    else if (app->aborting)
        prompt(app, GUI_PROMPT_ABORTING, 1, app->rows - 1, "Aborting process tree...", rgb(colors->warning));
    else
        text(app, 1, app->rows - 1, SAY_ABORT, soft);
}
/* the whole window darkened, alpha of 255, the cutout beside the text too: the fades, and a box over the screen */
static void shade(App* app, Uint8 alpha) {
    static const GuiRgb black = {0, 0, 0};
    SDL_Rect safe;
    int width, height;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, 0, (float)width, (float)height, black, alpha);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* the screen fading in from black, or the menu or the run fading out to it */
static void overlay(App* app) {
    const GuiTheme* theme = &app->theme;
    Uint64 now = now_ms(app);
    float a = 0;
    if (entering(app))
        a = (float)(now > app->entering_ms ? now - app->entering_ms : 0) / (float)theme->fade_out_ms;
    else if (app->leaving_ms && theme->fade_out_ms)
        a = SDL_min(1.0f, (float)(now > app->leaving_ms ? now - app->leaving_ms : 0) / (float)theme->fade_out_ms);
    else if (app->fade_ms && theme->fade_in_ms && now < app->fade_ms + theme->fade_in_ms)
        a = 1.0f - (float)(now > app->fade_ms ? now - app->fade_ms : 0) / (float)theme->fade_in_ms;
    if (a > 0) shade(app, (Uint8)(a * 255));
}
static const char* ask_question(const App* app) {
    switch (app->confirm.ask) {
        case GUI_ASK_DEEP: return "List the games in its subfolders too?";
        case GUI_ASK_EXIT: return "Exit ScarletNX?";
        case GUI_ASK_OUTSIDE: return "Outside the app, Android checks every file access: games may run slower.";
        case GUI_ASK_ROOM: return app->room_line;
        default:
            return screen(app) == GUI_SCREEN_COMPILING ? "Abort the recompile? Its work is lost." : "Abort the run?";
    }
}
#define SAY_LEFT_BY    " was left by an older version: this one keeps it in "
#define SAY_LEFT_TITLE "Left by older versions"
/* a line of app->left: the file where an older build left it, and where this one keeps it */
static void left_paths(const char* line, char* old, char* now, size_t size) {
    const char* by = strstr(line, SAY_LEFT_BY);
    snprintf(old, size, "%.*s", (int)(by - line), line);
    by += strlen(SAY_LEFT_BY);
    snprintf(now, size, "%.*s", (int)strcspn(by, "\n"), by);
}
static void list_item(App* app, const GuiList* l, int i, const char* label, int focused, int activated);
static void fill_rect(App* app, GuiRect r, GuiRgb c) {
    if (r.w > 0 && r.h > 0) fill(app, (float)r.x, (float)r.y, (float)r.w, (float)r.h, c, 255);
}
static void outline(App* app, GuiRect r, GuiRgb c) {
    int s = app->scale;
    fill_rect(app, gui_rect(r.x, r.y, r.w, s), c);
    fill_rect(app, gui_rect(r.x, r.y + r.h - s, r.w, s), c);
    fill_rect(app, gui_rect(r.x, r.y, s, r.h), c);
    fill_rect(app, gui_rect(r.x + r.w - s, r.y, s, r.h), c);
}
/* value from pixel x, y up to the right edge of r: a line too long for it slides as the theme says */
static void text_in(App* app, GuiRect r, int x, int y, const char* value, SDL_Color color) {
    int w = cell_w(app), room = (r.x + r.w - x) / w, skip;
    if (room <= 0) return;
    skip = gui_slide((int)SDL_utf8strlen(value) - room, now_ms(app), app->theme.scroll_ms);
    while (skip-- > 0 && *value) SDL_StepUTF8(&value, NULL);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    for (; *value && room > 0; --room, x += w) glyph(app, (float)x, (float)y, gui_glyph(SDL_StepUTF8(&value, NULL)));
}
/* a box framed in the dim colour, filled with the page's (fill) or the frame alone (line); with a head when label is
   not NULL, its label on a band of ink (fill) or over a rule (line). Where its text starts */
static int pane(App* app, GuiRect r, const char* label) {
    const GuiTheme* t = &app->theme;
    int s = app->scale, filled = t->panels == GUI_PANELS_FILL;
    if (filled) fill_rect(app, r, t->page);
    if (label && filled) fill_rect(app, gui_rect(r.x, r.y, r.w, head_h(app)), t->ink);
    if (label && !filled) fill_rect(app, gui_rect(r.x, r.y + head_h(app) - s, r.w, s), t->dim);
    outline(app, r, t->dim);
    if (label) text_in(app, inner(app, r), r.x + cell_w(app), r.y + 2 * s, label, rgb(filled ? t->page : t->ink));
    return r.y + (label ? head_h(app) : 0) + 2 * s;
}
/* the box over the darkened screen, on the page colour whatever the theme's boxes, so nothing shows through it */
static int over_box(App* app, GuiRect r, const char* label) {
    shade(app, 128);
    fill_rect(app, r, app->theme.page);
    return pane(app, r, label);
}
/* a box over the screen, cols columns of text wide and h pixels high, in its middle */
static GuiRect over_rect(const App* app, int cols, int h) {
    int width, height, w;
    area(app, &width, &height);
    w = SDL_min((cols + 2) * cell_w(app), width - 2 * cell_w(app));
    return gui_rect((width - w) / 2, SDL_max(0, (height - h) / 2), w, h);
}
/* the bytes of s's first row at width columns: a path's up to a slash (gui_wrap), other text's up to a space */
static size_t wrap_at(const char* s, int width, int path) {
    const char *p = s, *space = NULL;
    int n;
    if (path) return gui_wrap(s, width);
    for (n = 0; *p && n < width; ++n) {
        SDL_StepUTF8(&p, NULL);
        if (*p == ' ') space = p;
    }
    return !*p ? strlen(s) : space ? (size_t)(space - s) : gui_wrap(s, width);
}
/* s from pixel x, y wrapped to the right edge of r (path: at its slashes), a row under the other while they fit above
   its bottom, or on one row that slides with the theme's [body] text = slide: the rows it took */
static int wrap_in(App* app, GuiRect r, int x, int y, const char* s, int path, SDL_Color color) {
    char row[1100];
    int width = (r.x + r.w - x) / cell_w(app), rows = 0;
    if (width <= 0) return 0;
    if (app->theme.text == GUI_TEXT_SLIDE) {
        if (!*s || y + cell_h(app) > r.y + r.h) return 0;
        text_in(app, r, x, y, s, color);
        return 1;
    }
    while (*s && y + (rows + 1) * cell_h(app) <= r.y + r.h) {
        size_t n = wrap_at(s, width, path);
        snprintf(row, sizeof row, "%.*s", (int)n, s);
        text_in(app, r, x, y + rows * cell_h(app), row, color);
        for (s += n; *s == ' ';) ++s; /* a row starts at a word */
        ++rows;
    }
    return rows;
}
/* the rows s takes wrapped to width columns, one where the theme slides it */
static int wrap_rows(const App* app, const char* s, int width, int path) {
    int rows = 0;
    if (width <= 0 || app->theme.text == GUI_TEXT_SLIDE) return 1;
    for (; *s; ++rows)
        for (s += wrap_at(s, width, path); *s == ' ';) ++s;
    return SDL_max(rows, 1);
}
/* a box of files over the screen, two lines each, as many as fit, how many more, then OK, focused: the folders and
   files an older version left (report 0), each where it is (-) and the space it takes; or what Clean old settings did
   (1), each file and what was done to it or why not. Any key closes it */
/* the space the older version's i-th folder, old, takes, or that it is still counted */
static void left_size(const App* app, int i, const char* old, char* out, size_t size) {
    const GuiSpace* space = app->left_space;
    if (space && i < __atomic_load_n(&space->counted, __ATOMIC_ACQUIRE) && !strcmp(space->dirs[i], old))
        gui_size_text(out, size, space->bytes[i]);
    else
        snprintf(out, size, "counting");
}
/* the i-th file, at: its two lines, their colours, whether the second is a path; the next file, NULL after the last */
static const char* files_entry(const App* app, int report, int i, const char* at, char* a, char* b, size_t size,
                               SDL_Color* ca, SDL_Color* cb, int* path) {
    SDL_Color failure = rgb(app->theme.failure), ink = rgb(app->theme.ink);
    const char* next;
    bool ok;
    if (!report) {
        left_paths(at, a, b, size);
        left_size(app, i, a, b, size);
        *ca = failure;
        *cb = rgb(app->theme.soft);
        *path = 0;
        next = strchr(at, '\n') + 1;
        return *next ? next : NULL;
    }
    next = gui_cleaned_entry(at, &ok, a, b, size);
    *ca = ok ? ink : failure;
    *cb = ok ? rgb(app->theme.soft) : failure;
    *path = 0;
    return next;
}
static void render_files(App* app, int report) {
    char a[4400], b[4400], label[96];
    const char *first = report ? app->cleaned.text : app->left, *at = first;
    int width, height, cols, room, used = 0, shown, i, y, path, s = app->scale, item = item_h(app);
    int count = report ? app->cleaned.count : app->lefts, done = app->cleaned.done;
    SDL_Color ca, cb;
    GuiRect r, text;
    GuiList ok;
    area(app, &width, &height);
    cols = width / cell_w(app) - 6;
    room = (height - head_h(app) - item - 24 * s) / cell_h(app) - 2; /* the files' rows, and one for how many more */
    for (shown = 0; at && shown < count; ++shown) {
        int need;
        at = files_entry(app, report, shown, at, a, b, sizeof a, &ca, &cb, &path);
        need = wrap_rows(app, a, cols - 2, 1) + wrap_rows(app, b, cols - 2, path);
        if (shown && used + need > room) break;
        used += need;
    }
    r = over_rect(app, cols, head_h(app) + (used + (shown < count)) * cell_h(app) + item + 16 * s);
    if (!report)
        snprintf(label, sizeof label, "%s", SAY_LEFT_TITLE);
    else if (count > done)
        snprintf(label, sizeof label, "Clean old settings: %d done, %d not", done, count - done);
    else
        snprintf(label, sizeof label, "Clean old settings: %d done", done);
    y = over_box(app, r, label) + 2 * s;
    text = inner(app, r);
    for (i = 0, at = first; at && i < shown; ++i) {
        at = files_entry(app, report, i, at, a, b, sizeof a, &ca, &cb, &path);
        text_in(app, text, text.x, y, "-", ca);
        y += wrap_in(app, text, text.x + 2 * cell_w(app), y, a, 1, ca) * cell_h(app);
        y += wrap_in(app, text, text.x + 2 * cell_w(app), y, b, path, cb) * cell_h(app);
    }
    if (shown < count) {
        snprintf(label, sizeof label, "and %d more, which Share log lists", count - shown);
        text_in(app, text, text.x, y, label, rgb(app->theme.soft));
    }
    list_place(app, &ok, gui_rect(text.x, r.y + r.h - item - 6 * s, SDL_min(text.w, 26 * cell_w(app)), item), 1, 0);
    snprintf(label, sizeof label, "%sOK", app->theme.marker);
    list_item(app, &ok, 0, label, true, false);
}
/* the question over a run, the menu or a form: the question, wrapped to the screen, its two choices as boxes, the one
   kept focused */
static void ask_boxes(App* app, GuiRect* r, GuiList* choices) {
    int s = app->scale, item = item_h(app), choices_h = 2 * (item + 6 * s), rows;
    const char* question = ask_question(app);
    *r = over_rect(app, SDL_max((int)SDL_utf8strlen(question), 16), 0);
    rows = wrap_rows(app, question, inner(app, *r).w / cell_w(app), 0);
    *r = over_rect(app, SDL_max((int)SDL_utf8strlen(question), 16), rows * cell_h(app) + choices_h + 12 * s);
    list_place(app, choices,
               gui_rect(r->x + cell_w(app), r->y + rows * cell_h(app) + 10 * s, r->w - 2 * cell_w(app), choices_h), 2,
               app->confirm.selected);
}
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
#define SAY_AGAIN "tap again"
#else
#define SAY_AGAIN "press again"
#endif
static void render_ask(App* app) {
    static const char* const choices[][2] = {{"Keep going", "Abort"},
                                             {"Stay", "Exit"},
                                             {"Only this folder", "With subfolders"},
                                             {"Back", "Use it"},
                                             {"Back", "Run anyway"}};
    GuiRect r;
    GuiList l;
    char label[96];
    int i;
    ask_boxes(app, &r, &l);
    over_box(app, r, NULL);
    wrap_in(app, inner(app, r), r.x + cell_w(app), r.y + 4 * app->scale, ask_question(app), 0, rgb(app->theme.warning));
    for (i = 0; i < 2; ++i) {
        /* a choice asked twice says so once pressed */
        snprintf(label, sizeof label, "%s%s%s", app->theme.marker, choices[app->confirm.ask][i],
                 i == GUI_CONFIRM_ABORT && app->confirm.armed ? ": " SAY_AGAIN : "");
        list_item(app, &l, i, label, app->confirm.selected == i, false);
    }
}
/* the boot's splash: a drawing of Wimsy's, picked at random, drawn a third of the window's shorter side on a theme's
   splash page, then the menu as the theme brings it in */
static int splash_size(const App* app) {
    SDL_Rect view;
    SDL_GetRenderViewport(app->renderer, &view);
    return SDL_max(1, SDL_min(view.w, view.h) / 3);
}
/* the drawing at side pixels; the program's icon when it cannot be, said once */
static SDL_Texture* splash_texture(App* app, int side) {
    SDL_Texture* texture = NULL;
    void* pixels = app->splash_drawing ? SDL_malloc((size_t)side * (size_t)side * 4) : NULL;
    if (pixels && !snx_splash_draw(app->splash_drawing, side, pixels, side * 4)) {
        texture = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, side, side);
        if (texture && (!SDL_UpdateTexture(texture, NULL, pixels, side * 4) ||
                        !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED))) {
            SDL_DestroyTexture(texture);
            texture = NULL;
        }
    }
    SDL_free(pixels);
    if (!texture && app->splash_drawing) {
        fprintf(stderr, "scarletnx: the splash's %s could not be drawn %d pixels square, so the icon shows\n",
                app->splash_svg->name, side);
        snx_splash_close(app->splash_drawing);
        app->splash_drawing = NULL;
    }
    if (!texture) {
        SDL_Surface* icon = snx_window_icon_surface();
        texture = icon ? SDL_CreateTextureFromSurface(app->renderer, icon) : NULL;
        SDL_DestroySurface(icon);
    }
    return texture;
}
/* the splash over, its time up or nothing to show: the menu comes in as the theme brings it */
static void splash_end(App* app) {
    if (app->splash) SDL_DestroyTexture(app->splash);
    app->splash = NULL;
    snx_splash_close(app->splash_drawing);
    app->splash_drawing = NULL;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
}
/* svg's drawing on t's splash page until its time */
static void splash_with(App* app, const GuiTheme* t, const SnxSplashSvg* svg) {
    app->splash_svg = svg;
    app->splash_drawing = snx_splash_open(svg);
    if (!app->splash_drawing)
        fprintf(stderr, "scarletnx: the splash's %s could not be read, so the icon shows\n", svg->name);
    app->splash_side = splash_size(app);
    app->splash = splash_texture(app, app->splash_side);
    if (!app->splash) {
        snx_splash_close(app->splash_drawing);
        app->splash_drawing = NULL;
        return;
    }
    app->splash_page = t->splash_page;
    app->splash_until = SDL_GetTicks() + t->splash_ms;
}
static void splash_start(App* app) {
    const GuiTheme* t = gui_splash_theme(&app->theme, (int)SDL_rand(100));
    if (t->splash_ms) splash_with(app, t, &snx_splash_svgs[SDL_rand(snx_splash_svg_count)]);
}
static int splash_shown(App* app) {
    SDL_Rect view;
    int side;
    if (!app->splash) return 0;
    if (now_ms(app) >= app->splash_until) {
        splash_end(app);
        return 0;
    }
    side = splash_size(app);
    if (side != app->splash_side) { /* the window changed: drawn again at its size */
        SDL_DestroyTexture(app->splash);
        app->splash_side = side;
        app->splash = splash_texture(app, side);
        if (!app->splash) {
            splash_end(app);
            return 0;
        }
    }
    SDL_GetRenderViewport(app->renderer, &view);
    SDL_SetRenderDrawColor(app->renderer, app->splash_page.r, app->splash_page.g, app->splash_page.b, 255);
    SDL_RenderClear(app->renderer);
    {
        SDL_FRect at = {(float)((view.w - side) / 2), (float)((view.h - side) / 2), (float)side, (float)side};
        SDL_RenderTexture(app->renderer, app->splash, NULL, &at);
    }
    SDL_RenderPresent(app->renderer);
    return 1;
}
#ifdef SDL_PLATFORM_ANDROID
#define SAY_MENU_KEYS SAY_MENU
#else
#define SAY_MENU_KEYS SAY_MENU " ESC: exit."
#endif
/* the keys the foot of the screen shown says, Tab's after them: what it draws, and what a click on one presses */
static const char* shown_keys(const App* app, char* out, size_t size) {
    const char* keys = SAY_MENU_KEYS;
    if (app->editing)
        keys = app->form == GUI_FORM_PAD        ? SAY_PAD
               : app->editor.picking            ? SAY_PICKER
               : app->form == GUI_FORM_THEME    ? SAY_EDITOR
               : app->form == GUI_FORM_SETTINGS ? SAY_GROUPS
               : app->form == GUI_FORM_APP      ? SAY_APP
               : app->form == GUI_FORM_FOLDERS  ? SAY_FOLDERS
               : app->form == GUI_FORM_PLACES   ? SAY_PLACES
                                                : SAY_SETTINGS;
    else if (app->menu.view == GUI_LOAD_GAMES)
        keys = SAY_GAMES;
    else if (pick_screen(app))
        keys = SAY_KEYS_HOW;
    return screen_keys(app, keys, out, size);
}
/* the colour picker's box, its rows and the bars of the first three, in pixels */
typedef struct {
    GuiRect box, rows[GUI_PICK_ROWS], bars[3];
} GuiPicker;
static int item_text_y(const App* app, GuiRect b) { return b.y + app->scale; }
static GuiRect list_box(const GuiList* l, int i) {
    return gui_rect(l->at.x + l->margin, l->at.y + (i - l->first) * l->pitch, l->at.w - l->margin - l->arrows, l->item);
}
static void touch(App* app, float x, float y);
/* the entry under pixel x, y, -1 for none; a finger takes the whole width */
static int list_hit(const GuiList* l, float x, float y, bool touch) {
    int i;
    for (i = l->first; i < l->first + l->shown; ++i) {
        GuiRect b = list_box(l, i);
        if (y >= (float)b.y && y < (float)(b.y + b.h + (l->pitch - l->item)) &&
            (touch ? x >= (float)l->at.x && x < (float)(l->at.x + l->at.w) : x >= (float)b.x && x < (float)(b.x + b.w)))
            return i;
    }
    return -1;
}
/* an entry as a box of its own: on the dim colour (fill) or framed in it (line); the focused one inverted on the focus
   colour with a rule over and under it (fill), or framed and written in it; the cursor in the margin. focused 2: that
   look without the cursor, for the group whose rows have the focus */
static void list_item(App* app, const GuiList* l, int i, const char* label, int focused, int activated) {
    const GuiTheme* t = &app->theme;
    GuiRect b = list_box(l, i);
    int s = app->scale, filled = t->panels == GUI_PANELS_FILL, inverted = focused && t->focus_bar;
    if (inverted)
        fill_rect(app, b, t->focus);
    else if (filled)
        fill_rect(app, b, t->dim);
    if (!inverted && (!filled || focused)) outline(app, b, focused ? t->focus : t->dim);
    if (focused && filled && l->pitch - l->item >= 6 * s) { /* where the gap has room for them */
        fill_rect(app, gui_rect(b.x, b.y - 3 * s, b.w, s), t->focus);
        fill_rect(app, gui_rect(b.x, b.y + b.h + 2 * s, b.w, s), t->focus);
    }
    text_in(app, inner(app, b), b.x + cell_w(app) / 2, item_text_y(app, b), label,
            rgb(inverted    ? t->page
                : activated ? t->active
                : focused   ? t->focus
                            : t->ink));
    if (focused == 1 && t->cursor[0]) text_in(app, l->at, l->at.x, item_text_y(app, b), t->cursor, rgb(t->focus));
}
/* a label between a list's boxes, as "Recent games" */
static void list_label(App* app, const GuiList* l, int i, const char* label) {
    GuiRect b = list_box(l, i);
    text_in(app, b, b.x, item_text_y(app, b), label, rgb(app->theme.soft));
}
/* arrows past the boxes' right edge: up beside entry first, down beside entry last */
static void list_arrows(App* app, const GuiList* l, int first, int last, int up, int down) {
    int x = l->at.x + l->at.w - cell_w(app);
    if (up) text_in(app, l->at, x, item_text_y(app, list_box(l, first)), "\x1e", rgb(app->theme.soft));
    if (down) text_in(app, l->at, x, item_text_y(app, list_box(l, last)), "\x1f", rgb(app->theme.soft));
}
/* while entries are hidden above or below the ones shown */
static void list_marks(App* app, const GuiList* l) {
    list_arrows(app, l, l->first, l->first + l->shown - 1, l->first > 0, l->first + l->shown < l->count);
}
/* The keys of a screen: "Keys: what they do." sentences, each drawn as the keys, inverted (the theme's focus bar) or in
   the focus colour, and what they do; a row holds as many as fit, against its right edge */
static int hint_next(const char** s, char* key, char* what, size_t size) {
    const char *p = *s, *end, *colon;
    size_t n;
    while (*p == ' ') ++p;
    if (!*p) return 0;
    end = strstr(p, ". ");
    if (!end) end = p + strlen(p) - (p[strlen(p) - 1] == '.');
    colon = strstr(p, ": ");
    if (colon && colon > end) colon = NULL;
    n = colon ? (size_t)(colon - p) : 0;
    snprintf(key, size, "%.*s", (int)n, p);
    p = colon ? colon + 2 : p;
    snprintf(what, size, "%.*s", (int)(end - p), p);
    *s = *end ? end + 1 : end;
    return 1;
}
static int hint_width(const char* key, const char* what) {
    return (int)SDL_utf8strlen(key) + (key[0] ? 1 : 0) + (int)SDL_utf8strlen(what);
}
/* a click or a tap, for hints() to say which sentence with keys it is on: their keys, and how many columns into the
   sentence (-1: the margin before it); key empty for none */
typedef struct {
    float x, y;
    char key[128];
    int col;
} GuiHintHit;
/* the rows keys take in width columns; with draw, drawn from pixel y in r, on the foot's band of ink when band; with
   hit, the sentence its pixel is on, a column of the gap either side of one its own */
static int hints(App* app, const char* keys, int width, GuiRect r, int y, int draw, int band, GuiHintHit* hit) {
    const GuiTheme* t = &app->theme;
    char key[128], what[128];
    const char* row = keys;
    int rows = 0, s = app->scale, lit;
    for (;;) {
        const char *at = row, *end = row, *u = row;
        int used = 0, x;
        while (hint_next(&at, key, what, sizeof key)) {
            int w = hint_width(key, what) + (used ? 2 : 0);
            if (used && used + w > width) break;
            used += w;
            end = at;
        }
        if (end == row) break;
        for (x = r.x + r.w - used * cell_w(app); (draw || hit) && u != end && hint_next(&u, key, what, sizeof key);) {
            int cy = y + rows * cell_h(app), k = (int)SDL_utf8strlen(key), w = hint_width(key, what) * cell_w(app);
            if (hit && k && hit->y >= (float)cy && hit->y < (float)(cy + cell_h(app)) &&
                hit->x >= (float)(x - cell_w(app)) && hit->x < (float)(x + w + cell_w(app))) {
                snprintf(hit->key, sizeof hit->key, "%s", key);
                hit->col = (int)SDL_floorf((hit->x - (float)x) / (float)cell_w(app));
            }
            lit = draw && k && app->hovering && app->hover_y >= (float)cy && app->hover_y < (float)(cy + cell_h(app)) &&
                  app->hover_x >= (float)(x - cell_w(app)) && app->hover_x < (float)(x + w + cell_w(app)) &&
                  hint_pick(key, (int)SDL_floorf((app->hover_x - (float)x) / (float)cell_w(app))) >= 0;
            if (lit) /* the whole sentence, as a button under the pointer: inverted */
                fill_rect(app, gui_rect(x - s, cy, w + 2 * s, cell_h(app)), band ? t->page : t->focus);
            else if (draw && k && t->focus_bar)
                fill_rect(app, gui_rect(x - s, cy, k * cell_w(app) + 2 * s, cell_h(app)), band ? t->soft : t->focus);
            if (draw && k)
                text_in(app, r, x, cy, key,
                        rgb(lit                    ? (band ? t->ink : t->page)
                            : t->focus_bar || band ? t->page
                                                   : t->focus));
            if (draw)
                text_in(app, r, x + (k + (k ? 1 : 0)) * cell_w(app), cy, what,
                        rgb(lit    ? (band ? t->ink : t->page)
                            : band ? t->page
                                   : t->ink));
            x += w + 2 * cell_w(app);
        }
        ++rows;
        row = end;
    }
    return SDL_max(rows, 1);
}
/* ---- the foot and the tips: the keys' box, the tips' beside it or on a row under the head ---- */
/* the foot's letters, a size under the rest's but 1 at the least: what draws or measures the keys, and the tips when
   they are in the foot, does it with app->scale set to them, then puts the menus' back */
static int foot_scale(const App* app) { return SDL_max(1, app->scale - 1); }
/* the text's room in a box of them: past the bar at its left in a box, else the box's own width */
static GuiRect foot_text(const App* app, GuiRect r, int box) {
    int s = app->scale;
    return box ? gui_rect(r.x + 3 * cell_w(app), r.y + 2 * s, r.w - 4 * cell_w(app), r.h - 4 * s)
               : gui_rect(r.x, r.y + 2 * s, r.w, r.h - 4 * s);
}
/* one of them: with box a pane with a bar down its left, on the foot's band when band; where its text goes */
static GuiRect foot_box(App* app, GuiRect r, int box, int band) {
    const GuiTheme* t = &app->theme;
    int s = app->scale;
    if (!r.h) return r;
    if (box && !band) pane(app, r, NULL);
    if (box)
        fill_rect(app, gui_rect(r.x + 2 * s, r.y + 2 * s, 3 * s, r.h - 4 * s),
                  band                           ? t->page
                  : t->panels == GUI_PANELS_FILL ? t->ink
                                                 : t->soft);
    return foot_text(app, r, box);
}
static void foot(App* app, const GuiPlace* p, const char* tips, SDL_Color color, const char* keys) {
    const GuiTheme* t = &app->theme;
    int band = t->footer == GUI_STYLE_BAR, below = p->tips.h && p->tips.y == p->keys.y, s = app->scale;
    int small = foot_scale(app);
    GuiRect foot = below ? gui_rect(p->tips.x, p->keys.y, p->keys.x + p->keys.w - p->tips.x, p->keys.h) : p->keys;
    GuiRect text;
    if (!p->keys.h) return;
    if (band) fill_rect(app, foot, t->ink);
    if (t->footer_rule) fill_rect(app, gui_rect(foot.x, foot.y - cell_h(app) / 4 - s / 2, foot.w, s), t->soft);
    if (below && band && !(color.r == t->failure.r && color.g == t->failure.g && color.b == t->failure.b))
        color = rgb(t->page); /* on the band but a failure */
    if (below) app->scale = small;
    text = foot_box(app, p->tips, t->tips_box, below && band);
    if (below && t->tips_wrap)
        wrap_in(app, text, text.x, text.y, tips, 0, color);
    else if (p->tips.h) /* one row: a long one slides */
        text_in(app, text, text.x, text.y, tips, color);
    app->scale = small;
    text = foot_box(app, p->keys, t->footer_box, band);
    hints(app, keys, text.w / cell_w(app), text, text.y, 1, band, NULL);
    app->scale = s;
}
/* the foot of a screen: the message while there is one, a failure's in its colour, else about */
static void screen_foot(App* app, const GuiPlace* p, const char* about, const char* keys) {
    const GuiTheme* t = &app->theme;
    foot(app, p, app->message[0] ? app->message : about,
         rgb(app->message[0] ? (app->abnormal_exit ? t->failure : t->focus) : t->ink), keys);
}
/* ---- the head: the tabs, and the title after them or under them ---- */
static int title_size(const App* app) { return (int)SDL_clamp(app->theme.title_size, 1, 3); }
/* value from pixel x, y in letters size times the text's, cut at the right edge of r */
static void big_text(App* app, GuiRect r, int x, int y, const char* value, int size, SDL_Color color) {
    float w = 8.0f * (float)(app->scale * size), h = 16.0f * (float)(app->scale * size);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    for (; *value && (float)x + w <= (float)(r.x + r.w); x += (int)w) {
        unsigned char c = gui_glyph(SDL_StepUTF8(&value, NULL));
        SDL_FRect source = {(float)(c % 16 * 8), (float)(c / 16 * 16), 8, 16}, at = {(float)x, (float)y, w, h};
        SDL_RenderTexture(app->renderer, app->font, &source, &at);
    }
}
static const char* screen_title(const App* app, char* out, size_t size) {
    char* c;
    if (!app->editing)
        return app->theme
            .title[(app->menu.running || app->finished) && !entering(app) ? screen(app) : menu_screen(app)];
    if (app->form == GUI_FORM_THEME)
        snprintf(out, size, "THEME EDITOR  /  %s", app->editor.id);
    else if (app->form == GUI_FORM_PAD) {
        if (app->pad_player) /* a player's own */
            snprintf(out, size, "SETTINGS  /  %s  /  PLAYER %d  /  %s", app->group_keys[app->group].key,
                     app->pad_player, app->settings.keys[app->page].key);
        else
            snprintf(out, size, "SETTINGS  /  %s  /  %s", app->group_keys[app->group].key,
                     app->settings.keys[app->page].key);
        for (c = out; *c; ++c) *c = (char)SDL_toupper((unsigned char)*c);
    } else
        snprintf(out, size, "SETTINGS");
    return out;
}
/* the status box: the usage, the disk and the working folder, wrapped to width columns; with draw, drawn in r */
static int status_rows(App* app, int width, GuiRect r, int y, int draw) {
    char usage[256], disk[96], cwd[1100];
    const char* lines[3];
    int i, rows = 0;
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    gui_usage_text(usage, sizeof usage, app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    disk_text(app, disk, sizeof disk);
    snprintf(cwd, sizeof cwd, "CWD: %s", app->cwd ? app->cwd : "(unavailable)");
    lines[0] = usage;
    lines[1] = disk;
    lines[2] = cwd;
    for (i = 0; i < 3; ++i) {
        if (!lines[i][0]) continue;
        if (draw)
            rows += wrap_in(app, r, r.x, y + rows * cell_h(app), lines[i], i == 2, i == 2 ? ink : soft);
        else
            rows += wrap_rows(app, lines[i], width, i == 2);
    }
    return rows;
}
/* the screens the tabs go round, Tab and Shift+Tab or a click on one: the menu, Games and the Settings */
enum { GUI_TAB_MENU, GUI_TAB_GAMES, GUI_TAB_SETTINGS, GUI_TABS };
static const char* const tab_names[GUI_TABS] = {"Menu", "Games", "Settings"};
static int tab_shown(const App* app) {
    if (app->editing) return GUI_TAB_SETTINGS;
    return app->menu.view == GUI_LOAD_GAMES ? GUI_TAB_GAMES : GUI_TAB_MENU;
}
/* tab i in the row of tabs: two columns wider than its name, a column after the one before it */
static GuiRect tab_rect(const App* app, GuiRect row, int i) {
    int x = row.x, k;
    for (k = 0; k < i; ++k) x += ((int)strlen(tab_names[k]) + 3) * cell_w(app);
    return gui_rect(x, row.y, ((int)strlen(tab_names[i]) + 2) * cell_w(app), row.h);
}
/* the tabs, the one shown as the focused box is; on the head's band in its colours swapped */
static void draw_tabs(App* app, GuiRect row, int band) {
    const GuiTheme* t = &app->theme;
    GuiRgb ink = band ? t->page : t->ink, page = band ? t->ink : t->page, focus = band ? t->page : t->focus;
    int i, s = app->scale, shown = tab_shown(app);
    if (!row.h) return;
    for (i = 0; i < GUI_TABS; ++i) {
        GuiRect r = tab_rect(app, row, i);
        int on = i == shown, inverted = on && t->focus_bar;
        if (inverted)
            fill_rect(app, r, focus);
        else if (t->panels == GUI_PANELS_FILL)
            fill_rect(app, r, t->dim);
        if (!inverted && (t->panels == GUI_PANELS_OUTLINE || on)) outline(app, r, on ? focus : t->dim);
        text_in(app, r, r.x + cell_w(app), r.y + 2 * s, tab_names[i], rgb(inverted ? page : on ? focus : ink));
    }
}
/* the page's faint grid behind the boxes, in the dim colour */
static void backdrop(App* app) {
    const GuiTheme* t = &app->theme;
    int width, height, s = app->scale, x, y;
    if (!t->grid) return;
    area(app, &width, &height);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, t->dim.r, t->dim.g, t->dim.b, 72);
    for (x = 0; x < width; x += 6 * s) SDL_RenderLine(app->renderer, (float)x, 0, (float)x, (float)height);
    for (y = 0; y < height; y += 6 * s) SDL_RenderLine(app->renderer, 0, (float)y, (float)width, (float)y);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
}
static void chrome(App* app, const GuiPlace* p) {
    const GuiTheme* t = &app->theme;
    int band = t->head == GUI_STYLE_BAR, size = title_size(app), s = app->scale;
    char out[160];
    const char* title = screen_title(app, out, sizeof out);
    SDL_SetRenderDrawColor(app->renderer, t->page.r, t->page.g, t->page.b, 255);
    SDL_RenderClear(app->renderer);
    backdrop(app);
    if (!p->head.h) return;
    if (band) fill_rect(app, gui_rect(p->head.x, p->head.y - 2 * s, p->head.w, p->head.h + 4 * s), t->ink);
    if (t->head_rule)
        fill_rect(app, gui_rect(p->head.x, p->head.y + p->head.h + cell_h(app) / 4 - s / 2, p->head.w, s), t->dim);
    draw_tabs(app, p->tabs, band);
    if (!p->title.h) return;
    if (t->title_shadow)
        big_text(app, p->title, p->title.x + size * s, p->title.y + size * s, title, size, rgb(t->dim));
    big_text(app, p->title, p->title.x, p->title.y, title, size, rgb(band ? t->page : t->ink));
}
static void place_screen(App* app, GuiPlace* p, const char* keys, int list_cols, int shows) {
    const GuiTheme* t = &app->theme;
    GuiPlaceIn in;
    GuiRect none = {0, 0, 0, 0};
    int width, height, s = app->scale, size = title_size(app);
    char out[160];
    const char* title = screen_title(app, out, sizeof out);
    area(app, &width, &height);
    memset(&in, 0, sizeof in);
    in.width = width;
    in.height = height;
    in.cw = cell_w(app);
    in.ch = cell_h(app);
    if (shows & GUI_SHOWS_TABS) {
        GuiRect last = tab_rect(app, none, GUI_TABS - 1);
        in.tabs = cell_h(app) + 4 * s;
        in.tabs_w = last.x + last.w;
    }
    if (title[0]) {
        int shadow = t->title_shadow ? size * s : 0;
        in.title = size * cell_h(app) + shadow;
        in.title_w = (int)SDL_utf8strlen(title) * size * cell_w(app) + shadow;
        in.title_beside = t->head_place == GUI_HEAD_TABS;
    }
    if (t->tips_place == GUI_TIPS_TOP) in.tips = cell_h(app) + 4 * s;
    in.list_cols = list_cols ? SDL_min(SDL_max(list_cols, 20), SDL_max(20, (width / cell_w(app) - 2) * 3 / 5)) : 0;
    in.fixed = t->sizing == GUI_SIZING_FIXED && list_cols && list_cols < width / cell_w(app); /* a list, not a run */
    app->scale = foot_scale(app);
    { /* the widest key, which a row cannot break */
        const char* at = keys;
        char key[128], what[128];
        while (hint_next(&at, key, what, sizeof key))
            in.keys_w = SDL_max(in.keys_w, (hint_width(key, what) + (t->footer_box ? 4 : 0)) * cell_w(app));
    }
    in.foot = cell_h(app);
    gui_place(&in, p); /* the columns, which the foot's rows and the status box's follow */
    in.foot = SDL_max(in.tips || !t->tips_wrap ? 1 : 2,
                      hints(app, keys, foot_text(app, p->keys, t->footer_box).w / cell_w(app), none, 0, 0, 0, NULL)) *
                  cell_h(app) +
              4 * app->scale;
    app->scale = s;
    if (shows & GUI_SHOWS_STATUS && in.list_cols && t->footer != GUI_STYLE_NONE) /* at the side's width */
        in.status =
            head_h(app) + 4 * s + status_rows(app, inner(app, p->side).w / cell_w(app), none, 0, 0) * cell_h(app);
    gui_place(&in, p);
}
static void status_box(App* app, GuiRect r) {
    GuiRect text = inner(app, r);
    int y;
    if (!r.h) return;
    y = pane(app, r, "Status");
    status_rows(app, text.w / cell_w(app), gui_rect(text.x, y, text.w, r.y + r.h - y), y, 1);
}
/* ---- the menu: the items, a box each; beside them the focused one ---- */
typedef struct {
    GuiPlace place;
    GuiList list; /* entry i is item i */
} GuiMenuBoxes;
/* what an item is about, for the tips and the side box */
static const char* item_about(const App* app, int item) {
    static const char* const loads[] = {"Recompiles a game's .nsp, .xci or .nro, then runs it",
                                        "Runs an ARM64 Linux program on the CPU", "Opens a 3DS .cia"};
    if (item >= GUI_SWITCH && item <= GUI_3DS) return loads[item - GUI_SWITCH];
    if (item == GUI_KEYS) return "Installs prod.keys, which games are extracted with";
    if (item == GUI_EXIT) return "Closes ScarletNX";
    {
        GuiMenu menu = app->menu;
        menu.selected = item;
        return gui_setting_description(&menu);
    }
}
static void menu_boxes(App* app, GuiMenuBoxes* m) {
    int i, longest = 0;
    char label[160], keys[256];
    for (i = 0; i < GUI_ITEM_COUNT; ++i) {
        menu_label(app, i, label, sizeof label);
        longest = SDL_max(longest, (int)SDL_utf8strlen(label));
    }
    place_screen(app, &m->place, shown_keys(app, keys, sizeof keys),
                 longest + 2 + (int)SDL_utf8strlen(app->theme.cursor), GUI_SHOWS_STATUS | GUI_SHOWS_TABS);
    list_place(app, &m->list, m->place.list, GUI_ITEM_COUNT, app->menu.selected);
}
/* the last run's output, under what the side box says */
static void last_output(App* app, GuiRect r, int y) {
    SDL_Color soft = rgb(app->theme.soft);
    int i, x;
    if (!app->has_output || entering(app)) return;
    y += cell_h(app);
    y += wrap_in(app, r, r.x, y, "Last output (no scrollback):", 0, soft) * cell_h(app);
    SDL_SetTextureColorMod(app->font, app->theme.ink.r, app->theme.ink.g, app->theme.ink.b);
    for (i = 0; i < app->console.rows && y + cell_h(app) <= r.y + r.h; ++i, y += cell_h(app))
        for (x = 0; x < app->console.cols && (x + 1) * cell_w(app) <= r.w; ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)y, app->console.cells[i][x]);
}
/* the side box: the focused item, what it is about, and for the keys the ones in use; the last run's output under it */
static void menu_side(App* app, const GuiMenuBoxes* m) {
    int item = m->list.focus, y;
    GuiRect r = inner(app, m->place.side);
    char line[1100];
    if (!m->place.side.h) return;
    y = pane(app, m->place.side, items[item]);
    r = gui_rect(r.x, y, r.w, m->place.side.y + m->place.side.h - y);
    y += wrap_in(app, r, r.x, y, item_about(app, item), 0, rgb(app->theme.ink)) * cell_h(app);
    if (item == GUI_KEYS) {
        char* in_use = keys_in_use(app);
        snprintf(line, sizeof line, "In use: %s", in_use ? in_use : "none");
        y += wrap_in(app, r, r.x, y, line, 1, rgb(in_use ? app->theme.soft : app->theme.failure)) * cell_h(app);
        free(in_use);
    }
    last_output(app, r, y);
}
/* Setup device keys while no prod.keys is in use: its box in the failure colour every other blink_ms (steady with 0),
   over its usual look, so a first start shows what to do first */
static void keys_wanted(App* app, const GuiList* l, const char* label) {
    const GuiTheme* t = &app->theme;
    GuiRect b = list_box(l, GUI_KEYS);
    char* in_use;
    if (t->blink_ms && (now_ms(app) / t->blink_ms) % 2) return;
    in_use = keys_in_use(app);
    if (in_use) {
        free(in_use);
        return;
    }
    fill_rect(app, b, t->failure);
    text_in(app, inner(app, b), b.x + cell_w(app) / 2, item_text_y(app, b), label, rgb(t->page));
}
static void draw_menu(App* app) {
    GuiMenuBoxes m;
    char label[160], keys[256];
    int i;
    menu_boxes(app, &m);
    chrome(app, &m.place);
    for (i = m.list.first; i < m.list.first + m.list.shown; ++i) {
        menu_label(app, i, label, sizeof label);
        list_item(app, &m.list, i, label, i == m.list.focus, app->menu.activated && app->menu.active == i);
        if (i == GUI_KEYS) keys_wanted(app, &m.list, label);
    }
    list_marks(app, &m.list);
    menu_side(app, &m);
    status_box(app, m.place.status);
    screen_foot(app, &m.place, item_about(app, m.list.focus), shown_keys(app, keys, sizeof keys));
}
/* ---- Games: Add a folder, then "Recent games" over the recent rows and a page of the games, as many as the list's
   box holds ---- */
typedef struct {
    GuiPlace place;
    GuiList add;  /* Add a folder, a wider gap under it */
    GuiList list; /* entry i: the label and the recent rows under games_lead, then the page's games */
    size_t start; /* the page's first game */
    int lead;     /* games_lead */
} GuiGamesBoxes;
static void games_boxes(App* app, GuiGamesBoxes* g) {
    GuiRect r;
    size_t selected = app->browser.selected, head = app->browser.head;
    games_place(app, &g->place);
    r = g->place.list;
    g->start = app->browser.page * app->browser.page_size;
    g->lead = games_lead(app);
    list_place(app, &g->add, gui_rect(r.x, r.y, r.w, item_h(app)), 1, selected ? -1 : 0);
    list_place(app, &g->list, gui_rect(r.x, r.y + games_add_h(app), r.w, r.h - games_add_h(app)),
               g->lead + (int)gui_browser_items(&app->browser),
               !selected         ? -1
               : selected < head ? (int)selected
                                 : g->lead + (int)(selected - head));
    if (gui_browser_pages(&app->browser) > 1) g->list.arrows = cell_w(app); /* the pages' */
}
/* the focused row of Games: what it is, about it, and a line more for a recent game's or a game's kind */
static const char* games_focus(App* app, const GuiGamesBoxes* g, const char** about, char* kind, size_t size) {
    static const char* const kinds[] = {"Switch", "Linux ARM", "3DS"};
    size_t selected = app->browser.selected, head = app->browser.head;
    kind[0] = 0;
    if (!selected) {
        *about = "Pick a folder of games, listed after games/";
        return "Add a folder";
    }
    if (selected < head && (int)selected - 1 < recent_shown(app)) {
        const GuiRecentGame* r = &app->recent.game[selected - 1];
        snprintf(kind, size, "%s, %s", kinds[r->driver >= GUI_SWITCH && r->driver <= GUI_3DS ? r->driver : 0],
                 r->prepared ? "ready to run" : "prepared when it runs");
        *about = r->path;
        return r->name;
    }
    if (selected < head) {
        *about = "The recent games not shown";
        return "...show more";
    }
    {
        const char* name = app->games.names[g->start + selected - head]; /* its file's: what kind it is */
        snprintf(kind, size, "%s",
                 gui_is_cia(name)    ? "3DS CIA, opened with scarletnx-3ds"
                 : gui_is_dump(name) ? "A dump, prepared when it runs"
                                     : "Ready to run");
        *about = app->games.paths[g->start + selected - head];
        return app->games.shown[g->start + selected - head];
    }
}
static void draw_games(App* app) {
    GuiGamesBoxes g;
    GuiRect r;
    char label[400], line[1100], keys[256], kind[96];
    const char *about, *name;
    size_t i, items = gui_browser_items(&app->browser), head = app->browser.head;
    SDL_Color soft = rgb(app->theme.soft), ink = rgb(app->theme.ink);
    int y, e;
    games_boxes(app, &g);
    chrome(app, &g.place);
    snprintf(label, sizeof label, "%sAdd a folder", app->theme.marker);
    list_item(app, &g.add, 0, label, app->browser.selected == 0, false);
    for (e = g.list.first; e < g.list.first + g.list.shown; ++e) {
        if (e == 0 && g.lead)
            list_label(app, &g.list, e, "Recent games");
        else if (e < g.lead) {
            recent_label(app, e - 1, label, sizeof label);
            list_item(app, &g.list, e, label, app->browser.selected == (size_t)e, false);
        } else {
            i = (size_t)(e - g.lead);
            snprintf(label, sizeof label, "%s%s", app->theme.marker, app->games.shown[g.start + i]);
            list_item(app, &g.list, e, label, app->browser.selected == head + i,
                      app->browser.activated && app->browser.active == g.start + i);
        }
    }
    /* beside it the focused row: what it is and about it; the page, and where the games are looked for */
    name = games_focus(app, &g, &about, kind, sizeof kind);
    r = inner(app, g.place.side);
    if (g.place.side.h) {
        y = pane(app, g.place.side, name);
        r = gui_rect(r.x, y, r.w, g.place.side.y + g.place.side.h - y);
        y += wrap_in(app, r, r.x, y, about, app->browser.selected != 0, ink) * cell_h(app);
        if (kind[0]) y += wrap_in(app, r, r.x, y, kind, 0, soft) * cell_h(app);
        y += cell_h(app);
        if (!app->games.count)
            y += wrap_in(app, r, r.x, y, SAY_NO_GAMES, 0, soft) * cell_h(app);
        else {
            snprintf(line, sizeof line, "Page %zu/%zu, %zu " SAY_COUNT, app->browser.page + 1,
                     gui_browser_pages(&app->browser), app->games.count);
            y += wrap_in(app, r, r.x, y, line, 0, soft) * cell_h(app);
        }
        y += wrap_in(app, r, r.x, y, app->games_dir ? app->games_dir : "games/", 1, soft) * cell_h(app);
        for (i = 0; i < (size_t)app->folder_count; ++i)
            y += wrap_in(app, r, r.x, y, app->folders[i], 1, soft) * cell_h(app);
        last_output(app, r, y);
    }
    if (items) /* games on the pages before and after */
        list_arrows(app, &g.list, g.lead, g.lead + (int)items - 1, app->browser.page > 0,
                    app->browser.page + 1 < gui_browser_pages(&app->browser));
    status_box(app, g.place.status);
    screen_foot(app, &g.place, about, shown_keys(app, keys, sizeof keys));
}
/* a line that waits on the user, at pixel row y of r: plain at its left, or framed, placed and blinking as the theme's
   prompt says */
static void prompt_in(App* app, GuiRect r, int y, GuiPromptLine line, const char* value, SDL_Color color) {
    char framed[160];
    const GuiPrompt* p = &app->theme.prompt;
    int x = r.x, w;
    if (p->lines[line]) {
        if (p->blink && app->theme.blink_ms && (now_ms(app) / app->theme.blink_ms) % 2) return;
        snprintf(framed, sizeof framed, "%s%s%s", p->open, value, p->close);
        value = framed;
        w = (int)SDL_utf8strlen(framed) * cell_w(app);
        if (p->align == GUI_ALIGN_CENTER) x = r.x + (r.w - w) / 2;
        if (p->align == GUI_ALIGN_RIGHT) x = r.x + r.w - w;
    }
    text_in(app, r, SDL_max(r.x, x), y, value, color);
}
/* ---- the screen a file is picked or dropped on: one box with where the keys go and the ones in use, the line that
   waits in its middle, Back at its foot */
typedef struct {
    GuiPlace place;
    GuiList back;
    int prompt; /* the waiting line's pixel row */
} GuiPickBoxes;
static void pick_boxes(App* app, GuiPickBoxes* k) {
    GuiRect r;
    int item = item_h(app);
    char keys[256];
    place_screen(app, &k->place, shown_keys(app, keys, sizeof keys), 0, GUI_SHOWS_TABS);
    r = inner(app, k->place.list);
    list_place(
        app, &k->back,
        gui_rect(r.x, k->place.list.y + k->place.list.h - item - 4 * app->scale, SDL_min(r.w, 24 * cell_w(app)), item),
        1, -1);
    k->prompt = k->place.list.y + k->place.list.h / 2;
}
static void draw_pick(App* app) {
    GuiPickBoxes k;
    GuiRect r;
    bool keys = app->menu.view == GUI_LOAD_KEYS;
    char line[1100], hints_line[256], *in_use = keys ? keys_in_use(app) : NULL;
    int y;
    pick_boxes(app, &k);
    chrome(app, &k.place);
    y = pane(app, k.place.list, items[keys ? GUI_KEYS : app->menu.active]);
    r = inner(app, k.place.list);
    r = gui_rect(r.x, y, r.w, k.back.at.y - y);
    if (keys) {
        snprintf(line, sizeof line, "Installs to: %s", app->keys_path ? app->keys_path : "Home directory unavailable.");
        y += wrap_in(app, r, r.x, y, line, 1, rgb(app->theme.ink)) * cell_h(app);
    }
    if (keys && keys_left(app)) { /* what the last try did, and when the menu comes back */
        GuiRgb said = app->keys_ok ? app->theme.focus : app->theme.failure;
        y += cell_h(app);
        y += wrap_in(app, r, r.x, y, app->keys_ok ? "Installed" : "Not installed", 0, rgb(said)) * cell_h(app);
        y += wrap_in(app, r, r.x, y, app->message, 1, rgb(app->keys_ok ? app->theme.ink : said)) * cell_h(app);
        snprintf(line, sizeof line, "Back to the menu in %d s%s", keys_left(app),
                 app->keys_ok ? "" : ", or pick another file");
        y += wrap_in(app, r, r.x, y, line, 0, rgb(app->theme.soft)) * cell_h(app);
        if (app->keys_ok) { /* the keys just installed: nothing to warn about, nor to pick */
            free(in_use);
            in_use = NULL;
        }
    }
    if (in_use && app->keys_path && !strcmp(in_use, app->keys_path))
        y += wrap_in(app, r, r.x, y, "Keys already setup. This will overwrite the previous ones", 0,
                     rgb(app->theme.failure)) *
             cell_h(app);
    else if (in_use) {
        snprintf(line, sizeof line, "Keys in use: %s. Keys dropped here are read before them", in_use);
        y += wrap_in(app, r, r.x, y, line, 1, rgb(app->theme.soft)) * cell_h(app);
    }
    free(in_use);
    if (!keys || !app->keys_done || !app->keys_ok)
        prompt_in(app, r, SDL_max(y, k.prompt), GUI_PROMPT_KEYS, keys ? SAY_KEYS_WAIT : SAY_FILE_WAIT,
                  rgb(app->theme.focus));
    list_item(app, &k.back, 0, "Back [ESC]", false, false); /* nothing to move to: ESC's */
    snprintf(line, sizeof line, "%s: " SAY_PICK_BOX, items[keys ? GUI_KEYS : app->menu.active]);
    screen_foot(app, &k.place, line, shown_keys(app, hints_line, sizeof hints_line));
}
static void draw_run(App* app) {
    GuiPlace p;
    GuiRect r, text;
    const GuiTheme* t = &app->theme;
    int y, x, i, cols, rows, s = app->scale;
    GuiPromptLine line =
        app->finished ? (app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE) : GUI_PROMPT_ABORTING;
    run_place(app, &p);
    chrome(app, &p);
    y = pane(app, p.list, "Output");
    r = inner(app, p.list);
    run_console(app, &p, &cols, &rows);
    SDL_SetTextureColorMod(app->font, t->ink.r, t->ink.g, t->ink.b);
    for (i = 0; i < SDL_min(rows, app->console.rows); ++i)
        for (x = 0; x < SDL_min(cols, app->console.cols); ++x)
            if (app->console.cells[i][x] != ' ')
                glyph(app, (float)(r.x + x * cell_w(app)), (float)(y + i * cell_h(app)), app->console.cells[i][x]);
    y = pane(app, p.side, items[app->menu.selected]);
    text = inner(app, p.side);
    text = gui_rect(text.x, y, text.w, p.side.y + p.side.h - y);
    y += wrap_in(app, text, text.x, y, app->file ? app->file : "", 1, rgb(t->ink)) * cell_h(app);
    if (app->finished)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, app->message, 0,
                rgb(app->abnormal_exit ? t->failure : t->focus));
    else if (app->aborting)
        wrap_in(app, text, text.x, y + cell_h(app) / 2, "Aborting process tree...", 0, rgb(t->failure));
    status_box(app, p.status);
    foot(app, &p, app->file ? app->file : "", rgb(t->soft), "");
    app->scale = foot_scale(app); /* the ESC line where the keys go, in their letters */
    text = foot_text(app, p.keys, t->footer_box);
    if (app->finished || app->aborting)
        prompt_in(app, text, text.y, line, app->finished ? SAY_DONE : "Aborting process tree...", rgb(t->failure));
    else
        text_in(app, text, text.x + text.w - (int)strlen(SAY_ABORT) * cell_w(app), text.y, SAY_ABORT, rgb(t->failure));
    app->scale = s;
}
/* the menu's screen or a run's, in app->theme, on the render target as it is; the boxes over it */
static void draw_screen(App* app) {
    const GuiTheme* t = &app->theme;
    int idle = (!app->menu.running && !app->finished) || entering(app);
    if (!idle && t->run == GUI_RUN_STEPS) {
        SDL_SetRenderDrawColor(app->renderer, t->run_colors.page.r, t->run_colors.page.g, t->run_colors.page.b, 255);
        SDL_RenderClear(app->renderer);
        render_steps(app);
    } else if (!idle)
        draw_run(app);
    else if (app->menu.view == GUI_LOAD_GAMES)
        draw_games(app);
    else if (pick_screen(app))
        draw_pick(app);
    else
        draw_menu(app);
    if (app->confirm.shown) render_ask(app);
    if (idle && app->left_shown && app->lefts) render_files(app, 0);
    overlay(app);
}
static const char* driver_name(int item) {
    switch (item) {
#ifdef SDL_PLATFORM_ANDROID
        /* the app's libraries folder only takes lib*.so: the drivers go in as ones */
        case GUI_SWITCH: return "libscarletnx-run.so";
        case GUI_LINUX: return "libscarletnx-gnuabi.so";
        case GUI_3DS: return "libscarletnx-3ds.so";
#else
        case GUI_SWITCH: return "scarletnx";
        case GUI_LINUX: return "scarletnx-gnuabi";
        case GUI_3DS: return "scarletnx-3ds";
#endif
        default: return NULL;
    }
}
static char* find_driver(const App* app, int item) {
    const char* name = driver_name(item);
    if (app->self && item == GUI_SWITCH) return snx_strdup(app->self);
    if (!name) return NULL;
#ifdef _WIN32
    char* filename = snx_format("%s.exe", name);
#else
    char* filename = snx_strdup(name);
#endif
    char* local = snx_path_join(app->drivers, filename);
    char* found = snx_find_tool(local);
    free(filename);
    free(local);
    return found ? found : snx_find_tool(name);
}
/* what argv prints, NULL unless it ends with 0, killed after ten seconds; owned */
static char* capture(App* app, const char* const* argv) {
    char buffer[4096], *out = snx_strdup("");
    size_t size = 0;
    int status = 1, n;
    Uint64 until = SDL_GetTicks() + 10000;
    if (!gui_process_start(app->process, argv, app->message, sizeof(app->message))) status = -1;
    while (status >= 0 && !gui_process_finished(app->process, &status) && SDL_GetTicks() < until + 5000) {
        char* grown;
        n = gui_process_read(app->process, buffer, sizeof buffer);
        if (n > 0 && (grown = realloc(out, size + (size_t)n + 1)) != NULL) {
            out = grown;
            memcpy(out + size, buffer, (size_t)n);
            out[size += (size_t)n] = 0;
        } else if (n <= 0)
            SDL_Delay(1);
        if (SDL_GetTicks() > until) gui_process_terminate(app->process);
    }
    if (status) {
        free(out);
        return NULL;
    }
    return out;
}
/* the Settings' rows: the runtime's list, read the first time from the program that runs Switch inputs, after the
   launcher's own */
static GuiSettings* settings_of(App* app) {
    char* driver;
    char* list;
    if (app->settings.keys) return &app->settings;
    driver = find_driver(app, GUI_SWITCH);
#ifdef SDL_PLATFORM_IOS /* no program to ask: the game's .ini keeps its settings */
    list = NULL;
#else
    {
        const char* argv[] = {driver, "--settings", NULL};
        list = driver ? capture(app, argv) : NULL;
    }
#endif
    gui_settings_read(&app->settings, list ? list : "");
    free(list);
    free(driver);
    /* the launcher's own choices win over the game's .ini, as its items did: a phone plays on the whole screen, its
       pad shown */
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    gui_settings_set(&app->settings, "fullscreen", "1");
    gui_settings_set(&app->settings, "controls", "1");
#else
    gui_settings_set(&app->settings, "fullscreen", "0");
    gui_settings_set(&app->settings, "controls", "0");
#endif
    gui_settings_mark(&app->settings);
    gui_settings_load(&app->settings, app->settings_file);
    return &app->settings;
}
/* ---- the theme editor's preview: the menu or a run in the theme being edited ---- */
#define GUI_PREVIEW_MS 8000
static const char preview_run[] = "Extracting Cult of the Lamb.nsp\n"
                                  "Extracted 3 NCAs, the program's 4 modules\n"
                                  "Recompiling Cult of the Lamb (4 modules)\n"
                                  "Functions: 48211  instructions: 2915002\n"
                                  "LLVM: 5 code units and the module descriptor\n"
                                  "  compiled block 1/6: code_000, 9874 functions (8.1 s)\n"
                                  "  compiled block 2/6: code_001, 10022 functions (8.4 s)\n"
                                  "  compiled block 3/6: code_002, 9567 functions (7.9 s)\n";
/* the menu or a run drawn in the theme being edited, at 1x, in app->preview; false when it cannot be made */
static bool preview_texture(App* app) {
    static App saved; /* the whole app, put back after: drawing the preview borrows it */
    int run = app->editor.preview != GUI_SCREEN_MENU;
    Uint64 now = now_ms(app);
    if (!app->preview) {
        app->preview = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 400);
        if (!app->preview) return false;
    }
    if (!app->preview_since || now - app->preview_since >= GUI_PREVIEW_MS) {
        app->preview_since = SDL_max(now, 1);
        gui_steps_reset(&app->preview_steps);
        gui_steps_feed(&app->preview_steps, preview_run, sizeof preview_run - 1, app->preview_since);
        gui_console_init(&app->preview_console, 78, 17);
        gui_console_feed(&app->preview_console, preview_run, sizeof preview_run - 1);
    }
    saved = *app;
    app->theme = app->editor.theme;
    app->scale = 1;
    app->cols = 80;
    app->rows = 25;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = run;
    app->editing = false; /* the menu's own tabs */
    app->finished = app->aborting = app->abnormal_exit = app->confirm.shown = false;
    app->native = 0;
    app->entering_ms = app->leaving_ms = 0;
    app->fade_ms = run ? app->preview_since : 0; /* a run fades in from black, as after the menu */
    app->message[0] = 0;
    app->has_output = run;
    if (run) {
        app->steps = app->preview_steps;
        app->console = app->preview_console;
        app->file = (char*)"games/Cult of the Lamb.nsp";
        app->menu.selected = GUI_SWITCH;
    }
    SDL_SetRenderTarget(app->renderer, app->preview);
    SDL_SetRenderViewport(app->renderer, NULL);
    draw_screen(app);
    SDL_SetRenderTarget(app->renderer, NULL);
    *app = saved;
    set_area(app);
    return true;
}
static void screen_tab(App* app, int step);
static int tab_hit(App* app, float x, float y);
/* the editor on the theme the menu shows: a folder of themes/ keeps its file's extras and its name, a built-in is
   saved as a new theme */
static void open_editor(App* app) {
    GuiThemeFile file;
    size_t size = 0;
    char* text = gui_theme_text((const char* const*)app->themes_dirs, 2, app->theme.id, &size);
    int from_file = text && gui_theme_file_read(&file, text, size);
    free(text);
    gui_editor_start(&app->editor, &app->theme, from_file ? &file : NULL, from_file ? app->theme.id : "mytheme");
    if (from_file) gui_theme_file_free(&file);
    app->editing = true;
    app->form = GUI_FORM_THEME;
    app->preview_since = 0;
}
/* the bytes of path, a folder or a file; for a game laid out beside the others its files beside it too */
static uint64_t space_of(const char* path, bool games, const int* stop) {
    static const char* const beside[] = {"_code.so", "_code.dll", ".exe", ".abi", ".hints", ".ini", ".log", "_data"};
    uint64_t bytes = snx_is_dir(path) ? gui_tree_size(path, stop) : snx_file_size(path);
    size_t i;
    for (i = 0; games && path[0] && !snx_is_dir(path) && i < sizeof beside / sizeof beside[0]; ++i) {
        char* file = snx_format("%s%s", path, beside[i]);
        bytes += snx_is_dir(file) ? gui_tree_size(file, stop) : snx_file_size(file);
        free(file);
    }
    return bytes;
}
static int SDLCALL count_space(void* data) {
    GuiSpace* space = (GuiSpace*)data;
    for (int i = 0; i < space->count; ++i) {
        space->bytes[i] = space->dirs[i][0] ? space_of(space->dirs[i], space->games, &space->stop) : 0;
        space->used += space->bytes[i];
        __atomic_store_n(&space->counted, i + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&space->done, 1, __ATOMIC_RELEASE);
    return 0;
}
static void space_end(GuiSpace** space, SDL_Thread** counter) {
    if (*counter) {
        __atomic_store_n(&(*space)->stop, 1, __ATOMIC_RELAXED);
        SDL_WaitThread(*counter, NULL);
        *counter = NULL;
    }
    free(*space);
    *space = NULL;
}
/* count folders' space (NULL or "" none) in place of the count before, on a thread; here if none can start */
static void space_count(GuiSpace** space, SDL_Thread** counter, char* const* dirs, int count, bool games) {
    size_t size = sizeof **space + (size_t)count * (sizeof(uint64_t) + sizeof(char*));
    char* at;
    int i;
    space_end(space, counter);
    for (i = 0; i < count; ++i) size += (dirs[i] ? strlen(dirs[i]) : 0) + 1;
    if (!(*space = (GuiSpace*)calloc(1, size))) return;
    (*space)->count = count;
    (*space)->games = games;
    (*space)->bytes = (uint64_t*)(*space + 1);
    (*space)->dirs = (char**)((*space)->bytes + count);
    for (i = 0, at = (char*)((*space)->dirs + count); i < count; at += strlen(at) + 1, ++i)
        (*space)->dirs[i] = strcpy(at, dirs[i] ? dirs[i] : "");
    if (!(*counter = SDL_CreateThread(count_space, "space", *space))) count_space(*space);
}
static void space_stop(App* app) { space_end(&app->space, &app->counter); }
/* the space left on the disk of path, or of the folder it will be made in */
static uint64_t free_space(const char* path) {
    char* at = snx_strdup(path);
    uint64_t left;
    while (!snx_is_dir(at)) {
        char* up = snx_dirname(at);
        int top = !strcmp(up, at);
        free(at);
        at = up;
        if (top) break;
    }
    left = snx_free_space(at);
    free(at);
    return left;
}
static void space_start(App* app) {
    /* settings, logs and caches, the menu's files among them; an older version's work/ is the data folder's */
    char* dirs[GUI_SPACE_DIRS] = {snx_strdup(app->games_dir),
                                  snx_path_join(app->data_dir, "work"),
                                  app->themes_dirs[0] ? snx_strdup(app->themes_dirs[0]) : NULL,
                                  menu_folder(app, SNX_USER_CONFIG, false),
                                  menu_folder(app, SNX_USER_STATE, false),
                                  menu_folder(app, SNX_USER_CACHE, false),
                                  app->place[GUI_PLACE_WORK] ? snx_strdup(app->place[GUI_PLACE_WORK]) : NULL};
    int i;
    app->free_bytes = free_space(app->data_dir);
    space_count(&app->space, &app->counter, dirs, GUI_SPACE_DIRS, false);
    for (i = 0; i < GUI_SPACE_DIRS; ++i) free(dirs[i]);
}
/* ---- the Settings: its groups, App then a group per section of the settings; a group's rows; the App's ---- */
static const GuiThemeKey app_keys[] = {
    {"", "Theme", GUI_KEY_ACTION, offsetof(GuiAppForm, theme), sizeof(((GuiAppForm*)0)->theme), NULL, 0,
     "Built-in themes, then each themes/<name>/theme.ini"},
    {"", "Text size", GUI_KEY_ACTION, offsetof(GuiAppForm, text), sizeof(((GuiAppForm*)0)->text), NULL, 0,
     "Bigger letters scroll the menu. Auto fits the screen"},
    {"", "Theme editor", GUI_KEY_ACTION, 0, 0, NULL, 0,
     "This theme's keys with a preview, saved as themes/<name>/theme.ini"},
    {"", "Game folders", GUI_KEY_ACTION, offsetof(GuiAppForm, folders), sizeof(((GuiAppForm*)0)->folders), NULL, 0,
     "Folders of games listed with games/"},
    {"", "Folders", GUI_KEY_ACTION, offsetof(GuiAppForm, places), sizeof(((GuiAppForm*)0)->places), NULL, 0,
     "Where the data, the games and the extractions are"},
    {"", "Clean old settings", GUI_KEY_ACTION, 0, 0, NULL, 0,
     "The settings the runtime and the launcher no longer read: Enter twice"}};
enum { GUI_APP_THEME, GUI_APP_TEXT, GUI_APP_EDITOR, GUI_APP_FOLDERS, GUI_APP_PLACES, GUI_APP_CLEAN, GUI_APP_ROWS };
static void app_values(App* app) {
    GuiAppForm* f = &app->app_form;
    int moved;
    snprintf(f->theme, sizeof f->theme, "%s", app->theme.name);
    if (app->menu.text_size)
        snprintf(f->text, sizeof f->text, "%dx", app->menu.text_size);
    else
        snprintf(f->text, sizeof f->text, "Auto");
    if (app->folder_count)
        snprintf(f->folders, sizeof f->folders, "games/ and %d more", app->folder_count);
    else
        snprintf(f->folders, sizeof f->folders, "games/");
    moved = (app->place[GUI_PLACE_DATA] != NULL) + (app->place[GUI_PLACE_GAMES] != NULL) +
            (app->place[GUI_PLACE_WORK] != NULL);
    if (moved)
        snprintf(f->places, sizeof f->places, "%d picked", moved);
    else
        snprintf(f->places, sizeof f->places, "Defaults");
}
/* ---- the player's game folders: listed after games/, kept a line each in folders_file ---- */
static void folders_load(App* app) {
    size_t size = 0;
    char *text = app->folders_file ? (char*)snx_read_file(app->folders_file, &size) : NULL, *line, *next;
    for (line = text; line && *line && app->folder_count < GUI_FOLDERS_MAX; line = next) {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        if (*line) {
            size_t n = strlen(line), deep = sizeof GUI_FOLDER_DEEP - 1;
            app->folders_deep[app->folder_count] = n > deep && !strcmp(line + n - deep, GUI_FOLDER_DEEP);
            if (app->folders_deep[app->folder_count]) line[n - deep] = 0;
            app->folders[app->folder_count++] = snx_strdup(line);
        }
    }
    free(text);
}
/* 0, else the note says why not */
static int folders_save(App* app) {
    size_t size = 1, n;
    char *text, *dir;
    int i, failed;
    if (!app->folders_file) return 0;
    for (i = 0; i < app->folder_count; ++i) size += strlen(app->folders[i]) + sizeof GUI_FOLDER_DEEP;
    text = (char*)malloc(size);
    for (i = 0, n = 0; text && i < app->folder_count; ++i)
        n += (size_t)snprintf(text + n, size - n, "%s%s\n", app->folders[i],
                              app->folders_deep[i] ? GUI_FOLDER_DEEP : "");
    dir = snx_dirname(app->folders_file);
    failed = !text || !dir || snx_mkdirs(dir) || snx_write_file(app->folders_file, text, n);
    if (failed)
        snprintf(app->editor.note, sizeof app->editor.note, "Not saved: %s could not be written", app->folders_file);
    free(dir);
    free(text);
    return failed;
}
static void open_folders(App* app, int row);
/* a folder the player picked, dropped or pasted: a folder that is there, once. What happened goes on the folders'
   screen, or in the menu's message when the picker answered after the player left it */
static void folder_add(App* app, const char* path, bool deep) {
    char *absolute = path ? snx_absolute(path) : NULL, note[sizeof app->editor.note];
    int i, added = 0, here = app->editing && app->form == GUI_FORM_FOLDERS;
    for (i = 0; absolute && i < app->folder_count && strcmp(app->folders[i], absolute); ++i) {}
    if (!absolute || !snx_is_dir(absolute))
        snprintf(note, sizeof note, "Not a folder: %s", path ? path : "(none)");
    else if (app->games_dir && !strcmp(absolute, app->games_dir))
        snprintf(note, sizeof note, "games/ is always listed");
    else if (i < app->folder_count)
        snprintf(note, sizeof note, "Already listed: %s", absolute);
    else if (app->folder_count == GUI_FOLDERS_MAX)
        snprintf(note, sizeof note, "At most %d folders", GUI_FOLDERS_MAX);
    else {
        app->folders_deep[app->folder_count] = deep;
        app->folders[app->folder_count++] = absolute;
        absolute = NULL;
        added = 1;
        snprintf(note, sizeof note, "%s",
                 folders_save(app) ? app->editor.note
                 : deep            ? "Added, with its subfolders"
                                   : "Added");
    }
    free(absolute);
    if (here && added) open_folders(app, app->folder_count);
    if (here)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
    else
        snprintf(app->message, sizeof app->message, "Game folders: %s", note);
}
static void folder_remove(App* app, int folder) {
    char note[160];
    free(app->folders[folder]);
    memmove(app->folders + folder, app->folders + folder + 1, (size_t)(app->folder_count - folder - 1) * sizeof(char*));
    memmove(app->folders_deep + folder, app->folders_deep + folder + 1,
            (size_t)(app->folder_count - folder - 1) * sizeof(bool));
    --app->folder_count;
    note[0] = 0;
    if (folders_save(app)) snprintf(note, sizeof note, "%s", app->editor.note);
    open_folders(app, SDL_min(folder + 1, app->folder_count));
    snprintf(app->editor.note, sizeof app->editor.note, "%s", note[0] ? note : "Taken off the list; the folder stays");
}
/* a folder picked: one with folders in it asks first whether their games are listed too (Only this folder, With
   subfolders); another is added as it is */
static void folder_ask(App* app, const char* path) {
    char* absolute = path ? snx_absolute(path) : NULL;
    if (absolute && snx_is_dir(absolute) && gui_has_folders(absolute, GUI_GAMES_RUN | GUI_GAMES_CIA)) {
        free(app->folder_asked);
        app->folder_asked = absolute;
        gui_confirm_open(&app->confirm, GUI_ASK_DEEP, false);
        return;
    }
    free(absolute);
    folder_add(app, path, false);
}
/* its answer: a choice taken adds the folder as it says, with its subfolders (deep) or not; Back adds nothing */
static void folder_answer(App* app, bool taken, bool deep) {
    char* path = app->folder_asked;
    app->folder_asked = NULL;
    if (taken)
        folder_add(app, path, deep);
    else if (app->editing && app->form == GUI_FORM_FOLDERS)
        snprintf(app->editor.note, sizeof app->editor.note, "Not added");
    free(path);
}
/* the system's folder picker answers on its own thread, as the file picker */
static void SDLCALL folder_picked(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.code = files == NULL; /* an error, which SDL_GetError names */
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    e.user.data2 = files ? NULL : SDL_strdup(SDL_GetError());
    SDL_PushEvent(&e);
}
/* a group's row of the Settings, its help the keys it holds */
static void group_add(App* app, const char* name, int first, const char* about) {
    int g = app->groups++;
    GuiThemeKey* k = &app->group_keys[g];
    memset(k, 0, sizeof *k);
    k->section = "";
    k->key = name;
    k->type = GUI_KEY_ACTION;
    k->about = app->group_about[g];
    snprintf(app->group_about[g], sizeof app->group_about[g], "%s", about);
    app->group_first[g] = first;
    app->group_rows[g] = 0;
}
static void settings_groups(App* app) {
    GuiSettings* s = settings_of(app);
    int r;
    app->groups = 0;
    group_add(app, "App", 0, "The theme, the menu's letters, the theme editor and the folders");
    for (r = 0; r < s->count; ++r) {
        int g = app->groups - 1;
        char* about = app->group_about[g];
        size_t n = strlen(about);
        if (strchr(s->keys[r].section, '/')) continue; /* a page's: its row in the group opens it */
        /* past the most groups the rest go in the last one: every row is still reached */
        if (g == 0 || (strcmp(s->keys[r].section, app->group_keys[g].key) && app->groups < GUI_GROUPS_MAX)) {
            group_add(app, s->keys[r].section, r, "");
            g = app->groups - 1;
            about = app->group_about[g];
            n = 0;
        }
        snprintf(about + n, sizeof app->group_about[g] - n, "%s%s", n ? ", " : "", s->keys[r].key);
        ++app->group_rows[g];
    }
}
/* the groups, row on the focus */
static void settings_top(App* app, int row) {
    settings_groups(app);
    gui_editor_form(&app->editor, app->group_keys, app->groups, &app->app_form, "Reset all to the defaults");
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_SETTINGS;
    app->editing = true;
}
static void open_app(App* app, int row) {
    app_values(app);
    gui_editor_form(&app->editor, app_keys, GUI_APP_ROWS, &app->app_form, NULL);
    app->editor.flat = true;
    app->editor.row = row;
    app->form = GUI_FORM_APP;
    app->group = 0;
    app->editing = true;
}
/* games/, then the player's folders, a row each with its state, and Add a folder */
static void open_folders(App* app, int row) {
    int i;
    for (i = 0; i <= app->folder_count; ++i) {
        GuiThemeKey* k = &app->folder_keys[i];
        const char* path = i ? app->folders[i - 1] : app->games_dir;
        memset(k, 0, sizeof *k);
        k->section = "";
        k->key = path ? path : "games/";
        k->type = GUI_KEY_ACTION;
        k->at = offsetof(GuiAppForm, folder) + (size_t)i * sizeof app->app_form.folder[0];
        k->size = sizeof app->app_form.folder[0];
        k->about = i ? "Del, or Enter twice: take it off the list; the folder stays"
                     : "The data folder's games/, always "
                       "listed";
        snprintf(app->app_form.folder[i], sizeof app->app_form.folder[i], "%s",
                 !path || !snx_is_dir(path)      ? "not found"
                 : i && app->folders_deep[i - 1] ? "+ subfolders"
                                                 : "");
    }
    gui_editor_form(&app->editor, app->folder_keys, app->folder_count + 1, &app->app_form, "Add a folder");
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_FOLDERS;
    app->folder_armed = 0;
    app->editing = true;
}
/* ---- Settings > App > Folders: the data folder, games/ and the extractions, each picked or its default ---- */
static const char* const place_labels[GUI_PLACES] = {"Data", "Games", "Extractions"};
static const char* const place_holds[GUI_PLACES] = {"games/, themes/ and prod.keys", "The recompiled games",
                                                    "Each dump's extraction"};
#ifdef SDL_PLATFORM_ANDROID
#define PLACE_HOW "Tap: pick another. The last row: all back to the defaults"
#else
#define PLACE_HOW "Enter: pick another. Del: the default"
#endif
/* the folders as Folders has them: each one picked, else its default from the data folder, the menu's own one first */
static void places_apply(App* app) {
    const char* data = app->place[GUI_PLACE_DATA] ? app->place[GUI_PLACE_DATA] : app->home_dir;
    char *copy, *work;
    if (!data) return;
    copy = snx_strdup(data);
    free(app->data_dir);
    app->data_dir = copy;
    free(app->games_dir);
    app->games_dir =
        app->place[GUI_PLACE_GAMES] ? snx_strdup(app->place[GUI_PLACE_GAMES]) : snx_path_join(app->data_dir, "games");
    free(app->themes_dirs[0]);
    app->themes_dirs[0] = snx_path_join(app->data_dir, "themes");
    /* where the driver puts them by itself (snx_work_dir) */
    work = snx_path_join(app->data_dir, "work");
    free(app->work_dir);
    app->work_dir =
        app->place[GUI_PLACE_WORK] ? snx_strdup(app->place[GUI_PLACE_WORK]) : snx_user_path(work, SNX_USER_CACHE);
    free(work);
    free(app->keys_path);
    app->keys_path = device_keys_path(app);
}
static const char* place_path(const App* app, int place) {
    return place == GUI_PLACE_DATA ? app->data_dir : place == GUI_PLACE_GAMES ? app->games_dir : app->work_dir;
}
/* a folder outside the app's own storage on Android, where every file access goes through its checks (FUSE) */
static bool outside_app(const App* app, const char* path) {
    return app->own_dir && !snx_path_within(path, app->own_dir);
}
/* the three rows: default or picked, or why the picked one cannot be; above, what it holds and its disk's space, and
   under the rows the folder in use */
static void open_places(App* app, int row) {
    int i;
    for (i = 0; i < GUI_PLACES; ++i) {
        GuiThemeKey* k = &app->place_keys[i];
        const char* path = place_path(app, i);
        const char* why = path && app->place[i] ? snx_folder_unfit(path, i == GUI_PLACE_GAMES) : NULL;
        char left[32];
        memset(k, 0, sizeof *k);
        k->section = "";
        k->key = place_labels[i];
        k->type = GUI_KEY_ACTION;
        k->at = offsetof(GuiAppForm, place) + (size_t)i * sizeof app->app_form.place[0];
        k->size = sizeof app->app_form.place[0];
        k->about = app->place_about[i];
        gui_size_text(left, sizeof left, path ? free_space(path) : 0);
        snprintf(app->app_form.place[i], sizeof app->app_form.place[i], "%s",
                 why                                       ? why
                 : app->place[i] && outside_app(app, path) ? "outside the app"
                 : app->place[i]                           ? "picked"
                                                           : "default");
        snprintf(app->place_about[i], sizeof app->place_about[i], "%s, %s free. %s", place_holds[i], left, PLACE_HOW);
        snprintf(app->place_line[i], sizeof app->place_line[i], "%s%s%s", path ? path : "(none)", why ? " " : "",
                 why ? why : "");
    }
    gui_editor_form(&app->editor, app->place_keys, GUI_PLACES, &app->app_form, "All back to the defaults");
    app->editor.flat = true;
    app->editor.row = SDL_min(row, gui_editor_rows(&app->editor) - 1);
    app->form = GUI_FORM_PLACES;
    app->editing = true;
}
/* a place's note: on the form while it shows, else in the menu's message */
static void place_note(App* app, const char* note) {
    if (app->editing && app->form == GUI_FORM_PLACES)
        snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
    else
        snprintf(app->message, sizeof app->message, "Folders: %s", note);
}
/* a place picked, absolute, or its default (NULL), kept in app.ini */
static void place_use(App* app, int place, char* absolute) {
    char note[sizeof app->editor.note];
    free(app->place[place]);
    app->place[place] = absolute;
    places_apply(app);
    space_start(app);
    if (app->editing && app->form == GUI_FORM_PLACES) open_places(app, place);
    if (app->app_file && app_save(app))
        snprintf(note, sizeof note, "Used, not kept for the next start: %s could not be written", app->app_file);
    else
        snprintf(note, sizeof note, "%s: %s%s", place_labels[place], place_path(app, place),
                 absolute ? ". The folder before keeps what it has" : ", the default");
    place_note(app, note);
}
/* a place picked (path), or its default (NULL). Only where the folders are changes: nothing in them is moved. A folder
   that cannot take what goes there is refused, never replaced by the default; one outside the app is asked about */
static void place_set(App* app, int place, const char* path) {
    char *absolute = path ? snx_absolute(path) : NULL, note[sizeof app->editor.note];
    const char* why = path ? (absolute ? snx_folder_unfit(absolute, place == GUI_PLACE_GAMES) : "is not there") : NULL;
    if (why) {
        snprintf(note, sizeof note, "Not changed: %s %s", path, why);
        free(absolute);
        place_note(app, note);
        return;
    }
    if (absolute && outside_app(app, absolute)) {
        free(app->outside_path);
        app->outside_path = absolute;
        app->outside_place = place;
        gui_confirm_open(&app->confirm, GUI_ASK_OUTSIDE, true);
        return;
    }
    place_use(app, place, absolute);
}
/* its answer: Use it, pressed twice, takes the folder; Back keeps the one in use */
static void outside_answer(App* app, bool taken) {
    char* path = app->outside_path;
    app->outside_path = NULL;
    if (taken)
        place_use(app, app->outside_place, path);
    else {
        free(path);
        place_note(app, "Not changed");
    }
}
/* the form's last row: every place back to its default */
static void places_reset(App* app) {
    int i, picked = 0;
    for (i = 0; i < GUI_PLACES; ++i) {
        picked += app->place[i] != NULL;
        free(app->place[i]);
        app->place[i] = NULL;
    }
    if (!picked) {
        place_note(app, "They are the defaults");
        return;
    }
    places_apply(app);
    space_start(app);
    if (app->editing && app->form == GUI_FORM_PLACES) open_places(app, GUI_PLACES);
    if (app->app_file && app_save(app)) {
        char note[sizeof app->editor.note];
        snprintf(note, sizeof note, "Used, not kept for the next start: %s could not be written", app->app_file);
        place_note(app, note);
    } else
        place_note(app, "All back to the defaults. The folders before keep what they have");
}
/* a run's folders picked in Folders, each still able to take what goes there: else out says which, and why */
static bool places_fit(const App* app, char* out, size_t size) {
    int i;
    for (i = 0; i < GUI_PLACES; ++i) {
        const char* why = app->place[i] ? snx_folder_unfit(app->place[i], i == GUI_PLACE_GAMES) : NULL;
        if (why) {
            snprintf(out, size, "%s folder %s %s. Settings > App > Folders picks another.", place_labels[i],
                     app->place[i], why);
            return false;
        }
    }
    return true;
}
/* a Switch homebrew's .nro, which runs without an extraction */
static bool nro_file(const char* path) {
    size_t n = strlen(path);
    return n > 4 && path[n - 4] == '.' && SDL_tolower((unsigned char)path[n - 3]) == 'n' &&
           SDL_tolower((unsigned char)path[n - 2]) == 'r' && SDL_tolower((unsigned char)path[n - 1]) == 'o';
}
/* a dump's extraction in the picked extractions folder, as the driver names it in its own: <work>[/<kind>]/<name> */
static char* dump_out(const App* app, const char* file, const char* kind) {
    char *name = snx_dump_name(file), *dir = kind ? snx_path_join(app->work_dir, kind) : snx_strdup(app->work_dir);
    char* out = snx_path_join(dir, name);
    free(name);
    free(dir);
    return out;
}
/* room on the disks for file's run (gui_room), not extracted yet; else room_line says what it takes. The free space is
   room_free in tests */
static bool room_fits(App* app, const char* file) {
    bool copy = app->menu.selected == GUI_SWITCH, fits = true;
    char *out, need_text[32], left_text[32];
    uint64_t need = 0, left = 0;
    if (copy && nro_file(file)) return true; /* no extraction */
    out = dump_out(app, file, copy ? NULL : "3ds");
    if (!snx_is_dir(out)) /* else extracted already, and its game made with it */
        fits = gui_room(snx_file_size(file), copy, app->room_free ? app->room_free : free_space(app->work_dir),
                        app->room_free ? app->room_free : free_space(app->games_dir),
                        snx_same_disk(app->work_dir, app->games_dir), &need, &left);
    if (!fits) {
        gui_size_text(need_text, sizeof need_text, need);
        gui_size_text(left_text, sizeof left_text, left);
        snprintf(app->room_line, sizeof app->room_line, "Needs about %s, %s free.", need_text, left_text);
    }
    free(out);
    return fits;
}
static void open_folder_picker(App* app) {
    if (!app->picked_folder) app->picked_folder = SDL_RegisterEvents(1);
    if (!app->picked_folder) return;
#ifdef SDL_PLATFORM_ANDROID
    { /* SDL has no folder picker there: the system's own, through the app */
        char message[sizeof app->editor.note - 40];
        if (gui_android_pick_folder(folder_picked, &app->picked_folder, message, sizeof message))
            ;
        else if (app->editing)
            snprintf(app->editor.note, sizeof app->editor.note, "The folder picker could not open: %s", message);
        else /* Games' Add a folder */
            snprintf(app->message, sizeof app->message, "Game folders: the folder picker could not open: %s", message);
    }
#elif defined(SDL_PLATFORM_IOS)
    { /* nor there: the document picker (dist/apple) */
        char message[sizeof app->editor.note - 40];
        if (gui_apple_pick(app->window, 1, folder_picked, &app->picked_folder, message, sizeof message))
            ;
        else if (app->editing)
            snprintf(app->editor.note, sizeof app->editor.note, "The folder picker could not open: %s", message);
        else
            snprintf(app->message, sizeof app->message, "Game folders: the folder picker could not open: %s", message);
    }
#else
    SDL_ShowOpenFolderDialog(folder_picked, &app->picked_folder, app->window, NULL, false);
#endif
}
/* Select on a folder's row: once arms it, twice takes it off; games/ stays */
static void folder_select(App* app, int row) {
    if (!row)
        snprintf(app->editor.note, sizeof app->editor.note, "games/ is always listed");
    else if (app->folder_armed == row)
        folder_remove(app, row - 1);
    else {
        app->folder_armed = row;
        snprintf(app->editor.note, sizeof app->editor.note, "Enter again takes it off the list");
    }
}
/* the setting a row of the open group or page is, and the row a setting is (-1: none) */
static int setting_of(const App* app, int row) { return app->view_count ? app->view_rows[row] : app->first + row; }
static int row_of(const App* app, int setting) {
    int r;
    if (!app->view_count) return setting - app->first;
    for (r = 0; r < app->view_count && app->view_rows[r] != setting; ++r) {}
    return r < app->view_count ? r : -1;
}
/* a player's key, gamepad_p<N>_name: N, else 0 */
static int player_key(const char* key, const char* name) {
    int n = 0, at = 0;
    if (!key || sscanf(key, "gamepad_p%d_%n", &n, &at) != 1 || !at || (name && strcmp(key + at, name))) return 0;
    return n >= 1 && n <= SNX_GUI_PLAYERS ? n : 0;
}
/* group g's rows, wherever they are: its section's own, and its pages' rows (a second page's comes after the first
   page's rows) */
static int in_group(const App* app, int g, int r) {
    return !strcmp(app->settings.keys[r].section, app->group_keys[g].key);
}
/* group g's page row of the players (the runtime's "Player" page), -1 for none */
static int players_row(const App* app, int g) {
    int r;
    for (r = 0; g > 0 && g < app->groups && r < app->settings.count; ++r)
        if (in_group(app, g, r) && app->settings.values[r].kind == GUI_SETTING_PAGE &&
            !strcmp(app->settings.keys[r].key, "Player"))
            return r;
    return -1;
}
/* Controllers by player, as Eden's tabs, into rows: its Player row first; every player's rows for player 0, player
   N's own for N with the rows no player has its own of. How many, 0 for a group without players */
static int player_rows(App* app, int g, int p, int* rows) {
    int players = players_row(app, g), r, n = 0;
    GuiSetting* row;
    if (players < 0) return 0;
    row = &app->settings.values[players];
    rows[n++] = players;
    for (r = row->first; p && r < row->first + row->count && n < GUI_VIEW_ROWS; ++r)
        if (player_key(app->settings.values[r].key, NULL) == p) rows[n++] = r; /* the player's, its page's */
    for (r = 0; r < app->settings.count && n < GUI_VIEW_ROWS; ++r) {
        const char* key = app->settings.values[r].key ? app->settings.values[r].key : "";
        if (r == players || !in_group(app, g, r)) continue;
        if (p && (app->settings.values[r].kind == GUI_SETTING_PAGE || !strcmp(key, "gamepad_swap_ab") ||
                  !strcmp(key, "gamepad_rumble")))
            continue; /* each player has its own of these */
        rows[n++] = r;
    }
    return n;
}
/* the open group as a view of its rows by player, its Player row saying which (< All > or < N >) */
static void group_view(App* app, int g) {
    int r, players = players_row(app, g);
    app->view_count = player_rows(app, g, app->player, app->view_rows);
    if (!app->view_count) return;
    if (app->player)
        snprintf(app->settings.values[players].text, sizeof app->settings.values[players].text, "< %d >", app->player);
    else
        snprintf(app->settings.values[players].text, sizeof app->settings.values[players].text, "< All >");
    for (r = 0; r < app->view_count; ++r) app->view_keys[r] = app->settings.keys[app->view_rows[r]];
    app->view_keys[0].about = "Each player's own, as Eden's tabs: Left/Right go round All and players 1 to 8";
    gui_editor_form(&app->editor, app->view_keys, app->view_count, app->settings.values, NULL);
    app->editor.flat = true;
}
static void open_group(App* app, int g) {
    if (!g) {
        open_app(app, 0);
        return;
    }
    app->view_count = 0;
    gui_editor_form(&app->editor, app->settings.keys + app->group_first[g], app->group_rows[g], app->settings.values,
                    NULL);
    app->editor.flat = true;
    app->form = GUI_FORM_GROUP;
    app->group = g;
    app->first = app->group_first[g];
    app->page = -1;
    group_view(app, g);
}
static void open_pad(App* app, int row);
static bool pad_page(const App* app, int row);
/* a page's rows (settings.h), from its row in the group; the button map's, the pad drawn */
static void open_page(App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    if (pad_page(app, row)) {
        open_pad(app, row);
        return;
    }
    gui_editor_form(&app->editor, app->settings.keys + page->first, page->count, app->settings.values, NULL);
    app->editor.flat = true;
    app->view_count = 0;
    app->first = page->first;
    app->page = row;
}
static void open_settings(App* app) {
    app->group = 0;
    settings_top(app, 0);
    space_start(app);
}
static void leave_editor(App* app) {
    gui_editor_stop(&app->editor);
    app->editing = false;
    SDL_StopTextInput(app->window);
}
static void pad_close(App* app);
/* ESC on a screen: a group's and the App's go back to the groups, the theme editor to where it was opened */
static void device_stop(App* app);
static void leave_form(App* app) {
    int form = app->form;
    device_stop(app);
    leave_editor(app);
    if (form == GUI_FORM_PAD) pad_close(app);
    if ((form == GUI_FORM_GROUP || form == GUI_FORM_PAD) && app->page >= 0) { /* a page: back to its row */
        int row = app->page;
        open_group(app, app->group);
        app->editor.row = SDL_max(row_of(app, row), 0);
        app->editing = true;
    } else if (form == GUI_FORM_GROUP || form == GUI_FORM_APP)
        settings_top(app, app->group);
    else if (form == GUI_FORM_FOLDERS)
        open_app(app, GUI_APP_FOLDERS);
    else if (form == GUI_FORM_PLACES)
        open_app(app, GUI_APP_PLACES);
    else if (form == GUI_FORM_THEME && app->theme_from_app) {
        app->theme_from_app = false;
        open_app(app, GUI_APP_EDITOR);
    }
}
static void clean_row(App* app);
/* the App's rows: the theme and the letters go round, either way, and the editor opens */
static void app_action(App* app, int row, int step) {
    if (row == GUI_APP_THEME)
        next_theme(app, step < 0 ? -1 : 1);
    else if (row == GUI_APP_TEXT)
        app->menu.text_size = (app->menu.text_size + (step < 0 ? GUI_TEXT_MAX : 1)) % (GUI_TEXT_MAX + 1);
    else if (row == GUI_APP_EDITOR && !step) {
        app->theme_from_app = true;
        open_editor(app);
        return;
    } else if (row == GUI_APP_FOLDERS && !step) {
        open_folders(app, 0);
        return;
    } else if (row == GUI_APP_PLACES && !step) {
        open_places(app, 0);
        return;
    } else if (row == GUI_APP_CLEAN) {
        if (!step) clean_row(app);
        return;
    }
    if (row == GUI_APP_THEME || row == GUI_APP_TEXT) app_save(app);
    app_values(app);
}
/* saved in themes/ of the data folder, and the menu's theme from then on */
static void save_editor(App* app) {
    const char* const* dirs = (const char* const*)app->themes_dirs;
    if (gui_editor_save(&app->editor, app->themes_dirs[0])) return;
    gui_themes_scan(&app->themes, dirs, 2);
    gui_theme_load(&app->theme, dirs, 2, app->editor.id);
    app_save(app);
}
/* a Settings pick, kept at once */
static void keep_settings(App* app) {
    if ((app->form != GUI_FORM_GROUP && app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_PAD) ||
        !app->editor.dirty)
        return;
    app->editor.dirty = false;
    gui_settings_sync(&app->settings);
    if (app->settings_file && gui_settings_save(&app->settings, app->settings_file))
        snprintf(app->editor.note, sizeof app->editor.note, "Not saved: %s could not be written", app->settings_file);
}
static int setting_row(const App* app, const char* key) {
    int i;
    for (i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].key && !strcmp(app->settings.values[i].key, key)) return i;
    return -1;
}
/* the page gamepad_map is on */
static bool pad_page(const App* app, int row) {
    const GuiSetting* page = &app->settings.values[row];
    int map = setting_row(app, "gamepad_map");
    return map >= 0 && map >= page->first && map < page->first + page->count;
}
/* the pad's name, its labels and what it holds now */
static void pad_sync(App* app) {
    SnxPadScreen* p = &app->pad;
    const char* name = app->gamepad ? SDL_GetGamepadName(app->gamepad) : NULL;
    size_t n = 0;
    int c;
    if (app->gamepad && !name) name = "Controller";
    while (name && *name && n + 1 < sizeof p->pad) /* in the font's glyphs, as the page's other texts */
        p->pad[n++] = (char)gui_glyph(SDL_StepUTF8(&name, NULL));
    p->pad[n] = 0;
    p->held = 0;
    if (!app->gamepad) {
        memset(p->labels, 0, sizeof p->labels);
        p->symmetric = 0;
        return;
    }
    snx_pad_labels(app->gamepad, p->labels);
    p->symmetric = snx_pad_symmetric(app->gamepad);
    for (c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (snx_pad_held(app->gamepad, c)) p->held |= 1u << c;
}
/* the map as gamepad_map says, A and B as gamepad_swap_ab does; a map it cannot read is named, and left out. Why there is
   no pad stays */
static void pad_start(App* app) {
    const GuiSettings* s = &app->settings;
    const char* swap = app->pad_swap >= 0 ? gui_settings_value(s, s->values[app->pad_swap].key) : NULL;
    int map = app->pad_map, on = swap ? atoi(swap) : -1;
    char error[64], note[sizeof app->pad.note];
    snprintf(note, sizeof note, "%s", app->pad.note);
    snx_pad_screen_start(&app->pad);
    snprintf(app->pad.note, sizeof app->pad.note, "%s", note);
    if (on < 0) { /* a player's "As All": every player's */
        swap = gui_settings_value(s, "gamepad_swap_ab");
        on = swap && atoi(swap);
    }
    app->pad.swap_ab = on;
    if (snx_pad_map_read(&app->pad.own, s->values[map].text, error, sizeof error))
        snprintf(app->editor.note, sizeof app->editor.note,
                 "gamepad_map's \"%s\" is not a control and the button it sends (south:B)", error);
    pad_sync(app);
}
/* the SDL mappings the page and the games read: SDL_GAMECONTROLLERCONFIG_FILE's, else gamecontrollerdb.txt beside
   settings.ini; NULL without a settings file */
static char* pad_mappings_file(const App* app) {
    const char* named = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE);
    char *folder, *path;
    if (named && *named) return snx_strdup(named);
    folder = app->settings_file ? snx_dirname(app->settings_file) : NULL;
    path = folder ? snx_path_join(folder, "gamecontrollerdb.txt") : NULL;
    free(folder);
    return path;
}
#ifndef SDL_PLATFORM_ANDROID
/* the mappings file the page names, for the games the launcher starts: one outside its games/ (a folder the player
   added) would read the user's otherwise. A file the player named stays theirs */
static void pad_mappings_share(const App* app) {
    char* file;
    if (SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE")) return;
    file = pad_mappings_file(app);
    if (file && snx_is_file(file)) SDL_setenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE", file, 1);
    free(file);
}
#endif
/* the first pad plugged in, when none is open */
static void pad_attach(App* app) {
    SDL_JoystickID* ids;
    int i, n = 0;
    if (app->gamepad || !app->pads_read) return;
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && !app->gamepad; i++) {
        app->gamepad = SDL_OpenGamepad(ids[i]);
        if (!app->gamepad)
            snprintf(app->pad.note, sizeof app->pad.note, "The controller could not be opened: %s", SDL_GetError());
    }
    SDL_free(ids);
    if (app->gamepad) app->pad.note[0] = 0; /* an earlier failed/unmapped pad no longer describes this one */
    if (!app->gamepad && !n) {              /* none, or a joystick SDL has no mapping for, named */
        SDL_JoystickID* joysticks = SDL_GetJoysticks(&n);
        app->pad.note[0] = 0;
        if (joysticks && n > 0) {
            char guid[33], *file = pad_mappings_file(app);
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(joysticks[0]), guid, sizeof guid);
            snprintf(app->pad.note, sizeof app->pad.note, "%s has no SDL mapping: add a line for GUID %s to %s",
                     SDL_GetJoystickNameForID(joysticks[0]) ? SDL_GetJoystickNameForID(joysticks[0]) : "The joystick",
                     guid, file ? file : "gamecontrollerdb.txt");
            free(file);
        }
        SDL_free(joysticks);
    }
    app->pad_pushed = 0;
    pad_sync(app);
}
static void pad_load_mappings(App* app) {
    const char* named = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE);
    char* path;
    if (named && *named) return; /* SDL loaded the explicit file at subsystem startup */
    path = pad_mappings_file(app);
    if (path && snx_is_file(path) && SDL_AddGamepadMappingsFromFile(path) < 0)
        snprintf(app->editor.note, sizeof app->editor.note, "SDL mappings could not be read: %s", SDL_GetError());
    free(path);
}
static void render(App* app);
/* the button map of map's row (row: what to come back to), A and B as swap's row says, for player (0: every one) */
static void open_pad_on(App* app, int row, int map, int swap, int player) {
    leave_editor(app);
    app->editing = true;
    app->form = GUI_FORM_PAD;
    app->page = row;
    app->pad_map = map;
    app->pad_swap = swap;
    app->pad_player = player;
    app->editor.note[0] = 0;
    app->pad.note[0] = 0;
    pad_start(app);
    render(app); /* SDL looks for pads for most of a second: the page shows first, saying so */
    /* two Joy-Cons one pad, or two, as the games take them (gamepad_joycon_pair, 1 when not picked) */
    {
        const char* pair = gui_settings_value(&app->settings, "gamepad_joycon_pair");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, pair && !atoi(pair) ? "0" : "1");
    }
    app->pads_read = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (!app->pads_read)
        snprintf(app->pad.note, sizeof app->pad.note, "Controllers cannot be read: %s", SDL_GetError());
    else
        pad_load_mappings(app);
    pad_attach(app);
}
/* the Button map page's: every player's map */
static void open_pad(App* app, int row) {
    open_pad_on(app, row, setting_row(app, "gamepad_map"), setting_row(app, "gamepad_swap_ab"), 0);
}
/* player N's map row's, its A and B as its own swap says */
static void open_player_pad(App* app, int map) {
    char key[32];
    int p = player_key(app->settings.values[map].key, "map");
    snprintf(key, sizeof key, "gamepad_p%d_swap_ab", p);
    open_pad_on(app, map, map, setting_row(app, key), p);
}
static void pad_close(App* app) {
    if (app->gamepad) SDL_CloseGamepad(app->gamepad);
    app->gamepad = NULL;
    if (app->pads_read) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    app->pads_read = false;
}
/* what the page did: the map kept at once, as the Settings' other picks */
static void pad_did(App* app, int did) {
    GuiSettings* s = &app->settings;
    int map = app->pad_map;
    if (did == SNX_PAD_DID_LEAVE) {
        leave_form(app);
        return;
    }
    if (did != SNX_PAD_DID_MAP) return;
    snx_pad_map_write(&app->pad.own, s->values[map].text, sizeof s->values[map].text);
    app->editor.note[0] = 0; /* a map it could not read is replaced */
    app->editor.dirty = true;
    keep_settings(app);
}
/* a control of the pad: while the page waits, the press it waits for; else the key the console's button it sends
   is in the game's menu: A selects, B goes back, the D-pad moves */
static void pad_control(App* app, int control) {
    static const int KEYS[SNX_PAD_BUTTONS] = {
        SNX_PAD_KEY_SELECT, SNX_PAD_KEY_BACK, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, SNX_PAD_KEY_LEFT, SNX_PAD_KEY_UP,
        SNX_PAD_KEY_RIGHT,  SNX_PAD_KEY_DOWN};
    SnxPadMap map;
    int bit;
    if (control < 0) return;
    if (app->pad.listening) {
        pad_did(app, snx_pad_screen_press(&app->pad, control));
        return;
    }
    snx_pad_screen_map(&app->pad, &map);
    bit = map.bit[control];
    if (bit >= 0 && bit < SNX_PAD_BUTTONS && KEYS[bit] >= 0) pad_did(app, snx_pad_screen_key(&app->pad, KEYS[bit]));
}
/* a trigger past SNX_PAD_TRIGGER is its control pressed, the left stick past half way the D-pad: once as each crosses */
static void pad_axis(App* app, const SDL_GamepadAxisEvent* a) {
    int side;
    if (a->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
        unsigned bit = 1u << (a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        bool on = a->value > SNX_PAD_TRIGGER, was = (app->pad_pushed & bit) != 0;
        app->pad_pushed = on ? app->pad_pushed | bit : app->pad_pushed & ~bit;
        if (on && !was) pad_control(app, a->axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? SNX_PAD_RT : SNX_PAD_LT);
        return;
    }
    if (a->axis != SDL_GAMEPAD_AXIS_LEFTX && a->axis != SDL_GAMEPAD_AXIS_LEFTY) return;
    for (side = 0; side < 2; ++side) {
        bool y = a->axis == SDL_GAMEPAD_AXIS_LEFTY, on = side ? a->value > 16384 : a->value < -16384;
        unsigned bit = 4u << (2 * y + side);
        bool was = (app->pad_pushed & bit) != 0;
        app->pad_pushed = on ? app->pad_pushed | bit : app->pad_pushed & ~bit;
        if (on && !was)
            pad_did(app, snx_pad_screen_key(&app->pad, y ? (side ? SNX_PAD_KEY_DOWN : SNX_PAD_KEY_UP)
                                                         : (side ? SNX_PAD_KEY_RIGHT : SNX_PAD_KEY_LEFT)));
    }
}
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key);
/* the page's keys; Del puts the map back to the .ini's */
static void pad_keyboard(App* app, const SDL_KeyboardEvent* k) {
    GuiKey key;
    if (k->key == SDLK_TAB && !k->repeat) { /* the next screen, as the foot says, the pads closed */
        screen_tab(app, k->mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (k->key == SDLK_DELETE || k->key == SDLK_BACKSPACE) {
        int focus = app->pad.focus;
        gui_settings_reset(&app->settings, app->pad_map);
        app->editor.note[0] = 0;
        pad_start(app);
        app->pad.focus = focus;
        app->editor.dirty = true;
        keep_settings(app);
        return;
    }
    if (!menu_key_of(k, &key)) return;
    switch (key) {
        case GUI_UP: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_UP)); break;
        case GUI_DOWN: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_DOWN)); break;
        case GUI_LEFT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_LEFT)); break;
        case GUI_RIGHT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_RIGHT)); break;
        case GUI_SELECT: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_SELECT)); break;
        case GUI_CANCEL: pad_did(app, snx_pad_screen_key(&app->pad, SNX_PAD_KEY_BACK)); break;
        default: break;
    }
}
/* the page's events: keys, its pad's presses, pads plugged in and out; false for what the rest takes */
/* every controller there, open while a Controller row waits: one already open stays as it is */
static void device_open(App* app) {
    SDL_JoystickID* ids;
    int i, k, n = 0;
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && app->listened_count < (int)SDL_arraysize(app->listened); ++i) {
        for (k = 0; k < app->listened_count && SDL_GetGamepadID(app->listened[k]) != ids[i]; ++k) {}
        if (k == app->listened_count && (app->listened[k] = SDL_OpenGamepad(ids[i])) != NULL) ++app->listened_count;
    }
    SDL_free(ids);
}
/* a player's Controller row: Enter waits for a press on a controller, which the player then takes, by its GUID ("/2"
   after it for the second of two alike), as Eden's Input Device. ESC leaves it as it was */
static void device_listen(App* app, int row) {
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        snprintf(app->editor.note, sizeof app->editor.note, "Controllers cannot be read: %s", SDL_GetError());
        return;
    }
    pad_load_mappings(app);
    app->device_listen = row + 1;
    device_open(app);
    snprintf(app->editor.note, sizeof app->editor.note, "Press a button on player %d's controller. ESC: as it was",
             player_key(app->settings.values[row].key, "device"));
}
static void device_stop(App* app) {
    if (!app->device_listen) return;
    app->device_listen = 0;
    while (app->listened_count) SDL_CloseGamepad(app->listened[--app->listened_count]);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
/* the controller pressed, the player's from now on */
static void device_take(App* app, SDL_JoystickID id) {
    GuiSetting* v = &app->settings.values[app->device_listen - 1];
    SDL_JoystickID* ids;
    char guid[33], one[33];
    const char* name = SDL_GetGamepadNameForID(id);
    int i, n = 0, alike = 0;
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, sizeof guid);
    ids = SDL_GetGamepads(&n);
    for (i = 0; ids && i < n && ids[i] != id; ++i) { /* the ones alike before it */
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(ids[i]), one, sizeof one);
        alike += !SDL_strcasecmp(one, guid);
    }
    SDL_free(ids);
    if (alike)
        snprintf(v->text, sizeof v->text, "%s/%d", guid, alike + 1);
    else
        snprintf(v->text, sizeof v->text, "%s", guid);
    snprintf(app->editor.note, sizeof app->editor.note, "Player %d: %s", player_key(v->key, "device"),
             name ? name : "the controller pressed");
    device_stop(app);
    app->editor.dirty = true;
    keep_settings(app);
}
/* while a Controller row waits: a press takes its controller, ESC stops; every other event waits too */
static bool device_event(App* app, const SDL_Event* e) {
    GuiKey key;
    if (!app->device_listen) return false;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
        device_take(app, e->gbutton.which);
    else if (e->type == SDL_EVENT_GAMEPAD_ADDED) /* plugged in while it waits: its press counts too */
        device_open(app);
    else if (e->type == SDL_EVENT_KEY_DOWN && menu_key_of(&e->key, &key) && key == GUI_CANCEL) {
        device_stop(app);
        snprintf(app->editor.note, sizeof app->editor.note, "As it was");
    } else
        return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT ||
               e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN;
    return true;
}
static bool pad_event(App* app, const SDL_Event* e) {
    SDL_JoystickID ours = app->gamepad ? SDL_GetGamepadID(app->gamepad) : 0;
    switch (e->type) {
        case SDL_EVENT_KEY_DOWN: pad_keyboard(app, &e->key); return true;
        case SDL_EVENT_TEXT_INPUT: return true;
        case SDL_EVENT_JOYSTICK_ADDED: /* one SDL has no mapping for is named, and no longer once it is out */
        case SDL_EVENT_JOYSTICK_REMOVED:
        case SDL_EVENT_GAMEPAD_ADDED: pad_attach(app); return true;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (ours && e->gdevice.which == ours) {
                SDL_CloseGamepad(app->gamepad);
                app->gamepad = NULL;
                app->pad.listening = 0;
                pad_attach(app);
            }
            return true;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (ours && e->gbutton.which == ours) pad_control(app, snx_pad_of_button(e->gbutton.button));
            return true;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (ours && e->gaxis.which == ours) pad_axis(app, &e->gaxis);
            return true;
        default: return false;
    }
}
/* a text of the page, in the font's glyphs (CP437) a byte each */
static void pad_text(App* app, const SnxPadShape* p, GuiRgb c) {
    float x = p->x;
    const char* s;
    SDL_SetTextureColorMod(app->font, c.r, c.g, c.b);
    for (s = p->text; *s; ++s, x += 8.0f * (float)app->scale) glyph(app, x, p->y, (unsigned char)*s);
}
/* a round part of the pad */
static void pad_disc(App* app, const SnxPadShape* p, GuiRgb c) {
    enum { SIDES = 32 };
    SDL_Vertex v[SIDES + 1];
    int index[SIDES * 3], k;
    float r = SDL_min(p->w, p->h) / 2;
    SDL_FColor color = {(float)c.r / 255, (float)c.g / 255, (float)c.b / 255, 1};
    for (k = 0; k <= SIDES; ++k) {
        float a = 2 * SDL_PI_F * (float)k / SIDES;
        v[k].position.x = p->x + p->w / 2 + (k ? r * SDL_cosf(a) : 0);
        v[k].position.y = p->y + p->h / 2 + (k ? r * SDL_sinf(a) : 0);
        v[k].color = color;
        v[k].tex_coord.x = v[k].tex_coord.y = 0;
    }
    for (k = 0; k < SIDES; ++k) {
        index[3 * k] = 0;
        index[3 * k + 1] = k + 1;
        index[3 * k + 2] = k + 1 < SIDES ? k + 2 : 1;
    }
    SDL_RenderGeometry(app->renderer, NULL, v, SIDES + 1, index, SIDES * 3);
}
/* the page in the theme's colours: the panels a little darker than the page, the focus as the menus' and what is held
   on the pad in the active colour */
static void render_pad(App* app) {
    const GuiTheme* theme = &app->theme;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    char keys[256];
    GuiPlace place;
    GuiRect r;
    int i;
    /* the pad drawn in the box between the head and the foot */
    place_screen(app, &place, shown_keys(app, keys, sizeof keys), 0, GUI_SHOWS_TABS);
    chrome(app, &place);
    r = inner(app, place.list);
    pad_sync(app);
    snx_pad_screen_lay(&app->pad, (float)r.x, (float)r.y, (float)r.w, (float)r.h, cw, ch, &app->pad_drawing);
    for (i = 0; i < app->pad_drawing.n; ++i) {
        const SnxPadShape* p = &app->pad_drawing.shape[i];
        GuiRgb on = p->style == SNX_PAD_STYLE_FOCUS ? theme->focus : theme->active;
        bool marked = p->style == SNX_PAD_STYLE_FOCUS || p->style == SNX_PAD_STYLE_LIT;
        if (p->kind == SNX_PAD_SHAPE_TEXT)
            pad_text(app, p,
                     marked                           ? theme->page
                     : p->style == SNX_PAD_STYLE_SOFT ? theme->soft
                     : p->style == SNX_PAD_STYLE_NOTE ? theme->failure
                                                      : theme->ink);
        else if (p->kind == SNX_PAD_SHAPE_DISC)
            pad_disc(app, p, marked ? on : theme->soft);
        else if (marked)
            fill(app, p->x, p->y, p->w, p->h, on, 255);
        else
            fill(app, p->x, p->y, p->w, p->h, theme->soft, p->kind == SNX_PAD_SHAPE_BAR ? 255 : 0x40);
    }
    foot(app, &place,
         app->editor.note[0] ? app->editor.note
         : !app->pads_read   ? "Looking for controllers..."
                             : "",
         rgb(app->editor.note[0] ? theme->failure : theme->soft), keys);
}
static void editor_key(App* app, GuiKey key) {
    GuiEditorDo what;
    /* a player's map row opens its button map, as every player's Button map page does; its Controller row waits for
       a press on the controller it takes */
    if (app->form == GUI_FORM_GROUP && key == GUI_SELECT && !app->editor.typing &&
        player_key(app->settings.values[setting_of(app, app->editor.row)].key, "map")) {
        open_player_pad(app, setting_of(app, app->editor.row));
        return;
    }
    if (app->form == GUI_FORM_GROUP && key == GUI_SELECT && !app->editor.typing &&
        player_key(app->settings.values[setting_of(app, app->editor.row)].key, "device")) {
        device_listen(app, setting_of(app, app->editor.row));
        return;
    }
    what = gui_editor_key(&app->editor, key);
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_SETTINGS && !app->editor.step) {
        open_group(app, app->editor.row);
        return;
    }
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_GROUP) { /* a page's row opens it, an All steps its rows */
        int row = setting_of(app, app->editor.row);
        if (app->settings.values[row].kind == GUI_SETTING_PAGE && !strcmp(app->settings.keys[row].key, "Player")) {
            /* the player shown, round All and 1 to 8 */
            app->player = (app->player + (app->editor.step < 0 ? SNX_GUI_PLAYERS : 1)) % (SNX_GUI_PLAYERS + 1);
            group_view(app, app->group);
            return;
        }
        if (app->settings.values[row].kind == GUI_SETTING_PAGE && !app->editor.step)
            open_page(app, row);
        else if (gui_settings_step_all(&app->settings, row, app->editor.step)) {
            app->editor.dirty = true;
            keep_settings(app);
        }
        return;
    }
    if (app->form == GUI_FORM_APP &&
        !(what == GUI_EDITOR_ACTION && app->editor.row == GUI_APP_CLEAN && !app->editor.step))
        app->clean_armed = false; /* another key between the two Enters */
    if (what == GUI_EDITOR_ACTION && app->form == GUI_FORM_APP) {
        app_action(app, app->editor.row, app->editor.step);
        return;
    }
    if (app->form == GUI_FORM_FOLDERS) {
        if (what == GUI_EDITOR_ACTION && !app->editor.step)
            folder_select(app, app->editor.row);
        else if (what == GUI_EDITOR_SAVE) /* Add a folder */
            open_folder_picker(app);
        if (what != GUI_EDITOR_ACTION || app->editor.step) app->folder_armed = 0;
        if (what != GUI_EDITOR_LEAVE) return;
    }
    if (app->form == GUI_FORM_PLACES) {
        if (what == GUI_EDITOR_ACTION && !app->editor.step) {
            app->place_row = app->editor.row;
            open_folder_picker(app);
        } else if (what == GUI_EDITOR_SAVE) /* All back to the defaults */
            places_reset(app);
        if (what != GUI_EDITOR_LEAVE) return;
    }
    if (what == GUI_EDITOR_SAVE && app->form == GUI_FORM_SETTINGS) { /* the groups' last row */
        gui_settings_reset(&app->settings, -1);
        app->editor.dirty = true;
    } else if (what == GUI_EDITOR_SAVE && app->form == GUI_FORM_THEME)
        save_editor(app);
    keep_settings(app);
    if (what == GUI_EDITOR_LEAVE) {
        leave_form(app);
        return;
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->form == GUI_FORM_GROUP) { /* the menu on the whole screen too, as the games, unless fullscreen is 0 */
        const char* fullscreen = gui_settings_value(&app->settings, "fullscreen");
        SDL_SetWindowFullscreen(app->window, !fullscreen || strcmp(fullscreen, "0"));
    }
#endif
    if (app->editor.typing && !SDL_TextInputActive(app->window))
        SDL_StartTextInput(app->window); /* a phone's keyboard comes up */
    else if (!app->editor.typing && SDL_TextInputActive(app->window))
        SDL_StopTextInput(app->window);
}
/* keys and typed text: while a value is typed, Space and the letters are its text */
static void editor_input(App* app, const SDL_Event* e) {
    GuiKey key;
    if (e->type == SDL_EVENT_TEXT_INPUT) {
        gui_editor_type(&app->editor, e->text.text);
        return;
    }
    if (app->form == GUI_FORM_THEME && e->key.key == SDLK_S && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        save_editor(app);
        return;
    }
    /* Del, or Backspace when nothing is typed: a setting back to its default */
    if (app->form == GUI_FORM_GROUP && !app->editor.typing &&
        (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        gui_settings_reset(&app->settings, setting_of(app, app->editor.row));
        app->editor.dirty = true;
        keep_settings(app);
        return;
    }
    /* the game folders: Del takes one off, Ctrl+V adds the one pasted */
    if (app->form == GUI_FORM_FOLDERS && (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        if (app->editor.row && app->editor.row <= app->folder_count)
            folder_remove(app, app->editor.row - 1);
        else if (!app->editor.row)
            snprintf(app->editor.note, sizeof app->editor.note, "games/ is always listed");
        return;
    }
    /* a place: Del puts back its default, Ctrl+V picks the folder pasted */
    if (app->form == GUI_FORM_PLACES && app->editor.row < GUI_PLACES &&
        (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE)) {
        place_set(app, app->editor.row, NULL);
        return;
    }
    if (app->form == GUI_FORM_PLACES && app->editor.row < GUI_PLACES && e->key.key == SDLK_V &&
        (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        char* path = gui_drop_path(clipboard ? clipboard : "", true);
        if (path)
            place_set(app, app->editor.row, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "Paste one local folder's path");
        free(path);
        SDL_free(clipboard);
        return;
    }
    if (app->form == GUI_FORM_FOLDERS && e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        char* path = gui_drop_path(clipboard ? clipboard : "", true);
        if (path)
            folder_ask(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "Paste one local folder's path");
        free(path);
        SDL_free(clipboard);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat && !app->editor.typing) {
        screen_tab(app, e->key.mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (app->form == GUI_FORM_THEME && e->key.key == SDLK_P && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if (app->editor.typing) {
        if (e->key.key == SDLK_BACKSPACE) gui_editor_erase(&app->editor);
        if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {
            char* clipboard = SDL_GetClipboardText();
            gui_editor_type(&app->editor, clipboard ? clipboard : "");
            SDL_free(clipboard);
        }
        if (e->key.key == SDLK_SPACE) return;
    }
    if (menu_key_of(&e->key, &key)) editor_key(app, key);
}
/* ---- the Settings and the theme editor: the groups on the left, the open or the focused group's rows on the
   right; the theme editor's keys on the left, the preview on the right ---- */
typedef struct {
    GuiPlace place;
    GuiList left, right;
    GuiList back;          /* Back over the open group's rows, for a click or a finger: ESC's */
    GuiEditor peek;        /* the focused group's rows, shown while the focus is on the groups */
    const GuiEditor* rows; /* the right box's rows: the editor's, or peek */
    GuiRect info;          /* the right box's foot, for a line about its rows */
} GuiFormBoxes;
/* the group the groups' list shows open, -1 for none: the one being edited, App for its folders */
static int form_group(const App* app) {
    if (app->form == GUI_FORM_GROUP) return app->group;
    return app->form == GUI_FORM_APP || app->form == GUI_FORM_FOLDERS || app->form == GUI_FORM_PLACES ? 0 : -1;
}
/* the columns a form's row label takes, the widest's */
static int form_labels(const GuiEditor* e) {
    char label[64];
    int r, labels = 8;
    for (r = 0; r < gui_editor_rows(e); ++r) {
        gui_editor_label(e, r, label, sizeof label);
        labels = SDL_max(labels, (int)strlen(label));
    }
    return labels;
}
/* the line at the foot of a group's rows, about what they hold, NULL on the screens with none */
static const char* form_note(const App* app) {
    if (app->form == GUI_FORM_PLACES)
        return app->editor.row < GUI_PLACES ? app->place_line[app->editor.row] : "Data, games and extractions";
    if (app->form != GUI_FORM_SETTINGS && app->form != GUI_FORM_GROUP) return NULL;
    return app->settings.count > GUI_SETTINGS_OWN ? "For the next runs. [default]: as the game's .ini says"
                                                  : "The runtime listed no settings";
}
static void form_boxes(App* app, GuiFormBoxes* f) {
    const char* note = form_note(app);
    char keys[256];
    int rows, top, back;
    int theme = app->form == GUI_FORM_THEME, width, height, g, longest = (int)strlen("Reset all to the defaults");
    area(app, &width, &height);
    for (g = 0; g < app->groups; ++g) longest = SDL_max(longest, (int)strlen(app->group_keys[g].key));
    place_screen(app, &f->place, shown_keys(app, keys, sizeof keys),
                 theme ? width / cell_w(app)
                       : longest + 2 + (int)SDL_utf8strlen(app->theme.marker) + (int)SDL_utf8strlen(app->theme.cursor),
                 GUI_SHOWS_TABS);
    f->rows = &app->editor;
    if (app->form == GUI_FORM_SETTINGS) { /* the focused group's rows, to see before opening it */
        int g = app->editor.row;
        memset(&f->peek, 0, sizeof f->peek);
        if (g == 0) {
            app_values(app);
            gui_editor_form(&f->peek, app_keys, GUI_APP_ROWS, &app->app_form, NULL);
        } else if (g < app->groups) {
            int n = player_rows(app, g, 0, app->peek_rows), r; /* Controllers: every player's */
            for (r = 0; r < n; ++r) app->peek_keys[r] = app->settings.keys[app->peek_rows[r]];
            if (n)
                gui_editor_form(&f->peek, app->peek_keys, n, app->settings.values, NULL);
            else
                gui_editor_form(&f->peek, app->settings.keys + app->group_first[g], app->group_rows[g],
                                app->settings.values, NULL);
        } else /* Reset all: a form of no rows, where an editor of none would be a theme's (its folder and Save) */
            gui_editor_form(&f->peek, NULL, 0, &app->app_form, NULL);
        f->peek.flat = true;
        f->peek.row = -1;
        f->rows = &f->peek;
    }
    if (theme)
        list_place(app, &f->left, f->place.list, gui_editor_rows(&app->editor), app->editor.row);
    else
        list_place(app, &f->left, f->place.list, app->groups + 1,
                   app->form == GUI_FORM_SETTINGS ? app->editor.row : -1);
    rows = note ? wrap_rows(app, note, inner(app, f->place.side).w / cell_w(app), 0) : 1; /* the note whole */
    f->info = gui_rect(f->place.side.x, f->place.side.y + f->place.side.h - rows * cell_h(app) - 4 * app->scale,
                       f->place.side.w, rows * cell_h(app) + 2 * app->scale);
    top = f->place.side.y + head_h(app) + 4 * app->scale;
    back = !theme && app->form != GUI_FORM_SETTINGS ? item_h(app) + 24 * app->scale : 0; /* wider than the rows' */
    list_place(app, &f->back,
               gui_rect(f->place.side.x + cell_w(app), top, f->place.side.w - 2 * cell_w(app), item_h(app)),
               back ? 1 : 0, -1);
    list_place(app, &f->right,
               gui_rect(f->place.side.x + cell_w(app), top + back, f->place.side.w - 2 * cell_w(app),
                        f->info.y - top - back - 2 * app->scale),
               theme ? 0 : gui_editor_rows(f->rows), app->form == GUI_FORM_SETTINGS ? -1 : f->rows->row);
}
/* a row of a form as its box shows it: its label, the value past the widest label; one being typed ends where the
   typing is, its start giving way */
static void form_row(App* app, const GuiList* l, const GuiEditor* e, int r, int labels, int focused, int marked) {
    char label[64], value[96], line[200];
    const GuiThemeKey* k = gui_editor_key_of(e, r);
    int typing = focused && e->typing, room = (list_box(l, r).w - cell_w(app) * 3 / 2) / cell_w(app); /* list_item's */
    size_t n;
    gui_editor_label(e, r, label, sizeof label);
    if (typing)
        snprintf(value, sizeof value, "%s_", e->text);
    else if (k && e->object == (char*)app->settings.values) { /* a setting: a pick, else its default and [default] */
        int row = e->keys == app->view_keys   ? app->view_rows[k - app->view_keys]
                  : e->keys == app->peek_keys ? app->peek_rows[k - app->peek_keys]
                                              : (int)(k - app->settings.keys);
        gui_settings_shown(&app->settings, row, value, sizeof value);
        if (player_key(app->settings.values[row].key, "device") && !app->settings.values[row].text[0])
            snprintf(value, sizeof value, "Any [default]");        /* the next to connect */
        else if (player_key(app->settings.values[row].key, "map")) /* it opens the button map, as All's page does */
            snprintf(value, sizeof value, "%s", app->settings.values[row].text[0] ? "Its own >" : "As All >");
    } else
        gui_editor_value(e, r, value, sizeof value);
    if (value[0])
        snprintf(line, sizeof line, "%s%-*s %s", app->theme.marker, labels, label, value);
    else /* no value to line up: padding would make a short row too long for its box, and slide */
        snprintf(line, sizeof line, "%s%s", app->theme.marker, label);
    n = SDL_utf8strlen(line);
    if (typing && room > 4 && n > (size_t)room) { /* the typed end in view */
        const char* tail = value;
        size_t skip = n - (size_t)room + 3;
        while (skip-- && *tail) SDL_StepUTF8(&tail, NULL);
        snprintf(line, sizeof line, "%s%-*s ...%s", app->theme.marker, labels, label, tail);
    }
    list_item(app, l, r, line, focused, marked);
    if (k && k->type == GUI_KEY_COLOUR) { /* its colour, framed in soft so a page-coloured one shows */
        GuiRect b = list_box(l, r);
        const GuiRgb* c = (const GuiRgb*)((const char*)(e->object ? e->object : (const char*)&e->theme) + k->at);
        int x = b.x + cell_w(app) / 2 + (int)SDL_utf8strlen(line) * cell_w(app) + cell_w(app), s = app->scale;
        if (x + 2 * cell_w(app) <= b.x + b.w) {
            fill_rect(app, gui_rect(x, b.y + 3 * s, 2 * cell_w(app), b.h - 6 * s), app->theme.soft);
            fill_rect(app, gui_rect(x + s, b.y + 4 * s, 2 * cell_w(app) - 2 * s, b.h - 8 * s), *c);
        }
    }
}
/* what the bar says of the focused row: the note of the last key, else the row's help */
static const char* form_about(const App* app, SDL_Color* color) {
    const GuiEditor* e = &app->editor;
    const GuiThemeKey* k = gui_editor_key_of(e, e->row);
    *color = rgb(e->note[0] ? (e->leaving ? app->theme.failure : app->theme.focus) : app->theme.ink);
    if (e->note[0]) return e->note;
    if (k) return k->about;
    if (app->form == GUI_FORM_SETTINGS) return "Every setting back to its default";
    if (app->form == GUI_FORM_FOLDERS) return "Pick, drop or paste a folder of games";
    if (app->form == GUI_FORM_PLACES) return "Each folder back to its default. What is in them stays";
    return e->object ? "" : e->row == GUI_EDITOR_FOLDER ? "The folder of themes/ it is saved in" : "Ctrl+S saves too";
}
static void draw_picker(App* app);
static void draw_form(App* app) {
    GuiFormBoxes f;
    char keys[256], line[1100];
    const char* about;
    SDL_Color color;
    int r, labels, theme = app->form == GUI_FORM_THEME;
    form_boxes(app, &f);
    chrome(app, &f.place);
    if (theme) {
        labels = form_labels(&app->editor);
        for (r = f.left.first; r < f.left.first + f.left.shown; ++r)
            form_row(app, &f.left, &app->editor, r, labels, r == app->editor.row, false);
    } else
        for (r = f.left.first; r < f.left.first + f.left.shown; ++r) {
            snprintf(line, sizeof line, "%s%s", app->theme.marker,
                     r < app->groups ? app->group_keys[r].key : "Reset all to the defaults");
            list_item(app, &f.left, r, line,
                      app->form == GUI_FORM_SETTINGS ? r == app->editor.row : 2 * (r == form_group(app)), false);
        }
    list_marks(app, &f.left);
    if (theme) { /* the preview, 16 by 10, and what switches it */
        GuiRect p = inner(app, f.place.side);
        int w = SDL_min(p.w, (f.info.y - f.place.side.y - 2 * cell_h(app)) * 16 / 10), y;
        y = pane(app, f.place.side, "Preview");
        if (w > 0 && preview_texture(app)) {
            SDL_FRect box = {(float)(p.x + (p.w - w) / 2), (float)y, (float)w, (float)(w * 10 / 16)};
            SDL_RenderTexture(app->renderer, app->preview, NULL, &box);
            text_in(app, p, p.x, y + w * 10 / 16 + 2 * app->scale,
                    app->editor.preview != GUI_SCREEN_MENU ? "A run (Ctrl+P: the menu)" : "The menu (Ctrl+P: a run)",
                    rgb(app->theme.soft));
        }
        if (w > 0) y += w * 10 / 16 + 2 * app->scale + cell_h(app);
        snprintf(line, sizeof line, "Saves in %s", app->themes_dirs[0] ? app->themes_dirs[0] : "(no themes folder)");
        wrap_in(app, gui_rect(p.x, y, p.w, f.place.side.y + f.place.side.h - y), p.x, y, line, 1, rgb(app->theme.soft));
    } else {
        int g = app->form == GUI_FORM_SETTINGS ? app->editor.row : app->group;
        pane(app, f.place.side,
             app->form == GUI_FORM_FOLDERS                   ? "Game folders"
             : app->form == GUI_FORM_PLACES                  ? "Folders"
             : app->form == GUI_FORM_GROUP && app->page >= 0 ? app->settings.keys[app->page].key
             : g < app->groups                               ? app->group_keys[g].key
                                                             : "Reset all to the defaults");
        if (f.back.count) {
            snprintf(line, sizeof line, "%sBack [ESC]", app->theme.marker);
            list_item(app, &f.back, 0, line, false, false);
        }
        labels = form_labels(f.rows);
        for (r = f.right.first; r < f.right.first + f.right.shown; ++r)
            form_row(app, &f.right, f.rows, r, labels, f.rows == &app->editor && r == app->editor.row,
                     app->form == GUI_FORM_FOLDERS && r == app->folder_armed && r);
        list_marks(app, &f.right);
        if (form_note(app))
            wrap_in(app, inner(app, f.info), f.info.x + cell_w(app), f.info.y, form_note(app), 0, rgb(app->theme.soft));
    }
    draw_picker(app);
    about = form_about(app, &color);
    foot(app, &f.place, about, color, shown_keys(app, keys, sizeof keys));
}
/* ---- the colour picker: over the theme editor's keys, so the preview shows the colour as it changes;
   a bar per channel, then the colour typed beside it and the one it had ---- */
static void picker_place(App* app, GuiPicker* p) {
    int s = app->scale, cw = cell_w(app), item = cell_h(app) + 6 * s, x0, x1, top, width, height, w, h, i;
    GuiFormBoxes f;
    area(app, &width, &height);
    form_boxes(app, &f);
    x0 = f.place.list.x;
    x1 = f.place.list.x + f.place.list.w;
    top = head_h(app) + 2 * s; /* under the pane's head */
    w = SDL_min(x1 - x0, 46 * cw);
    h = top + GUI_PICK_ROWS * item + 4 * s;
    p->box = gui_rect(x0 + (x1 - x0 - w) / 2, SDL_max(0, (height - h) / 2), w, h);
    for (i = 0; i < GUI_PICK_ROWS; ++i)
        p->rows[i] = gui_rect(p->box.x + cw, p->box.y + top + i * item, p->box.w - 2 * cw, item);
    for (i = 0; i < 3; ++i) /* the channel's letter before it, its value after it */
        p->bars[i] = gui_rect(p->rows[i].x + 3 * cw, p->rows[i].y + 3 * s, p->rows[i].w - 8 * cw, item - 6 * s);
}
/* the value of a bar's strip x pixels in: a strip is a pixel at 1x, the first 0 and the last 255 (a bar is whole
   half cells, so its inside is whole strips) */
static int strip_value(const App* app, GuiRect b, int x) {
    int s = app->scale, w = b.w - 2 * s;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    return w > s ? x / s * s * 255 / (w - s) : 0;
}
/* a colour swatch framed in soft, so a page-coloured one shows */
static void swatch(App* app, GuiRect r, GuiRgb c) {
    fill_rect(app, r, app->theme.soft);
    fill_rect(app, gui_rect(r.x + app->scale, r.y + app->scale, r.w - 2 * app->scale, r.h - 2 * app->scale), c);
}
static uint8_t* channel(GuiRgb* c, int i) { return i == GUI_PICK_RED ? &c->r : i == GUI_PICK_GREEN ? &c->g : &c->b; }
static void draw_picker(App* app) {
    static const char* const names[3] = {"R", "G", "B"};
    GuiEditor* e = &app->editor;
    const GuiTheme* t = &app->theme;
    const GuiThemeKey* k = gui_editor_key_of(e, e->row);
    GuiRgb *c = gui_editor_colour(e), shade;
    GuiPicker p;
    char line[96];
    int i, x, s = app->scale, cw = cell_w(app);
    if (!e->picking || !c) return;
    picker_place(app, &p);
    snprintf(line, sizeof line, "[%s] %s", k->section, k->key);
    over_box(app, p.box, line);
    for (i = 0; i < GUI_PICK_ROWS; ++i) {
        GuiRect r = p.rows[i];
        int focused = e->channel == i, band = focused && t->focus_bar, y = r.y + 3 * s;
        SDL_Color ink = rgb(band ? t->page : focused ? t->focus : t->ink);
        if (band) fill_rect(app, r, t->focus);
        if (i == GUI_PICK_HEX) { /* the colour as typed, the one it is now and the one it had */
            if (e->typing)
                snprintf(line, sizeof line, "%s_", e->text);
            else
                gui_editor_value(e, e->row, line, sizeof line);
            text_in(app, r, r.x + cw / 2, y, line, ink);
            x = r.x + cw / 2 + 10 * cw;
            if (x + 4 * cw <= r.x + r.w) swatch(app, gui_rect(x, y, 4 * cw, cell_h(app)), *c);
            if (x + 13 * cw <= r.x + r.w) {
                text_in(app, r, x + 5 * cw, y, "was", band ? ink : rgb(t->soft));
                swatch(app, gui_rect(x + 9 * cw, y, 4 * cw, cell_h(app)), e->was);
            }
            continue;
        }
        text_in(app, r, r.x + cw / 2, y, names[i], ink);
        snprintf(line, sizeof line, "%3d", *channel(c, i));
        text_in(app, r, p.bars[i].x + p.bars[i].w + cw, y, line, ink);
        { /* the colour the channel's every value would give, framed, and a mark where it stands */
            GuiRect b = p.bars[i];
            int w = b.w - 2 * s, at;
            if (w <= s) continue;
            fill_rect(app, b, t->soft);
            shade = *c;
            for (x = 0; x < w; x += s) {
                *channel(&shade, i) = (uint8_t)strip_value(app, b, x);
                fill_rect(app, gui_rect(b.x + s + x, b.y + s, s, b.h - 2 * s), shade);
            }
            at = b.x + s + *channel(c, i) * (w - s) / 255 / s * s + s / 2;
            fill_rect(app, gui_rect(at - s, b.y - 2 * s, 3 * s, b.h + 4 * s), band ? t->page : t->ink);
            fill_rect(app, gui_rect(at, b.y - s, s, b.h + 2 * s), band ? t->focus : t->page);
        }
    }
}
static void picker_set(App* app, const GuiPicker* p, int i, float x) {
    gui_editor_pick(&app->editor, i, strip_value(app, p->bars[i], (int)SDL_floorf(x) - (p->bars[i].x + app->scale)));
}
/* the pointer on the picker, at pixel x, y. A press on a bar's row sets its channel there and holds it, the pointer's
   moves dragging it until the press lifts; on #RRGGBB it types it; off the box it keeps the colour */
static void picker_pointer(App* app, float x, float y, bool press) {
    GuiEditor* e = &app->editor;
    GuiPicker p;
    int i;
    picker_place(app, &p);
    if (!press) {
        if (app->pick_press > 0 && app->pick_press <= 3) picker_set(app, &p, app->pick_press - 1, x);
        return;
    }
    for (i = 0; i < GUI_PICK_ROWS; ++i)
        if (x >= (float)p.box.x && x < (float)(p.box.x + p.box.w) && y >= (float)p.rows[i].y &&
            y < (float)(p.rows[i].y + p.rows[i].h))
            break;
    app->pick_press = i < 3 ? i + 1 : GUI_PICK_ROWS + 1;
    if (e->typing && i != GUI_PICK_HEX) editor_key(app, GUI_CANCEL);
    if (i < 3)
        picker_set(app, &p, i, x);
    else if (i == GUI_PICK_HEX && !e->typing) {
        e->channel = GUI_PICK_HEX;
        editor_key(app, GUI_SELECT);
    } else if (i == GUI_PICK_ROWS && (x < (float)p.box.x || x >= (float)(p.box.x + p.box.w) || y < (float)p.box.y ||
                                      y >= (float)(p.box.y + p.box.h)))
        gui_editor_pick_end(e, true);
}
/* a row of a form under the pointer: focused, or on the focused one Select, as a key would */
static void form_row_pointer(App* app, int r) {
    if (r == app->editor.row && !app->editor.typing)
        editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
static void go_tab(App* app, int tab);
/* a click on a form: a tab; a row of the list or of the right box; a group of the list opens it from a
   group's rows; the theme editor's preview switches */
static void form_pointer(App* app, float x, float y, bool touch) {
    GuiFormBoxes f;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        go_tab(app, i);
        return;
    }
    form_boxes(app, &f);
    if (app->form == GUI_FORM_THEME && x >= (float)f.place.side.x && x < (float)(f.place.side.x + f.place.side.w) &&
        y >= (float)f.place.side.y && y < (float)(f.place.side.y + f.place.side.h)) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if ((i = list_hit(&f.left, x, y, touch)) >= 0) {
        if (app->form == GUI_FORM_THEME || app->form == GUI_FORM_SETTINGS) {
            form_row_pointer(app, i);
            return;
        }
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        keep_settings(app);
        leave_editor(app);
        settings_top(app, i); /* another group: from its row, as Enter there */
        if (i < app->groups) open_group(app, i);
        return;
    }
    if (f.back.count && list_hit(&f.back, x, y, touch) >= 0) { /* as ESC: a typed value first, then the rows */
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        editor_key(app, GUI_CANCEL);
        return;
    }
    if ((i = list_hit(&f.right, x, y, touch)) < 0) return;
    if (app->form == GUI_FORM_SETTINGS) { /* a row of the focused group: it opens, on that row */
        if (app->editor.row >= app->groups) return;
        open_group(app, app->editor.row);
        app->editor.row = SDL_min(i, gui_editor_rows(&app->editor) - 1);
        return;
    }
    form_row_pointer(app, i);
}
static void render(App* app) {
    layout(app);
    if (splash_shown(app)) return;
    if (app->editing && app->form == GUI_FORM_PAD)
        render_pad(app);
    else if (app->editing) {
        draw_form(app);
        if (app->confirm.shown) render_ask(app); /* a folder picked on the folders' screen */
    } else
        draw_screen(app);
    if (app->cleaned_shown) render_files(app, 1); /* over the menu, or the App's rows it was started from */
    SDL_RenderPresent(app->renderer);
}
/* the game's name as its run gave it, from the step that names it, for its recent game */
static void recent_name(App* app, const char* label) {
    const GuiStep* named = gui_steps_last(&app->steps, label);
    if (!app->recent_file || app->recent_named || !app->file || !named || !named->detail[0]) return;
    app->recent_named = 1;
    if (gui_recent_rename(&app->recent, app->file, named->detail)) recent_changed(app);
}
static void list_games(App* app);
static uint64_t games_stamp(const App* app);
/* back to the menu from a finished run, which fades in */
static void leave_run(App* app) {
    app->finished = false;
    app->menu.view = GUI_LOAD_NONE;
    space_start(app); /* a run takes disk */
    app->message[0] = 0;
    recent_note(app);
    app->leaving_ms = 0;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
    layout(app);
}
#ifdef SDL_PLATFORM_ANDROID
/* A game runs in the app's game activity, a process of its own: no child of this one gets a window */
static int play(App* app, const char* game) {
    GuiSettings* s = settings_of(app);
    char* option = snx_format("--game=%s", game);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 3) * sizeof(*argv));
    int ok;
    argv[0] = option;
    argv[1 + gui_settings_options(s, 0, 1, argv + 1)] = NULL;
    ok = gui_android_play(argv, app->message, sizeof(app->message));
    free(argv);
    free(option);
    return ok;
}
/* the game a run of the driver made ready: the one it ran, when it was a game already, else in its folder of games/,
   where the driver names it, as it cannot open it; whether it opened */
static int ready(App* app) {
    const GuiStep* made = gui_steps_last(&app->steps, "READY");
    char* folder = made && !app->native ? snx_path_join(app->games_dir, made->detail) : NULL;
    char* game = !made         ? NULL
                 : app->native ? snx_strdup(app->file)
                 : folder      ? snx_path_join(folder, made->detail)
                               : NULL;
    int ok = game && play(app, game);
    free(game);
    free(folder);
    return ok;
}
/* the run in its notification: the game, the step on the screen and the percent, as they change, a second apart at
   most (Android drops notifications that come faster) */
static void tell(App* app) {
    char name[96], runs[3][112], line[sizeof app->told];
    size_t lengths[3];
    int percent = gui_steps_percent(&app->steps);
    Uint64 now = SDL_max(SDL_GetTicks(), 1);
    line[0] = 0;
    if (app->steps.count) {
        step_runs(&app->steps.step[app->steps.count - 1], app->theme.progress, now_ms(app), runs, lengths);
        snprintf(line, sizeof line, "%s%s%s", runs[0], runs[1], runs[2]);
    }
    if (app->told_ms && (now - app->told_ms < 1000 || (percent == app->told_percent && !strcmp(line, app->told))))
        return;
    run_name(app, name, sizeof name);
    gui_android_running(name, line, percent);
    snprintf(app->told, sizeof app->told, "%s", line);
    app->told_percent = percent;
    app->told_ms = now;
}
#endif
#ifdef SDL_PLATFORM_IOS
/* A game runs in this process once the menu has closed (gui_apple_play, src/nx/runtime/entry.c): an iOS app has no
   other */
static int play(App* app, const char* game) {
    GuiSettings* s = settings_of(app);
    char* option = snx_format("--game=%s", game);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 4) * sizeof(*argv));
    argv[0] = "scarletnx";
    argv[1] = option;
    argv[2 + gui_settings_options(s, 0, 1, argv + 2)] = NULL;
    gui_apple_play(argv);
    app->menu.quit = true;
    free(argv);
    free(option);
    return 1;
}
#endif
/* a line more while what an older version left at old (a folder, or a game's files) is there; this one keeps it at now */
static void left_line(App* app, const char* old, const char* now, bool there) {
    char *line, *grown;
    size_t n = app->left ? strlen(app->left) : 0;
    if (!there || !strcmp(old, now)) return;
    line = snx_format("%s" SAY_LEFT_BY "%s\n", old, now);
    grown = line ? (char*)realloc(app->left, n + strlen(line) + 1) : NULL;
    if (grown) {
        strcpy(grown + n, line);
        app->left = grown;
        app->lefts++;
    }
    free(line);
}
/* the games of folder an older version laid out beside each other (<name>, its code, <name>_data/): each one's folder
   now, games/<title ID>/ or games/<name>/ for one without an ID of its own (main.npdm in its data) */
static void left_games(App* app, const char* folder) {
    char** names = NULL;
    size_t count = 0, i, n;
    if (!folder || snx_list_dir(folder, &names, &count)) return;
    for (i = 0; i < count; ++i) {
        char *data, *npdm, *name, *old, *now, id[17];
        unsigned char* bytes;
        size_t size = 0;
        n = strlen(names[i]);
        if (n <= 5 || strcmp(names[i] + n - 5, "_data")) continue;
        data = snx_path_join(folder, names[i]);
        name = snx_format("%.*s", (int)(n - 5), names[i]);
        npdm = data ? snx_path_join(data, "exefs/main.npdm") : NULL;
        bytes = npdm && snx_is_file(npdm) ? (unsigned char*)snx_read_file(npdm, &size) : NULL;
        old = name ? snx_path_join(folder, name) : NULL;
        now = name ? snx_format("%s/%s/", folder, snx_title_text(snx_npdm_title(bytes, size), id) ? id : name) : NULL;
        if (old && now && data && snx_is_dir(data)) left_line(app, old, now, true);
        free(bytes);
        free(npdm);
        free(now);
        free(old);
        free(name);
        free(data);
    }
    snx_free_strings(names, count);
}
#ifdef SDL_PLATFORM_ANDROID
/* a folder with anything in it */
static bool has_files(const char* folder) {
    char** names = NULL;
    size_t count = 0;
    bool any = folder && !snx_list_dir(folder, &names, &count) && count;
    snx_free_strings(names, count);
    return any;
}
/* what older versions kept elsewhere on Android: /sdcard/ScarletNX's games and keys (a shared storage the app no longer
   uses), and the games/ in the app's own files from before it had All files access */
static void left_android(App* app) {
    static const char* const shared[] = {"games", "prod.keys", "title.keys"};
    const char* files = SDL_GetAndroidInternalStoragePath();
    char *old, *now;
    size_t i;
    for (i = 0; app->data_dir && i < sizeof shared / sizeof shared[0]; ++i) {
        old = snx_path_join(SNX_ANDROID_DIR, shared[i]);
        now = i ? snx_path_join(app->data_dir, shared[i]) : snx_strdup(app->games_dir);
        if (old && now) left_line(app, old, now, i ? snx_is_file(old) : has_files(old));
        free(old);
        free(now);
    }
    old = files ? snx_path_join(files, "games") : NULL;
    if (old && app->games_dir) left_line(app, old, app->games_dir, has_files(old));
    free(old);
}
#endif
/* the space each folder app->left names takes, counted again only when they change */
static void left_count(App* app) {
    char** olds = (char**)calloc((size_t)app->lefts + 1, sizeof *olds);
    const char* at = app->left;
    int i, same = app->left_space && app->left_space->count == app->lefts;
    for (i = 0; olds && i < app->lefts; ++i, at = strchr(at, '\n') + 1) {
        olds[i] = snx_format("%.*s", (int)(strstr(at, SAY_LEFT_BY) - at), at);
        same = same && olds[i] && !strcmp(olds[i], app->left_space->dirs[i]);
    }
    if (!olds || !app->lefts)
        space_end(&app->left_space, &app->left_counter);
    else if (!same)
        space_count(&app->left_space, &app->left_counter, olds, app->lefts, true);
    for (i = 0; olds && i < app->lefts; ++i) free(olds[i]);
    free(olds);
}
/* what older versions left where this one no longer looks, a folder at a time, for the menu to say while it is there:
   the menu's own files' folder (gui/), work/, each game laid out beside the others, a portable copy's games/ beside
   the program (its data in .scarletnx/ now), a build tree's games/, work/ and keys in its checkout. Nothing is moved:
   the player does that (docs/DEPRECATIONS.md) */
static void left_behind(App* app) {
    char *config = menu_folder(app, SNX_USER_CONFIG, false), *state = menu_folder(app, SNX_USER_STATE, false);
    char *data = snx_user_data_dir(), *gui = data ? snx_path_join(data, "gui") : NULL, *kept = NULL;
    char* work = app->data_dir ? snx_path_join(app->data_dir, "work") : NULL;
    char *moved = work ? snx_user_path(work, SNX_USER_CACHE) : NULL, *old = NULL, *now = NULL;
    int i;
    free(app->left);
    app->left = NULL;
    app->lefts = 0;
    app->left_at = SDL_GetTicks();
#ifndef SDL_PLATFORM_ANDROID /* the app's own storage, as before */
    /* gui/: the menu's own, now in the config and state folders, each where it goes */
    for (i = 0; gui && i < 2; ++i) {
        static const char* const files[2][4] = {{"settings.ini", "app.ini", "folders.txt", "gamecontrollerdb.txt"},
                                                {"recent.txt", "last-run.log", "", ""}};
        const char* to = i ? state : config;
        int k, there = 0;
        for (k = 0; k < 4 && files[i][k][0]; ++k) {
            char* file = snx_path_join(gui, files[i][k]);
            there |= file && snx_is_file(file);
            free(file);
        }
        if (to) left_line(app, gui, to, there);
    }
#ifdef SNX_XDG /* and the state folder's gui/ of the version just before */
    kept = snx_user_dir(SNX_USER_STATE);
    old = kept ? snx_path_join(kept, "gui") : NULL;
    if (old && state) left_line(app, old, state, snx_is_dir(old));
    free(old);
    old = NULL;
#endif
#endif
    if (work && moved) left_line(app, work, moved, snx_path_exists(work));
#ifdef SDL_PLATFORM_ANDROID
    left_android(app);
#endif
    left_games(app, app->games_dir);
    for (i = 0; i < app->folder_count; ++i) left_games(app, app->folders[i]);
    /* a portable copy's games/ beside the program, which a .scarletnx/ there holds now */
    if (app->drivers && app->data_dir) {
        old = snx_path_join(app->drivers, "games");
        now = snx_format("%s/" SNX_PORTABLE_DIR "/games", app->drivers);
        if (old && now && app->games_dir && strcmp(old, app->games_dir)) left_line(app, old, now, snx_is_dir(old));
    }
    free(old);
    free(now);
    /* the checkout a build tree's menu used as its data folder */
    for (i = 0; app->checkout && app->data_dir && i < SNX_CHECKOUT_FILES; ++i) {
        old = snx_path_join(app->checkout, snx_checkout_files[i]);
        now = snx_checkout_now(app->data_dir, i);
        if (old && now) left_line(app, old, now, snx_path_exists(old));
        free(old);
        free(now);
    }
    left_count(app);
    free(kept);
    free(moved);
    free(work);
    free(gui);
    free(data);
    free(state);
    free(config);
}
/* ---- Clean old settings (clean.c): [Deprecated] out of each game's .ini the menu reaches, and the keys the menu's own
   files hold that nothing reads ---- */
/* the folders a game's .ini can be in: the config folder's games/ (an installed build's), games/, the player's; each
   once. How many, each in out to free */
static int ini_folders(const App* app, char** out) {
    int n = 0, i, j, k;
    char* config = app->games_dir ? snx_user_path(app->games_dir, SNX_USER_CONFIG) : NULL;
    const char* listed[GUI_FOLDERS_MAX + 2];
    listed[n++] = config;
    listed[n++] = app->games_dir;
    for (i = 0; i < app->folder_count; ++i) listed[n++] = app->folders[i];
    for (i = 0, k = 0; i < n; ++i) {
        int seen = !listed[i];
        for (j = 0; j < k && !seen; ++j) seen = !strcmp(out[j], listed[i]);
        if (!seen) out[k++] = snx_strdup(listed[i]);
    }
    free(config);
    return k;
}
/* settings.ini's keys no row takes, while the runtime has listed its settings: without them every key of the
   runtime's would look old */
static int settings_unread(App* app, char* out, size_t size) {
    GuiSettings* s;
    if (size) out[0] = 0;
    if (!app->settings_file || !snx_is_file(app->settings_file)) return 0;
    s = settings_of(app);
    return s->count > GUI_SETTINGS_OWN ? gui_settings_unread(s, app->settings_file, out, size) : 0;
}
/* the .ini files with a [Deprecated] section; with done, each written again without it */
static int clean_inis(App* app, GuiCleaned* done) {
    char* folders[GUI_FOLDERS_MAX + 2];
    int n = ini_folders(app, folders), i, found = 0;
    for (i = 0; i < n; ++i) {
        found += gui_clean_inis(done, folders[i]);
        free(folders[i]);
    }
    return found;
}
/* a file of the menu's own without the unread keys, written again by save (0, else nonzero), an entry */
static void clean_keys(App* app, const char* file, const char* unread, int failed) {
    char what[1100];
    snprintf(what, sizeof what, "%s %s", failed ? "Not written without" : "Took out", unread);
    gui_cleaned_add(&app->cleaned, !failed, file, what);
}
/* it done, each file an entry of app->cleaned, which shows over the screen */
static void clean_old(App* app) {
    char unread[1024];
    gui_cleaned_free(&app->cleaned);
    clean_inis(app, &app->cleaned);
    if (settings_unread(app, unread, sizeof unread))
        clean_keys(app, app->settings_file, unread, gui_settings_save(&app->settings, app->settings_file));
    if (app_read(app, false, unread, sizeof unread)) /* the picks as the menu has them, nothing else */
        clean_keys(app, app->app_file, unread, app_save(app));
    app->cleaned_shown = app->cleaned.count > 0;
}
/* App's row: the first Enter says what it would do, the second does it */
static void clean_row(App* app) {
    char unread[16], *note = app->editor.note;
    size_t size = sizeof app->editor.note;
    int inis, keys, n;
    if (app->clean_armed) {
        app->clean_armed = false;
        clean_old(app);
        snprintf(note, size, "Cleaned: %d done, %d not", app->cleaned.done, app->cleaned.count - app->cleaned.done);
        return;
    }
    inis = clean_inis(app, NULL);
    keys = settings_unread(app, unread, sizeof unread) + app_read(app, false, unread, sizeof unread);
    if (!inis && !keys) {
        snprintf(note, size, "Nothing to clean: no [Deprecated] in a game's .ini, no old keys");
        return;
    }
    app->clean_armed = true;
    n = snprintf(note, size, "Enter again to clean:");
    if (inis) n += snprintf(note + n, size - (size_t)n, " [Deprecated] in %d .ini%s", inis, keys ? "," : "");
    if (keys && n < (int)size) snprintf(note + n, size - (size_t)n, " %d old key%s", keys, keys > 1 ? "s" : "");
}
static void pump(App* app) {
    keys_left(app);
    if (app->menu.running) {
        char buffer[4096];
        /* Bounded work per frame keeps keyboard/window events responsive even
           when a compiler floods the inherited pipe. There is no log backlog. */
        for (int reads = 0; reads < 16; ++reads) {
            int n = gui_process_read(app->process, buffer, sizeof(buffer));
            if (n <= 0) break;
            if (app->run_out) fwrite(buffer, 1, (size_t)n, app->run_out);
            gui_console_feed(&app->console, buffer, (size_t)n);
            gui_steps_feed(&app->steps, buffer, (size_t)n, now_ms(app));
            app->has_output = true;
        }
        recent_name(app, "LAUNCH");
#ifdef SDL_PLATFORM_ANDROID
        tell(app);
#endif
        int status;
        if (gui_process_finished(app->process, &status)) {
            app->menu.running = false;
            /* Normal exits wait for acknowledgement. ESC already acknowledged
               an abort, so return straight to the menu after nonblocking reaping. */
            app->finished = !app->aborting;
            app->abnormal_exit = app->aborting || status != 0;
            gui_steps_finish(&app->steps, !app->abnormal_exit);
            if (app->run_out) {
                fprintf(app->run_out, "\n[%s, status %d]\n", app->aborting ? "aborted" : "ended", status);
                fclose(app->run_out);
                app->run_out = NULL;
            }
            recent_name(app, "LAUNCH");
            recent_name(app, "RECOMPILE"); /* a run that ended before its game loaded */
            app->menu.view = GUI_LOAD_NONE;
            if (app->aborting)
                snprintf(app->message, sizeof(app->message), "Program ended abnormally (force-terminated by ESC).");
            else if (app->abnormal_exit)
                snprintf(app->message, sizeof(app->message), "Program ended abnormally (status %d).", status);
            else
                snprintf(app->message, sizeof(app->message), "Driver exited with status %d.", status);
            recent_note(app);
            app->aborting = false;
            app->confirm.shown = false; /* a run that ended while asking has nothing left to abort */
            layout(app);
#ifdef SDL_PLATFORM_ANDROID
            /* the game opens in its own activity, over this one: coming back from it is coming back to the menu.
               Android lets no app behind others open one, so then it waits for the menu to show */
            if (!app->abnormal_exit && app->hidden)
                app->play_later = true;
            else if (!app->abnormal_exit && ready(app))
                leave_run(app);
            /* seen here, the end needs no word; behind other apps one stays until the menu shows */
            gui_android_ran(!app->hidden ? NULL : app->abnormal_exit ? "Failed" : "Ready");
#endif
        }
    }
#ifdef SDL_PLATFORM_ANDROID
    if (app->play_later && !app->hidden) {
        app->play_later = false;
        if (ready(app)) leave_run(app);
    }
#endif
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->sampled = now;
    }
    if (app->left_shown && (!app->left_at || now - app->left_at >= 5000)) { /* the player may have moved them */
        left_behind(app);
        app->left_shown = app->lefts > 0;
    }
    if (app->leaving_ms && now_ms(app) - app->leaving_ms >= app->theme.fade_out_ms) leave_run(app);
    /* games put in or taken out of the folders while the menu is open, looked for once a second: Games and its list
       follow, the list on the file it had */
    if (!app->menu.running && !app->finished && now_ms(app) - app->games_checked >= 1000) {
        app->games_checked = now_ms(app);
        if (games_stamp(app) != app->games_stamp) {
            app->games_stamp = games_stamp(app);
            if (app->menu.view == GUI_LOAD_GAMES) {
                GuiBrowser was = app->browser;
                list_games(app);
                app->browser.page = was.page;
                app->browser.selected = was.selected;
                gui_browser_resize(&app->browser, app->browser.page_size);
            }
        }
    }
}
static void launch(App* app, char* absolute, bool prepared) {
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "That path is not a readable file. Try again or press ESC.");
        free(absolute);
        return;
    }
    /* The 3DS browser lists CIA inputs, not prepared host executables. */
    if (app->menu.selected == GUI_3DS) prepared = false;
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    /* a game in a folder of its own goes through the driver, which recompiles it first when another build made it; one
       an older version laid out beside the others runs as it is, as every game on iOS, which runs no other program */
#ifdef SDL_PLATFORM_IOS
    bool direct = native && !app->recompile;
#else
    bool direct = native && !app->recompile && !gui_game_folder(absolute);
#endif
    char* driver = direct ? NULL : find_driver(app, app->menu.selected);
    if (!direct && !driver) {
        snprintf(app->message, sizeof(app->message), "Cannot find %s beside the GUI or on PATH.",
                 driver_name(app->menu.selected) ? driver_name(app->menu.selected) : "the selected driver");
        free(absolute);
        return;
    }
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    if (direct) {
        if (play(app, absolute) && app->recent_file && !gui_recent_add(&app->recent, absolute, GUI_SWITCH, true))
            recent_changed(app);
        free(absolute);
        return;
    }
#endif
    /* the folders picked in Settings > App > Folders, which a Switch or 3DS run works in: refused when one cannot be */
    bool folders = (app->menu.selected == GUI_SWITCH && !native) || app->menu.selected == GUI_3DS;
    if (folders && !places_fit(app, app->message, sizeof app->message)) {
        free(driver);
        free(absolute);
        return;
    }
    /* a dump not extracted yet may need more room than its disks have: asked first, Run anyway pressed twice */
    if (folders && !app->room_ok && !room_fits(app, absolute)) {
        free(app->room_file);
        app->room_file = absolute;
        app->room_item = app->menu.selected;
        app->room_prepared = prepared;
        app->room_recompile = app->recompile;
        gui_confirm_open(&app->confirm, GUI_ASK_ROOM, true);
        free(driver);
        return;
    }
    /* the driver, the file, the settings, --repo and its folder, the picked games and extraction folders, NULL */
    GuiSettings* s = settings_of(app);
    const char** argv = (const char**)snx_alloc((size_t)(s->count + 10) * sizeof(*argv));
    char* out = NULL; /* a dump's extraction in the picked extractions folder */
    size_t n = 1;
    argv[0] = direct ? absolute : driver;
    if (app->recompile) argv[n++] = "-r";
    if (!direct) argv[n++] = absolute;
    /* Switch options only, never guest argv for the CPU-only Linux driver or the 3DS one; R's recompile takes the
       backend picked alone, no option of a run. The drivers work in the menu's data folder, so they list the same
       games */
    if (app->recompile && gui_settings_backend(s))
        argv[n++] = gui_settings_backend(s);
    else if (app->menu.selected == GUI_SWITCH && !app->recompile)
        n += gui_settings_options(s, 1, native, argv + n);
    if (folders || (native && !direct)) {
        argv[n++] = "--repo";
        argv[n++] = app->data_dir;
    }
    if (folders && app->menu.selected == GUI_SWITCH && app->place[GUI_PLACE_GAMES]) {
        argv[n++] = "--outdir";
        argv[n++] = app->games_dir;
    }
    if (folders && app->place[GUI_PLACE_WORK] && (app->menu.selected == GUI_3DS || !nro_file(absolute))) {
        out = dump_out(app, absolute, app->menu.selected == GUI_3DS ? "3ds" : NULL); /* an NRO has no extraction */
        argv[n++] = app->menu.selected == GUI_3DS ? "--output" : "--out";
        argv[n++] = out;
    }
    argv[n] = NULL;
#ifndef SDL_PLATFORM_ANDROID /* there the app's own storage, the one place both read */
    pad_mappings_share(app);
#endif
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    if (started && app->run_log) {
        size_t k;
        if (app->run_out) fclose(app->run_out);
        app->run_out = snx_fopen(app->run_log, "wb");
        for (k = 0; app->run_out && k < n; ++k) fprintf(app->run_out, "%s%s", k ? " " : "$ ", argv[k]);
        if (app->run_out) fputc('\n', app->run_out);
    }
    free(argv);
    free(out);
    gui_steps_reset(&app->steps);
    app->native = native;
    /* the menu fades out, then the run fades in */
    Uint64 now = SDL_max(now_ms(app), 1);
    app->entering_ms = started && app->theme.fade_out_ms ? now : 0;
    app->fade_ms = started ? now + (app->entering_ms ? app->theme.fade_out_ms : 0) : 0;
    app->leaving_ms = 0;
    free(driver);
    if (!started) {
        free(absolute);
        return;
    }
    free(app->file);
    app->file = absolute;
    if (app->recent_file && !gui_recent_add(&app->recent, absolute, app->menu.selected, prepared)) recent_changed(app);
    app->recent_named = 0;
    app->menu.view = GUI_LOAD_NONE;
    app->menu.running = true;
    app->finished = false;
    app->abnormal_exit = false;
    app->message[0] = 0;
    gui_console_init(&app->console, app->cols - 2, app->rows - 8);
    app->has_output = true;
    layout(app);
#ifdef SDL_PLATFORM_ANDROID
    /* now, while the menu shows: Android 12 starts no notification's service for an app behind others */
    app->told_ms = 0;
    tell(app);
#endif
}
/* the answer to a run's room: Run anyway, pressed twice, runs it as it was asked; Back runs nothing */
static void room_answer(App* app, bool taken) {
    char* file = app->room_file;
    int item = app->menu.selected;
    app->room_file = NULL;
    if (!taken) {
        free(file);
        return;
    }
    app->room_ok = true;
    app->recompile = app->room_recompile;
    app->menu.selected = app->room_item;
    launch(app, file, app->room_prepared);
    app->menu.selected = item;
    app->room_ok = false;
    app->recompile = false;
}
/* a recent game, run as the menu would: its driver, the options the menu has now */
static void open_recent(App* app, int index) {
    GuiRecentGame game = app->recent.game[index];
    if (!snx_is_file(game.path)) {
        snprintf(app->message, sizeof(app->message), "%s is gone (%s): it left the recent games.", game.name,
                 game.path);
        gui_recent_remove(&app->recent, index);
        recent_changed(app);
        return;
    }
    int item = app->menu.selected;
    app->menu.selected = game.driver;
    launch(app, snx_strdup(game.path), game.prepared);
    app->menu.selected = item;
}
/* a game of Games' page, at index of the list: a CIA is the 3DS driver's, anything else a Switch game's, ready to run
   or a dump to prepare */
static void open_game(App* app, size_t index) {
    int item = app->menu.selected;
    app->menu.selected = gui_is_cia(app->games.names[index]) ? GUI_3DS : GUI_SWITCH;
    launch(app, snx_absolute(app->games.paths[index]), !gui_is_dump(app->games.names[index]));
    if (!app->menu.running) app->menu.selected = item;
}
/* R on Games: the focused Switch game recompiled where it is, or its dump prepared again, playing nothing; else why
   not */
static void recompile_focus(App* app) {
    size_t row = app->browser.selected, head = app->browser.head;
    size_t index = app->browser.page * app->browser.page_size + row - head;
    bool recent = row && row < head && (int)row - 1 < recent_shown(app), game = row >= head && index < app->games.count;
    if (!recent && !game)
        snprintf(app->message, sizeof app->message, "No game in focus to recompile.");
    else if (recent ? app->recent.game[row - 1].driver != GUI_SWITCH : gui_is_cia(app->games.names[index]))
        snprintf(app->message, sizeof app->message, "R recompiles Switch games only.");
    else {
        app->recompile = true;
        if (recent)
            open_recent(app, (int)row - 1);
        else
            open_game(app, index);
        app->recompile = false;
    }
}
#define SNX_GUI_KEYS_MAX (1 << 20) /* a prod.keys is a few kilobytes: past this it is not one */
/* the bytes of file (a path, or on Android a document's URI, which SDL opens through its app), up to SNX_GUI_KEYS_MAX;
   NULL when it cannot be read or is bigger */
static char* keys_read(const char* file, size_t* size) {
    SDL_IOStream* io = SDL_IOFromFile(file, "rb");
    char* data = io ? (char*)malloc(SNX_GUI_KEYS_MAX + 1) : NULL;
    size_t n = 0, got = 1;
    while (data && n <= SNX_GUI_KEYS_MAX && got) n += got = SDL_ReadIO(io, data + n, SNX_GUI_KEYS_MAX + 1 - n);
    if (data && (n > SNX_GUI_KEYS_MAX || SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR)) {
        free(data);
        data = NULL;
    }
    if (io) SDL_CloseIO(io);
    *size = n;
    return data;
}
/* the keys read into keys_path */
/* what installing keys did, on their screen in app->message until it goes back to the menu by itself */
static void keys_result(App* app, bool ok) {
    if (app->menu.view != GUI_LOAD_KEYS) return;
    app->keys_ok = ok;
    app->keys_done = now_ms(app) ? now_ms(app) : 1;
}
static void keys_write(App* app, const char* data, size_t size) {
    if (!app->keys_path) {
        snprintf(app->message, sizeof(app->message), "Cannot locate your home directory. Keys were not installed.");
        keys_result(app, false);
        return;
    }
    /* Stage privately on the same filesystem, then replace atomically. A failed
       copy (or dropping the installed file itself) must not destroy existing keys. */
    char* directory = snx_dirname(app->keys_path);
    char* temporary = directory && !snx_mkdirs(directory) ? snx_temp_dir(directory) : NULL;
    char* staged = temporary ? snx_path_join(temporary, "prod.keys") : NULL;
    bool ok = staged && !snx_write_file(staged, data, size);
#ifndef _WIN32
    if (ok) ok = chmod(staged, 0600) == 0;
#endif
    if (ok) ok = !snx_replace(staged, app->keys_path);
    if (temporary) snx_remove_tree(temporary);
    free(staged);
    free(temporary);
    free(directory);
    snprintf(app->message, sizeof(app->message), "%s",
             ok ? "prod.keys installed: games can be extracted now."
                : "Cannot install prod.keys. Check file and folder permissions, then try again.");
    keys_result(app, ok);
}
static void install_keys(App* app, const char* source) {
    const char* name = source;
    char* data = NULL;
    size_t size = 0;
    if (source)
        for (const char* p = source; *p; ++p)
            if (*p == '/' || *p == '\\') name = p + 1;
    if (source && !strcmp(name, "prod.keys") && snx_is_file(source)) data = keys_read(source, &size);
    if (!data) {
        snprintf(app->message, sizeof(app->message), "Please drop a readable file named prod.keys.");
        keys_result(app, false);
    } else
        keys_write(app, data, size);
    free(data);
}
/* what a picked file that cannot be used says, for the platform's picker: Android's documents, a desktop's files */
#ifdef SDL_PLATFORM_ANDROID
#define SAY_KEYS_UNREAD "This file could not be read. Pick prod.keys again, from the device's storage (%s)."
#define SAY_KEYS_NOT    "This file is not a prod.keys. Pick the prod.keys file (%s)."
#define SAY_PICK_UNREAD                                                                                                \
    "Cannot read this file by its path. Check that ScarletNX may access the device's files in Android's settings, "    \
    "then pick it from the device's storage (%s)."
#else
#define SAY_KEYS_UNREAD "The picked file could not be read as prod.keys (%s)."
#define SAY_KEYS_NOT    "The picked file is not a prod.keys (%s)."
#define SAY_PICK_UNREAD "Pick the file from this computer's storage (%s)."
#endif
/* keys the picker gave as a document of an app's (Android's), read through it: a path to it may be one this app cannot
   read, or there is none (Downloads by number, #280). With no name to go by, its text has to be a prod.keys' */
static void install_keys_document(App* app, const char* uri) {
    size_t size = 0;
    char* data = keys_read(uri, &size);
    if (!data)
        snprintf(app->message, sizeof(app->message), SAY_KEYS_UNREAD, uri);
    else if (!gui_keys_text(data, size))
        snprintf(app->message, sizeof(app->message), SAY_KEYS_NOT, uri);
    else
        keys_write(app, data, size);
    if (!data || !gui_keys_text(data, size)) keys_result(app, false);
    free(data);
}
static void drop(App* app, const char* value, bool text_drop) {
    if ((app->menu.view != GUI_LOAD_DROP && app->menu.view != GUI_LOAD_KEYS) || app->menu.running || app->finished)
        return;
    char* path = gui_drop_path(value, text_drop);
    if (!path) {
        snprintf(app->message, sizeof(app->message), "Drop one local file path, not a list or remote URL.");
        keys_result(app, false);
        return;
    }
    char* absolute = snx_absolute(path);
    free(path);
    if (app->menu.view == GUI_LOAD_KEYS) {
        install_keys(app, absolute);
        free(absolute);
    } else {
        /* Hovering another main item must not change the driver for this drop. */
        app->menu.selected = app->menu.active;
        launch(app, absolute, false);
    }
}
/* the system's file picker answers on its own thread: the choice comes back to event() as an app->picked event */
static void SDLCALL picker_done(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    SDL_PushEvent(&e);
}
static void picked(App* app, const char* uri);
static void open_picker(App* app) {
    app->keys_done = 0; /* another try: the last one's outcome no longer stands */
    if (!app->picked) app->picked = SDL_RegisterEvents(1);
#ifdef SDL_PLATFORM_IOS /* SDL has no file dialog there: the document picker (dist/apple) */
    if (app->picked) gui_apple_pick(app->window, 0, picker_done, &app->picked, app->message, sizeof(app->message));
#else
    if (app->picked) SDL_ShowOpenFileDialog(picker_done, &app->picked, app->window, NULL, 0, NULL, false);
#endif
}
/* the last max bytes of a file, as text with only ASCII (what Java takes as a string from any log), or "" */
static char* log_tail(const char* path, size_t max) {
    size_t size = 0, i;
    unsigned char* data = path ? snx_read_file(path, &size) : NULL;
    char* text = (char*)snx_alloc(SDL_min(size, max) + 1);
    for (i = 0; i < SDL_min(size, max); ++i) {
        unsigned char c = data[size - SDL_min(size, max) + i];
        text[i] = c == '\n' || c == '\t' || (c >= 32 && c < 127) ? (char)c : '?';
    }
    text[i] = 0;
    free(data);
    return text;
}
/* the game log written last, games/<game>/<game>.log, or an older version's games/<game>.log: beside the games, or an
   installed build's in its state folder */
static char* newest_log(const App* app) {
    char *folder = snx_user_path(app->games_dir, SNX_USER_STATE), *newest = NULL, **names = NULL;
    size_t count = 0, i, n;
    SDL_Time time = 0;
    if (folder) snx_list_dir(folder, &names, &count);
    for (i = 0; i < count; ++i) {
        SDL_PathInfo info;
        char *path = snx_path_join(folder, names[i]), *log = NULL;
        n = strlen(names[i]);
        if (path && snx_is_dir(path)) { /* a game's folder: its own log */
            char* own = snx_format("%s.log", names[i]);
            log = snx_path_join(path, own);
            free(own);
        } else if (n > 4 && !strcmp(names[i] + n - 4, ".log")) {
            log = path;
            path = NULL;
        }
        if (log && SDL_GetPathInfo(log, &info) && info.type == SDL_PATHTYPE_FILE &&
            (!newest || info.modify_time > time)) {
            free(newest);
            newest = log;
            log = NULL;
            time = info.modify_time;
        }
        free(log);
        free(path);
    }
    snx_free_strings(names, count);
    free(folder);
    return newest;
}
/* the message on the screen, the last run's output and the newest game log, as scarletnx.log in the data folder's state
   folder (the working folder without one: started from the desktop, that is the home folder), then on Android handed
   to the share sheet, elsewhere its path copied */
static void share_log(App* app) {
    char *game, *run, *played, *text, *saved, *state = NULL;
    const char* folder;
    folder = app->cwd;
    if (app->data_dir && snx_app_dir(app->data_dir)) state = snx_app_kind_dir(app->data_dir, SNX_USER_STATE);
    if (state && snx_mkdirs(state)) {
        free(state);
        state = NULL;
    }
    if (state) folder = state;
    left_behind(app);
    game = newest_log(app);
    run = log_tail(app->run_log, 1 << 20);
    played = log_tail(game, 4 << 20);
    text = snx_format("%s\nMessage: %s\n%s%s%s\n== the last run: %s ==\n%s\n\n== %s ==\n%s\n", snx_build_info(),
                      app->message, app->left ? app->left : "", app->cleaned.count ? "Clean old settings:\n" : "",
                      app->cleaned.text ? app->cleaned.text : "", app->run_log ? app->run_log : "", run,
                      game ? game : "no game log", played);
    saved = folder ? snx_path_join(folder, "scarletnx.log") : NULL;
    if (!saved || snx_write_file(saved, text, strlen(text)))
        snprintf(app->message, sizeof(app->message), SAY_LOG_FAILED);
#ifdef SDL_PLATFORM_ANDROID
    else if (gui_android_share(saved, app->message, sizeof(app->message)))
        snprintf(app->message, sizeof(app->message), "Log saved to %s", saved);
#elif defined(SDL_PLATFORM_APPLE)
    else if (gui_apple_share(app->window, saved, app->message, sizeof(app->message)))
        snprintf(app->message, sizeof(app->message), "Log saved to %s", saved);
#else
    else if (SDL_SetClipboardText(saved))
        snprintf(app->message, sizeof(app->message), "Log saved to %s (path copied)", saved);
    else
        snprintf(app->message, sizeof(app->message), "Log saved to %s", saved);
#endif
    free(saved);
    free(text);
    free(played);
    free(run);
    free(game);
    free(state);
}
/* games/'s games and the player's folders', every kind; the first folder that cannot be read is named, the others
   still listed */
static void games_scan(App* app, GuiGames* games, char* message, size_t size) {
    int kinds = GUI_GAMES_RUN | GUI_GAMES_CIA, i;
    char error[sizeof app->message];
    gui_games_free(games);
    if (!gui_games_add(games, app->games_dir, kinds, error, sizeof error))
        snprintf(message, size, "Cannot open games/. Create it in the displayed working directory.");
    else
        message[0] = 0;
    for (i = 0; i < app->folder_count; ++i)
        if (!gui_games_add_deep(games, app->folders[i], kinds, app->folders_deep[i] ? GUI_FOLDER_DEPTH : 0, error,
                                sizeof error) &&
            !message[0])
            snprintf(message, size, "%s It is in Settings > App > Game folders.", error);
}
/* the names dir holds into h, and with depth those of its folders, down that many levels, hidden ones left alone */
static uint64_t stamp_dir(uint64_t h, const char* dir, int depth) {
    char** names = NULL;
    size_t count = 0, k;
    const char* c;
    if (!dir || snx_list_dir(dir, &names, &count)) count = (size_t)-1; /* none: not the same as an empty one */
    for (k = 0; k < count && names; ++k) {
        for (c = names[k];; ++c) {
            h = (h ^ (unsigned char)*c) * 1099511628211ull;
            if (!*c) break;
        }
        if (depth > 0 && names[k][0] != '.') {
            char* sub = snx_path_join(dir, names[k]);
            if (sub && snx_is_dir(sub)) h = stamp_dir(h, sub, depth - 1);
            free(sub);
        }
    }
    h = (h ^ (uint64_t)count) * 1099511628211ull;
    if (names) snx_free_strings(names, count);
    return h;
}
/* the names games/ and the player's folders hold (a folder kept with its subfolders, theirs too), hashed: a game put
   in, taken out or renamed changes it, and so does a folder that comes or goes */
static uint64_t games_stamp(const App* app) {
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    int i;
    for (i = -1; i < app->folder_count; ++i)
        h = stamp_dir(h, i < 0 ? app->games_dir : app->folders[i],
                      i >= 0 && app->folders_deep[i] ? GUI_FOLDER_DEPTH : 0);
    return h;
}
static void list_games(App* app) {
    games_scan(app, &app->games, app->message, sizeof app->message);
    app->games_stamp = games_stamp(app);
    memset(&app->browser, 0, sizeof(app->browser));
    app->browser.count = app->games.count;
    app->browser.head = games_head(app);
    layout(app);
}
/* the screen a tab opens: the menu, Games or the Settings. A theme edited and not saved keeps its screen, the note
   saying why */
static void go_tab(App* app, int tab) {
    if (app->editing) {
        if (app->editor.typing || tab == GUI_TAB_SETTINGS) return;
        if (app->form == GUI_FORM_THEME && app->editor.dirty) {
            snprintf(app->editor.note, sizeof app->editor.note,
                     "Not saved: Ctrl+S saves it, ESC twice leaves without it");
            return;
        }
        keep_settings(app);
        if (app->form == GUI_FORM_PAD) pad_close(app);
        device_stop(app);
        leave_editor(app);
        app->theme_from_app = false;
    }
    if (tab == tab_shown(app)) return;
    app->message[0] = 0;
    app->menu.view = tab == GUI_TAB_GAMES ? GUI_LOAD_GAMES : GUI_LOAD_NONE;
    if (tab == GUI_TAB_GAMES)
        list_games(app);
    else if (tab == GUI_TAB_SETTINGS)
        open_settings(app);
}
/* the tab step tabs from the one shown, round the three: Tab and Shift+Tab, a pad's shoulders */
static void screen_tab(App* app, int step) {
    if (app->menu.running || app->finished || app->confirm.shown || app->splash) return;
    go_tab(app, ((tab_shown(app) + step) % GUI_TABS + GUI_TABS) % GUI_TABS);
}
static void menu_key(App* app, GuiKey key) {
    GuiLoadView before = app->menu.view; /* the screen this key was pressed on */
    if (app->finished) {
        if (key == GUI_CANCEL && app->theme.fade_out_ms && !app->leaving_ms)
            app->leaving_ms = SDL_max(now_ms(app), 1); /* the run fades out first; pump() comes back */
        else if (key == GUI_CANCEL)
            leave_run(app);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        /* Resizes and key events can arrive in the same SDL poll batch. */
        layout(app);
        GuiBrowserAction action = gui_browser_key(&app->browser, key);
        size_t row = app->browser.selected;
        if (action == GUI_BROWSER_BACK) {
            app->menu.view = GUI_LOAD_NONE;
            app->message[0] = 0;
        } else if (action == GUI_BROWSER_HEAD && !row)
            open_folder_picker(app);
        else if (action == GUI_BROWSER_HEAD && (int)row - 1 < recent_shown(app))
            open_recent(app, (int)row - 1);
        else if (action ==
                 GUI_BROWSER_HEAD) { /* ...show more: every recent game, the focus on the first that was hidden */
            app->recent_all = true;
            app->browser.head = games_head(app);
            layout(app);
        } else if (action == GUI_BROWSER_OPEN) {
            size_t index = app->browser.page * app->browser.page_size + row - app->browser.head;
            if (index < app->games.count) open_game(app, index);
        }
        return;
    }
    if (pick_screen(app) && key != GUI_SELECT && key != GUI_CANCEL) return; /* nothing there to move to */
    switch (gui_menu_key(&app->menu, key)) {
        case GUI_MENU_SETTINGS: open_settings(app); break;
        case GUI_MENU_LOG: share_log(app); break;
#ifdef SDL_PLATFORM_ANDROID
        case GUI_MENU_DATA: gui_android_open_data(app->message, sizeof app->message); break;
#endif
        default: break;
    }
    if (key == GUI_CANCEL || (key == GUI_SELECT && pick_screen(app))) app->message[0] = 0;
    /* the screen opens first, so what happens shows there; a select on it (a tap in its box) opens the picker, whose
       answer may come at once */
    if (key == GUI_SELECT && app->menu.view == before && (before == GUI_LOAD_DROP || before == GUI_LOAD_KEYS))
        open_picker(app);
    if (app->menu.view == GUI_LOAD_GAMES) list_games(app);
    /* Back or ESC with nothing open to close: on Android the app goes behind, as an app's first screen does; on a
       computer the menu asks before it closes, as rEden does */
    if (key == GUI_CANCEL && before == GUI_LOAD_NONE && app->menu.view == GUI_LOAD_NONE && !app->menu.running) {
#ifdef SDL_PLATFORM_ANDROID
        gui_android_background(app->message, sizeof(app->message));
#else
        gui_confirm_open(&app->confirm, GUI_ASK_EXIT, false);
#endif
    }
}
static void abort_run(App* app) {
    if (app->aborting) return;
    /* Do not ask a hung program to shut down or wait on it in the event
       handler. Kill its tree now and let pump reap it without blocking. */
    gui_process_terminate(app->process);
    app->aborting = true;
}
/* a key on the question shown (gui_confirm_key); answered, what it asked about: the menu closed, a run aborted or
   started, a folder added or used as the choice says */
static void ask_key(App* app, GuiKey key) {
    GuiAsk ask = app->confirm.ask;
    bool second = gui_confirm_key(&app->confirm, key);
    if (app->confirm.shown) return;
    if (ask == GUI_ASK_DEEP)
        folder_answer(app, key == GUI_SELECT, second);
    else if (ask == GUI_ASK_OUTSIDE)
        outside_answer(app, second);
    else if (ask == GUI_ASK_ROOM)
        room_answer(app, second);
    else if (second && ask == GUI_ASK_EXIT)
        app->menu.quit = true;
    else if (second)
        abort_run(app);
}
/* the pointer on the question, at pixel x, y: a choice under it takes the focus, a click presses it; a click off the
   box is Back */
static void ask_pointer(App* app, float x, float y, bool click) {
    GuiRect r;
    GuiList l;
    int i;
    ask_boxes(app, &r, &l);
    if ((i = list_hit(&l, x, y, true)) >= 0) {
        gui_confirm_focus(&app->confirm, i);
        if (click) ask_key(app, GUI_SELECT);
    } else if (click && (x < (float)r.x || x >= (float)(r.x + r.w) || y < (float)r.y || y >= (float)(r.y + r.h)))
        ask_key(app, GUI_CANCEL);
}
/* the tab at pixel x, y, -1 for none */
static int tab_hit(App* app, float x, float y) {
    GuiPlace p;
    int i;
    place_screen(app, &p, "", 0, GUI_SHOWS_TABS);
    for (i = 0; i < GUI_TABS; ++i) {
        GuiRect r = tab_rect(app, p.tabs, i);
        if (x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y && y < (float)(r.y + r.h)) return i;
    }
    return -1;
}
/* the pointer on a screen, at pixel x, y: the box under it takes the focus, and a click selects it; on the
   screen a file is picked on, a click on the box the waiting line is in opens the picker */
static void screen_pointer(App* app, float x, float y, bool click, bool touch) {
    GuiMenuBoxes m;
    int i;
    if ((i = tab_hit(app, x, y)) >= 0) {
        if (click) go_tab(app, i);
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        GuiGamesBoxes g;
        games_boxes(app, &g);
        if (list_hit(&g.add, x, y, touch) == 0)
            app->browser.selected = 0;
        else if ((i = list_hit(&g.list, x, y, touch)) > 0 || (i == 0 && !g.lead))
            app->browser.selected = i < g.lead ? (size_t)i : app->browser.head + (size_t)(i - g.lead);
        else
            return; /* off them, or on the label */
        if (click) menu_key(app, GUI_SELECT);
        return;
    }
    if (pick_screen(app)) {
        GuiPickBoxes k;
        pick_boxes(app, &k);
        if (click && list_hit(&k.back, x, y, touch) == 0)
            menu_key(app, GUI_CANCEL);
        else if (click && y >= (float)k.place.list.y && y < (float)k.back.at.y && x >= (float)k.place.list.x &&
                 x < (float)(k.place.list.x + k.place.list.w))
            menu_key(app, GUI_SELECT); /* the box the waiting line is in */
        return;
    }
    menu_boxes(app, &m);
    if ((i = list_hit(&m.list, x, y, touch)) < 0) return;
    app->menu.selected = i;
    if (click) menu_key(app, GUI_SELECT);
}
/* the keys the foot names, a word each ("Up/Down" is two): what a click or a tap on the word presses. Space selects
   where the foot names it, as Enter; Add a folder's opens the folder picker, as Enter on its row */
static const struct {
    const char* word;
    SDL_Keycode key;
    SDL_Keymod mod;
} hint_keys[] = {{"ESC", SDLK_ESCAPE, 0},
                 {"Back", SDLK_AC_BACK, 0},
                 {"Tab", SDLK_TAB, 0},
                 {"Enter", SDLK_RETURN, 0},
                 {"Space", SDLK_RETURN, 0},
                 {"click", SDLK_RETURN, 0},
                 {"Tap", SDLK_RETURN, 0},
                 {"A", SDLK_RETURN, 0},
                 {"Up", SDLK_UP, 0},
                 {"Down", SDLK_DOWN, 0},
                 {"Left", SDLK_LEFT, 0},
                 {"Right", SDLK_RIGHT, 0},
                 {"PgUp", SDLK_PAGEUP, 0},
                 {"PgDn", SDLK_PAGEDOWN, 0},
                 {"PgDown", SDLK_PAGEDOWN, 0},
                 {"Swipe across", SDLK_PAGEDOWN, 0},
                 {"Del", SDLK_DELETE, 0},
                 {"R", SDLK_R, 0},
                 {"Recompile", SDLK_R, 0},
                 {"Ctrl+S", SDLK_S, SDL_KMOD_CTRL},
                 {"Ctrl+V", SDLK_V, SDL_KMOD_CTRL},
                 {"Add a folder", SDLK_UNKNOWN, 0}};
/* the hint_keys entry of the n bytes at word, -1 for none */
static int hint_word(const char* word, size_t n) {
    int i;
    for (i = 0; i < (int)SDL_arraysize(hint_keys); ++i)
        if (strlen(hint_keys[i].word) == n && !strncmp(hint_keys[i].word, word, n)) return i;
    return -1;
}
/* the hint_keys entry a click or a tap col columns into the foot's sentence of key (hints()) presses, -1 for none: the
   word of key it is on, the slash after a word its own and the margin before the first's; the sentence's text, and the
   margin after it, what every word does when they all do one thing */
static int hint_pick(const char* key, int col) {
    const char *word = key, *end;
    int k = (int)strlen(key), pick = -1, first = 1, w;
    for (;; word = end + 1) {
        end = strchr(word, '/');
        if (!end) end = word + strlen(word);
        w = hint_word(word, (size_t)(end - word));
        if (col < k && col <= (int)(end - key)) {
            pick = w;
            break;
        }
        if (col >= k && first)
            pick = w;
        else if (col >= k && (w < 0 || pick < 0 || hint_keys[w].key != hint_keys[pick].key ||
                              hint_keys[w].mod != hint_keys[pick].mod))
            pick = -1;
        first = 0;
        if (!*end) break;
    }
    return pick;
}
/* the key it picks pressed, as on the keyboard; whether there was one */
static bool hint_press(App* app, const char* key, int col) {
    int pick = hint_pick(key, col);
    SDL_Event e;
    if (pick < 0) return false;
    if (hint_keys[pick].key == SDLK_UNKNOWN) {
        open_folder_picker(app);
        return true;
    }
    SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = hint_keys[pick].key;
    e.key.mod = hint_keys[pick].mod;
    e.key.down = true;
    event(app, &e);
    return true;
}
/* the boxes of the screen shown, and the keys its foot says */
static const char* shown_place(App* app, GuiPlace* p, char* keys, size_t size) {
    if (app->editing && app->form == GUI_FORM_PAD)
        place_screen(app, p, shown_keys(app, keys, size), 0, GUI_SHOWS_TABS); /* render_pad's */
    else if (app->editing) {
        GuiFormBoxes f;
        form_boxes(app, &f);
        *p = f.place;
    } else if (app->menu.view == GUI_LOAD_GAMES)
        games_place(app, p);
    else if (pick_screen(app)) {
        GuiPickBoxes k;
        pick_boxes(app, &k);
        *p = k.place;
    } else {
        GuiMenuBoxes m;
        menu_boxes(app, &m);
        *p = m.place;
    }
    return shown_keys(app, keys, size);
}
/* a click or a tap at pixel x, y on a key of the foot presses it (hint_press), the keys' box's whole height their rows';
   whether it did */
static bool foot_press(App* app, float x, float y) {
    GuiPlace p;
    GuiRect text;
    GuiHintHit hit;
    char keys[256];
    const char* say = shown_place(app, &p, keys, sizeof keys);
    int s = app->scale, rows;
    if (x < (float)p.keys.x || x >= (float)(p.keys.x + p.keys.w) || y < (float)p.keys.y ||
        y >= (float)(p.keys.y + p.keys.h))
        return false;
    app->scale = foot_scale(app);
    text = foot_text(app, p.keys, app->theme.footer_box);
    rows = hints(app, say, text.w / cell_w(app), text, text.y, 0, 0, NULL);
    hit.x = x;
    hit.y = SDL_clamp(y, (float)text.y, (float)(text.y + rows * cell_h(app) - 1));
    hit.key[0] = 0;
    hints(app, say, text.w / cell_w(app), text, text.y, 0, 0, &hit);
    app->scale = s;
    return hit.key[0] && hint_press(app, hit.key, hit.col);
}
static void pointer(App* app, float window_x, float window_y, bool click, bool touch) {
    float x, y;
    bool picking = app->editing && app->editor.picking;
    if ((app->menu.running && !app->confirm.shown) || app->finished) {
        app->hovering = false; /* a run's foot presses nothing */
        return;
    }
    layout(app);
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    app->hovering = !click && !touch; /* a finger hovers nowhere, and a click has done what it lit */
    app->hover_x = x;
    app->hover_y = y;
    /* under a question, the folder's too, a click is the question's */
    if (click && !app->confirm.shown && foot_press(app, x, y)) {
        if (touch && picking) app->pick_press = GUI_PICK_ROWS + 1; /* the picker's finger, pressed as it went down */
        return;
    }
    if (app->confirm.shown) /* over a run, the menu, Games or a form */
        ask_pointer(app, x, y, click);
    else if (app->editing && app->form == GUI_FORM_PAD) {
        if (click) pad_did(app, snx_pad_screen_tap(&app->pad, snx_pad_screen_hit(&app->pad_drawing, x, y)));
    } else if (app->editing && app->editor.picking)
        picker_pointer(app, x, y, click);
    else if (app->editing) {
        if (click) form_pointer(app, x, y, touch);
    } else
        screen_pointer(app, x, y, click, touch);
}
/* the file the system's picker chose, as a dropped one; none (cancelled) stays on the screen and says so */
static void picked(App* app, const char* uri) {
    char* path;
    if (uri && app->menu.view == GUI_LOAD_KEYS && strstr(uri, "://") && strncmp(uri, "file://", 7)) {
        install_keys_document(app, uri);
        return;
    }
    path = uri ? gui_document_path(uri) : NULL;
#ifdef SDL_PLATFORM_ANDROID
    if (uri && !path) path = gui_android_document_file(uri);
#endif
    if (!uri)
        snprintf(app->message, sizeof(app->message), "No file was picked.");
    else if (!path) {
        snprintf(app->message, sizeof(app->message), SAY_PICK_UNREAD, uri);
        keys_result(app, false);
    } else
        drop(app, path, false);
    free(path);
}
/* ESC: a run is aborted, or asked about first when ask_abort says so; anything else goes back */
static void cancel(App* app) {
    if (!app->menu.running)
        menu_key(app, GUI_CANCEL);
    else if (app->ask_abort && !app->aborting)
        gui_confirm_open(&app->confirm, GUI_ASK_ABORT, false);
    else
        abort_run(app);
}
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland
   give a first finger and any finger may have, and no real finger's id sets bit 63 */
static Uint64 finger_id(const SDL_TouchFingerEvent* f) {
    return (Uint64)f->fingerID | (f->touchID == SDL_PEN_TOUCHID ? 1ull << 63 : 0);
}
/* a row up or down for a finger's drag and the wheel, which stop at the first and last rows where the keys go round */
static void scroll(App* app, GuiKey key) {
    int row;
    if (app->editing && app->form == GUI_FORM_PAD) {
        pad_did(app, snx_pad_screen_key(&app->pad, key == GUI_UP ? SNX_PAD_KEY_UP : SNX_PAD_KEY_DOWN));
        return;
    }
    if (app->editing) {
        editor_key(app, key); /* stops at its ends */
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        if (key == GUI_UP ? app->browser.selected > 0
                          : app->browser.selected + 1 < app->browser.head + gui_browser_items(&app->browser))
            menu_key(app, key);
        return;
    }
    row = app->menu.selected;
    menu_key(app, key);
    if (key == GUI_UP ? app->menu.selected > row : app->menu.selected < row)
        menu_key(app, key == GUI_UP ? GUI_DOWN : GUI_UP); /* it went round: back */
}
/* the menu's rows go past the screen: its letters are too big for it */
static bool menu_scrolls(App* app) {
    GuiMenuBoxes m;
    layout(app);
    menu_boxes(app, &m);
    return m.list.shown < m.list.count;
}
/* a drag up or down from where the finger went down, on a menu whose letters make it scroll */
static bool drags(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y;
    return SDL_fabsf(dy) > 0.05f && SDL_fabsf(dy) > 2 * SDL_fabsf(dx) && app->menu.view != GUI_LOAD_GAMES &&
           !app->menu.running && !app->finished && (app->editing ? app->form != GUI_FORM_PAD : menu_scrolls(app));
}
/* the focus follows the finger, up as it goes up, a row each time it passes a row's height, so the highlight is seen
   going */
static void drag(App* app, float y) {
    float row = 1.0f / (float)SDL_max(1, app->rows);
    while (y <= app->finger_mark - row) {
        scroll(app, GUI_UP);
        app->finger_mark -= row;
    }
    while (y >= app->finger_mark + row) {
        scroll(app, GUI_DOWN);
        app->finger_mark += row;
    }
}
/* a finger lifted at x, y (0 to 1 across the window): a swipe across turns the games' pages; a tap clicks there, is ESC
   on a finished run and on a running one's ESC line */
static void touch(App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y, rx, ry;
    int w, h;
    if (SDL_fabsf(dx) > 0.1f && SDL_fabsf(dx) > 2 * SDL_fabsf(dy)) {
        if (app->menu.view == GUI_LOAD_GAMES && !app->menu.running && !app->finished)
            menu_key(app, dx < 0 ? GUI_PAGE_DOWN : GUI_PAGE_UP);
        return;
    }
    if (drags(app, x, y)) {
        drag(app, y); /* what its last moves did not reach */
        return;
    }
    if (SDL_fabsf(dx) > 0.03f || SDL_fabsf(dy) > 0.03f) return; /* a drag, no tap */
    if (app->finished) {
        menu_key(app, GUI_CANCEL);
        return;
    }
    SDL_GetWindowSize(app->window, &w, &h);
    if (app->confirm.shown) {
        pointer(app, x * (float)w, y * (float)h, true, true);
        return;
    }
    if (app->menu.running) {
        GuiPlace p;
        layout(app);
        if (!SDL_RenderCoordinatesFromWindow(app->renderer, x * (float)w, y * (float)h, &rx, &ry)) return;
        if (app->theme.run != GUI_RUN_STEPS) { /* the foot the ESC line is in */
            run_place(app, &p);
            if (ry >= (float)p.keys.y && ry < (float)(p.keys.y + p.keys.h)) cancel(app);
        } else if ((int)(ry / (float)(16 * app->scale)) == app->rows - 1) /* the loading screen's last row */
            cancel(app);
        return;
    }
    pointer(app, x * (float)w, y * (float)h, true, true);
}
#ifdef SDL_PLATFORM_ANDROID
/* SDL hands the app's lifecycle to watchers only, inside SDL_PollEvent on this thread */
static bool SDLCALL lifecycle(void* data, SDL_Event* e) {
    App* app = (App*)data;
    if (e->type == SDL_EVENT_DID_ENTER_BACKGROUND) app->hidden = true;
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) app->hidden = false;
    return true;
}
#endif
/* the menu's key for a key press; false for none and for Enter or Space held down */
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key) {
    switch (k->key) {
        case SDLK_UP: *key = GUI_UP; return true;
        case SDLK_DOWN: *key = GUI_DOWN; return true;
        case SDLK_LEFT: *key = GUI_LEFT; return true;
        case SDLK_RIGHT: *key = GUI_RIGHT; return true;
        case SDLK_PAGEUP: *key = GUI_PAGE_UP; return true;
        case SDLK_PAGEDOWN: *key = GUI_PAGE_DOWN; return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE: *key = GUI_SELECT; return !k->repeat;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK: *key = GUI_CANCEL; return true;
        default: return false;
    }
}
static void event(App* app, const SDL_Event* e) {
    GuiKey key;
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        app->menu.quit = true;
        return;
    }
    /* what Clean old settings did takes the first key, button or finger, which closes it */
    if (app->cleaned_shown &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN)) {
        app->cleaned_shown = false;
        return;
    }
    /* the box of an older version's files takes the first key, button or finger, which closes it */
    if (app->left_shown && !app->splash &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_FINGER_DOWN)) {
        app->left_shown = false;
        return;
    }
    /* the splash takes the input it gets: a key, a button or a finger ends it, and does nothing else */
    if (app->splash && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        app->splash_until = 0;
        return;
    }
    /* a question takes every key while it shows, over the menu, Games or a form (a run's is the run's, below); a held
       key answers nothing */
    if (app->confirm.shown && !app->menu.running &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && menu_key_of(&e->key, &key)) ask_key(app, key);
        return;
    }
    if (app->editing && app->form == GUI_FORM_PAD && pad_event(app, e)) return;
    if (app->editing && device_event(app, e)) return;
    if (app->editing && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        editor_input(app, e);
        return;
    }
    if ((e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) && app->editing &&
        app->form == GUI_FORM_PLACES) {
        char* path = gui_drop_path(e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        if (!path)
            snprintf(app->editor.note, sizeof app->editor.note, "Drop one local folder");
        else if (app->editor.row < GUI_PLACES)
            place_set(app, app->editor.row, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "Drop it on the folder it takes the place of");
        free(path);
        return;
    }
    if ((e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) && app->editing &&
        app->form == GUI_FORM_FOLDERS) {
        char* path = gui_drop_path(e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        if (path)
            folder_ask(app, path);
        else
            snprintf(app->editor.note, sizeof app->editor.note, "Drop one local folder");
        free(path);
        return;
    }
    if (e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) {
        drop(app, e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        return; /* SDL3 owns the event's data, unlike SDL2. */
    }
    /* the folder picker's answer: a folder, none, or why it could not open */
    if (app->picked_folder && e->type == app->picked_folder) {
        char* error = (char*)e->user.data2;
        int place = app->place_row;
        app->place_row = -1;
        if (e->user.data1 && place >= 0)
            place_set(app, place, (const char*)e->user.data1);
        else if (e->user.data1)
            folder_ask(app, (const char*)e->user.data1);
        else {
            char note[sizeof app->editor.note];
            if (e->user.code)
                snprintf(note, sizeof note, "The folder picker could not open: %s", error ? error : "no reason given");
            else
                snprintf(note, sizeof note, "No folder was picked.");
            if (app->editing && (app->form == GUI_FORM_FOLDERS || app->form == GUI_FORM_PLACES))
                snprintf(app->editor.note, sizeof app->editor.note, "%s", note);
            else
                snprintf(app->message, sizeof app->message, "%s: %s", place >= 0 ? "Folders" : "Game folders", note);
        }
        SDL_free(e->user.data1);
        SDL_free(error);
        return;
    }
    if (app->picked && e->type == app->picked) {
        picked(app, e->user.data1);
        SDL_free(e->user.data1);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN && !app->finger) {
        int w, h;
        app->finger = finger_id(&e->tfinger);
        app->finger_x = e->tfinger.x;
        app->finger_y = app->finger_mark = e->tfinger.y;
        SDL_GetWindowSize(app->window, &w, &h);
        app->pick_press = 0;                     /* the finger's own, not a mouse's whose lift was missed */
        if (app->editing && app->editor.picking) /* the picker's bars take a finger as it goes down, to drag */
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, true, true);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION && finger_id(&e->tfinger) == app->finger) {
        int w, h;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pick_press)
            pointer(app, e->tfinger.x * (float)w, e->tfinger.y * (float)h, false, true);
        else if (drags(app, e->tfinger.x, e->tfinger.y))
            drag(app, e->tfinger.y);
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger && app->pick_press) { /* the picker had it: no tap */
        app->finger = 0;
        app->pick_press = 0;
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) &&
        finger_id(&e->tfinger) == app->finger) {
        app->finger = 0;
        if (e->tfinger.touchID == SDL_PEN_TOUCHID) {
            app->pen_held = true;
            app->pen_x = e->tfinger.x;
            app->pen_y = e->tfinger.y;
        }
        if (e->type == SDL_EVENT_FINGER_UP) touch(app, e->tfinger.x, e->tfinger.y);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        pointer(app, e->motion.x, e->motion.y, false, false);
        return;
    }
    if (e->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        app->hovering = false;
        return;
    }
    /* a notch of the wheel is a row, as a finger's drag, the way the system turns it; not under a box */
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        int rows = e->wheel.integer_y;
        if (app->splash || app->menu.running || app->finished || app->confirm.shown || app->left_shown ||
            (app->editing && app->editor.picking))
            return;
        for (; rows > 0; --rows) scroll(app, GUI_UP);
        for (; rows < 0; ++rows) scroll(app, GUI_DOWN);
        return;
    }
    /* a hovering pen highlights as the mouse does (it sends no mouse motion). Touching, it is a finger, and once it
       lifts its hover waits until it has moved away as far as a tap may: the focus a drag moved would otherwise jump
       to the row under the hover at the lift, which SDL sends right after it */
    if (e->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
        app->pen_held = false;
        return;
    }
    if (e->type == SDL_EVENT_PEN_MOTION) {
        int w, h;
        if (e->pmotion.pen_state & SDL_PEN_INPUT_DOWN) return;
        SDL_GetWindowSize(app->window, &w, &h);
        if (app->pen_held && w > 0 && h > 0 && SDL_fabsf(e->pmotion.x / (float)w - app->pen_x) <= 0.03f &&
            SDL_fabsf(e->pmotion.y / (float)h - app->pen_y) <= 0.03f)
            return;
        app->pen_held = false;
        pointer(app, e->pmotion.x, e->pmotion.y, false, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
        pointer(app, e->button.x, e->button.y, true, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.button == SDL_BUTTON_LEFT) {
        app->pick_press = 0; /* a drag on the picker's bar ends */
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    /* a running run takes ESC, and the question, while it shows, every key; a held key asks and answers nothing */
    if (app->menu.running) {
        if (e->key.repeat || !menu_key_of(&e->key, &key)) return;
        if (app->confirm.shown)
            ask_key(app, key);
        else if (key == GUI_CANCEL)
            cancel(app);
        return;
    }
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) &&
        (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        drop(app, clipboard, true);
        SDL_free(clipboard);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat) {
        screen_tab(app, e->key.mod & SDL_KMOD_SHIFT ? -1 : 1);
        return;
    }
    if (e->key.key == SDLK_R && !e->key.repeat && !(e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) &&
        app->menu.view == GUI_LOAD_GAMES && !app->finished) {
        layout(app); /* the page a resize in the same poll gave */
        recompile_focus(app);
        return;
    }
    if (menu_key_of(&e->key, &key)) menu_key(app, key);
}
/* the themes that come with the GUI: beside it in a build tree, else where an install puts data, share/scarletnx/themes
   beside its bin/ (/usr/local, the Windows zip) */
static char* shipped_themes(const char* program_dir) {
    char* beside = snx_path_join(program_dir, "themes");
    if (snx_is_dir(beside)) return beside;
    free(beside);
    return snx_path_join(program_dir, "../share/scarletnx/themes");
}
static bool initialize(App* app, const char* self) {
    SDL_WindowFlags flags;
    memset(app, 0, sizeof(*app));
    app->menu.selected = GUI_SWITCH; /* the first item until Games shows */
    if (self) app->self = snx_strdup(self);
    app->theme = *gui_theme_builtin("yorha"); /* every platform's look until Settings > App picks another */
#ifdef SDL_PLATFORM_ANDROID
    app->ask_abort = true;
    gui_android_start();
    /* back is ESC here: without this it would close the app */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    /* a run goes on behind other apps: its output is read, or it stops once the pipe is full */
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
#endif
    /* touch() takes the fingers, and a finger and the mouse each arrive once: SDL would also send each as the other,
       and a pen as both (it stays a finger), so one tap would select twice */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
#ifdef SDL_PLATFORM_IOS
    gui_apple_start();
#endif
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
#if defined(SDL_PLATFORM_ANDROID) || defined(SDL_PLATFORM_IOS)
    /* the whole screen, the system's bars out of the way, as the Settings' fullscreen says: a phone held wide is short */
    flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    if (!SDL_CreateWindowAndRenderer(snx_build_info(), 1280, 800, flags, &app->window, &app->renderer)) return false;
    snx_window_icon(app->window);
    SDL_SetRenderVSync(app->renderer, 1);
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    app->font = create_font(app->renderer);
    app->process = gui_process_create();
    app->cwd = SDL_GetCurrentDirectory();
#ifdef SDL_PLATFORM_ANDROID
    app->drivers = gui_android_libraries() ? snx_strdup(gui_android_libraries()) : NULL;
    /* the app's own storage: no FUSE between it and the drivers, as the shared storage has */
    if (gui_android_own_folder()) {
        SDL_free(app->cwd);
        app->cwd = SDL_GetCurrentDirectory();
    }
    app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
    app->own_dir = SDL_GetAndroidInternalStoragePath() ? snx_strdup(SDL_GetAndroidInternalStoragePath()) : NULL;
#else
    app->drivers = self ? snx_dirname(self) : SDL_GetBasePath() ? snx_strdup(SDL_GetBasePath()) : NULL;
    app->data_dir = snx_data_dir(app->drivers);
    if (!app->data_dir) app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
    app->checkout = snx_checkout(GUI_CHECKOUT, GUI_BUILD, app->drivers);
#endif
    app->home_dir = snx_strdup(app->data_dir);
    app->place_row = -1;
    places_apply(app); /* games/, themes/, work/ and the keys, by the data folder */
    if (app->drivers) app->themes_dirs[1] = shipped_themes(app->drivers);
    gui_console_init(&app->console, 78, 17);
    if (!app->font || !app->process || !app->drivers) {
        SDL_SetError("Cannot initialize GUI resources");
        return false;
    }
#ifdef SDL_PLATFORM_ANDROID
    SDL_AddEventWatch(lifecycle, app);
#endif
    layout(app);
    return true;
}
static void cleanup(App* app) {
#ifdef SDL_PLATFORM_ANDROID
    SDL_RemoveEventWatch(lifecycle, app);
    gui_android_ran(NULL); /* the run goes with the menu */
#endif
    gui_process_destroy(app->process);
    space_stop(app);
    space_end(&app->left_space, &app->left_counter);
    gui_settings_free(&app->settings);
    free(app->settings_file);
    for (int i = 0; i < app->folder_count; ++i) free(app->folders[i]);
    free(app->folder_asked);
    free(app->own_dir);
    free(app->outside_path);
    free(app->room_file);
    free(app->folders_file);
    free(app->app_file);
    gui_cleaned_free(&app->cleaned);
    free(app->left);
    free(app->self);
    free(app->drivers);
    free(app->file);
    free(app->data_dir);
    free(app->home_dir);
    for (int i = 0; i < GUI_PLACES; ++i) free(app->place[i]);
    free(app->work_dir);
    free(app->checkout);
    free(app->games_dir);
    free(app->themes_dirs[0]);
    free(app->themes_dirs[1]);
    gui_themes_free(&app->themes);
    free(app->keys_path);
    free(app->recent_file);
    if (app->run_out) fclose(app->run_out);
    free(app->run_log);
    gui_games_free(&app->games);
    SDL_free(app->cwd);
    if (app->editing) gui_editor_stop(&app->editor);
    SDL_DestroyTexture(app->preview);
    SDL_DestroyTexture(app->splash);
    snx_splash_close(app->splash_drawing);
    SDL_DestroyTexture(app->font);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}
/* the settings, its picks and its game folders in a file each */
static char* settings_path(const char* config) { return config ? snx_path_join(config, "settings.ini") : NULL; }
int snx_gui_main(const char* self) {
    App app;
    char *prefs, *config;
    if (!initialize(&app, self)) {
        fprintf(stderr, "scarletnx: %s\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", SDL_GetError(), app.window);
        cleanup(&app);
        return 1;
    }
    /* the menu's own files, opened here, not in initialize(), so the tests keep their own */
    prefs = menu_folder(&app, SNX_USER_STATE, true);
    config = menu_folder(&app, SNX_USER_CONFIG, true);
    if (prefs) recent_open(&app, snx_path_join(prefs, "recent.txt"));
    if (prefs) app.run_log = snx_path_join(prefs, "last-run.log");
    app.settings_file = settings_path(config);
    if (app.settings_file) {
        app.folders_file = snx_path_join(config, "folders.txt");
        app.app_file = snx_path_join(config, "app.ini");
        folders_load(&app);
        app_load(&app);
    }
    free(config);
    app.games_stamp = games_stamp(&app);
    space_start(&app); /* the footer's disk */
    free(prefs);
    left_behind(&app);
    app.left_shown = app.lefts > 0;
    splash_start(&app);
    while (!app.menu.quit) {
        SDL_Event e;
#ifdef SDL_PLATFORM_ANDROID
        /* behind other apps with no run to read, the menu waits to show again */
        if (app.hidden && !app.menu.running && SDL_WaitEvent(&e)) event(&app, &e);
#endif
        while (SDL_PollEvent(&e)) event(&app, &e);
        pump(&app);
#ifdef SDL_PLATFORM_ANDROID
        if (!app.hidden)
#endif
            render(&app);
        SDL_Delay(16);
    }
    cleanup(&app);
    return 0;
}
#ifndef SNX_GUI_NO_MAIN /* the menu alone: Android's activity runs it, and a build without the Switch runtime */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    return snx_gui_main(NULL);
}
#endif
