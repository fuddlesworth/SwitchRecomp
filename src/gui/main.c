/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
#include "common.h"
#include "browser.h"
#include "editor.h"
#include "font.h"
#include "model.h"
#include "gui_process.h"
#include "steps.h"
#include "theme.h"
#include "window_icon.h"
#include "build_info.h"
#include "recent.h"
#ifdef SDL_PLATFORM_ANDROID
#include "android.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

/* the checkout the drivers were built from: a build tree's menu lists the games/ its driver makes */
#ifdef SNX_SOURCE_DIR
#define GUI_CHECKOUT SNX_SOURCE_DIR
#else
#define GUI_CHECKOUT NULL
#endif

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
    GuiBanner banner;
    GuiSteps steps;
    GuiRecent recent;
    char* recent_file; /* where the recent games are kept; NULL keeps none */
    char* run_log;     /* where a run's output goes as it comes, the last run's; NULL keeps none */
    FILE* run_out;
    int recent_named;           /* the run's own name went to its recent game */
    Uint64 clock;               /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    const char* drivers;
    char *cwd, *file, *games_dir, *keys_path;
    char* data_dir;       /* games/, work/ and themes/: the driver's too, which gets it as --repo (snx_data_dir) */
    char* themes_dirs[2]; /* themes/ in the data folder, then beside the GUI */
    char message[512];
    int cols, rows, scale, log_top;
    int auto_scale; /* Text size Auto's scale, chosen when the window opens: a resize keeps the letters as they were */
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
    GuiConfirm confirm; /* the question before a run is aborted, when ask_abort says to ask it */
    bool ask_abort;     /* ESC asks first: on a phone it is Back, an edge swipe easily made by mistake */
    int native;
    Uint64 finger;            /* the finger on the screen (finger_id), 0 for none */
    float finger_x, finger_y; /* where it went down, 0 to 1 across the window */
    float finger_mark;        /* the height a drag last moved the focus at, a row at a time */
    bool pen_held;            /* a pen lifted: its hover leaves the focus where the lift put it until it moves away */
    float pen_x, pen_y;       /* where it lifted, 0 to 1 across the window */
    Uint32 picked;            /* the event a file from the system's picker comes back as */
    SDL_Texture* splash;      /* the boot's splash, its icon, until splash_until */
    Uint64 splash_until;
    GuiRgb splash_page;
    bool editing; /* the theme editor's screen instead of the menu's */
    GuiEditor editor;
    SDL_Texture* preview;   /* the editor's: the menu or a run drawn in its theme, at 1x */
    GuiSteps preview_steps; /* the run it shows, over again every GUI_PREVIEW_MS */
    GuiConsole preview_console;
    Uint64 preview_since;
} App;
static const char* const items[] = {"Load Switch NSP/XCI/NRO",
                                    "Load Linux ARM ELF",
                                    "Load 3DS CIA",
                                    "Setup device keys",
                                    "Fullscreen",
                                    "Touch",
                                    "On-screen controls",
                                    "Enable libgccjit",
                                    "Enable gdbstub",
                                    "Theme",
                                    "Theme editor",
                                    "Text size",
                                    "Share log",
                                    "Exit"};
/* what the screens say to press: a phone has a finger, the back button and the system's file picker, nothing to drop */
#ifdef SDL_PLATFORM_ANDROID
static const char* const load_labels[] = {"-> Select a file", "-> Select file from games/ instead"};
#define SAY_MENU       "Tap: select. Back: go back."
#define SAY_LOAD       "Choose a load option."
#define SAY_DROP       "Tap -> Select a file again to open the file picker. Back: go back."
#define SAY_GAMES      "Tap: open. Swipe across: turn the page. Back: go back."
#define SAY_KEYS       "Setup device keys: tap Pick prod.keys to open the file picker"
#define SAY_KEYS_HOW   "Back: go back."
#define SAY_KEYS_WAIT  "Pick prod.keys"
#define SAY_DONE       "Tap to return to menu"
#define SAY_ABORT      "Tap here to abort and return to menu"
#define SAY_NO_GAMES   "No games in games/: put a game's .nro, .xci or .nsp there."
#define SAY_COUNT      "game(s)"
#define SAY_LOG_FAILED "The log could not be saved: allow the app access to files."
#define SAY_EDITOR     "Tap a key to change it or type it. Back: go back."
#else
static const char* const load_labels[] = {"-> Select via drag and drop", "-> Select file from games/ instead"};
#define SAY_MENU       "Arrows/mouse: move. Space/Enter/click: select."
#define SAY_LOAD       "Choose a load option. ESC: back."
#define SAY_DROP       "Drop a file now. Ctrl+V: paste a path. ESC: back."
#define SAY_GAMES      "Up/Down: select. Enter/Space: open. Left/Right or PgUp/PgDown: page."
#define SAY_KEYS       "Setup device keys: drag and drop to this window"
#define SAY_KEYS_HOW   "Ctrl+V: paste a path. ESC: back."
#define SAY_KEYS_WAIT  "Drop prod.keys here"
#define SAY_DONE       "Press [ESC] to return to menu"
#define SAY_ABORT      "Press [ESC] to abort and return to menu"
#define SAY_NO_GAMES   "No executables found in games/."
#define SAY_COUNT      "executable(s)"
#define SAY_LOG_FAILED "The log could not be saved: check the folder's permissions."
#define SAY_EDITOR     "Up/Down: key. Left/Right: change. Enter: type. Ctrl+S: save. ESC: back."
#endif
static SDL_Color rgb(GuiRgb c) {
    SDL_Color out;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = 255;
    return out;
}
static Uint64 now_ms(const App* app) { return app->clock ? app->clock : SDL_GetTicks(); }

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
static void glyph(App* app, int x, int y, unsigned char c) {
    SDL_FRect source = {(float)((c % 16) * 8), (float)((c / 16) * 16), 8, 16};
    SDL_FRect destination = {(float)(x * 8 * app->scale), (float)(y * 16 * app->scale), (float)(8 * app->scale),
                             (float)(16 * app->scale)};
    SDL_RenderTexture(app->renderer, app->font, &source, &destination);
}
static void text(App* app, int x, int y, const char* value, SDL_Color color) {
    int skip = gui_slide((int)SDL_utf8strlen(value) - (app->cols - 1 - x), now_ms(app), app->theme.scroll_ms);
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    while (skip-- > 0 && *value) SDL_StepUTF8(&value, NULL);
    while (*value && x < app->cols - 1) {
        Uint32 codepoint = SDL_StepUTF8(&value, NULL);
        glyph(app, x++, y, gui_glyph(codepoint));
    }
}
/* a line that waits on the user: plain at x, or framed, placed and blinking as the theme's prompt says */
static void prompt(App* app, GuiPromptLine line, int x, int y, const char* value, SDL_Color color) {
    char framed[160];
    int width;
    const GuiPrompt* p = &app->theme.prompt;
    if (!p->lines[line]) {
        text(app, x, y, value, color);
        return;
    }
    if (p->blink && app->theme.blink_ms && (now_ms(app) / app->theme.blink_ms) % 2) return;

    snprintf(framed, sizeof framed, "%s%s%s", p->open, value, p->close);
    width = (int)SDL_utf8strlen(framed);
    if (p->align == GUI_ALIGN_CENTER) x = (app->cols - width) / 2;
    if (p->align == GUI_ALIGN_RIGHT) x = app->cols - 1 - width;
    text(app, SDL_max(0, x), y, framed, color);
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
    SDL_FRect r;
    int width, height, bottom = row + rows == app->rows && app->rows < GUI_MAX_ROWS;
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    r.x = 0;
    r.y = row ? (float)(safe.y + row * 16 * app->scale) : 0;
    r.w = (float)width;
    r.h = (bottom ? (float)height : (float)(safe.y + (row + rows) * 16 * app->scale)) - r.y;
    SDL_SetRenderViewport(app->renderer, NULL);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderViewport(app->renderer, &safe);
}
/* a line between two rows, across the text area */
static void rule(App* app, int row, GuiRgb c) {
    SDL_FRect r;
    r.x = (float)(8 * app->scale);
    r.y = (float)(row * 16 * app->scale - app->scale);
    r.w = (float)((app->cols - 2) * 8 * app->scale);
    r.h = (float)app->scale;
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(app->renderer, &r);
}
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
/* the menu of an installed build, whose folder is the user's data folder (snx_data_dir) */
static bool installed(const App* app) {
    char* user = snx_user_data_dir();
    bool same = user && app->data_dir && !strcmp(user, app->data_dir);
    free(user);
    return same;
}
#endif
/* where the keys are installed: an installed build's data folder, which the driver reads first, so nothing is written
   in the home folder itself (XDG); else ~/.switch */
static char* device_keys_path(const App* app) {
#ifdef SNX_XDG
    if (installed(app)) return snx_path_join(app->data_dir, "prod.keys");
#else
    (void)app;
#endif
    return home_keys_path();
}
/* the prod.keys the driver reads: those installed here, else for an installed build the ones in ~/.switch */
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
static void footer(App* app, int row, int rows, GuiRgb page, GuiRgb ink, GuiRgb soft) {
    char usage[256];
    int bar = app->theme.footer == GUI_STYLE_BAR;
    if (bar) band(app, row, rows, ink);
    if (app->theme.footer_rule) rule(app, row, soft);
    gui_usage_text(usage, sizeof(usage), app->usage.available, app->usage.memory_bytes, app->usage.cpu_percent,
                   SDL_GetNumLogicalCPUCores());
    text(app, 1, row, usage, rgb(bar ? page : soft));
    text(app, 1, row + 1, "CWD:", rgb(bar ? page : soft));
    text(app, 6, row + 1, app->cwd ? app->cwd : "(unavailable)", rgb(bar ? page : ink));
}
/* a selectable line: activated, focused (in the focus colour, or on a band) or plain */
static void entry(App* app, int x, int y, const char* label, int activated, int focused) {
    if (focused && app->theme.focus_bar) {
        band(app, y, 1, app->theme.focus);
        text(app, x, y, label, rgb(app->theme.page));
    } else
        text(app, x, y, label, rgb(activated ? app->theme.active : focused ? app->theme.focus : app->theme.ink));
}
static GuiScreen screen(const App* app) {
    if (app->menu.running || app->finished)
        return app->native || app->banner.seen ? GUI_SCREEN_LOADING : GUI_SCREEN_COMPILING;
    return app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
           : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                             : GUI_SCREEN_MENU;
}
static void next_theme(App* app) {
    size_t i;
    const char* const* dirs = (const char* const*)app->themes_dirs;
    gui_themes_scan(&app->themes, dirs, 2);
    i = 0;
    while (i < app->themes.count && strcmp(app->themes.ids[i], app->theme.id)) ++i;
    if (app->themes.count) gui_theme_load(&app->theme, dirs, 2, app->themes.ids[(i + 1) % app->themes.count]);
}
/* the recent games take a title row, a row each and a blank one above the items */
static int recent_rows(const App* app) { return app->recent.count ? app->recent.count + 2 : 0; }
/* the menu's last row: below it a blank, the message, and the focused item's description */
static int menu_last_row(const App* app) { return app->rows - 7; }
static int menu_row_in_full(const App* app, int item) {
    return 4 + recent_rows(app) + item + (app->menu.view != GUI_LOAD_NONE && item > app->menu.active ? 2 : 0);
}
/* how far the menu slides up for the focus to show: only when its letters are too big for the screen */
static int menu_offset(const App* app) {
    int row = app->menu.recent_focus ? 4 + app->menu.recent_focus
              : app->menu.view != GUI_LOAD_NONE && app->menu.focus_load
                  ? menu_row_in_full(app, app->menu.active) + 1 + app->menu.load_selected
                  : menu_row_in_full(app, app->menu.selected);
    return SDL_max(0, row - menu_last_row(app));
}
static int menu_row(const App* app, int item) { return menu_row_in_full(app, item) - menu_offset(app); }
static int recent_row(const App* app, int game) { return 5 + game - menu_offset(app); }
/* whether a row of the menu shows: under the head and above the message */
static int menu_shows(const App* app, int row) { return row >= 4 && row <= menu_last_row(app); }
static void recent_label(const App* app, int index, char* label, size_t size) {
    snprintf(label, size, "%s%s", app->theme.marker, app->recent.game[index].name);
}
/* the list as the menu and the file have it */
static void recent_changed(App* app) {
    app->menu.recent = app->recent.count;
    if (app->menu.recent_focus > app->recent.count) app->menu.recent_focus = app->recent.count;
    if (app->recent_file) gui_recent_save(&app->recent, app->recent_file);
}
static void recent_open(App* app, char* file) {
    free(app->recent_file);
    app->recent_file = file;
    gui_recent_load(&app->recent, file);
    app->menu.recent = app->recent.count;
    app->menu.recent_focus = 0;
}
static void menu_label(const App* app, int item, char* label, size_t size) {
    if (item == GUI_FULLSCREEN || item == GUI_CONTROLS || item == GUI_GCCJIT || item == GUI_GDBSTUB) {
        bool enabled = item == GUI_FULLSCREEN ? app->menu.fullscreen
                       : item == GUI_CONTROLS ? app->menu.controls
                       : item == GUI_GCCJIT   ? app->menu.gccjit
                                              : app->menu.gdbstub;
        snprintf(label, size, "%s[%s] %s", app->theme.marker, enabled ? "ON" : "OFF", items[item]);
    } else if (item == GUI_THEME) {
        snprintf(label, size, "%s%s: %s", app->theme.marker, items[item], app->theme.name);
    } else if (item == GUI_TEXT) {
        if (app->menu.text_size)
            snprintf(label, size, "%s%s: %dx", app->theme.marker, items[item], app->menu.text_size);
        else
            snprintf(label, size, "%s%s: Auto", app->theme.marker, items[item]);
    } else if (item == GUI_TOUCH) {
        static const char* const touch[] = {"Fingers", "Fingers and mouse", "Off"}; /* by GUI_TOUCH_* */
        snprintf(label, size, "%s%s: %s", app->theme.marker, items[item], touch[app->menu.touch]);
    } else {
        snprintf(label, size, "%s%s", app->theme.marker, items[item]);
    }
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
    app->log_top = run ? 4 : menu_row(app, GUI_EXIT) + 6;
    gui_browser_resize(&app->browser, (size_t)SDL_max(1, app->rows - 10));
    int log_rows = SDL_max(1, app->rows - app->log_top - 4);
    gui_console_resize(&app->console, SDL_max(1, app->cols - 2), log_rows);
}
/* text from a pixel position, chars at most, for the lines that slide or fade in */
static float text_px(App* app, float x, float y, const char* value, size_t chars, SDL_Color color, float alpha) {
    size_t i;
    float w = 8.0f * app->scale, h = 16.0f * app->scale, limit = (float)(app->cols - 1) * w;
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(app->font, (Uint8)(alpha * 255));
    for (i = 0; *value && i < chars; ++i, x += w) {
        unsigned char c = gui_glyph(SDL_StepUTF8(&value, NULL));
        SDL_FRect source, destination;
        if (x + w > limit) continue;
        source.x = (float)((c % 16) * 8);
        source.y = (float)((c / 16) * 16);
        source.w = 8;
        source.h = 16;
        destination.x = x;
        destination.y = y;
        destination.w = w;
        destination.h = h;
        SDL_RenderTexture(app->renderer, app->font, &source, &destination);
    }
    SDL_SetTextureAlphaMod(app->font, 255);
    return x;
}
static void fill(App* app, float x, float y, float w, float h, GuiRgb c, Uint8 alpha) {
    SDL_FRect r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    SDL_SetRenderDrawBlendMode(app->renderer, alpha == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, alpha);
    SDL_RenderFillRect(app->renderer, &r);
    SDL_SetRenderDrawBlendMode(app->renderer, SDL_BLENDMODE_NONE);
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
/* the game's name as the run knows it: the runner's banner, else the file's own, in capitals */
static void run_name(const App* app, char* out, size_t size) {
    int i;
    const char* q;
    size_t n;
    const char* dot;
    const char* name = app->file ? app->file : "";
    int from_file = 1;
    for (i = 0; i < app->steps.count; ++i)
        if (!strcmp(app->steps.step[i].label, "LAUNCH")) {
            name = app->steps.step[i].detail;
            from_file = false;
        }
    for (q = name; *q; ++q)
        if (*q == '/' || *q == '\\') name = q + 1;
    n = 0;
    dot = from_file ? strrchr(name, '.') : NULL;
    for (q = name; *q && q != dot && n + 1 < size; ++q) out[n++] = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
    out[n] = 0;
}
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
/* a step's line: its label, its detail in capitals and its status */
static size_t step_runs(const GuiStep* s, char run[3][112], size_t length[3]) {
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
    else if (s->total)
        snprintf(run[2], 112, " %u / %u", s->value, s->total);
    else
        run[2][0] = 0;
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
        step_runs(s, runs[rows], lengths[rows]);
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
            SDL_FRect r;
            r.x = cw + (float)k * (seg + gap);
            r.y = sy;
            r.w = seg;
            r.h = ch * 0.4f;
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
/* the screen fading in from black, or the menu or the run fading out to it */
static void overlay(App* app) {
    static const GuiRgb black = {0, 0, 0};
    SDL_Rect safe;
    int width;
    int height;
    const GuiTheme* theme = &app->theme;
    Uint64 now = now_ms(app);
    float a = 0;
    if (entering(app))
        a = (float)(now > app->entering_ms ? now - app->entering_ms : 0) / (float)theme->fade_out_ms;
    else if (app->leaving_ms && theme->fade_out_ms)
        a = SDL_min(1.0f, (float)(now > app->leaving_ms ? now - app->leaving_ms : 0) / (float)theme->fade_out_ms);
    else if (app->fade_ms && theme->fade_in_ms && now < app->fade_ms + theme->fade_in_ms)
        a = 1.0f - (float)(now > app->fade_ms ? now - app->fade_ms : 0) / (float)theme->fade_in_ms;
    if (a <= 0) return;

    /* the whole window, the cutout beside the text too */
    SDL_GetRenderViewport(app->renderer, &safe);
    SDL_GetRenderOutputSize(app->renderer, &width, &height);
    SDL_SetRenderViewport(app->renderer, NULL);
    fill(app, 0, 0, (float)width, (float)height, black, (Uint8)(a * 255));
    SDL_SetRenderViewport(app->renderer, &safe);
}
static const char* ask_question(const App* app) {
    if (app->confirm.exit) return "Exit ScarletNX?";
    return screen(app) == GUI_SCREEN_COMPILING ? "Abort the recompile? Its work is lost." : "Abort the run?";
}
/* the question's box, in cells: a blank row, the question, a blank row, its two choices and a blank row, two columns
   either side of the question */
static void ask_box(const App* app, int* col, int* row, int* cols) {
    *cols = SDL_min(app->cols - 2, (int)SDL_utf8strlen(ask_question(app)) + 4);
    *col = (app->cols - *cols) / 2;
    *row = SDL_max(0, (app->rows - 6) / 2);
}
/* the question over a running run, in the run screen's colours, its choices as the menu's entries */
static void render_ask(App* app) {
    static const char* const choices[2][2] = {{"Keep going", "Abort"}, {"Stay", "Exit"}}; /* by exit, GUI_CONFIRM_* */
    const GuiTheme* t = &app->theme;
    int steps = t->run == GUI_RUN_STEPS && !app->confirm.exit, col, row, cols, i;
    GuiRgb page = steps ? t->run_colors.page : t->page, ink = steps ? t->run_colors.ink : t->ink;
    GuiRgb soft = steps ? t->run_colors.soft : t->soft, warning = steps ? t->run_colors.warning : t->warning;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale, line = (float)app->scale;
    if (!app->confirm.shown || (!app->menu.running && !app->confirm.exit)) return;

    ask_box(app, &col, &row, &cols);
    fill(app, (float)col * cw, (float)row * ch, (float)cols * cw, 6 * ch, page, 255);
    fill(app, (float)col * cw, (float)row * ch, (float)cols * cw, line, soft, 255);
    fill(app, (float)col * cw, (float)(row + 6) * ch - line, (float)cols * cw, line, soft, 255);
    fill(app, (float)col * cw, (float)row * ch, line, 6 * ch, soft, 255);
    fill(app, (float)(col + cols) * cw - line, (float)row * ch, line, 6 * ch, soft, 255);
    text(app, col + 2, row + 1, ask_question(app), rgb(warning));
    for (i = 0; i < 2; ++i) {
        char label[64];
        int focused = app->confirm.selected == i, bar = focused && (steps || t->focus_bar);
        snprintf(label, sizeof label, "%s%s", t->marker, choices[app->confirm.exit][i]);
        if (bar)
            fill(app, (float)col * cw + line, (float)(row + 3 + i) * ch, (float)cols * cw - 2 * line, ch, ink, 255);
        text(app, col + 2, row + 3 + i, label, rgb(bar ? page : focused ? t->focus : ink));
    }
}
/* the boot's splash: the program's icon on a theme's splash page, then the menu as the theme brings it in */
static void splash_start(App* app) {
    const GuiTheme* t = gui_splash_theme(&app->theme, (int)SDL_rand(100));
    SDL_Surface* icon = t->splash_ms ? snx_window_icon_surface() : NULL;
    if (!icon) return;
    app->splash = SDL_CreateTextureFromSurface(app->renderer, icon);
    SDL_DestroySurface(icon);
    app->splash_page = t->splash_page;
    app->splash_until = SDL_GetTicks() + t->splash_ms;
}
static int splash_shown(App* app) {
    SDL_Rect view;
    float size;
    if (!app->splash) return 0;
    if (now_ms(app) >= app->splash_until) {
        SDL_DestroyTexture(app->splash);
        app->splash = NULL;
        app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
        return 0;
    }
    SDL_GetRenderViewport(app->renderer, &view);
    size = 128.0f * (float)SDL_max(1, SDL_min(view.w, view.h) / 3 / 128); /* whole steps of its 128 pixels */
    SDL_SetRenderDrawColor(app->renderer, app->splash_page.r, app->splash_page.g, app->splash_page.b, 255);
    SDL_RenderClear(app->renderer);
    {
        SDL_FRect at = {((float)view.w - size) / 2, ((float)view.h - size) / 2, size, size};
        SDL_RenderTexture(app->renderer, app->splash, NULL, &at);
    }
    SDL_RenderPresent(app->renderer);
    return 1;
}
/* the menu's screen or a run's, in app->theme, on the render target as it is */
static void draw_screen(App* app) {
    SDL_Color ink, soft, focus, paper, active, failure;
    const char* title;
    int head, foot, idle;
    const GuiTheme* theme;
    theme = &app->theme;
    idle = (!app->menu.running && !app->finished) || entering(app);
    head = theme->head != GUI_STYLE_NONE;
    foot = theme->footer != GUI_STYLE_NONE;
    ink = rgb(theme->ink);
    soft = rgb(theme->soft);
    focus = rgb(theme->focus);
    paper = rgb(theme->page);
    active = rgb(theme->active);
    failure = rgb(theme->failure);
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    if (!idle && theme->run == GUI_RUN_STEPS) {
        GuiRgb page = theme->run_colors.page;
        SDL_SetRenderDrawColor(app->renderer, page.r, page.g, page.b, 255);
        SDL_RenderClear(app->renderer);
        render_steps(app);
        render_ask(app);
        overlay(app);
        return;
    }
    title = theme->title[idle ? (app->menu.view == GUI_LOAD_GAMES  ? GUI_SCREEN_GAMES
                                 : app->menu.view == GUI_LOAD_KEYS ? GUI_SCREEN_KEYS
                                                                   : GUI_SCREEN_MENU)
                              : screen(app)];
    if (head && title[0]) {
        if (theme->head == GUI_STYLE_BAR) band(app, 0, 1, theme->ink);
        text(app, 1, 0, title, theme->head == GUI_STYLE_BAR ? paper : ink);
    }
    if (head && theme->head_rule) rule(app, 4, theme->soft);
    if (app->menu.view == GUI_LOAD_GAMES && idle) {
        size_t i;
        if (head) {
            text(app, 1, 1, SAY_GAMES, ink);
            text(app, 1, 2, app->games_dir, soft);
        }
        entry(app, 1, 3, "Back [ESC]", false, app->browser.selected == 0);
        size_t start = app->browser.page * app->browser.page_size;
        size_t count = gui_browser_items(&app->browser);
        for (i = 0; i < count; ++i)
            entry(app, 1, 5 + (int)i, app->games.names[start + i],
                  app->browser.activated && app->browser.active == start + i, app->browser.selected == i + 1);
        if (!app->games.count) text(app, 1, 5, SAY_NO_GAMES, soft);
        char page[128];
        snprintf(page, sizeof(page), "Page %zu/%zu   %zu " SAY_COUNT, app->browser.page + 1,
                 gui_browser_pages(&app->browser), app->games.count);
        text(app, 1, app->rows - 5, page, soft);
        text(app, 1, app->rows - 4, app->message, focus);
    } else if (app->menu.view == GUI_LOAD_KEYS && idle) {
        if (head) {
            text(app, 1, 1, SAY_KEYS, ink);
            text(app, 1, 2, SAY_KEYS_HOW, soft);
            text(app, 1, 3, app->keys_path ? app->keys_path : "Home directory unavailable.", ink);
        }
        char* in_use = keys_in_use(app);
        if (in_use && app->keys_path && !strcmp(in_use, app->keys_path))
            text(app, 1, 4, "Keys already setup. This will overwrite the previous ones", failure);
        else if (in_use) {
            char line[1100];
            snprintf(line, sizeof(line), "Keys in use: %s. Keys dropped here are read before them", in_use);
            text(app, 1, 4, line, soft);
        }
        free(in_use);
        text(app, 1, 6, app->message, focus);
        entry(app, 1, 8, "Back [ESC]", false, app->browser.selected == 0);
        prompt(app, GUI_PROMPT_KEYS, 1, SDL_max(app->rows / 2, 10), SAY_KEYS_WAIT, focus);
    } else if (idle) {
        if (head) {
            text(app, 1, 1, SAY_MENU, ink);
            text(app, 1, 2, app->menu.view == GUI_LOAD_DROP ? SAY_DROP : SAY_LOAD, soft);
        }
        if (app->recent.count && menu_shows(app, recent_row(app, -1)))
            text(app, 1, recent_row(app, -1), "Recent games", soft);
        {
            int game;
            char label[160];
            for (game = 0; game < app->recent.count; ++game) {
                if (!menu_shows(app, recent_row(app, game))) continue;
                recent_label(app, game, label, sizeof label);
                entry(app, 1, recent_row(app, game), label, false,
                      app->menu.recent_focus == game + 1 && (app->menu.view == GUI_LOAD_NONE || !app->menu.focus_load));
            }
        }
        for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
            bool is_active = app->menu.activated && app->menu.active == i;
            bool is_focused = app->menu.selected == i && !app->menu.recent_focus &&
                              (app->menu.view == GUI_LOAD_NONE || !app->menu.focus_load);
            int row = menu_row(app, i);
            char label[100];
            menu_label(app, i, label, sizeof(label));
            if (menu_shows(app, row)) entry(app, 1, row, label, is_active, is_focused);
            if (i == app->menu.active && app->menu.view != GUI_LOAD_NONE) {
                if (menu_shows(app, row + 1))
                    entry(app, 3, row + 1, load_labels[GUI_LOAD_DRAG],
                          app->menu.load_activated && app->menu.load_active == GUI_LOAD_DRAG,
                          app->menu.focus_load && app->menu.load_selected == GUI_LOAD_DRAG);
                if (menu_shows(app, row + 2))
                    entry(app, 3, row + 2, load_labels[GUI_LOAD_DIRECTORY],
                          app->menu.load_activated && app->menu.load_active == GUI_LOAD_DIRECTORY,
                          app->menu.focus_load && app->menu.load_selected == GUI_LOAD_DIRECTORY);
            }
        }
        /* with recent games a small window has no room left for these: they would run into the footer */
        if (app->log_top - 4 < app->rows - 4)
            text(app, 1, app->log_top - 4, app->message, app->abnormal_exit ? failure : focus);
        if (app->has_output && !entering(app) && app->log_top - 2 < app->rows - 4)
            text(app, 1, app->log_top - 2, "Last output (no scrollback):", soft);
    } else if (head) {
        text(app, 1, 1, app->finished ? app->message : items[app->menu.selected],
             app->abnormal_exit ? failure
             : app->finished    ? focus
                                : active);
        text(app, 1, 2, app->file ? app->file : "", ink);
        if (app->finished)
            prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, 3, SAY_DONE, failure);
        else if (app->aborting)
            prompt(app, GUI_PROMPT_ABORTING, 1, 3, "Aborting process tree...", failure);
        else
            text(app, 1, 3, SAY_ABORT, failure);
    }
    if (app->has_output && app->menu.view != GUI_LOAD_GAMES && app->menu.view != GUI_LOAD_KEYS && !entering(app)) {
        SDL_SetTextureColorMod(app->font, theme->ink.r, theme->ink.g, theme->ink.b);
        for (int y = 0; y < app->console.rows; ++y)
            for (int x = 0; x < app->console.cols; ++x)
                if (app->log_top + y < app->rows - 4 && app->console.cells[y][x] != ' ')
                    glyph(app, x + 1, app->log_top + y, app->console.cells[y][x]);
    }
    if (idle && (app->menu.view == GUI_LOAD_NONE ||
                 ((app->menu.view == GUI_LOAD_OPTIONS || app->menu.view == GUI_LOAD_DROP) && !app->menu.focus_load)))
        text(app, 1, app->rows - 4,
             app->menu.recent_focus ? app->recent.game[app->menu.recent_focus - 1].path
                                    : gui_setting_description(&app->menu),
             ink);
    if (foot) footer(app, app->rows - 3, 3, theme->page, theme->ink, theme->soft);
    render_ask(app);
    overlay(app);
}

/* ---- the theme editor (editor.c): its rows on the left, the menu or a run in its theme on the right ---- */
#define GUI_PREVIEW_MS 8000
static const char preview_run[] = "Extracting Cult of the Lamb.nsp\n"
                                  "Extracted 3 NCAs, the program's 4 modules\n"
                                  "Recompiling Cult of the Lamb (4 modules)\n"
                                  "Functions: 48211  instructions: 2915002\n"
                                  "LLVM: 5 code units and the module descriptor\n"
                                  "  compiled block 1/6: code_000, 9874 functions (8.1 s)\n"
                                  "  compiled block 2/6: code_001, 10022 functions (8.4 s)\n"
                                  "  compiled block 3/6: code_002, 9567 functions (7.9 s)\n";
/* the preview's cells: the right half, its page 640 x 400 (80 x 25 at 1x) kept 1.6 times as wide as high */
static void preview_box(const App* app, int* col, int* row, int* cols, int* rows) {
    *cols = SDL_max(16, app->cols / 2 - 2);
    *rows = *cols * 5 / 16;
    *col = app->cols - *cols;
    *row = 4;
}
static void render_preview(App* app) {
    static App saved; /* the whole app, put back after: drawing the preview borrows it */
    SDL_FRect box;
    int col, row, cols, rows;
    int run = app->editor.preview != GUI_SCREEN_MENU;
    Uint64 now = now_ms(app);
    preview_box(app, &col, &row, &cols, &rows);
    if (!app->preview) {
        app->preview = SDL_CreateTexture(app->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, 640, 400);
        if (!app->preview) return;
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
    app->finished = app->aborting = app->abnormal_exit = app->confirm.shown = false;
    app->native = app->banner.seen = 0;
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
    app->log_top = run ? 4 : menu_row(app, GUI_EXIT) + 6;
    SDL_SetRenderTarget(app->renderer, app->preview);
    SDL_SetRenderViewport(app->renderer, NULL);
    draw_screen(app);
    SDL_SetRenderTarget(app->renderer, NULL);
    *app = saved;
    set_area(app);
    box.x = (float)(col * 8 * app->scale);
    box.y = (float)(row * 16 * app->scale);
    box.w = (float)(cols * 8 * app->scale);
    box.h = (float)(rows * 16 * app->scale);
    SDL_RenderTexture(app->renderer, app->preview, NULL, &box);
    text(app, col, row + rows, run ? "Preview: a run (Tab: the menu)" : "Preview: the menu (Tab: a run)",
         rgb(app->theme.soft));
}
/* the editor's rows between rows 4 and the last three, as many as fit */
static int editor_rows(const App* app) { return SDL_max(1, app->rows - 7); }
static void render_editor(App* app) {
    GuiEditor* e = &app->editor;
    const GuiTheme* theme = &app->theme;
    const GuiThemeKey* focused = gui_editor_key_of(e->row);
    char label[64], value[96], title[96];
    int r, col, row, cols, rows, width;
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    preview_box(app, &col, &row, &cols, &rows);
    width = col - 2; /* the rows' text, left of the preview */
    snprintf(title, sizeof title, "THEME EDITOR  /  %s", e->id);
    if (theme->head == GUI_STYLE_BAR) band(app, 0, 1, theme->ink);
    text(app, 1, 0, title, rgb(theme->head == GUI_STYLE_BAR ? theme->page : theme->ink));
    text(app, 1, 1, SAY_EDITOR, rgb(theme->ink));
    if (e->note[0])
        text(app, 1, 2, e->note, rgb(e->leaving ? theme->failure : theme->focus));
    else if (focused)
        text(app, 1, 2, focused->about, rgb(theme->soft));
    gui_editor_scroll(e, editor_rows(app));
    for (r = e->top; r < gui_editor_rows() && r - e->top < editor_rows(app); ++r) {
        const GuiThemeKey* k = gui_editor_key_of(r);
        int y = 4 + r - e->top, at = 1 + 25;
        char line[160];
        gui_editor_label(r, label, sizeof label);
        if (r == e->row && e->typing)
            snprintf(value, sizeof value, "%s_", e->text);
        else
            gui_editor_value(e, r, value, sizeof value);
        snprintf(line, sizeof line, "%s%-24s %s", theme->marker, label, value);
        {
            int n = 0; /* what fits left of the preview, a character at a time */
            char* c;
            for (c = line; *c && n < width; ++c) n += ((unsigned char)*c & 0xC0) != 0x80;
            while (((unsigned char)*c & 0xC0) == 0x80) ++c;
            *c = 0;
        }
        entry(app, 1, y, line, false, r == e->row);
        if (k && k->type == GUI_KEY_COLOUR) {
            const GuiRgb* c = (const GuiRgb*)((const char*)&e->theme + k->at);
            float px = (float)app->scale, x0, y0 = (float)y * 16 * px + 2 * px;
            at += (int)SDL_utf8strlen(theme->marker) + 8;
            x0 = (float)at * 8 * px;
            if (at + 2 <= width) { /* framed in soft, so a page-coloured one shows */
                fill(app, x0, y0, 16 * px, 12 * px, theme->soft, 255);
                fill(app, x0 + px, y0 + px, 14 * px, 10 * px, *c, 255);
            }
        }
    }
    snprintf(value, sizeof value, "Saves in %s", app->themes_dirs[0] ? app->themes_dirs[0] : "(no themes folder)");
    text(app, 1, app->rows - 2, value, rgb(theme->soft));
    render_preview(app);
}
static bool menu_key_of(const SDL_KeyboardEvent* k, GuiKey* key);
/* the editor on the theme the menu shows: a folder of themes/ keeps its file's extras and its name, a built-in is
   saved as a new theme */
static void open_editor(App* app) {
    GuiThemeFile file;
    int from_file = 0;
    size_t i;
    for (i = 0; i < 2 && !from_file; ++i) {
        char* folder = app->themes_dirs[i] ? snx_path_join(app->themes_dirs[i], app->theme.id) : NULL;
        char* path = folder ? snx_path_join(folder, "theme.ini") : NULL;
        size_t size = 0;
        unsigned char* text = path ? snx_read_file(path, &size) : NULL;
        from_file = text && gui_theme_file_read(&file, (const char*)text, size);
        free(text);
        free(path);
        free(folder);
    }
    gui_editor_start(&app->editor, &app->theme, from_file ? &file : NULL, from_file ? app->theme.id : "mytheme");
    if (from_file) gui_theme_file_free(&file);
    app->editing = true;
    app->preview_since = 0;
}
static void leave_editor(App* app) {
    gui_editor_stop(&app->editor);
    app->editing = false;
    SDL_StopTextInput(app->window);
}
/* saved in themes/ of the data folder, and the menu's theme from then on */
static void save_editor(App* app) {
    const char* const* dirs = (const char* const*)app->themes_dirs;
    if (gui_editor_save(&app->editor, app->themes_dirs[0])) return;
    gui_themes_scan(&app->themes, dirs, 2);
    gui_theme_load(&app->theme, dirs, 2, app->editor.id);
}
static void editor_key(App* app, GuiKey key) {
    GuiEditorDo what = gui_editor_key(&app->editor, key);
    if (what == GUI_EDITOR_SAVE) save_editor(app);
    if (what == GUI_EDITOR_LEAVE) {
        leave_editor(app);
        return;
    }
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
    if (e->key.key == SDLK_S && (e->key.mod & SDL_KMOD_CTRL) && !e->key.repeat) {
        save_editor(app);
        return;
    }
    if (e->key.key == SDLK_TAB && !e->key.repeat) {
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
/* a click or a tap on a row focuses it, and on the focused one is Select; on the preview it switches the screen */
static void editor_pointer(App* app, float x, float y, bool click) {
    int col, row, cols, rows, r = app->editor.top + (int)y - 4;
    preview_box(app, &col, &row, &cols, &rows);
    if (!click) return;
    if (x >= col && x < col + cols && y >= row && y < row + rows + 1) {
        app->editor.preview = app->editor.preview == GUI_SCREEN_MENU ? GUI_SCREEN_COMPILING : GUI_SCREEN_MENU;
        app->preview_since = 0;
        return;
    }
    if (y < 4 || r < 0 || r >= gui_editor_rows() || (int)y - 4 >= editor_rows(app)) return;
    if (r == app->editor.row && !app->editor.typing)
        editor_key(app, GUI_SELECT);
    else if (r != app->editor.row) {
        if (app->editor.typing) editor_key(app, GUI_CANCEL);
        app->editor.row = r;
    }
}
static void render(App* app) {
    layout(app);
    if (splash_shown(app)) return;
    if (app->editing)
        render_editor(app);
    else
        draw_screen(app);
    SDL_RenderPresent(app->renderer);
}
/* the game's name as its run gave it, from the step that names it, for its recent game */
static void recent_name(App* app, const char* label) {
    int i;
    if (!app->recent_file || app->recent_named || !app->file) return;
    for (i = app->steps.count - 1; i >= 0; --i)
        if (!strcmp(app->steps.step[i].label, label) && app->steps.step[i].detail[0]) {
            app->recent_named = 1;
            if (gui_recent_rename(&app->recent, app->file, app->steps.step[i].detail)) recent_changed(app);
            return;
        }
}
/* back to the menu from a finished run, which fades in */
static void leave_run(App* app) {
    app->finished = false;
    app->menu.view = GUI_LOAD_NONE;
    app->message[0] = 0;
    app->leaving_ms = 0;
    app->fade_ms = app->theme.fade_in_ms ? SDL_max(now_ms(app), 1) : 0;
    layout(app);
}
#ifdef SDL_PLATFORM_ANDROID
/* the working folder moves to /sdcard/ScarletNX once the app may write there, which the user grants after it starts */
static bool android_folder(App* app) {
    char* cwd = gui_android_folder() ? SDL_GetCurrentDirectory() : NULL;
    if (!cwd || (app->cwd && !strcmp(cwd, app->cwd))) {
        SDL_free(cwd);
        return false;
    }
    SDL_free(app->cwd);
    app->cwd = cwd;
    free(app->data_dir);
    app->data_dir = snx_strdup(cwd);
    free(app->games_dir);
    app->games_dir = snx_path_join(cwd, "games");
    free(app->themes_dirs[0]);
    app->themes_dirs[0] = snx_path_join(cwd, "themes");
    return true;
}
/* A game runs in the app's game activity, a process of its own: no child of this one gets a window */
static int play(App* app, const char* game) {
    char* option = snx_format("--game=%s", game);
    const char* argv[5];
    int ok, n = 0;
    argv[n++] = option;
    /* the whole screen, the system's bars out of the way. OFF says so over the game's .ini */
    argv[n++] = app->menu.fullscreen ? "-f" : "--fullscreen=0";
    argv[n++] = gui_touch_option(&app->menu);
    argv[n++] = gui_controls_option(&app->menu);
    argv[n] = NULL;
    ok = gui_android_play(argv, app->message, sizeof(app->message));
    free(option);
    return ok;
}
/* the game a run of the driver made ready: there the driver names it, as it cannot open it; whether it opened */
static int ready(App* app) {
    int i, ok;
    for (i = app->steps.count - 1; i >= 0; --i)
        if (!strcmp(app->steps.step[i].label, "READY")) {
            char* game = snx_path_join(app->games_dir, app->steps.step[i].detail);
            ok = play(app, game);
            free(game);
            return ok;
        }
    return 0;
}
#endif
static void pump(App* app) {
    if (app->menu.running) {
        char buffer[4096];
        /* Bounded work per frame keeps keyboard/window events responsive even
           when a compiler floods the inherited pipe. There is no log backlog. */
        for (int reads = 0; reads < 16; ++reads) {
            int n = gui_process_read(app->process, buffer, sizeof(buffer));
            if (n <= 0) break;
            if (app->run_out) fwrite(buffer, 1, (size_t)n, app->run_out);
            gui_console_feed(&app->console, buffer, (size_t)n);
            gui_banner_feed(&app->banner, buffer, (size_t)n);
            gui_steps_feed(&app->steps, buffer, (size_t)n, now_ms(app));
            app->has_output = true;
        }
        recent_name(app, "LAUNCH");
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
            app->aborting = false;
            app->confirm.shown = false; /* a run that ended while asking has nothing left to abort */
            layout(app);
#ifdef SDL_PLATFORM_ANDROID
            /* the game opens in its own activity, over this one: coming back from it is coming back to the menu */
            if (!app->abnormal_exit && ready(app)) leave_run(app);
#endif
        }
    }
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->sampled = now;
    }
    if (app->leaving_ms && now_ms(app) - app->leaving_ms >= app->theme.fade_out_ms) leave_run(app);
}
static const char* driver_name(const App* app) {
    switch (app->menu.selected) {
#ifdef SDL_PLATFORM_ANDROID
        /* the app's libraries folder only takes lib*.so: the drivers go in as ones */
        case GUI_SWITCH: return "libscarletnx-run.so";
        case GUI_LINUX: return "libscarletnx-gnuabi.so";
        case GUI_3DS: return "libscarletnx-3ds.so";
#else
        case GUI_SWITCH: return "scarletnx-mariko";
        case GUI_LINUX: return "scarletnx-gnuabi";
        case GUI_3DS: return "scarletnx-3ds";
#endif
        default: return NULL;
    }
}
static char* find_driver(App* app) {
    const char* name = driver_name(app);
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
static void launch(App* app, char* absolute, bool prepared) {
#ifdef SDL_PLATFORM_ANDROID
    android_folder(app); /* the driver works where the menu stands */
#endif
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "That path is not a readable file. Try again or press ESC.");
        free(absolute);
        return;
    }
    /* The 3DS browser lists CIA inputs, not prepared host executables. */
    if (app->menu.selected == GUI_3DS) prepared = false;
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    char* driver = native ? NULL : find_driver(app);
    if (!native && !driver) {
        snprintf(app->message, sizeof(app->message), "Cannot find %s beside the GUI or on PATH.",
                 driver_name(app) ? driver_name(app) : "the selected driver");
        free(absolute);
        return;
    }
#ifdef SDL_PLATFORM_ANDROID
    if (native) {
        if (play(app, absolute) && app->recent_file && !gui_recent_add(&app->recent, absolute, GUI_SWITCH, true))
            recent_changed(app);
        free(absolute);
        return;
    }
#endif
    const char* argv[2 + GUI_LAUNCH_MAX + 1] = {native ? absolute : driver}; /* the driver, the file, NULL */
    size_t n = 1;
    if (!native) argv[n++] = absolute;
    n += gui_launch_options(&app->menu, native, app->data_dir, argv + n);
    argv[n] = NULL;
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    if (started && app->run_log) {
        size_t k;
        if (app->run_out) fclose(app->run_out);
        app->run_out = snx_fopen(app->run_log, "wb");
        for (k = 0; app->run_out && k < n; ++k) fprintf(app->run_out, "%s%s", k ? " " : "$ ", argv[k]);
        if (app->run_out) fputc('\n', app->run_out);
    }
    gui_banner_reset(&app->banner);
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
    app->menu.selected = game.driver;
    launch(app, snx_strdup(game.path), game.prepared);
    if (app->menu.running) app->menu.recent_focus = 1; /* back from the run, on the game that is now first */
}
static void install_keys(App* app, const char* source) {
    const char* name = source;
    if (source)
        for (const char* p = source; *p; ++p)
            if (*p == '/' || *p == '\\') name = p + 1;
    if (!source || strcmp(name, "prod.keys") || !snx_is_file(source)) {
        snprintf(app->message, sizeof(app->message), "Please drop a readable file named prod.keys.");
        return;
    }
    if (!app->keys_path) {
        snprintf(app->message, sizeof(app->message), "Cannot locate your home directory. Keys were not installed.");
        return;
    }
    /* Stage privately on the same filesystem, then replace atomically. A failed
       copy (or dropping the installed file itself) must not destroy existing keys. */
    char* directory = snx_dirname(app->keys_path);
    char* temporary = directory && !snx_mkdirs(directory) ? snx_temp_dir(directory) : NULL;
    char* staged = temporary ? snx_path_join(temporary, "prod.keys") : NULL;
    bool ok = staged && !snx_copy_file(source, staged);
#ifndef _WIN32
    if (ok) ok = chmod(staged, 0600) == 0;
#endif
    if (ok) ok = !snx_replace(staged, app->keys_path);
    if (temporary) snx_remove_tree(temporary);
    free(staged);
    free(temporary);
    free(directory);
    snprintf(app->message, sizeof(app->message), "%s",
             ok ? "Device keys installed successfully."
                : "Cannot install prod.keys. Check file and folder permissions, then try again.");
    if (ok) app->menu.view = GUI_LOAD_NONE;
}
static void drop(App* app, const char* value, bool text_drop) {
    if ((app->menu.view != GUI_LOAD_DROP && app->menu.view != GUI_LOAD_KEYS) || app->menu.running || app->finished)
        return;
    char* path = gui_drop_path(value, text_drop);
    if (!path) {
        snprintf(app->message, sizeof(app->message), "Drop one local file path, not a list or remote URL.");
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
        app->menu.focus_load = true;
        launch(app, absolute, false);
    }
}
#ifdef SDL_PLATFORM_ANDROID
/* the picker answers on its own thread: the choice comes back to event() as an app->picked event */
static void SDLCALL picker_done(void* userdata, const char* const* files, int filter) {
    SDL_Event e;
    (void)filter;
    SDL_zero(e);
    e.type = *(const Uint32*)userdata;
    e.user.data1 = files && files[0] ? SDL_strdup(files[0]) : NULL;
    SDL_PushEvent(&e);
}
static void open_picker(App* app) {
    if (!app->picked) app->picked = SDL_RegisterEvents(1);
    if (app->picked) SDL_ShowOpenFileDialog(picker_done, &app->picked, app->window, NULL, 0, NULL, false);
}
#endif
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
/* the game log written last, games/<game>.log: beside the games, or an installed build's in its state folder */
static char* newest_log(const App* app) {
    int count = 0, i;
    char* folder = snx_user_path(app->games_dir, SNX_USER_STATE);
    char** names = SDL_GlobDirectory(folder, "*.log", 0, &count);
    char* newest = NULL;
    SDL_Time time = 0;
    for (i = 0; names && i < count; ++i) {
        SDL_PathInfo info;
        char* path = snx_path_join(folder, names[i]);
        if (SDL_GetPathInfo(path, &info) && (!newest || info.modify_time > time)) {
            free(newest);
            newest = path;
            time = info.modify_time;
        } else
            free(path);
    }
    SDL_free(names);
    free(folder);
    return newest;
}
/* the message on the screen, the last run's output and the newest game log, as scarletnx.log: on Android in
   /sdcard/ScarletNX and handed to the share sheet as that file, elsewhere in the working folder with its path copied.
   An installed build saves it in its state folder (XDG), as the working folder is the home folder when the menu is
   started from the desktop */
static void share_log(App* app) {
    char *game, *run, *played, *text, *saved, *state = NULL;
    const char* folder;
#ifdef SDL_PLATFORM_ANDROID
    android_folder(app);
    folder = gui_android_folder();
#else
    folder = app->cwd;
#endif
#ifdef SNX_XDG
    if (installed(app)) state = snx_user_dir(SNX_USER_STATE);
    if (state && snx_mkdirs(state)) {
        free(state);
        state = NULL;
    }
    if (state) folder = state;
#endif
    game = newest_log(app);
    run = log_tail(app->run_log, 1 << 20);
    played = log_tail(game, 4 << 20);
    text = snx_format("%s\nMessage: %s\n\n== the last run: %s ==\n%s\n\n== %s ==\n%s\n", snx_build_info(), app->message,
                      app->run_log ? app->run_log : "", run, game ? game : "no game log", played);
    saved = folder ? snx_path_join(folder, "scarletnx.log") : NULL;
    if (!saved || snx_write_file(saved, text, strlen(text)))
        snprintf(app->message, sizeof(app->message), SAY_LOG_FAILED);
#ifdef SDL_PLATFORM_ANDROID
    else if (gui_android_share(saved, app->message, sizeof(app->message)))
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
static void list_games(App* app) {
#ifdef SDL_PLATFORM_ANDROID
    android_folder(app);
#endif
    if (app->menu.active == GUI_3DS)
        gui_cias_scan(&app->games, app->games_dir, app->message, sizeof(app->message));
    else
        gui_games_scan(&app->games, app->games_dir, app->message, sizeof(app->message));
    memset(&app->browser, 0, sizeof(app->browser));
    app->browser.count = app->games.count;
    layout(app);
}
static void menu_key(App* app, GuiKey key) {
    GuiLoadView before = app->menu.view; /* the screen this key was pressed on */
#ifdef SDL_PLATFORM_ANDROID
    bool fullscreen = app->menu.fullscreen;
#endif
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
        if (action == GUI_BROWSER_BACK) {
            app->menu.view = GUI_LOAD_OPTIONS;
            app->menu.focus_load = true;
            app->menu.load_activated = false;
            app->menu.selected = app->menu.active;
            app->message[0] = 0;
        } else if (action == GUI_BROWSER_OPEN) {
            size_t index = app->browser.page * app->browser.page_size + app->browser.selected - 1;
            if (index < app->games.count) {
                char* path = snx_path_join(app->games_dir, app->games.names[index]);
                char* absolute = snx_absolute(path);
                free(path);
                launch(app, absolute, !gui_is_dump(app->games.names[index]));
            }
        }
        return;
    }
    gui_menu_key(&app->menu, key);
#ifdef SDL_PLATFORM_ANDROID
    if (app->menu.fullscreen != fullscreen) SDL_SetWindowFullscreen(app->window, app->menu.fullscreen); /* as a game */
    /* the screen opens first, as on a computer, so what happens shows there; a select on it opens the picker */
    if (key == GUI_SELECT && app->menu.view == before && (before == GUI_LOAD_DROP || before == GUI_LOAD_KEYS))
        open_picker(app);
#endif
    if (app->menu.recent_open) {
        int index = app->menu.recent_open - 1;
        app->menu.recent_open = 0;
        if (index < app->recent.count) open_recent(app, index);
        return;
    }
    if (app->menu.theme_next) {
        app->menu.theme_next = 0;
        next_theme(app);
    }
    if (app->menu.theme_edit) {
        app->menu.theme_edit = 0;
        open_editor(app);
    }
    if (app->menu.share_log) {
        app->menu.share_log = 0;
        share_log(app);
    }
    if (key == GUI_CANCEL || (key == GUI_SELECT && app->menu.view == GUI_LOAD_KEYS)) app->message[0] = 0;
    if (app->menu.view == GUI_LOAD_GAMES) list_games(app);
    /* Back or ESC with nothing open to close: on Android the app goes behind, as an app's first screen does; on a
       computer the menu asks before it closes, as rEden does */
    if (key == GUI_CANCEL && before == GUI_LOAD_NONE && app->menu.view == GUI_LOAD_NONE && !app->menu.running) {
#ifdef SDL_PLATFORM_ANDROID
        gui_android_background(app->message, sizeof(app->message));
#else
        app->confirm.shown = true;
        app->confirm.exit = true;
        app->confirm.selected = GUI_CONFIRM_KEEP;
#endif
    }
}
/* a label's cells, or its whole row for a finger, which is wider than a mouse pointer */
static bool hit_text(const App* app, float x, float y, int col, int row, const char* label, bool touch) {
    int end = touch ? app->cols : SDL_min(app->cols - 1, col + (int)SDL_utf8strlen(label));
    return x >= (touch ? 0 : col) && x < end && y >= row && y < row + 1;
}
static void abort_run(App* app) {
    if (app->aborting) return;
    /* Do not ask a hung program to shut down or wait on it in the event
       handler. Kill its tree now and let pump reap it without blocking. */
    gui_process_terminate(app->process);
    app->aborting = true;
}
/* the question's second choice taken: the app closes, or the run is aborted */
static void confirmed(App* app) {
    if (app->confirm.exit)
        app->menu.quit = true;
    else
        abort_run(app);
}
/* the pointer on the question, at x, y in cells: on a choice it takes the focus, and a click takes the choice; a click
   off the box keeps going */
static void ask_pointer(App* app, float x, float y, bool click) {
    int col, row, cols;
    ask_box(app, &col, &row, &cols);
    if (x >= (float)col && x < (float)(col + cols) && y >= (float)(row + 3) && y < (float)(row + 5)) {
        app->confirm.selected = (int)y - row - 3;
        if (click && gui_confirm_key(&app->confirm, GUI_SELECT)) confirmed(app);
    } else if (click && (x < (float)col || x >= (float)(col + cols) || y < (float)row || y >= (float)(row + 6)))
        gui_confirm_key(&app->confirm, GUI_CANCEL);
}
static void pointer(App* app, float window_x, float window_y, bool click, bool touch) {
    if ((app->menu.running && !app->confirm.shown) || app->finished) return;
    layout(app);
    float x, y;
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    x /= 8 * app->scale;
    y /= 16 * app->scale;
    if (app->editing) {
        editor_pointer(app, x, y, click);
        return;
    }
    if (app->confirm.shown) {
        ask_pointer(app, x, y, click);
        return;
    }
    if (app->menu.view == GUI_LOAD_KEYS) {
        if (click && hit_text(app, x, y, 1, 8, "Back [ESC]", touch)) menu_key(app, GUI_CANCEL);
#ifdef SDL_PLATFORM_ANDROID
        else if (click && hit_text(app, x, y, 1, SDL_max(app->rows / 2, 10), SAY_KEYS_WAIT, true))
            menu_key(app, GUI_SELECT); /* its whole row: the theme may center or frame the prompt */
#endif
        return;
    }
    if (app->menu.view == GUI_LOAD_GAMES) {
        if (hit_text(app, x, y, 1, 3, "Back [ESC]", touch)) {
            app->browser.selected = 0;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
        size_t start = app->browser.page * app->browser.page_size;
        for (size_t i = 0; i < gui_browser_items(&app->browser); ++i) {
            if (!hit_text(app, x, y, 1, 5 + (int)i, app->games.names[start + i], touch)) continue;
            app->browser.selected = i + 1;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
        return;
    }
    if (app->menu.view != GUI_LOAD_NONE) {
        int row = menu_row(app, app->menu.active);
        for (int i = 0; i < 2; ++i) {
            if (!menu_shows(app, row + i + 1) || !hit_text(app, x, y, 3, row + i + 1, load_labels[i], touch)) continue;
            app->menu.load_selected = i;
            app->menu.focus_load = true;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
    }
    {
        int game;
        char label[160];
        for (game = 0; game < app->recent.count; ++game) {
            recent_label(app, game, label, sizeof label);
            if (!menu_shows(app, recent_row(app, game)) || !hit_text(app, x, y, 1, recent_row(app, game), label, touch))
                continue;
            app->menu.recent_focus = game + 1;
            app->menu.focus_load = false;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
    }
    for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
        char label[100];
        menu_label(app, i, label, sizeof(label));
        if (!menu_shows(app, menu_row(app, i)) || !hit_text(app, x, y, 1, menu_row(app, i), label, touch)) continue;
        app->menu.selected = i;
        app->menu.recent_focus = 0;
        app->menu.focus_load = false;
        if (click) menu_key(app, GUI_SELECT);
        return;
    }
}
/* the file the system's picker chose, as a dropped one; none (cancelled) stays on the screen and says so */
static void picked(App* app, const char* uri) {
    char* path = uri ? gui_document_path(uri) : NULL;
#ifdef SDL_PLATFORM_ANDROID
    if (uri && !path) path = gui_android_document_file(uri);
#endif
    if (!uri)
        snprintf(app->message, sizeof(app->message), "No file was picked.");
    else if (!path)
        snprintf(app->message, sizeof(app->message), "Pick the file from the device's storage, not from an app (%s).",
                 uri);
    else
        drop(app, path, false);
    free(path);
}
/* ESC: a run is aborted, or asked about first when ask_abort says so; anything else goes back */
static void cancel(App* app) {
    if (!app->menu.running)
        menu_key(app, GUI_CANCEL);
    else if (app->ask_abort && !app->aborting) {
        app->confirm.shown = true;
        app->confirm.exit = false;
        app->confirm.selected = GUI_CONFIRM_KEEP;
    } else
        abort_run(app);
}
/* the row of a running run's ESC line, -1 when it has none */
static int escape_row(const App* app) {
    if (app->theme.run == GUI_RUN_STEPS) return app->rows - 1;
    return app->theme.head != GUI_STYLE_NONE ? 3 : -1;
}
/* a finger's id, the pen's apart: SDL gives a pen's touch the id 1 on every platform, the id Android and Wayland
   give a first finger and any finger may have, and no real finger's id sets bit 63 */
static Uint64 finger_id(const SDL_TouchFingerEvent* f) {
    return (Uint64)f->fingerID | (f->touchID == SDL_PEN_TOUCHID ? 1ull << 63 : 0);
}
/* a drag up or down from where the finger went down, on a menu whose letters make it scroll */
static bool drags(const App* app, float x, float y) {
    float dx = x - app->finger_x, dy = y - app->finger_y;
    return SDL_fabsf(dy) > 0.05f && SDL_fabsf(dy) > 2 * SDL_fabsf(dx) && app->menu.view != GUI_LOAD_GAMES &&
           !app->menu.running && !app->finished &&
           (app->editing || menu_row_in_full(app, GUI_EXIT) > menu_last_row(app));
}
/* the focus follows the finger, up as it goes up, a row each time it passes a row's height, so the highlight is seen
   going */
static void drag(App* app, float y) {
    float row = 1.0f / (float)SDL_max(1, app->rows);
    void (*key)(App*, GuiKey) = app->editing ? editor_key : menu_key;
    while (y <= app->finger_mark - row) {
        key(app, GUI_UP);
        app->finger_mark -= row;
    }
    while (y >= app->finger_mark + row) {
        key(app, GUI_DOWN);
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
        layout(app);
        if (SDL_RenderCoordinatesFromWindow(app->renderer, x * (float)w, y * (float)h, &rx, &ry) &&
            (int)(ry / (float)(16 * app->scale)) == escape_row(app))
            cancel(app);
        return;
    }
    pointer(app, x * (float)w, y * (float)h, true, true);
}
#ifdef SDL_PLATFORM_ANDROID
/* SDL hands the app's lifecycle to watchers only, inside SDL_PollEvent on this thread. Back from the system's settings,
   the user may have just let the app write to /sdcard */
static bool SDLCALL lifecycle(void* data, SDL_Event* e) {
    App* app = (App*)data;
    if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND && android_folder(app) && app->menu.view == GUI_LOAD_GAMES)
        list_games(app);
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
    /* the splash takes the input it gets: a key, a button or a finger ends it, and does nothing else */
    if (app->splash && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                        e->type == SDL_EVENT_FINGER_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        app->splash_until = 0;
        return;
    }
    if (app->editing && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT)) {
        editor_input(app, e);
        return;
    }
    if (e->type == SDL_EVENT_DROP_FILE || e->type == SDL_EVENT_DROP_TEXT) {
        drop(app, e->drop.data, e->type == SDL_EVENT_DROP_TEXT);
        return; /* SDL3 owns the event's data, unlike SDL2. */
    }
    if (app->picked && e->type == app->picked) {
        picked(app, e->user.data1);
        SDL_free(e->user.data1);
        return;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN && !app->finger) {
        app->finger = finger_id(&e->tfinger);
        app->finger_x = e->tfinger.x;
        app->finger_y = app->finger_mark = e->tfinger.y;
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION && finger_id(&e->tfinger) == app->finger) {
        if (drags(app, e->tfinger.x, e->tfinger.y)) drag(app, e->tfinger.y);
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
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    /* a running run takes ESC, and the question, while it shows, every key; a held key asks and answers nothing */
    if (app->menu.running) {
        if (e->key.repeat || !menu_key_of(&e->key, &key)) return;
        if (app->confirm.shown) {
            if (gui_confirm_key(&app->confirm, key)) confirmed(app);
        } else if (key == GUI_CANCEL)
            cancel(app);
        return;
    }
    /* the exit question takes every key while it shows */
    if (app->confirm.shown) {
        if (!e->key.repeat && menu_key_of(&e->key, &key) && gui_confirm_key(&app->confirm, key)) confirmed(app);
        return;
    }
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) &&
        (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        drop(app, clipboard, true);
        SDL_free(clipboard);
        return;
    }
    if (menu_key_of(&e->key, &key)) menu_key(app, key);
}
static bool initialize(App* app) {
    SDL_WindowFlags flags;
    memset(app, 0, sizeof(*app));
#ifdef SDL_PLATFORM_ANDROID
    app->theme = *gui_theme_builtin("yorha");
    app->menu.fullscreen = true; /* a phone plays on the whole screen; Fullscreen off shows the system's bars */
    app->menu.controls = true;   /* and has no buttons but these */
    app->ask_abort = true;
    gui_android_start();
    /* back is ESC here: without this it would close the app */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#else
    app->theme = *gui_theme_builtin("default");
#endif
    /* touch() takes the fingers, and a finger and the mouse each arrive once: SDL would also send each as the other,
       and a pen as both (it stays a finger), so one tap would select twice */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_PEN_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
#ifdef SDL_PLATFORM_ANDROID
    /* the whole screen, the system's bars out of the way, as Fullscreen says: a phone held wide is short */
    flags = (app->menu.fullscreen ? SDL_WINDOW_FULLSCREEN : 0) | SDL_WINDOW_HIGH_PIXEL_DENSITY;
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
    app->drivers = gui_android_libraries();
    app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
#else
    app->drivers = SDL_GetBasePath();
    app->data_dir = snx_data_dir(GUI_CHECKOUT, app->drivers);
    if (!app->data_dir) app->data_dir = snx_strdup(app->cwd ? app->cwd : ".");
#endif
    app->games_dir = snx_path_join(app->data_dir, "games");
    app->themes_dirs[0] = snx_path_join(app->data_dir, "themes");
    if (app->drivers) app->themes_dirs[1] = snx_path_join(app->drivers, "themes");
    app->keys_path = device_keys_path(app);
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
#endif
    gui_process_destroy(app->process);
    free(app->file);
    free(app->data_dir);
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
    SDL_DestroyTexture(app->font);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}
#ifdef SNX_XDG
/* recent.txt and last-run.log: in the state folder's gui/ (~/.local/state/scarletnx/gui/). An older build's in the data
   folder's stay, a line saying so each start. Elsewhere SDL_GetPrefPath's folder (under %APPDATA% on Windows) */
static char* state_folder(void) {
    static const char* const names[] = {"recent.txt", "last-run.log"};
    char *state = snx_user_dir(SNX_USER_STATE), *data = snx_user_data_dir();
    char *folder = state ? snx_path_join(state, "gui") : NULL, *old = data ? snx_path_join(data, "gui") : NULL;
    size_t i;
    for (i = 0; folder && old && i < sizeof(names) / sizeof(*names); ++i) {
        char *from = snx_path_join(old, names[i]), *to = snx_path_join(folder, names[i]);
        if (snx_left_behind(from, to))
            fprintf(stderr, "scarletnx-gui: %s was left by an older version: this one keeps it in %s\n", from, to);
        free(from);
        free(to);
    }
    if (folder && snx_mkdirs(folder)) {
        free(folder);
        folder = NULL;
    }
    free(old);
    free(data);
    free(state);
    return folder;
}
#endif
#ifndef SNX_GUI_NO_MAIN
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    App app;
    char* prefs;
    if (!initialize(&app)) {
        fprintf(stderr, "scarletnx-gui: %s\n", SDL_GetError());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ScarletNX", SDL_GetError(), app.window);
        cleanup(&app);
        return 1;
    }
    /* the menu's own files; main() opens them, not initialize(), so the tests keep their own */
#ifdef SNX_XDG
    prefs = state_folder();
#else
    {
        char* sdl = SDL_GetPrefPath("scarletnx", "gui");
        prefs = sdl ? snx_strdup(sdl) : NULL;
        SDL_free(sdl);
    }
#endif
    if (prefs) recent_open(&app, snx_path_join(prefs, "recent.txt"));
    if (prefs) app.run_log = snx_path_join(prefs, "last-run.log");
    free(prefs);
    splash_start(&app);
    while (!app.menu.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) event(&app, &e);
        pump(&app);
        render(&app);
        SDL_Delay(16);
    }
    cleanup(&app);
    return 0;
}
#endif
