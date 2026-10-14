/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <SDL3/SDL.h>
#ifndef SNX_GUI_NO_MAIN
#include <SDL3/SDL_main.h>
#endif
#include "common.h"
#include "browser.h"
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
    char* recent_file;          /* where the recent games are kept; NULL keeps none */
    char* run_log;              /* where a run's output goes as it comes, the last run's; NULL keeps none */
    FILE* run_out;
    int recent_named;           /* the run's own name went to its recent game */
    Uint64 clock;               /* fixed time for tests; 0 is the real clock */
    Uint64 fade_ms, leaving_ms; /* a screen fading in from black since; the run fading out since */
    Uint64 entering_ms;         /* the menu fading out since, before a run fades in */
    const char* drivers;
    char *cwd, *file, *games_dir, *keys_path;
    char* themes_dirs[2]; /* themes/ in the working directory, then beside the GUI */
    char message[512];
    int cols, rows, scale, log_top;
    Uint64 sampled;
    bool has_output, finished, aborting, abnormal_exit;
    int native;
    SDL_FingerID finger;      /* the finger on the screen, 0 for none */
    float finger_x, finger_y; /* where it went down, 0 to 1 across the window */
    Uint32 picked;            /* the event a file from the system's picker comes back as */
} App;
static const char* const items[] = {"Load Switch NSP/XCI/NRO", "Load Linux ARM ELF", "Setup device keys",
                                   "Fullscreen", "Enable libgccjit", "Enable gdbstub", "Theme",
#ifdef SDL_PLATFORM_ANDROID
                                   "Share log",
#endif
                                   "Exit"};
/* what the screens say to press: a phone has a finger, the back button and the system's file picker, nothing to drop */
#ifdef SDL_PLATFORM_ANDROID
static const char* const load_labels[] = {"-> Select a file", "-> Select file from games/ instead"};
#define SAY_MENU "Tap: select. Back: go back."
#define SAY_LOAD "Choose a load option."
#define SAY_DROP "Pick the file in the window that opened. Back: go back."
#define SAY_GAMES "Tap: open. Swipe across: turn the page. Back: go back."
#define SAY_KEYS "Setup device keys: pick prod.keys in the window that opened"
#define SAY_KEYS_HOW "Back: go back."
#define SAY_KEYS_WAIT "Pick prod.keys"
#define SAY_DONE "Tap to return to menu"
#define SAY_ABORT "Tap here to abort and return to menu"
#else
static const char* const load_labels[] = {"-> Select via drag and drop", "-> Select file from games/ instead"};
#define SAY_MENU "Arrows/mouse: move. Space/Enter/click: select."
#define SAY_LOAD "Choose a load option. ESC: back."
#define SAY_DROP "Drop a file now. Ctrl+V: paste a path. ESC: back."
#define SAY_GAMES "Up/Down: select. Enter/Space: open. Left/Right or PgUp/PgDown: page."
#define SAY_KEYS "Setup device keys: drag and drop to this window"
#define SAY_KEYS_HOW "Ctrl+V: paste a path. ESC: back."
#define SAY_KEYS_WAIT "Drop prod.keys here"
#define SAY_DONE "Press [ESC] to return to menu"
#define SAY_ABORT "Press [ESC] to abort and return to menu"
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
    SDL_SetTextureColorMod(app->font, color.r, color.g, color.b);
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
/* rows of the whole width, as a band behind text */
static void band(App* app, int row, int rows, GuiRgb c) {
    int width, height;
    SDL_FRect r;
    area(app, &width, &height);
    r.x = 0;
    r.y = (float)(row * 16 * app->scale);
    r.w = (float)width;
    r.h = (float)(rows * 16 * app->scale);
    SDL_SetRenderDrawColor(app->renderer, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(app->renderer, &r);
}
/* a line between two rows, across the text area */
static void rule(App* app, int row) {
    SDL_FRect r;
    r.x = (float)(8 * app->scale);
    r.y = (float)(row * 16 * app->scale - app->scale);
    r.w = (float)((app->cols - 2) * 8 * app->scale);
    r.h = (float)app->scale;
    SDL_SetRenderDrawColor(app->renderer, app->theme.soft.r, app->theme.soft.g, app->theme.soft.b, 255);
    SDL_RenderFillRect(app->renderer, &r);
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
    if (app->menu.running || app->finished) return app->native || app->banner.seen ? GUI_SCREEN_LOADING
                                                                                    : GUI_SCREEN_COMPILING;
    return app->menu.view == GUI_LOAD_GAMES ? GUI_SCREEN_GAMES
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
static int menu_spacing(const App* app) { return app->rows >= 30 ? 2 : 1; }
/* the recent games take a title row, a row each and a blank one above the items */
static int recent_rows(const App* app) { return app->recent.count ? app->recent.count + 2 : 0; }
static int menu_row(const App* app, int item) {
    return 4 + recent_rows(app) + item * menu_spacing(app) +
           (app->menu.view != GUI_LOAD_NONE && item > app->menu.active ? 2 : 0);
}
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
    if (item == GUI_FULLSCREEN || item == GUI_GCCJIT || item == GUI_GDBSTUB) {
        bool enabled = item == GUI_FULLSCREEN ? app->menu.fullscreen
                       : item == GUI_GCCJIT  ? app->menu.gccjit : app->menu.gdbstub;
        snprintf(label, size, "%s[%s] %s", app->theme.marker, enabled ? "ON" : "OFF", items[item]);
    } else if (item == GUI_THEME) {
        snprintf(label, size, "%s%s: %s", app->theme.marker, items[item], app->theme.name);
    } else {
        snprintf(label, size, "%s%s", app->theme.marker, items[item]);
    }
}
/* still the menu, fading out, while a run starts */
static int entering(const App* app) {
    return app->entering_ms && now_ms(app) < app->entering_ms + app->theme.fade_out_ms;
}
static void layout(App* app) {
    int run;
    int width, height;
    set_area(app);
    area(app, &width, &height);
    app->scale = SDL_min(width / 640, height / 400);
    /* a phone packs its pixels tight: the display's own scale, for text and taps of a size, as far as the menu still
       fits with five recent games and a submenu (render) */
    app->scale = SDL_max(app->scale, SDL_min((int)(SDL_GetWindowDisplayScale(app->window) + 0.5f),
                                             height / (16 * (19 + GUI_ITEM_COUNT))));
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
    for (q = name; *q && q != dot && n + 1 < size; ++q)
        out[n++] = *q >= 'a' && *q <= 'z' ? (char)(*q - 32) : *q;
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
    marker = note ? 3 : 0; other = total - length[cut];
    left = (size_t)room > other + marker ? (size_t)room - other - marker : 0;
    end = run[cut];
    for (i = 0; i < left; ++i) SDL_StepUTF8(&end, NULL);
    at = (size_t)(end - run[cut]);
    snprintf(run[cut] + at, 112 - at, "%s", note ? "..." : "");
    length[cut] = left + marker;
}
/* a run as a loading screen: the title, the name and the ring; the steps typed down the page, their status last;
   the progress bar and its percentage */
static void render_steps(App* app) {
    /* the rows: each step, then every line it kept when detailed, folded once it is done when collapse says so;
       each ends before the right edge, with room for a step's dots and the cursor */
    enum { ROWS = GUI_STEPS_MAX + GUI_NOTES_MAX, SEGMENTS = 40 };
    static char runs[ROWS][3][112];
    static size_t lengths[ROWS][3], typed[ROWS];
    static const GuiStep* owner[ROWS];
    static int is_note[ROWS];
    static Uint64 shown[ROWS];
    static const GuiRgb white = {255, 255, 255};
    const GuiTheme* theme = &app->theme;
    const GuiPalette* colors = &theme->run_colors;
    /* since: a line that came while the menu faded out comes in with the run */
    Uint64 now = now_ms(app), since = app->fade_ms, free_at = 0;
    float cw = 8.0f * (float)app->scale, ch = 16.0f * (float)app->scale;
    int width, height, name_col, col, y, i, r, rows = 0, last_step = -1, typing = -1, top = 3, bottom, fit, first,
                                               cursor;
    const char* title;
    char name[96], head[128];
    SDL_Color ink, soft, failure;
    area(app, &width, &height);
    if (theme->texture)
        for (y = 3; y < height; y += 4) fill(app, 0, (float)y, (float)width, 1, white, 14);
    ink = rgb(colors->ink); soft = rgb(colors->soft); failure = rgb(colors->failure);

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
        const GuiNote * notes[GUI_NOTES_MAX];
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
    /* when each row types: async, each from when it came; sync, one after the other, a finished step's faster */
    for (r = 0; r < rows; ++r) {
        Uint64 start;
        float per;
        size_t total = lengths[r][0] + lengths[r][1] + lengths[r][2];
        typed[r] = total;
        if (theme->line_in != GUI_LINE_TYPING || !theme->typing_ms) continue;
        per = (float)theme->typing_ms / (theme->typing_sync && owner[r]->state != GUI_STEP_ACTIVE ? 3.0f : 1.0f);
        start = theme->typing_sync ? SDL_max(shown[r], free_at) : shown[r];
        free_at = start + (Uint64)(per * (float)total);
        typed[r] = now < start ? 0 : (size_t)SDL_min((float)total, (float)(now - start) / per);
        if (now >= start && typed[r] < total) typing = r;
    }
    bottom = theme->bar ? app->rows - 5 : app->rows - 2;
    fit = SDL_max(1, bottom - top);
    first = SDL_max(0, rows - fit);
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
        float seg = (span - gap * (SEGMENTS - 1)) / SEGMENTS, sy = ch * (float)(app->rows - 3) + ch * 0.3f;
        char number[16];
        fill(app, cw, ch * (float)(app->rows - 4) + ch / 2, (float)(app->cols - 2) * cw, (float)app->scale, colors->dim,
             255);
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
        if (percent < 0) snprintf(number, sizeof number, " --%%");
        else snprintf(number, sizeof number, "%3d%%", percent);
        text(app, app->cols - 6, app->rows - 3, number, ink);
        text(app, 1, app->rows - 2, name, ink);
    }
    if (app->finished)
        prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, app->rows - 1,
               SAY_DONE, soft);
    else if (app->aborting)
        prompt(app, GUI_PROMPT_ABORTING, 1, app->rows - 1, "Aborting process tree...", rgb(colors->warning));
    else
        text(app, 1, app->rows - 1, SAY_ABORT, soft);
}
/* the screen fading in from black, or the menu or the run fading out to it */
static void overlay(App* app) {
    static const GuiRgb black = {0, 0, 0};
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

    area(app, &width, &height);
    fill(app, 0, 0, (float)width, (float)height, black, (Uint8)(a * 255));
}
static void render(App* app) {
    SDL_Color ink, soft, focus, paper, active, failure;
    const char* title;
    int head, foot, idle;
    const GuiTheme * theme;
    layout(app);
    theme = &app->theme;
    idle = (!app->menu.running && !app->finished) || entering(app);
    head = theme->head != GUI_STYLE_NONE; foot = theme->footer != GUI_STYLE_NONE;
    ink = rgb(theme->ink); soft = rgb(theme->soft); focus = rgb(theme->focus); paper = rgb(theme->page);
    active = rgb(theme->active); failure = rgb(theme->failure);
    SDL_SetRenderDrawColor(app->renderer, theme->page.r, theme->page.g, theme->page.b, 255);
    SDL_RenderClear(app->renderer);
    if (!idle && theme->run == GUI_RUN_STEPS) {
        GuiRgb page = theme->run_colors.page;
        SDL_SetRenderDrawColor(app->renderer, page.r, page.g, page.b, 255);
        SDL_RenderClear(app->renderer);
        render_steps(app);
        overlay(app);
        SDL_RenderPresent(app->renderer);
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
    if (head && theme->head_rule) rule(app, 4);
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
        if (!app->games.count) text(app, 1, 5, "No executables found in games/.", soft);
        char page[128];
        snprintf(page, sizeof(page), "Page %zu/%zu   %zu executable(s)", app->browser.page + 1,
                 gui_browser_pages(&app->browser), app->games.count);
        text(app, 1, app->rows - 5, page, soft);
        text(app, 1, app->rows - 4, app->message, focus);
    } else if (app->menu.view == GUI_LOAD_KEYS && idle) {
        if (head) {
            text(app, 1, 1, SAY_KEYS, ink);
            text(app, 1, 2, SAY_KEYS_HOW, soft);
            text(app, 1, 3, app->keys_path ? app->keys_path : "Home directory unavailable.", ink);
        }
        if (app->keys_path && snx_is_file(app->keys_path))
            text(app, 1, 4, "Keys already setup. This will overwrite the previous ones", failure);
        text(app, 1, 6, app->message, focus);
        entry(app, 1, 8, "Back [ESC]", false, app->browser.selected == 0);
        prompt(app, GUI_PROMPT_KEYS, 1, SDL_max(app->rows / 2, 10), SAY_KEYS_WAIT, focus);
    } else if (idle) {
        if (head) {
            text(app, 1, 1, SAY_MENU, ink);
            text(app, 1, 2,
                 app->menu.view == GUI_LOAD_DROP ? SAY_DROP : SAY_LOAD,
                 soft);
        }
        if (app->recent.count) text(app, 1, 4, "Recent games", soft);
        {
            int game;
            char label[160];
            for (game = 0; game < app->recent.count; ++game) {
                recent_label(app, game, label, sizeof label);
                entry(app, 1, 5 + game, label, false,
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
            entry(app, 1, row, label, is_active, is_focused);
            if (i == app->menu.active && app->menu.view != GUI_LOAD_NONE) {
                entry(app, 3, row + 1, load_labels[GUI_LOAD_DRAG],
                      app->menu.load_activated && app->menu.load_active == GUI_LOAD_DRAG,
                      app->menu.focus_load && app->menu.load_selected == GUI_LOAD_DRAG);
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
             app->abnormal_exit ? failure : app->finished ? focus : active);
        text(app, 1, 2, app->file ? app->file : "", ink);
        if (app->finished)
            prompt(app, app->abnormal_exit ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE, 1, 3, SAY_DONE,
                   failure);
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
    if (foot) {
        char footer[256];
        int bar = theme->footer == GUI_STYLE_BAR;
        if (bar) band(app, app->rows - 3, 3, theme->ink);
        if (theme->footer_rule) rule(app, app->rows - 3);

        if (app->usage.available)
            snprintf(footer, sizeof(footer), "GUI + children: RAM %.1f MiB (sum RSS)  CPU %.1f%% (100%% = 1 core)",
                     (double)app->usage.memory_bytes / 1048576, app->usage.cpu_percent);
        else
            snprintf(footer, sizeof(footer), "GUI + children: resource usage unavailable");
        text(app, 1, app->rows - 3, footer, bar ? paper : soft);
        text(app, 1, app->rows - 2, "CWD:", bar ? paper : soft);
        text(app, 6, app->rows - 2, app->cwd ? app->cwd : "(unavailable)", bar ? paper : ink);
    }
    overlay(app);
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
/* A game runs in the app's game activity, a process of its own: no child of this one gets a window */
static int play(App* app, const char* game) {
    char* option = snx_format("--game=%s", game);
    const char* argv[2] = {option, NULL};
    int ok = gui_android_play(argv, app->message, sizeof(app->message));
    free(option);
    return ok;
}
/* the game a run of the driver made ready: there the driver names it, as it cannot open it */
static void ready(App* app) {
    int i;
    for (i = app->steps.count - 1; i >= 0; --i)
        if (!strcmp(app->steps.step[i].label, "READY")) {
            char* game = snx_path_join(app->games_dir, app->steps.step[i].detail);
            play(app, game);
            free(game);
            return;
        }
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
#ifdef SDL_PLATFORM_ANDROID
            if (!app->abnormal_exit) ready(app);
#endif
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
            layout(app);
        }
    }
    Uint64 now = SDL_GetTicks();
    if (!app->sampled || now - app->sampled >= 500) {
        app->usage = gui_process_usage(app->process);
        app->sampled = now;
    }
    if (app->leaving_ms && now_ms(app) - app->leaving_ms >= app->theme.fade_out_ms) leave_run(app);
}
static char* find_driver(App* app) {
#ifdef SDL_PLATFORM_ANDROID
    /* the app's libraries folder only takes lib*.so: the driver goes in as one */
    const char* name = app->menu.selected == GUI_SWITCH ? "libscarletnx-run.so" : "libscarletnx-gnuabi.so";
#else
    const char* name = app->menu.selected == GUI_SWITCH ? "scarletnx-mariko" : "scarletnx-gnuabi";
#endif
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
    if (!absolute || !snx_is_file(absolute)) {
        snprintf(app->message, sizeof(app->message), "That path is not a readable file. Try again or press ESC.");
        free(absolute);
        return;
    }
    bool native = prepared && app->menu.selected == GUI_SWITCH;
    char* driver = native ? NULL : find_driver(app);
    if (!native && !driver) {
        snprintf(app->message, sizeof(app->message), "Cannot find %s beside the GUI or on PATH.",
                 app->menu.selected == GUI_SWITCH ? "scarletnx-mariko" : "scarletnx-gnuabi");
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
    const char* argv[7] = {native ? absolute : driver};
    size_t n = 1;
    if (!native) argv[n++] = absolute;
    n += gui_launch_options(&app->menu, native, argv + n);
    argv[n] = NULL;
    bool started = gui_process_start(app->process, argv, app->message, sizeof(app->message));
    if (started && app->run_log) {
        if (app->run_out) fclose(app->run_out);
        app->run_out = snx_fopen(app->run_log, "wb");
        for (size_t k = 0; app->run_out && k < n; ++k) fprintf(app->run_out, "%s%s", k ? " " : "$ ", argv[k]);
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
    if (app->recent_file && !gui_recent_add(&app->recent, absolute, app->menu.selected == GUI_SWITCH ? GUI_SWITCH
                                                                                                      : GUI_LINUX,
                                            prepared))
        recent_changed(app);
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
static char* device_keys_path(void) {
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
#ifdef SDL_PLATFORM_ANDROID
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
/* the game log written last, games/<game>.log */
static char* newest_log(const App* app) {
    int count = 0, i;
    char** names = SDL_GlobDirectory(app->games_dir, "*.log", 0, &count);
    char* newest = NULL;
    SDL_Time time = 0;
    for (i = 0; names && i < count; ++i) {
        SDL_PathInfo info;
        char* path = snx_path_join(app->games_dir, names[i]);
        if (SDL_GetPathInfo(path, &info) && (!newest || info.modify_time > time)) {
            free(newest);
            newest = path;
            time = info.modify_time;
        } else
            free(path);
    }
    SDL_free(names);
    return newest;
}
/* the message on the screen, the last run's output and the newest game log: saved to Download/ScarletNX (else the
   app's own folder on the shared storage) and handed to the share sheet */
static void share_log(App* app) {
    char *game = newest_log(app), *run = log_tail(app->run_log, 128 << 10), *played = log_tail(game, 128 << 10);
    char* text = snx_format("%s\nMessage: %s\n\n== the last run: %s ==\n%s\n\n== %s ==\n%s\n", snx_build_info(),
                            app->message, app->run_log ? app->run_log : "", run, game ? game : "no game log", played);
    const char* folders[2] = {"/storage/emulated/0/Download/ScarletNX", SDL_GetAndroidExternalStoragePath()};
    char* saved = NULL;
    int i;
    for (i = 0; i < 2 && !saved; ++i) {
        char* path = folders[i] && !snx_mkdirs(folders[i]) ? snx_path_join(folders[i], "scarletnx-log.txt") : NULL;
        if (path && !snx_write_file(path, text, strlen(text)))
            saved = path;
        else
            free(path);
    }
    if (gui_android_share(text, app->message, sizeof(app->message)))
        snprintf(app->message, sizeof(app->message), "Log saved to %s", saved ? saved : "nowhere (no folder to write)");
    free(saved);
    free(text);
    free(played);
    free(run);
    free(game);
}
#endif
static void menu_key(App* app, GuiKey key) {
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
                launch(app, absolute, true);
            }
        }
        return;
    }
    gui_menu_key(&app->menu, key);
#ifdef SDL_PLATFORM_ANDROID
    if (key == GUI_SELECT && (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS)) open_picker(app);
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
#ifdef SDL_PLATFORM_ANDROID
    if (app->menu.share_log) {
        app->menu.share_log = 0;
        share_log(app);
    }
#endif
    if (key == GUI_CANCEL || (key == GUI_SELECT && app->menu.view == GUI_LOAD_KEYS)) app->message[0] = 0;
    if (app->menu.view == GUI_LOAD_GAMES) {
        gui_games_scan(&app->games, app->games_dir, app->message, sizeof(app->message));
        app->browser = (GuiBrowser){.count = app->games.count};
        layout(app);
    }
}
/* a label's cells, or its whole row for a finger, which is wider than a mouse pointer */
static bool hit_text(const App* app, float x, float y, int col, int row, const char* label, bool touch) {
    int end = touch ? app->cols : SDL_min(app->cols - 1, col + (int)SDL_utf8strlen(label));
    return x >= (touch ? 0 : col) && x < end && y >= row && y < row + 1;
}
static void pointer(App* app, float window_x, float window_y, bool click, bool touch) {
    if (app->menu.running || app->finished) return;
    layout(app);
    float x, y;
    if (!SDL_RenderCoordinatesFromWindow(app->renderer, window_x, window_y, &x, &y)) return;
    x /= 8 * app->scale;
    y /= 16 * app->scale;
    if (app->menu.view == GUI_LOAD_KEYS) {
        if (click && hit_text(app, x, y, 1, 8, "Back [ESC]", touch)) menu_key(app, GUI_CANCEL);
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
            if (!hit_text(app, x, y, 3, row + i + 1, load_labels[i], touch)) continue;
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
            if (!hit_text(app, x, y, 1, 5 + game, label, touch)) continue;
            app->menu.recent_focus = game + 1;
            app->menu.focus_load = false;
            if (click) menu_key(app, GUI_SELECT);
            return;
        }
    }
    for (int i = 0; i < GUI_ITEM_COUNT; ++i) {
        char label[100];
        menu_label(app, i, label, sizeof(label));
        if (!hit_text(app, x, y, 1, menu_row(app, i), label, touch)) continue;
        app->menu.selected = i;
        app->menu.recent_focus = 0;
        app->menu.focus_load = false;
        if (click) menu_key(app, GUI_SELECT);
        return;
    }
}
/* the file the system's picker chose, as a dropped one; none (cancelled) goes back */
static void picked(App* app, const char* uri) {
    char* path = uri ? gui_document_path(uri) : NULL;
    if (!uri)
        menu_key(app, GUI_CANCEL);
    else if (!path)
        snprintf(app->message, sizeof(app->message), "Pick the file from the device's storage, not from an app.");
    else
        drop(app, path, false);
    free(path);
}
/* ESC: a run is aborted, anything else goes back */
static void cancel(App* app) {
    if (!app->menu.running)
        menu_key(app, GUI_CANCEL);
    else if (!app->aborting) {
        /* Do not ask a hung program to shut down or wait on it in the event
           handler. Kill its tree now and let pump reap it without blocking. */
        gui_process_terminate(app->process);
        app->aborting = true;
    }
}
/* the row of a running run's ESC line, -1 when it has none */
static int escape_row(const App* app) {
    if (app->theme.run == GUI_RUN_STEPS) return app->rows - 1;
    return app->theme.head != GUI_STYLE_NONE ? 3 : -1;
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
    if (SDL_fabsf(dx) > 0.03f || SDL_fabsf(dy) > 0.03f) return; /* a drag, no tap */
    if (app->finished) {
        menu_key(app, GUI_CANCEL);
        return;
    }
    SDL_GetWindowSize(app->window, &w, &h);
    if (app->menu.running) {
        layout(app);
        if (SDL_RenderCoordinatesFromWindow(app->renderer, x * (float)w, y * (float)h, &rx, &ry) &&
            (int)(ry / (float)(16 * app->scale)) == escape_row(app))
            cancel(app);
        return;
    }
    pointer(app, x * (float)w, y * (float)h, true, true);
}
static void event(App* app, const SDL_Event* e) {
    if (e->type == SDL_EVENT_QUIT || e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        app->menu.quit = true;
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
        app->finger = e->tfinger.fingerID;
        app->finger_x = e->tfinger.x;
        app->finger_y = e->tfinger.y;
        return;
    }
    if ((e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) && e->tfinger.fingerID == app->finger) {
        app->finger = 0;
        if (e->type == SDL_EVENT_FINGER_UP) touch(app, e->tfinger.x, e->tfinger.y);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        pointer(app, e->motion.x, e->motion.y, false, false);
        return;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT) {
        pointer(app, e->button.x, e->button.y, true, false);
        return;
    }
    if (e->type != SDL_EVENT_KEY_DOWN) return;
    if (app->menu.running) {
        if (e->key.key == SDLK_ESCAPE || e->key.key == SDLK_AC_BACK) cancel(app);
        return;
    }
    if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL) &&
        (app->menu.view == GUI_LOAD_DROP || app->menu.view == GUI_LOAD_KEYS) && !e->key.repeat) {
        char* clipboard = SDL_GetClipboardText();
        drop(app, clipboard, true);
        SDL_free(clipboard);
        return;
    }
    switch (e->key.key) {
        case SDLK_UP: menu_key(app, GUI_UP); break;
        case SDLK_DOWN: menu_key(app, GUI_DOWN); break;
        case SDLK_LEFT: menu_key(app, GUI_LEFT); break;
        case SDLK_RIGHT: menu_key(app, GUI_RIGHT); break;
        case SDLK_PAGEUP: menu_key(app, GUI_PAGE_UP); break;
        case SDLK_PAGEDOWN: menu_key(app, GUI_PAGE_DOWN); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!e->key.repeat) menu_key(app, GUI_SELECT);
            break;
        case SDLK_ESCAPE:
        case SDLK_AC_BACK: menu_key(app, GUI_CANCEL); break;
        default: break;
    }
}
static bool initialize(App* app) {
    memset(app, 0, sizeof(*app));
#ifdef SDL_PLATFORM_ANDROID
    app->theme = *gui_theme_builtin("yorha");
    gui_android_start();
    /* back is ESC here: without this it would close the app */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#else
    app->theme = *gui_theme_builtin("default");
#endif
    /* touch() takes the fingers: SDL would send each as a mouse too */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) return false;
#ifdef SDL_PLATFORM_ANDROID
    /* the whole screen, the system's bars out of the way: a phone held wide is short */
    SDL_WindowFlags flags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    if (!SDL_CreateWindowAndRenderer(snx_build_info(), 1280, 800, flags, &app->window, &app->renderer))
        return false;
    SDL_SetWindowMinimumSize(app->window, 800, 480);
    snx_window_icon(app->window);
    SDL_SetRenderVSync(app->renderer, 1);
    SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
    SDL_SetEventEnabled(SDL_EVENT_DROP_TEXT, true);
    app->font = create_font(app->renderer);
    app->process = gui_process_create();
    app->cwd = SDL_GetCurrentDirectory();
    app->games_dir = snx_path_join(app->cwd ? app->cwd : ".", "games");
    app->themes_dirs[0] = snx_path_join(app->cwd ? app->cwd : ".", "themes");
#ifdef SDL_PLATFORM_ANDROID
    app->drivers = gui_android_libraries();
#else
    app->drivers = SDL_GetBasePath();
#endif
    if (app->drivers) app->themes_dirs[1] = snx_path_join(app->drivers, "themes");
    app->keys_path = device_keys_path();
    gui_console_init(&app->console, 78, 17);
    if (!app->font || !app->process || !app->drivers) {
        SDL_SetError("Cannot initialize GUI resources");
        return false;
    }
    layout(app);
    return true;
}
static void cleanup(App* app) {
    gui_process_destroy(app->process);
    free(app->file);
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
    SDL_DestroyTexture(app->font);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
}
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
    /* the user's data folder (~/.local/share/scarletnx/gui/ on Linux, under %APPDATA% on Windows); main() opens
       it, not initialize(), so the tests keep their own */
    prefs = SDL_GetPrefPath("scarletnx", "gui");
    if (prefs) recent_open(&app, snx_path_join(prefs, "recent.txt"));
    if (prefs) app.run_log = snx_path_join(prefs, "last-run.log");
    SDL_free(prefs);
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
