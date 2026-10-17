/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the actual renderer and SDL event handlers without production test flags. */
#define SNX_GUI_NO_MAIN
#include "gui/main.c"
#include <assert.h>
#ifndef _WIN32
#include <sys/statvfs.h> /* Folders on a disk mounted noexec */
#endif
/* the default theme's colours, as the checks below name them */
static const SDL_Color normal = {220, 220, 220, 255}, yellow = {255, 255, 0, 255}, red = {255, 0, 0, 255},
                       pink = {255, 85, 255, 255}, black = {0, 0, 0, 255};

static void key(App* app, SDL_Keycode code) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
/* a finger down at pixel x, y of the render and lifted at to_x, to_y, as SDL gives it: 0 to 1 across the window */
static void swipe_px(App* app, int x, int y, int to_x, int to_y) {
    SDL_Event e = {0};
    float wx, wy;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    e.tfinger.fingerID = 7;
    e.type = SDL_EVENT_FINGER_DOWN;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)to_x + 0.5f, (float)to_y + 0.5f, &wx, &wy));
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    event(app, &e);
}
/* a key with a modifier held, and text as a keyboard types it */
static void key_with(App* app, SDL_Keycode code, SDL_Keymod mod) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.mod = mod;
    event(app, &e);
}
static void typed(App* app, const char* text) {
    SDL_Event e = {0};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = text;
    event(app, &e);
}
/* what the system's picker answers, from its own thread */
static void pick(App* app, const char* uri) {
    SDL_Event e = {0};
    e.type = app->picked;
    e.user.data1 = uri ? SDL_strdup(uri) : NULL;
    assert(SDL_PushEvent(&e));
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void colored_text_on(App* app, int col, int row, const char* prompt, SDL_Color color, SDL_Color back) {
    /* Verify the exact text, its glyph color and what is behind it in the real renderer. */
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; prompt[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                assert(SDL_ReadSurfacePixel(image, ((i + col) * 8 + x) * app->scale, (row * 16 + y) * app->scale, &r,
                                            &g, &b, &a));
                bool ink = (gui_vga_font[(unsigned char)prompt[i] * 16 + y] & (0x80 >> x)) != 0;
                SDL_Color want = ink ? color : back;
                assert(r == want.r && g == want.g && b == want.b);
            }
    SDL_DestroySurface(image);
}
static SDL_Color pixel(App* app, float x, float y) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    Uint8 r = 0, g = 0, b = 0, a = 0; /* mingw's assert returns, as far as the compiler knows */
    SDL_Color out;
    assert(image && SDL_ReadSurfacePixel(image, (int)x, (int)y, &r, &g, &b, &a));
    SDL_DestroySurface(image);
    out.r = r;
    out.g = g;
    out.b = b;
    out.a = 255;
    return out;
}
static int same_colour(SDL_Color a, SDL_Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
/* Setup device keys' box in the failure colour, at the clock given (a blink's half): inside it, past its label */
static int keys_red(App* app, Uint64 clock) {
    GuiMenuBoxes m;
    GuiRect b;
    app->clock = clock;
    render(app);
    menu_boxes(app, &m);
    b = list_box(&m.list, GUI_KEYS);
    return same_colour(pixel(app, (float)(b.x + b.w - 3 * app->scale - 1), (float)(b.y + b.h / 2)),
                       rgb(app->theme.failure));
}
static int near_colour(SDL_Color a, SDL_Color b, int by) {
    return abs(a.r - b.r) <= by && abs(a.g - b.g) <= by && abs(a.b - b.b) <= by;
}
/* the splash on the screen: page all around the square of side pixels in the middle, and in it svg's drawing at that
   size on the page as snx_splash_draw makes it (premultiplied, so over black its own colours) */
static void splash_drawn(App* app, const SDL_Rect* view, const SnxSplashSvg* svg, int side, SDL_Color page) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    Uint32* drawing = malloc((size_t)side * (size_t)side * 4);
    int left = (view->w - side) / 2, top = (view->h - side) / 2, x, y, drawn = 0;
    SnxSplashDrawing* read = snx_splash_open(svg);
    assert(image && drawing && read && !snx_splash_draw(read, side, drawing, side * 4));
    snx_splash_close(read);
    for (y = 0; y < view->h; ++y)
        for (x = 0; x < view->w; ++x) {
            Uint8 r = 0, g = 0, b = 0, a = 0;
            int in = x >= left && x < left + side && y >= top && y < top + side;
            assert(SDL_ReadSurfacePixel(image, x, y, &r, &g, &b, &a));
            if (!in) {
                assert(r == page.r && g == page.g && b == page.b);
            } else {
                Uint32 p = drawing[(y - top) * side + (x - left)];
                int alpha = (int)(p >> 24), want[3] = {(int)(p >> 16 & 255), (int)(p >> 8 & 255), (int)(p & 255)};
                int c, got[3] = {r, g, b}, on[3] = {page.r, page.g, page.b};
                for (c = 0; c < 3; ++c) assert(abs(got[c] - (want[c] + on[c] * (255 - alpha) / 255)) <= 2);
                drawn += alpha > 0;
            }
        }
    assert(drawn > side * side / 4);
    free(drawing);
    SDL_DestroySurface(image);
}
/* text in color anywhere on the render, glyph by glyph in the font's own bytes: its ink pixels in color, the others
   not (a box's frame may cross the font's last row, which is empty); 1 and where it starts, else 0 */
static int find_text(App* app, const char* text, SDL_Color color, int size, int* at_x, int* at_y) {
    SDL_Surface *read = SDL_RenderReadPixels(app->renderer, NULL), *image;
    int s = app->scale * size, n = (int)strlen(text), first = -1, fx = 0, fy = 0, found = 0, x, y;
    assert(read);
    image = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(read);
    assert(image);
#define SNX_AT(px, py) ((const Uint8*)image->pixels + (py) * image->pitch + (px) * 4)
#define SNX_IS(c)      ((c)[0] == color.r && (c)[1] == color.g && (c)[2] == color.b)
    for (int i = 0; i < n && first < 0; ++i) /* the first ink, to look for */
        for (int gy = 0; gy < 15 && first < 0; ++gy)
            for (int gx = 0; gx < 8 && first < 0; ++gx)
                if (gui_vga_font[(unsigned char)text[i] * 16 + gy] & (0x80 >> gx)) {
                    first = i;
                    fx = (i * 8 + gx) * s;
                    fy = gy * s;
                }
    assert(first >= 0);
    for (y = 0; !found && y + 15 * s <= image->h; ++y)
        for (x = 0; !found && x + n * 8 * s <= image->w; ++x) {
            if (!SNX_IS(SNX_AT(x + fx, y + fy))) continue;
            found = 1;
            for (int i = 0; found && i < n; ++i)
                for (int gy = 0; found && gy < 15; ++gy)
                    for (int gx = 0; found && gx < 8; ++gx) {
                        int ink = (gui_vga_font[(unsigned char)text[i] * 16 + gy] & (0x80 >> gx)) != 0;
                        found = ink == SNX_IS(SNX_AT(x + (i * 8 + gx) * s, y + gy * s));
                    }
            if (found) {
                *at_x = x;
                *at_y = y;
            }
        }
#undef SNX_IS
#undef SNX_AT
    SDL_DestroySurface(image);
    return found;
}
/* the text drawn, or not, in color, its letters size times the text's: a check that fails names its line */
#define shows(app, text, color)           shows_at(__LINE__, app, text, color, 1, 1)
#define hides(app, text, color)           shows_at(__LINE__, app, text, color, 1, 0)
#define shows_big(app, text, color, size) shows_at(__LINE__, app, text, color, size, 1)
static void shows_at(int line, App* app, const char* text, SDL_Color color, int size, int drawn) {
    int x, y;
    if (find_text(app, text, color, size, &x, &y) == drawn) return;
    if (drawn)
        fprintf(stderr, "gui_sdl.c:%d: \"%s\" is not drawn in %02x%02x%02x\n", line, text, color.r, color.g, color.b);
    else
        fprintf(stderr, "gui_sdl.c:%d: \"%s\" is drawn in %02x%02x%02x at %d,%d\n", line, text, color.r, color.g,
                color.b, x, y);
    if (getenv("SNX_GUI_FAILED_SHOT")) { /* what was drawn, to look at */
        SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
        if (image) SDL_SaveBMP(image, getenv("SNX_GUI_FAILED_SHOT"));
        SDL_DestroySurface(image);
    }
    assert(!"the text drawn");
}
/* the same in the foot's letters, a size under the rest's (foot_scale) */
#define shows_foot(app, text, color) foot_shows_at(__LINE__, app, text, color, 1)
#define hides_foot(app, text, color) foot_shows_at(__LINE__, app, text, color, 0)
static void foot_shows_at(int line, App* app, const char* text, SDL_Color color, int drawn) {
    int scale = app->scale;
    app->scale = foot_scale(app);
    shows_at(line, app, text, color, 1, drawn);
    app->scale = scale;
}
/* the boxes a pointer or a finger goes to: a menu's item, Games' rows (0 Add a folder, then the page's games; with
   recent a recent row), a form's row on the left or the right; a pointer at a box's middle */
static GuiRect entry_box(App* app, int item) {
    GuiMenuBoxes m;
    layout(app);
    menu_boxes(app, &m);
    return list_box(&m.list, item);
}
static GuiRect game_box(App* app, int i, bool recent) {
    GuiGamesBoxes g;
    layout(app);
    games_boxes(app, &g);
    assert(app->menu.view == GUI_LOAD_GAMES && (!recent || i < g.lead - 1));
    return recent ? list_box(&g.list, i + 1) : i ? list_box(&g.list, g.lead + i - 1) : list_box(&g.add, 0);
}
static GuiRect row_box(App* app, int row, bool right) {
    GuiFormBoxes f;
    layout(app);
    form_boxes(app, &f);
    return list_box(right ? &f.right : &f.left, row);
}
/* text drawn from pixel x, y in the font's own bytes: its glyphs in color, around them back but on the font's last row,
   empty, which a box's frame may cover; a pixel that differs is named with the check's line */
#define text_px_is(app, x0, y0, text, color, back) text_px_is_at(__LINE__, app, x0, y0, text, color, back)
static void text_px_is_at(int line, App* app, int x0, int y0, const char* text, SDL_Color color, SDL_Color back) {
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; text[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                int px = x0 + (i * 8 + x) * app->scale, py = y0 + y * app->scale;
                assert(SDL_ReadSurfacePixel(image, px, py, &r, &g, &b, &a));
                bool ink = (gui_vga_font[(unsigned char)text[i] * 16 + y] & (0x80 >> x)) != 0;
                SDL_Color want = ink ? color : back;
                if ((y == 15 && !ink) || (r == want.r && g == want.g && b == want.b)) continue;
                fprintf(stderr, "gui_sdl.c:%d: \"%s\" from %d,%d: '%c' at %d,%d is %02x%02x%02x, not %02x%02x%02x\n",
                        line, text, x0, y0, text[i], px, py, r, g, b, want.r, want.g, want.b);
                assert(!"the text drawn");
            }
    SDL_DestroySurface(image);
}
/* the same in the foot's letters */
#define foot_px_is(app, x0, y0, text, color, back) foot_px_is_at(__LINE__, app, x0, y0, text, color, back)
static void foot_px_is_at(int line, App* app, int x0, int y0, const char* text, SDL_Color color, SDL_Color back) {
    int scale = app->scale;
    app->scale = foot_scale(app);
    text_px_is_at(line, app, x0, y0, text, color, back);
    app->scale = scale;
}
/* where the text of a box of the foot goes, in its letters (foot_text) */
static GuiRect foot_room(App* app, GuiRect r, int box) {
    int scale = app->scale;
    GuiRect out;
    app->scale = foot_scale(app);
    out = foot_text(app, r, box);
    app->scale = scale;
    return out;
}
/* the mouse at pixel x, y of the render, moved there or clicking */
static void mouse_px(App* app, int x, int y, bool click) {
    SDL_Event e = {0};
    float wx, wy;
    layout(app);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.type = click ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_MOTION;
    e.button.button = SDL_BUTTON_LEFT;
    if (click) {
        e.button.x = wx;
        e.button.y = wy;
    } else {
        e.motion.x = wx;
        e.motion.y = wy;
    }
    event(app, &e);
}
/* a finger tapping pixel x, y of the render */
static void tap_px(App* app, int x, int y) {
    SDL_Event e = {0};
    float wx, wy;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (float)x + 0.5f, (float)y + 0.5f, &wx, &wy));
    e.tfinger.fingerID = 9;
    e.tfinger.x = wx / (float)w;
    e.tfinger.y = wy / (float)h;
    e.type = SDL_EVENT_FINGER_DOWN;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    event(app, &e);
}
static void point(App* app, GuiRect b, bool click) { mouse_px(app, b.x + b.w / 2, b.y + b.h / 2, click); }
/* a click, or a tap, on column col of the key the foot draws in color, in its letters */
#define press_key(app, key, col, tap, color) press_key_at(__LINE__, app, key, col, tap, color)
static void press_key_at(int line, App* app, const char* key, int col, bool tap, SDL_Color color) {
    int x, y, fs = foot_scale(app), scale = app->scale;
    render(app);
    app->scale = fs;
    if (!find_text(app, key, color, 1, &x, &y)) shows_at(line, app, key, color, 1, 1); /* says which */
    app->scale = scale;
    x += col * 8 * fs + 4 * fs;
    y += 8 * fs;
    if (tap)
        tap_px(app, x, y);
    else
        mouse_px(app, x, y, true);
}
static void touch_box(App* app, GuiRect b) { tap_px(app, b.x + b.w / 2, b.y + b.h / 2); }
/* a path the GUI keeps against one the test made: on Windows the system hands paths back with backslashes, where
   snx_path_join put a '/' */
static bool same_path(const char* a, const char* b) {
#ifdef _WIN32
    for (; *a && *b; ++a, ++b)
        if (*a != *b && !(strchr("/\\", *a) && strchr("/\\", *b))) return false;
    return *a == *b;
#else
    return !strcmp(a, b);
#endif
}
/* the Settings' pick for key is value; NULL: the .ini's */
static bool setting_is(App* app, const char* key, const char* value) {
    const char* v = gui_settings_value(&app->settings, key);
    return value ? v && !strcmp(v, value) : !v;
}
/* Select on a row of Settings > App, then back to the menu unless it opened a screen of its own */
static void app_select(App* app, int row) {
    open_app(app, row);
    key(app, SDLK_RETURN);
    if (app->editing && app->form == GUI_FORM_APP) leave_editor(app);
}
/* the launcher's own two Settings on or off */
static void launcher(App* app, bool on) {
    assert(gui_settings_set(&app->settings, "libgccjit", on && SNX_GUI_HAS_GCCJIT ? "on" : "off"));
    assert(gui_settings_set(&app->settings, "gdbstub", on ? "on" : "off"));
}
/* the focus on the game of Games named name, its page shown */
static void focus_game(App* app, const char* name) {
    size_t i = 0;
    list_games(app);
    layout(app);
    while (i < app->games.count && strcmp(app->games.names[i], name)) ++i;
    assert(i < app->games.count && app->browser.page_size);
    app->browser.page = i / app->browser.page_size;
    app->browser.selected = app->browser.head + i % app->browser.page_size;
}
static void finish(App* app, int status) {
    assert(app->menu.running && !app->finished && !app->aborting && !app->abnormal_exit);
    int selected = app->menu.selected;
    Uint64 until = SDL_GetTicks() + 10000;
    while (app->menu.running && SDL_GetTicks() < until) {
        pump(app);
        render(app);
        SDL_Delay(5);
    }
    assert(!app->menu.running && app->finished && app->menu.view == GUI_LOAD_NONE);
    char expected[80];
    snprintf(expected, sizeof(expected),
             status ? "Program ended abnormally (status %d)." : "Driver exited with status %d.", status);
    assert(!strcmp(app->message, expected) && app->abnormal_exit == (status != 0) && app->has_output);
    shows_foot(app, "Press [ESC] to return to menu", red);   /* where the keys go, in their letters */
    if (status) shows(app, "Program ended abnormally", red); /* beside the output, how it ended */
    /* a theme can make that line a prompt: framed and placed, the done and the failed one apart */
    GuiPromptLine line = status ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE;
    app->theme.prompt.lines[line] = true;
    app->theme.prompt.blink = false;
    render(app);
    shows_foot(app, "[ Press [ESC] to return to menu ]", red);
    app->theme.prompt.lines[line] = false;
    app->theme.prompt.lines[status ? GUI_PROMPT_DONE : GUI_PROMPT_FAILED] = true;
    render(app);
    hides_foot(app, "[ Press [ESC] to return to menu ]", red);
    shows_foot(app, "Press [ESC] to return to menu", red);
    app->theme.prompt = gui_theme_builtin(app->theme.id)->prompt;
    for (int i = 0; i < 3; ++i) pump(app);
    mouse_px(app, 4, 4, true); /* A completed process still requires ESC acknowledgement. */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    key(app, SDLK_SPACE);
    assert(!app->menu.running && app->finished && app->menu.selected == selected);
    key(app, SDLK_ESCAPE);
    assert(!app->menu.running && !app->finished && app->menu.view == GUI_LOAD_NONE);
    assert(!app->menu.quit && !app->message[0] && app->has_output);
    /* the item the run was started from has the focus again */
    assert(app->menu.selected == selected && setting_is(app, "fullscreen", "1"));
    render(app);
}
/* what the button map page last drew: a text holding s, or the shape of id of a kind and style (-1: any) */
static bool drawn_text(const App* app, const char* s) {
    for (int i = 0; i < app->pad_drawing.n; ++i)
        if (app->pad_drawing.shape[i].kind == SNX_PAD_SHAPE_TEXT && strstr(app->pad_drawing.shape[i].text, s))
            return true;
    return false;
}
static const SnxPadShape* drawn(const App* app, int id, int kind, int style) {
    for (int i = 0; i < app->pad_drawing.n; ++i) {
        const SnxPadShape* p = &app->pad_drawing.shape[i];
        if (p->id == id && p->kind == kind && (style < 0 || p->style == style)) return p;
    }
    return NULL;
}
/* the page's events as the loop gets them, after the virtual pad's changes */
static void pads(App* app) {
    SDL_Event e;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&e)) event(app, &e);
}
static void pad_press(App* app, SDL_Joystick* j, SDL_GamepadButton button) {
    SDL_SetJoystickVirtualButton(j, button, true);
    pads(app);
    SDL_SetJoystickVirtualButton(j, button, false);
    pads(app);
}
static bool picks_have(App* app, const char* line) {
    size_t size;
    char* kept = (char*)snx_read_file(app->settings_file, &size);
    bool has = kept && strstr(kept, line);
    free(kept);
    return has;
}
/* Settings > Controllers by player, as Eden's tabs: its Player row goes round All (every player's rows) and 1 to 8
   (the player's own and those no player has its own of); a player's Button map is the button map page on its keys,
   its A and B as its own swap says, else every player's; ESC comes back to the player's rows */
static void players_view(App* app) {
    static const char list[] =
        "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\tSwap A/B\tSwap A/B\t0=Off|1=On\n"
        "Controllers\tgamepad_deadzone\t10\tint\t0\t90\tDead zone\tStick dead zone\t0=0%|10=10%\n"
        "Controllers\tgamepad_rumble\t100\tint\t0\t100\tVibration\tVibration\t0=Off|100=100%\n"
        "Controllers/Button map\tgamepad_map\t\ttext\t\t\tEach control's button\tButton map\t\n"
        "Controllers/Player\tgamepad_p1_device\t\ttext\t\t\tIts controller\tController\t\n"
        "Controllers/Player\tgamepad_p1_swap_ab\t-1\tint\t-1\t1\tIts A and B\tSwap A/B\t-1=As All|0=Off|1=On\n"
        "Controllers/Player\tgamepad_p1_map\t\ttext\t\t\tIts map\tButton map\t\n"
        "Controllers/Player\tgamepad_p2_device\t\ttext\t\t\t\tController\t\n"
        "Controllers/Player\tgamepad_p2_swap_ab\t-1\tint\t-1\t1\t\tSwap A/B\t-1=As All|0=Off|1=On\n"
        "Controllers/Player\tgamepad_p2_map\t\ttext\t\t\t\tButton map\t\n";
    int r;
    gui_settings_free(&app->settings);
    memset(&app->settings, 0, sizeof app->settings);
    assert(!gui_settings_read(&app->settings, list));
    gui_settings_mark(&app->settings);
    app->player = 0;
    open_settings(app);
    key(app, SDLK_DOWN); /* past Launch, the launcher's own */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    /* All: the Player row, then every player's */
    assert(app->form == GUI_FORM_GROUP && app->view_count == 5 && app->editor.row == 0);
    assert(!strcmp(app->settings.values[setting_of(app, 0)].text, "< All >"));
    assert(row_of(app, setting_row(app, "gamepad_swap_ab")) == 1 &&
           row_of(app, setting_row(app, "gamepad_p1_map")) < 0);
    render(app);
    shows(app, "- Player", yellow); /* focused */
    /* Right: player 1, its own rows and the dead zone; no every player's Swap A/B, Vibration or Button map */
    key(app, SDLK_RIGHT);
    assert(app->player == 1 && app->view_count == 5 && !strcmp(app->settings.values[setting_of(app, 0)].text, "< 1 >"));
    assert(row_of(app, setting_row(app, "gamepad_p1_swap_ab")) > 0 &&
           row_of(app, setting_row(app, "gamepad_swap_ab")) < 0);
    assert(row_of(app, setting_row(app, "gamepad_deadzone")) > 0 &&
           row_of(app, setting_row(app, "gamepad_p2_map")) < 0);
    render(app);
    shows(app, "As All [default]", normal);
    shows(app, "As All >", normal); /* its Button map, every player's until it has its own */
    /* its Controller: Any while it names none; Enter waits for a press, which gives it the controller pressed by its
       GUID ("/2" for the second of two alike); ESC leaves it as it was */
    {
        SDL_VirtualJoystickDesc d;
        SDL_JoystickID one, two;
        SDL_Joystick *j1, *j2;
        char guid[33], line[40];
        shows(app, "Any [default]", normal);
        app->editor.row = row_of(app, setting_row(app, "gamepad_p1_device"));
        key(app, SDLK_RETURN);
        assert(app->device_listen && strstr(app->editor.note, "Press a button on player 1's controller"));
        key(app, SDLK_ESCAPE);
        assert(!app->device_listen && !gui_settings_value(&app->settings, "gamepad_p1_device") &&
               app->form == GUI_FORM_GROUP && !SDL_WasInit(SDL_INIT_GAMEPAD));
        key(app, SDLK_RETURN);
        assert(SDL_InitSubSystem(SDL_INIT_JOYSTICK)); /* the pads stay between the waits, as real ones do */
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT;
        d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.vendor_id = 0x045e;
        d.product_id = 0x02ea;
        d.name = "One Pad";
        one = SDL_AttachVirtualJoystick(&d);
        two = SDL_AttachVirtualJoystick(&d);
        j1 = SDL_OpenJoystick(one);
        j2 = SDL_OpenJoystick(two);
        assert(j1 && j2);
        pads(app);
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(one), guid, sizeof guid);
        pad_press(app, j2, SDL_GAMEPAD_BUTTON_SOUTH);
        snprintf(line, sizeof line, "%s/2", guid);
        assert(!app->device_listen && !strcmp(gui_settings_value(&app->settings, "gamepad_p1_device"), line));
        assert(!strcmp(app->editor.note, "Player 1: One Pad") && picks_have(app, "gamepad_p1_device="));
        key(app, SDLK_RETURN);
        pad_press(app, j1, SDL_GAMEPAD_BUTTON_SOUTH);
        assert(!strcmp(gui_settings_value(&app->settings, "gamepad_p1_device"), guid));
        key(app, SDLK_DELETE); /* the next to connect again */
        assert(!gui_settings_value(&app->settings, "gamepad_p1_device"));
        SDL_CloseJoystick(j1);
        SDL_CloseJoystick(j2);
        assert(SDL_DetachVirtualJoystick(one) && SDL_DetachVirtualJoystick(two));
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    }
    /* round: Left twice is 8 (no rows of its own here), Right twice back to 1 */
    app->editor.row = 0;
    key(app, SDLK_LEFT);
    key(app, SDLK_LEFT);
    assert(app->player == 8 && app->view_count == 2);
    key(app, SDLK_RIGHT);
    key(app, SDLK_RIGHT);
    assert(app->player == 1);
    /* its Swap A/B and every player's: its own wins, As All takes every player's */
    assert(gui_settings_set(&app->settings, "gamepad_swap_ab", "1"));
    app->editor.row = row_of(app, setting_row(app, "gamepad_p1_map"));
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && app->pad_player == 1 && app->pad_map == setting_row(app, "gamepad_p1_map"));
    assert(app->pad.swap_ab); /* As All: on */
    render(app);
    shows(app, "SETTINGS  /  CONTROLLERS  /  PLAYER 1  /  BUTTON MAP", normal);
    /* a change keeps the player's map, every player's untouched */
    app->pad.focus = SNX_PAD_LT;
    key(app, SDLK_RIGHT);
    assert(gui_settings_value(&app->settings, "gamepad_p1_map") && !gui_settings_value(&app->settings, "gamepad_map"));
    assert(strstr(gui_settings_value(&app->settings, "gamepad_p1_map"), "lt:ZR"));
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_GROUP && app->player == 1 &&
           setting_of(app, app->editor.row) == setting_row(app, "gamepad_p1_map"));
    assert(gui_settings_set(&app->settings, "gamepad_p1_swap_ab", "0"));
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && !app->pad.swap_ab); /* its own: off */
    key(app, SDLK_ESCAPE);
    /* its map its own now; Del on a player's row: back to As All */
    render(app);
    shows(app, "Its own >", yellow); /* focused, as ESC came back to it */
    app->editor.row = row_of(app, setting_row(app, "gamepad_p1_swap_ab"));
    key(app, SDLK_DELETE);
    assert(!gui_settings_value(&app->settings, "gamepad_p1_swap_ab"));
    /* every row the view shows is a row of the settings, the Player row first */
    for (r = 0; r < app->view_count; ++r) assert(setting_of(app, r) >= 0 && setting_of(app, r) < app->settings.count);
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_SETTINGS);
    app->player = 0;
    leave_editor(app);
}
/* Settings > Controllers > Button map: the pad drawn (pad_screen.h) with what each control sends, on SDL's virtual
   Xbox pad. No presets: each control the button in its place, gamepad_map over that, A and B as gamepad_swap_ab says;
   the keys, the pad's presses, its triggers and stick, a click and the wheel act on it, each change kept in the picks
   at once, Del puts the map back, and ESC goes back to the page's row with the pads closed */
static void button_map(App* app) {
    static const char list[] =
        "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\tSwap A/B\tSwap A/B\t0=Off|1=On\n"
        "Controllers\tgamepad_deadzone\t10\tint\t0\t90\tDead zone\tStick dead zone\t0=0%|10=10%\n"
        "Controllers\tgamepad_joycon_pair\t1\tint\t0\t1\tJoy-Cons\tPair Joy-Cons\t0=Separate|1=Together\n"
        "Controllers/Button map\tgamepad_map\t\ttext\t\t\tEach control's button\tButton map\t\n";
    SDL_VirtualJoystickDesc d;
    SDL_JoystickID id;
    SDL_Joystick* j;
    SDL_Event e = {0};
    const SnxPadShape* box;
    float x = 0, y = 0; /* mingw's assert returns, as far as the compiler knows */
    int page = -1, focus;
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x045e/0x02ea"); /* a real pad plugged in stays out */
    gui_settings_free(&app->settings);
    memset(&app->settings, 0, sizeof app->settings);
    assert(!gui_settings_read(&app->settings, list));
    gui_settings_mark(&app->settings);
    for (int i = 0; i < app->settings.count; ++i)
        if (app->settings.values[i].kind == GUI_SETTING_PAGE) page = i;
    open_settings(app);
    key(app, SDLK_DOWN); /* past Launch, the launcher's own */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_GROUP && !strcmp(app->group_keys[app->group].key, "Controllers"));
    app->editor.row = page - app->first;
    key(app, SDLK_RETURN);
    /* drawn before SDL looks for pads, which takes it most of a second, saying so */
    shows(app, "Looking for controllers...", rgb(app->theme.soft));
    /* no pad: no presets, the first control focused, A and B where the console's are */
    assert(app->form == GUI_FORM_PAD && app->pads_read && !app->gamepad && app->pad.focus == SNX_PAD_LT &&
           !app->pad.swap_ab);
    /* two Joy-Cons one pad, as gamepad_joycon_pair has it when not picked */
    assert(!strcmp(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS), "1"));
    render(app);
    shows(app, "SETTINGS  /  CONTROLLERS  /  BUTTON MAP", normal);
    assert(drawn_text(app, "No controller") && !drawn_text(app, "By position") && !drawn_text(app, "By label"));
    assert(drawn(app, SNX_PAD_LT, SNX_PAD_SHAPE_BOX, SNX_PAD_STYLE_FOCUS) && drawn_text(app, "LT sends ZL"));
    /* its texts in the font's own glyphs, a byte each */
    {
        SnxPadShape shape = {SNX_PAD_SHAPE_TEXT,
                             SNX_PAD_STYLE_PLAIN,
                             -1,
                             16.0f * (float)app->scale,
                             16.0f * 20 * (float)app->scale,
                             0,
                             0,
                             "\xFE \x82"};
        SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
        SDL_RenderClear(app->renderer);
        pad_text(app, &shape, app->theme.ink);
        colored_text_on(app, 2, 20, "\xFE \x82", rgb(app->theme.ink), black);
    }
    key(app, SDLK_RETURN); /* with no pad, nothing to wait for */
    assert(!app->pad.listening && !gui_settings_value(&app->settings, "gamepad_map"));
    /* a pad plugged in is opened, named, and what it holds lit */
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = 0x045e;
    d.product_id = 0x02ea;
    d.name = "Man\xC3\xA9 Pad"; /* UTF-8, shown in the font's glyphs */
    id = SDL_AttachVirtualJoystick(&d);
    j = SDL_OpenJoystick(id);
    assert(id && j);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) /* at rest: a trigger at the bottom of its range */
        SDL_SetJoystickVirtualAxis(j, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SDL_JOYSTICK_AXIS_MIN : 0);
    pads(app);
    assert(app->gamepad && SDL_GetGamepadID(app->gamepad) == id);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, true); /* X: no key of the page's */
    pads(app);
    render(app);
    assert(!strcmp(app->pad.pad, "Man\x82 Pad"));
    assert(!drawn_text(app, "No controller"));
    assert(drawn_text(app, "Man\x82 Pad"));
    assert(drawn(app, SNX_PAD_NORTH, SNX_PAD_SHAPE_DISC, SNX_PAD_STYLE_LIT) && app->form == GUI_FORM_PAD);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, false);
    pads(app);
    /* its presses as the game's menu takes them, by the button each sends: the D-pad moves; A, the right one as on
       the console, waits for a press, which takes the focused control's button and gives its own */
    app->pad.focus = SNX_PAD_SOUTH;
    pad_press(app, j, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    assert(app->pad.focus == SNX_PAD_RS);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_DPAD_UP);
    assert(app->pad.focus == SNX_PAD_SOUTH && !app->pad.listening);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_EAST);
    assert(app->pad.listening);
    pad_press(app, j, SDL_GAMEPAD_BUTTON_EAST);
    assert(!app->pad.listening && app->pad.focus == SNX_PAD_EAST && app->form == GUI_FORM_PAD);
    /* the whole map is the player's from then on, whatever pad comes next */
    assert(setting_is(app, "gamepad_map",
                      "south:A east:B west:Y north:X lb:L rb:R lt:ZL rt:ZR ls:L3 rs:R3 back:Minus "
                      "start:Plus up:Up down:Down left:Left right:Right"));
    assert(picks_have(app, "gamepad_map=south:A east:B west:Y"));
    /* a trigger is pressed once it is past SNX_PAD_TRIGGER */
    app->pad.focus = SNX_PAD_LT;
    key(app, SDLK_RETURN);
    assert(app->pad.listening);
    /* a trigger's 0 to 32767 from its axis' whole range */
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (Sint16)(2 * (SNX_PAD_TRIGGER - 100) - 32768));
    pads(app);
    assert(app->pad.listening);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    pads(app);
    assert(!app->pad.listening && strstr(gui_settings_value(&app->settings, "gamepad_map"), " lt:ZR rt:ZL "));
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    pads(app);
    /* the left stick past half way is the D-pad, a step each time it crosses */
    focus = app->pad.focus;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    pads(app);
    assert(app->pad.focus != focus);
    focus = app->pad.focus;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 32000);
    pads(app);
    assert(app->pad.focus == focus);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pads(app);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 20000);
    pads(app);
    assert(app->pad.focus != focus);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, 0);
    pads(app);
    /* the wheel moves as Up and Down; a click on a control's box focuses it, a second one waits for a press */
    focus = app->pad.focus;
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.integer_y = 1;
    event(app, &e);
    assert(app->pad.focus != focus);
    render(app);
    box = drawn(app, SNX_PAD_NORTH, SNX_PAD_SHAPE_BOX, -1);
    assert(box && SDL_RenderCoordinatesToWindow(app->renderer, box->x + box->w / 2, box->y + box->h / 2, &x, &y));
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = x;
    e.button.y = y;
    event(app, &e);
    assert(app->pad.focus == SNX_PAD_NORTH && !app->pad.listening);
    event(app, &e);
    assert(app->pad.listening);
    key(app, SDLK_ESCAPE); /* stops waiting, stays */
    assert(!app->pad.listening && app->form == GUI_FORM_PAD);
    /* Del: the map the game's .ini's again; Swap A/B, its row's, over it when the page opens */
    key(app, SDLK_DELETE);
    assert(!gui_settings_value(&app->settings, "gamepad_map") && !picks_have(app, "gamepad_"));
    {
        SnxPadMap shown;
        assert(gui_settings_set(&app->settings, "gamepad_swap_ab", "1"));
        pad_start(app);
        snx_pad_screen_map(&app->pad, &shown);
        assert(app->pad.swap_ab && shown.bit[SNX_PAD_SOUTH] == 0 && shown.bit[SNX_PAD_EAST] == 1); /* A, B */
        assert(gui_settings_set(&app->settings, "gamepad_swap_ab", ".ini"));
        pad_start(app);
        assert(!app->pad.swap_ab);
    }
    /* the pad unplugged: none shown, nothing waited for */
    app->pad.focus = SNX_PAD_NORTH;
    key(app, SDLK_RETURN);
    assert(app->pad.listening);
    SDL_CloseJoystick(j);
    assert(SDL_DetachVirtualJoystick(id));
    pads(app);
    assert(!app->gamepad && !app->pad.listening);
    render(app);
    assert(drawn_text(app, "No controller"));
    /* a joystick SDL has no gamepad mapping for, plugged in: named in the middle, with its GUID and the file its
       mapping goes in, in the failure colour; gone once it is unplugged */
    {
        char *folder = snx_dirname(app->settings_file), *file = snx_path_join(folder, "gamecontrollerdb.txt");
        SDL_VirtualJoystickDesc odd;
        SDL_JoystickID stick;
        assert(!app->editor.note[0] && !app->pad.note[0]);
        SDL_INIT_INTERFACE(&odd);
        odd.type = SDL_JOYSTICK_TYPE_UNKNOWN;
        odd.naxes = 2;
        odd.nbuttons = 4;
        odd.vendor_id = 0x1234;
        odd.product_id = 0x5678;
        odd.name = "Odd Stick";
        stick = SDL_AttachVirtualJoystick(&odd);
        assert(stick);
        pads(app);
        assert(!app->gamepad && !strncmp(app->pad.note, "Odd Stick has no SDL mapping: add a line for GUID ",
                                         sizeof "Odd Stick has no SDL mapping: add a line for GUID " - 1));
        assert(strstr(app->pad.note, file) && !app->editor.note[0]);
        render(app);
        assert(drawn_text(app, "Odd Stick has no SDL"));
        {
            int i, noted = 0;
            for (i = 0; i < app->pad_drawing.n; ++i) noted |= app->pad_drawing.shape[i].style == SNX_PAD_STYLE_NOTE;
            assert(noted);
        }
        assert(SDL_DetachVirtualJoystick(stick));
        pads(app);
        assert(!app->pad.note[0]);
        free(file);
        free(folder);
    }
    /* ESC: back to the page's row, the pads closed */
    key(app, SDLK_ESCAPE);
    assert(app->form == GUI_FORM_GROUP && app->page < 0 && app->editor.row == page - app->first);
    assert(!app->pads_read && !SDL_WasInit(SDL_INIT_GAMEPAD));
    /* a map it cannot read is named, in the failure colour, and the page shows the labels' */
    assert(gui_settings_set(&app->settings, "gamepad_map", "south:Q"));
    assert(gui_settings_set(&app->settings, "gamepad_joycon_pair", "0")); /* and the Joy-Cons apart */
    key(app, SDLK_RETURN);
    assert(app->form == GUI_FORM_PAD && strstr(app->editor.note, "\"south:Q\" is not a control"));
    assert(!strcmp(SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS), "0"));
    assert(gui_settings_set(&app->settings, "gamepad_joycon_pair", ".ini"));
    assert(app->pad.own.bit[SNX_PAD_SOUTH] == SNX_PAD_UNSET);
    render(app);
    {
        char start[21]; /* in the tips */
        snprintf(start, sizeof start, "%s", app->editor.note);
        shows(app, start, rgb(app->theme.failure));
    }
    key(app, SDLK_TAB); /* the next screen, as the foot says: the Menu, the pads closed */
    assert(!app->editing && app->menu.view == GUI_LOAD_NONE && !app->pads_read && !SDL_WasInit(SDL_INIT_GAMEPAD));
    /* the games it starts read the mappings file the page names, once it is there; one the player named stays */
    {
        char *folder = snx_dirname(app->settings_file), *db = snx_path_join(folder, "gamecontrollerdb.txt");
        const char* named;
        SDL_unsetenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE");
        pad_mappings_share(app);
        assert(!SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE"));
        assert(!snx_write_file(db, "", 0));
        pad_mappings_share(app);
        named = SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE");
        assert(named && !strcmp(named, db));
        SDL_setenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE", "/elsewhere/mine.txt", 1);
        pad_mappings_share(app);
        assert(!strcmp(SDL_getenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE"), "/elsewhere/mine.txt"));
        SDL_unsetenv_unsafe("SDL_GAMECONTROLLERCONFIG_FILE");
        assert(!snx_remove_tree(db));
        free(db);
        free(folder);
    }
}
/* a running step's line ends in its progress as the theme says, a done one in OK */
static void step_line(void) {
    GuiStep s;
    char runs[3][112];
    size_t lengths[3];
    memset(&s, 0, sizeof s);
    snprintf(s.label, sizeof s.label, "UNPACK");
    snprintf(s.detail, sizeof s.detail, "Game.xci");
    s.value = 1024;
    s.total = 4096;
    s.megabytes = 1;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!strcmp(runs[0], "UNPACK:") && !strcmp(runs[1], " GAME.XCI") && !strcmp(runs[2], " 25%"));
    step_runs(&s, GUI_PROGRESS_AMOUNT, 0, runs, lengths);
    assert(!strcmp(runs[2], " 1.0 / 4.0 GB") && lengths[2] == 13);
    s.state = GUI_STEP_DONE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!strcmp(runs[2], "...OK"));
    s.total = 0;
    s.state = GUI_STEP_ACTIVE;
    step_runs(&s, GUI_PROGRESS_PERCENT, 0, runs, lengths);
    assert(!runs[2][0] && !lengths[2]);
}
static int SDLCALL hang(void* unused) {
    (void)unused;
    for (;;) SDL_Delay(100);
}
/* the keys screen's outcome: installed or not, said on it, then the menu once it has shown long enough, not before */
static void keys_outcome(App* app, bool ok) {
    Uint64 at = app->keys_done, shown = ok ? GUI_KEYS_OK_MS : GUI_KEYS_FAILED_MS;
    assert(app->menu.view == GUI_LOAD_KEYS && at && app->keys_ok == ok);
    app->clock = at + shown - 1;
    pump(app);
    render(app);
    shows(app, ok ? "Installed" : "Not installed", rgb(ok ? app->theme.focus : app->theme.failure));
    shows(app, "Back to the menu in 1 s", rgb(app->theme.soft));
    assert(app->menu.view == GUI_LOAD_KEYS);
    app->clock = at + shown;
    pump(app);
    assert(app->menu.view == GUI_LOAD_NONE && !app->keys_done);
    app->clock = 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--settings")) { /* as the runtime lists its settings */
        fputs("Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\n"
              "Graphics\tresolution\t1\tfloat\t0.25\t4\tInternal 3D resolution\t\t\n"
              "On-screen controls\tcontrols\t0\tint\t0\t1\tThe on-screen controls\tShow controls\t0=Off|1=On\n"
              "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\tA control drawn\tA\t0=Hidden|1=Shown\n"
              "On-screen controls/Buttons\tcontrols_b_shown\t\tint\t0\t1\tA control drawn\tB\t0=Hidden|1=Shown\n",
              stdout);
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "hang in join")) {
        SDL_Thread* thread = SDL_CreateThread(hang, "hung worker", NULL);
        assert(thread && !snx_write_file(argv[1], "ready", 5));
        SDL_WaitThread(thread, NULL); /* Never returns. ESC must kill, not request a graceful exit. */
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--fullscreen=1") && !strcmp(argv[2], "--controls=0")) {
        puts("prepared game stdout");
        fputs("prepared game stderr\n", stderr);
        return 0;
    }
    if (argc > 1 && strstr(argv[0], "scarletnx-3ds")) {
        /* in the menu's data folder, as the Switch driver */
        assert(argc == 4 && snx_is_file(argv[1]) && strstr(argv[1], "3ds input.CIA") && !strcmp(argv[2], "--repo") &&
               argv[3][0]);
        fputs("CIA execution is not implemented (stage 2)\n", stderr);
        return 3;
    }
    if (argc > 1 && strstr(argv[1], "picked folders")) { /* Settings > App > Folders' games and extractions */
        int i = 2;
        bool nro = strstr(argv[1], ".nro") != NULL; /* runs without an extraction */
        while (i < argc && strcmp(argv[i], "--repo")) ++i;
        assert(i + (nro ? 4 : 6) == argc && argv[i + 1][0] && !strcmp(argv[i + 2], "--outdir") &&
               strstr(argv[i + 3], "picked games"));
        assert(nro || (!strcmp(argv[i + 4], "--out") && strstr(argv[i + 5], "picked work") &&
                       !strcmp(argv[i + 5] + strlen(argv[i + 5]) - 15, "/picked folders")));
        puts("Recompiling picked folders\nScarletNX - picked folders");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-r") && strstr(argv[2], "room dump")) { /* R's, its room asked: not played */
        int i = 3;
        if (SNX_GUI_HAS_GCCJIT) assert(!strcmp(argv[i++], "--backend=gcc"));
        assert(i + 2 == argc && !strcmp(argv[i], "--repo") && argv[i + 1][0]);
        puts("Recompiling room dump");
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "room dump")) { /* asked about its room first */
        int i = 2;
        char* folder = snx_dirname(argv[1]);
        while (i < argc && strcmp(argv[i], "--repo")) ++i;
        /* a dump from a folder of its own, the folders their defaults: the data folder alone, no --out or --outdir, so
           the extraction goes to the data folder's work/ (on Android in the app's own storage), never beside it */
        assert(i + 2 == argc && !snx_path_within(argv[i + 1], folder) && !snx_path_within(folder, argv[i + 1]));
        free(folder);
        puts("Recompiling room dump\nScarletNX - room dump");
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "option flags")) {
        int i = 2;
        assert(argc == 8 + SNX_GUI_HAS_GCCJIT && !strcmp(argv[i++], "--fullscreen=1") &&
               !strcmp(argv[i++], "--controls=0"));
        if (SNX_GUI_HAS_GCCJIT) assert(!strcmp(argv[i++], "--backend=gcc"));
        assert(!strcmp(argv[i++], "-S") && !strcmp(argv[i++], "-s"));
        assert(!strcmp(argv[i++], "--repo") && argv[i++][0] && i == argc);
        puts("Recompiling option flags\nScarletNX - option flags"); /* the runner's banner: now loading */
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-r") && strstr(argv[2], "folder game")) { /* R on it: recompiled, not played */
        int i = 3;
        assert(strstr(argv[0], "runtime") && snx_is_file(argv[2]));
        if (SNX_GUI_HAS_GCCJIT) assert(!strcmp(argv[i++], "--backend=gcc"));
        assert(i + 2 == argc && !strcmp(argv[i], "--repo") && argv[i + 1][0]);
        puts("Recompiling folder game (1 modules)\nBuilt folder game");
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "folder game")) { /* a game in a folder of its own, through the menu's driver */
        assert(strstr(argv[0], "runtime") && argc == 6 && snx_is_file(argv[1]) && !strcmp(argv[2], "--fullscreen=1") &&
               !strcmp(argv[3], "--controls=0") && !strcmp(argv[4], "--repo") && argv[5][0]);
        puts("Recompiling folder game (1 modules)\nBuilt folder game\nScarletNX - folder game");
        return 0;
    }
    if (argc > 1) {
        assert(argc == 2 || argc == 6);
        assert(snx_is_file(argv[1]));
        if (strstr(argv[1], "Game00")) {
            assert(argc == 2 && strstr(argv[0], "gnuabi"));
            puts("Linux file selected from games/");
            return 0;
        }
        assert(strstr(argv[1], "game with spaces"));
        bool nx = strstr(argv[0], "runtime") != NULL; /* the program the menu is part of */
        if (nx)
            assert(argc == 6 && !strcmp(argv[2], "--fullscreen=1") && !strcmp(argv[3], "--controls=0") &&
                   !strcmp(argv[4], "--repo") && argv[5][0]);
        else
            assert(argc == 2);
        size_t size;
        unsigned char* data = snx_read_file(argv[1], &size);
        assert(data && size == 1);
        ++data[0];
        assert(!snx_write_file(argv[1], data, 1));
        free(data);
        puts("driver stdout");
        fputs("driver stderr\n", stderr);
        return nx ? 0 : 7; /* Both successful and failed drivers need ESC acknowledgement. */
    }
    App app;
    /* the menu's data folder from the test's own environment, never the user's */
    SDL_setenv_unsafe("XDG_DATA_HOME", "/nonexistent/snx-gui-test", 1);
    SDL_setenv_unsafe("XDG_CONFIG_HOME", "/nonexistent/snx-gui-test/config", 1);
    SDL_setenv_unsafe("XDG_STATE_HOME", "/nonexistent/snx-gui-test/state", 1);
    SDL_setenv_unsafe("XDG_CACHE_HOME", "/nonexistent/snx-gui-test/cache", 1);
    /* the menu runs Switch inputs with the runtime it is part of, the other drivers from beside it */
    assert(initialize(&app, "/nonexistent/snx-gui-test/bin/scarletnx"));
    assert(!strcmp(app.self, "/nonexistent/snx-gui-test/bin/scarletnx"));
    assert(!strcmp(app.drivers, "/nonexistent/snx-gui-test/bin"));
    assert(!strcmp(app.theme.id, "yorha"));    /* the menu opens in YoRHa everywhere */
    app.theme = *gui_theme_builtin("default"); /* the checks below read Default's colours */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(SDL_PLATFORM_ANDROID)
    /* no checkout and no games/ beside the test: games/ and themes/ come from the user's data folder */
    assert(!strcmp(app.data_dir, "/nonexistent/snx-gui-test/scarletnx"));
    assert(!strcmp(app.games_dir, "/nonexistent/snx-gui-test/scarletnx/games"));
    assert(!strcmp(app.themes_dirs[0], "/nonexistent/snx-gui-test/scarletnx/themes"));
    /* and the keys go in it, where the driver reads them first, not in ~/.switch */
    assert(!strcmp(app.keys_path, "/nonexistent/snx-gui-test/scarletnx/prod.keys"));
#endif
    /* the themes that come with the GUI: beside it in a build tree, else in share/scarletnx beside its bin/ */
    {
        char *root = snx_temp_dir(NULL), *bin = snx_path_join(root, "bin"), *beside = snx_path_join(bin, "themes");
        char *shared = snx_path_join(bin, "../share/scarletnx/themes"), *found;
        assert(root && !snx_mkdirs(bin));
        found = shipped_themes(bin);
        assert(!strcmp(found, shared));
        free(found);
        assert(!snx_mkdirs(beside));
        found = shipped_themes(bin);
        assert(!strcmp(found, beside));
        free(found);
        assert(!snx_remove_tree(root));
        free(shared);
        free(beside);
        free(bin);
        free(root);
    }
    /* the window says which build it is: commit, compiler, platform */
    assert(!strcmp(SDL_GetWindowTitle(app.window), snx_build_info()) && !strncmp(snx_build_info(), "ScarletNX ", 10));
    /* the boot's splash: a drawing of Wimsy's in the middle of the default theme's black page, a third of the window's
       shorter side, until its time, drawn again when the window changes; a key ends it and does nothing else. Every
       drawing built in shows. A theme's own page shows in its chance of boots, the default's in the others */
    {
        SDL_Rect view;
        SDL_Color black = {0, 0, 0, 255}, wimsy = {17, 206, 161, 255}; /* wimsy.svg's middle */
        SnxSplashDrawing* read;
        int i, w, h;
        assert(SDL_GetWindowSize(app.window, &w, &h));
        for (i = 0; i < snx_splash_svg_count; ++i) {
            splash_with(&app, gui_theme_builtin("default"), &snx_splash_svgs[i]);
            app.clock = app.splash_until - 1;
            render(&app);
            assert(app.splash && app.splash_svg == &snx_splash_svgs[i]);
            SDL_GetRenderViewport(app.renderer, &view);
            assert(app.splash_side == SDL_min(view.w, view.h) / 3);
            splash_drawn(&app, &view, &snx_splash_svgs[i], app.splash_side, black);
            if (!i) assert(near_colour(pixel(&app, view.w / 2.0f, view.h / 2.0f), wimsy, 8));
            if (i + 1 < snx_splash_svg_count) {
                SDL_DestroyTexture(app.splash);
                app.splash = NULL;
                snx_splash_close(app.splash_drawing);
                app.splash_drawing = NULL;
            }
        }
        read = app.splash_drawing; /* read once: a new size only draws it again */
        assert(read && SDL_SetWindowSize(app.window, 900, 600));
        SDL_SyncWindow(app.window);
        render(&app);
        SDL_GetRenderViewport(app.renderer, &view);
        assert(view.w == 900 && view.h == 600 && app.splash && app.splash_side == 200 && app.splash_drawing == read);
        splash_drawn(&app, &view, app.splash_svg, 200, black);
        key(&app, SDLK_RETURN);
        assert(!app.splash_until && app.menu.selected == GUI_SWITCH && !app.menu.activated);
        render(&app);
        assert(!app.splash && !app.splash_drawing);
        app.clock = 0;
        assert(SDL_SetWindowSize(app.window, w, h));
        SDL_SyncWindow(app.window);
        render(&app);
        assert(gui_splash_theme(gui_theme_builtin("yorha"), 24) == gui_theme_builtin("yorha"));
        assert(gui_splash_theme(gui_theme_builtin("yorha"), 25) == gui_theme_builtin("default"));
    }
    char* temporary = snx_temp_dir(NULL);
    char* self = snx_executable(argv[0]);
    char* input = snx_path_join(temporary, "game with spaces;literal.nro");
    assert(!snx_write_file(input, "0", 1));
    const char* names[] = {"runtime", "scarletnx-gnuabi", "scarletnx-3ds"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
#ifdef _WIN32
        char* filename = snx_format("%s.exe", names[i]);
#else
        char* filename = snx_strdup(names[i]);
#endif
        char* path = snx_path_join(temporary, filename);
        assert(!snx_copy_file(self, path) && !snx_make_executable(path));
        free(filename);
        free(path);
    }
    free(app.drivers);
    app.drivers = snx_strdup(temporary);
    free(app.self);
#ifdef _WIN32
    app.self = snx_path_join(temporary, "runtime.exe");
#else
    app.self = snx_path_join(temporary, "runtime");
#endif
    free(app.games_dir);
    app.games_dir = snx_path_join(temporary, "games");
    app.settings_file = snx_path_join(temporary, "config/settings.ini"); /* the Settings' picks, kept */
    app.folders_file = snx_path_join(temporary, "config/folders.txt");   /* the player's game folders */
    app.app_file = snx_path_join(temporary, "config/app.ini");           /* Settings > App's picks */
    render(&app);
    shows(&app, "- Load Switch NSP/XCI/NRO", yellow);
    assert(!app.menu.activated);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_3DS);
    render(&app);
    shows(&app, "- Load 3DS CIA", yellow);
    shows(&app, "- Load Switch NSP/XCI/NRO", normal);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_KEYS);
    /* Never touch the real user's keys: exercise installation in a private home. */
    free(app.keys_path);
    char* switch_dir = snx_path_join(temporary, ".switch");
    app.keys_path = snx_path_join(switch_dir, "prod.keys");
    char* source_keys = snx_path_join(temporary, "prod.keys");
    assert(!snx_write_file(source_keys, "first keys", 10));
    /* no prod.keys in use: Setup device keys blinks red, focused or not, its label on the page colour; steady with
       blink_ms 0. A home without keys, as an installed build also takes ~/.switch's */
    {
        Uint64 on = 1000 * app.theme.blink_ms * 2, off = on + app.theme.blink_ms;
        char *bare = snx_path_join(temporary, "bare-home"),
             *saved_home = getenv("HOME") ? SDL_strdup(getenv("HOME")) : NULL;
        assert(!snx_mkdirs(bare));
        SDL_setenv_unsafe("HOME", bare, 1);
        assert(!keys_red(&app, off) && keys_red(&app, on));
        shows(&app, "- Setup device keys", rgb(app.theme.page));
        key(&app, SDLK_DOWN);
        assert(app.menu.selected != GUI_KEYS && keys_red(&app, on) && !keys_red(&app, off));
        key(&app, SDLK_UP);
        app.theme.blink_ms = 0;
        assert(keys_red(&app, on) && keys_red(&app, off));
        app.theme.blink_ms = gui_theme_builtin(app.theme.id)->blink_ms;
        app.clock = 0;
        if (saved_home)
            SDL_setenv_unsafe("HOME", saved_home, 1);
        else
            SDL_unsetenv_unsafe("HOME");
        SDL_free(saved_home);
        free(bare);
    }
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    render(&app);
    /* the drop line waits on the user: framed, in the middle of its box, blinking at blink_ms; steady, at the left
       or the right of the box as the theme says, or plain */
    {
        const char* waiting = "[ Pick or drop prod.keys ]";
        int width = (int)strlen(waiting) * 8 * app.scale;
        GuiPickBoxes k;
        GuiRect r;
        app.clock = 1000 * app.theme.blink_ms * 2;
        render(&app);
        pick_boxes(&app, &k);
        r = inner(&app, k.place.list);
        text_px_is(&app, r.x + (r.w - width) / 2, k.prompt, waiting, yellow, black);
        app.clock += app.theme.blink_ms;
        render(&app);
        hides(&app, "Pick or drop prod.keys", yellow);
        app.theme.prompt.blink = false; /* steady */
        render(&app);
        text_px_is(&app, r.x + (r.w - width) / 2, k.prompt, waiting, yellow, black);
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        text_px_is(&app, r.x + r.w - width, k.prompt, waiting, yellow, black);
        app.theme.prompt.align = GUI_ALIGN_LEFT;
        render(&app);
        text_px_is(&app, r.x, k.prompt, waiting, yellow, black);
        app.theme.prompt.lines[GUI_PROMPT_KEYS] = false; /* plain */
        render(&app);
        text_px_is(&app, r.x, k.prompt, "Pick or drop prod.keys    ", yellow, black);
        app.theme.prompt = gui_theme_builtin(app.theme.id)->prompt;
        app.clock = 0;
    }
    drop(&app, input, false); /* A game is not a key file and must not launch. */
    assert(app.menu.view == GUI_LOAD_KEYS && !app.menu.running && !snx_is_file(app.keys_path));
    drop(&app, "file://remote/prod.keys", true);
    assert(app.menu.view == GUI_LOAD_KEYS && !snx_is_file(app.keys_path));
    SDL_Event keys_event = {0};
    keys_event.type = SDL_EVENT_DROP_FILE;
    keys_event.drop.data = source_keys;
    event(&app, &keys_event);
    assert(!app.menu.running && strstr(app.message, "installed"));
    keys_outcome(&app, true);
    size_t keys_size;
    unsigned char* keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 10 && !memcmp(keys_data, "first keys", 10));
    free(keys_data);
    assert(!keys_red(&app, 1000 * app.theme.blink_ms * 2)); /* the keys in: as the other entries */
    app.clock = 0;
#ifndef _WIN32
    struct stat keys_stat;
    assert(!stat(app.keys_path, &keys_stat) && (keys_stat.st_mode & 0777) == 0600);
#endif
    key(&app, SDLK_RETURN);
    render(&app);
    shows(&app, "Keys already setup. This will overwrite", red);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    key(&app, SDLK_RETURN);
    assert(!snx_write_file(source_keys, "replacement", 11));
    char* quoted_keys = snx_format("\"%s\"", source_keys);
    keys_event.type = SDL_EVENT_DROP_TEXT;
    keys_event.drop.data = quoted_keys;
    event(&app, &keys_event);
    free(quoted_keys);
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    keys_outcome(&app, true);
    key(&app, SDLK_RETURN);
    drop(&app, app.keys_path, false); /* Dropping the destination itself is safe. */
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    keys_outcome(&app, true);
    key(&app, SDLK_RETURN);
    assert(!snx_remove_tree(source_keys));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_file_size(app.keys_path) == 11);
    /* Failed replacement must leave the destination untouched and permit retry. */
    assert(!snx_remove_tree(app.keys_path) && !snx_mkdirs(app.keys_path));
    assert(!snx_write_file(source_keys, "replacement", 11));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_is_dir(app.keys_path) && app.message[0]);
    assert(app.keys_done && !app.keys_ok); /* not installed: said, then the menu after a while */
    assert(!snx_remove_tree(app.keys_path));
    drop(&app, source_keys, false); /* a try that works says so in place of it */
    assert(snx_is_file(app.keys_path));
    keys_outcome(&app, true);
    /* a try that fails, left alone, goes back to the menu too, its line saying why */
    app.menu.view = GUI_LOAD_KEYS;
    drop(&app, "file://remote/prod.keys", true);
    keys_outcome(&app, false);
    assert(strstr(app.message, "local file path"));
    /* a document of an app's (Android's picker): read through it, as a path to it may not be readable or there is none
       (Downloads by number, #280), and installed when its text is a prod.keys', whatever its name; refused when it is
       not one or is too big. The picker's message for a document with no path stays on the other screens */
    {
        static const char keys_text[] = "; dumped\r\nheader_key = 00112233aabbccdd\r\n";
        char *document = snx_path_join(temporary, "msf 12"), *big = (char*)calloc(1, SNX_GUI_KEYS_MAX + 2);
        assert(big && !snx_write_file(document, keys_text, sizeof keys_text - 1));
        app.menu.view = GUI_LOAD_KEYS;
        install_keys_document(&app, document);
        keys_data = snx_read_file(app.keys_path, &keys_size);
        assert(app.menu.view == GUI_LOAD_KEYS && app.keys_ok && keys_data && keys_size == sizeof keys_text - 1 &&
               !memcmp(keys_data, keys_text, keys_size));
        free(keys_data);
        assert(!snx_write_file(document, "a game, not keys", 16));
        app.menu.view = GUI_LOAD_KEYS;
        install_keys_document(&app, document);
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "is not a prod.keys"));
        memset(big, 'a', SNX_GUI_KEYS_MAX + 1);
        memcpy(big, "header_key = 00\n", 16);
        assert(!snx_write_file(document, big, SNX_GUI_KEYS_MAX + 1));
        install_keys_document(&app, document);
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "could not be read as prod.keys"));
        assert(snx_file_size(app.keys_path) == sizeof keys_text - 1);
        picked(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "could not be read as prod.keys"));
        /* one of the storage's too, as its path may not be this app's to read */
        app.message[0] = 0;
        picked(&app, "content://com.android.externalstorage.documents/document/primary%3ADownload%2Fprod.keys");
        assert(app.menu.view == GUI_LOAD_KEYS && strstr(app.message, "could not be read as prod.keys"));
        app.menu.view = GUI_LOAD_DROP;
        picked(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
        assert(app.menu.view == GUI_LOAD_DROP && strstr(app.message, "from this computer's storage"));
        app.menu.view = GUI_LOAD_NONE;
        app.message[0] = 0;
        free(big);
        free(document);
    }
    free(switch_dir);
    free(source_keys);
#ifdef SNX_XDG
    /* keys another tool left in ~/.switch, which the driver reads after the data folder's, are the installed build's
       keys in use until some are installed here */
    {
        char *home = snx_path_join(temporary, "home"), *saved_home = getenv("HOME") ? SDL_strdup(getenv("HOME")) : NULL;
        char *dir = snx_path_join(home, ".switch"), *found = snx_path_join(dir, "prod.keys"), *in_use;
        assert(!snx_mkdirs(dir) && !snx_write_file(found, "home keys", 9));
        SDL_setenv_unsafe("HOME", home, 1);
        in_use = keys_in_use(&app);
        assert(in_use && !strcmp(in_use, app.keys_path)); /* those installed above come first */
        free(in_use);
        assert(!snx_remove_tree(app.keys_path));
        in_use = keys_in_use(&app);
        assert(in_use && !strcmp(in_use, found));
        free(in_use);
        key(&app, SDLK_RETURN);
        assert(app.menu.view == GUI_LOAD_KEYS);
        render(&app);
        key(&app, SDLK_ESCAPE);
        if (saved_home)
            SDL_setenv_unsafe("HOME", saved_home, 1);
        else
            SDL_unsetenv_unsafe("HOME");
        SDL_free(saved_home);
        free(found);
        free(dir);
        free(home);
    }
#endif
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_SETTINGS);
    render(&app);
    shows(&app, "The game's .ini settings,", normal); /* what it is about, in the tips and beside the list */
    mouse_px(&app, 2, 2, true);                       /* Blank space must not activate the current selection. */
    assert(!app.editing);
    /* Settings: the launcher's own, then each setting the runtime lists, a row each with its help above; what the
       menu's items used to set is in them */
    key(&app, SDLK_SPACE);
    assert(app.editing && app.editor.object && app.editor.count == GUI_SETTINGS_OWN + 3);
    /* and the disk: what ScarletNX's folders take, counted on a thread, and what is left */
    {
        char line[256], used[32], left[32];
        uint64_t total = 0;
        Uint64 until = SDL_GetTicks() + 5000;
        char* big = snx_path_join(app.games_dir, "big.bin");
        char* bytes = (char*)calloc(1, 3 << 20);
        /* the test's own folders alone: the user's data folder (%APPDATA%\scarletnx on Windows) may be another
           test's at the same time */
        char *data_dir = app.data_dir, *themes = app.themes_dirs[0];
        app.data_dir = snx_path_join(temporary, "data");
        app.themes_dirs[0] = snx_path_join(app.data_dir, "themes");
        assert(bytes && !snx_mkdirs(app.games_dir) && !snx_write_file(big, bytes, 3 << 20));
        key(&app, SDLK_ESCAPE); /* counted again as the Settings open */
        key(&app, SDLK_SPACE);
        assert(app.space && app.free_bytes);
        render(&app);
        while (!__atomic_load_n(&app.space->done, __ATOMIC_ACQUIRE) && SDL_GetTicks() < until) SDL_Delay(5);
        for (int i = 0; i < GUI_SPACE_DIRS; ++i) total += gui_tree_size(app.space->dirs[i], &app.space->stop);
        assert(app.space->done && app.space->used == total && same_path(app.space->dirs[0], app.games_dir));
        gui_size_text(used, sizeof used, total);
        gui_size_text(left, sizeof left, app.free_bytes);
        /* in the Status box, after the RAM and the CPU */
        snprintf(line, sizeof line, "Disk: %s, %s free", used, left);
        key(&app, SDLK_ESCAPE);
        render(&app);
        shows(&app, line, rgb(app.theme.soft));
        shows(&app, "GUI + children: ", rgb(app.theme.soft));
        assert(total == 3 << 20 && !strncmp(line, "Disk: 3 MB, ", 12));
        key(&app, SDLK_SPACE);
        assert(!snx_remove_tree(app.games_dir)); /* the browser's checks below start with no games/ */
        free(app.themes_dirs[0]);
        free(app.data_dir);
        app.themes_dirs[0] = themes;
        app.data_dir = data_dir;
        free(bytes);
        free(big);
    }
    /* the groups first: App, then a group per section the runtime lists, and the reset of them all; beside them the
       focused group's rows, in the tips what it holds */
    render(&app);
    assert(app.form == GUI_FORM_SETTINGS && app.groups == 5 && gui_editor_rows(&app.editor) == 6);
    shows(&app, "SETTINGS", normal);
    shows(&app, "The theme, the menu's", normal);
    shows(&app, "- App", yellow);
    shows(&app, "- Launch", normal);
    shows(&app, "- Display", normal);
    shows(&app, "- Graphics", normal);
    shows(&app, "- On-screen controls", normal);
    shows(&app, "- Reset all to the defaults", normal);
    hides(&app, "\x1e", rgb(app.theme.soft)); /* every row shows: no arrows */
    hides(&app, "\x1f", rgb(app.theme.soft));
    key(&app, SDLK_DOWN);
    render(&app);
    shows(&app, "libgccjit, gdbstub", normal); /* a group's help names its settings */
    key(&app, SDLK_RIGHT);                     /* a group opens with Enter only */
    assert(app.form == GUI_FORM_SETTINGS);
    key(&app, SDLK_RETURN);
    /* a group's settings on the right, a row each with its help in the tips; the group named over them */
    assert(app.form == GUI_FORM_GROUP && app.group == 1 && app.editor.count == GUI_SETTINGS_OWN);
    render(&app);
    shows(&app, "Launch", normal);
    shows(&app, SNX_GUI_HAS_GCCJIT ? "libgccjit will be" : "Program was not", normal); /* in the tips */
    shows(&app, "- libgccjit off", yellow);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "libgccjit", SNX_GUI_HAS_GCCJIT ? "on" : NULL));
    key(&app, SDLK_LEFT);
    assert(!gui_settings_value(&app.settings, "libgccjit"));
    point(&app, row_box(&app, 1, true), true); /* a click focuses a row, one on the focused row changes it */
    assert(app.editor.row == 1 && !gui_settings_value(&app.settings, "gdbstub"));
    render(&app);
    shows(&app, "Enables gdbstub", normal);
    point(&app, row_box(&app, 1, true), true);
    assert(setting_is(&app, "gdbstub", "on"));
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "gdbstub"));
    key(&app, SDLK_RETURN);
    assert(setting_is(&app, "gdbstub", "on"));
    /* ESC goes back to the groups, on the one it came from */
    key(&app, SDLK_ESCAPE);
    assert(app.editing && app.form == GUI_FORM_SETTINGS && app.editor.row == 1);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_GROUP && app.group == 2);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "fullscreen", "1"));
    render(&app);
    shows(&app, "Display", normal);
    shows(&app, "- fullscreen 1", yellow); /* the launcher's choice was 0 */
    /* Del puts a setting back as it started */
    key(&app, SDLK_DELETE);
    assert(setting_is(&app, "fullscreen", "0"));
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    /* a number typed, and nothing typed for the .ini's again */
    render(&app);
    shows(&app, "- resolution 1 [default]", yellow); /* not picked: the default */
    key(&app, SDLK_RETURN);
    typed(&app, "0.5");
    key(&app, SDLK_RETURN);
    assert(setting_is(&app, "resolution", "0.5"));
    key(&app, SDLK_RETURN);
    for (int i = 0; i < 3; ++i) key(&app, SDLK_BACKSPACE);
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "resolution"));
    key(&app, SDLK_ESCAPE);
    /* the game's menu's settings, named and worded as it shows them; a page's row opens the page, All first */
    {
        char line[64];
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        render(&app);
        shows(&app, "On-screen controls", normal);
        snprintf(line, sizeof line, "- %-13s %s", "Show controls", "Off"); /* the launcher's choice, 0 */
        shows(&app, line, yellow);
        snprintf(line, sizeof line, "- %-13s %s", "Buttons", ">");
        shows(&app, line, normal);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RIGHT); /* a page opens with Enter only */
        assert(app.page < 0);
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_GROUP && app.page >= 0 && app.editor.count == 3);
        render(&app);
        shows(&app, "Buttons", normal); /* the page open, over its rows */
        snprintf(line, sizeof line, "- %-8s %s", "All", "[default]");
        shows(&app, line, yellow);
        snprintf(line, sizeof line, "- %-8s %s", "B", "[default]"); /* an empty default */
        shows(&app, line, normal);
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "controls_a_shown", "0") && setting_is(&app, "controls_b_shown", "0"));
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RIGHT);
        assert(setting_is(&app, "controls_a_shown", "1") && setting_is(&app, "controls_b_shown", "0"));
        render(&app);
        snprintf(line, sizeof line, "- %-8s %s", "All", "Mixed");
        shows(&app, line, normal);
        snprintf(line, sizeof line, "- %-8s %s", "A", "Shown");
        shows(&app, line, yellow);
        /* ESC comes back to the page's row, Del on it puts its rows back */
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_GROUP && app.page < 0 && app.editor.row == 1);
        key(&app, SDLK_DELETE);
        assert(!gui_settings_value(&app.settings, "controls_a_shown") &&
               !gui_settings_value(&app.settings, "controls_b_shown"));
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 4);
    }
    /* the groups' last row puts every setting back as it started */
    app.editor.row = gui_editor_rows(&app.editor) - 1;
    key(&app, SDLK_RETURN);
    assert(!gui_settings_value(&app.settings, "gdbstub") && setting_is(&app, "fullscreen", "0"));
    app.editor.row = 2; /* fullscreen 1 again, for the runs below */
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RIGHT);
    assert(setting_is(&app, "fullscreen", "1"));
    key(&app, SDLK_ESCAPE);
    /* App: the theme and the letters, either way round, and the theme editor, which comes back here */
    app.editor.row = 0;
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME && GUI_APP_ROWS == 6);
    {
        char first[64], line[96];
        snprintf(first, sizeof first, "%s", app.theme.name);
        render(&app);
        shows(&app, "- App", yellow); /* the group open, as focused while its rows have the focus */
        snprintf(line, sizeof line, "- %-18s %s", "Theme", first);
        shows(&app, line, yellow);
        shows(&app, "- Text size          Auto", normal);
        shows(&app, "- Theme editor", normal);
        shows(&app, "- Folders            Defaults", normal);
        shows(&app, "- Clean old settings", normal);
        hides(&app, "- Layout", normal);
        key(&app, SDLK_RIGHT);
        assert(strcmp(app.theme.name, first) && !strcmp(app.app_form.theme, app.theme.name));
        key(&app, SDLK_LEFT);
        assert(!strcmp(app.theme.name, first));
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.text_size == 1 && !strcmp(app.app_form.text, "1x"));
    key(&app, SDLK_LEFT);
    assert(!app.menu.text_size && !strcmp(app.app_form.text, "Auto"));
    key(&app, SDLK_LEFT);
    assert(app.menu.text_size == GUI_TEXT_MAX);
    key(&app, SDLK_RIGHT);
    assert(!app.menu.text_size && app.editor.row == GUI_APP_TEXT);
    /* the App's picks kept at once in app.ini, which the next start reads back */
    {
        size_t size = 0;
        char *kept, expected[512], id[64], first_id[64];
        const char* broken = "[app]\ntheme = gone\ntext_size = 9\nlayout = boxes\ncolour = red\ntext_size = 2x\n"
                             "[other]\nlayout = panels\n";
        char* blocker = snx_path_join(temporary, "not a folder");
        char* kept_file = app.app_file;
        snprintf(first_id, sizeof first_id, "%s", app.theme.id);
        key(&app, SDLK_RIGHT); /* 1x */
        key(&app, SDLK_UP);
        key(&app, SDLK_RIGHT); /* the next theme */
        snprintf(id, sizeof id, "%s", app.theme.id);
        assert(strcmp(id, first_id));
        snprintf(expected, sizeof expected,
                 "; Settings > App in the launcher, as they were left\n[app]\ntheme = %s\ntext_size = 1\n", id);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && size == strlen(expected) && !memcmp(kept, expected, size));
        free(kept);
        /* a start with them: the theme and the letters as they were left, and nothing to say */
        gui_theme_load(&app.theme, (const char* const*)app.themes_dirs, 2, first_id);
        app.menu.text_size = 0;
        app.message[0] = 0;
        app_load(&app);
        assert(!strcmp(app.theme.id, id) && app.menu.text_size == 1 && app.scale == 1 && !app.message[0]);
        /* back as they were, kept so */
        key(&app, SDLK_LEFT);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_LEFT);
        assert(!strcmp(app.theme.id, first_id) && !app.menu.text_size && !app.editor.note[0]);
        snprintf(expected, sizeof expected,
                 "; Settings > App in the launcher, as they were left\n[app]\ntheme = %s\ntext_size = auto\n",
                 first_id);
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && size == strlen(expected) && !memcmp(kept, expected, size));
        free(kept);
        /* a file a hand broke: what it does not know is left, named, and the menu keeps what it has */
        assert(!snx_write_file(app.app_file, broken, strlen(broken)));
        app_load(&app);
        assert(!strcmp(app.theme.id, first_id) && !app.menu.text_size);
        snprintf(expected, sizeof expected,
                 "Not used from %s: theme = gone, text_size = 9, layout = boxes, colour = red, text_size = 2x, "
                 "[other] layout = panels",
                 app.app_file);
        assert(!strcmp(app.message, expected));
        app.message[0] = 0;
        /* a file that cannot be written: the pick holds for the session, and the note says it is not kept */
        assert(!snx_write_file(blocker, "file", 4));
        app.app_file = snx_path_join(blocker, "app.ini");
        key(&app, SDLK_RIGHT);
        assert(app.menu.text_size == 1 && !snx_is_file(app.app_file));
        snprintf(expected, sizeof expected, "Not kept for the next start: %s could not be written", app.app_file);
        assert(!strcmp(app.editor.note, expected));
        key(&app, SDLK_LEFT);
        assert(!app.menu.text_size);
        free(app.app_file);
        app.app_file = kept_file;
        remove(blocker);
        free(blocker);
        app.editor.note[0] = 0;
    }
    /* Clean old settings, Enter twice: the first says what it would do, another key in between starts again, the
       second does it and shows what it did over the screen, until a key closes it; then there is nothing left */
    {
        char *old_ini = snx_path_join(app.games_dir, "Old game.ini"), *kept, unread[64], expected[512];
        const char* game_ini =
            "[Other]\nlanguage=en\n\n[Deprecated]\n; renderer: Vulkan is the only one\nrenderer=gl\n";
        const char* gone = "[app]\ntheme = gone\n"; /* a theme taken away */
        SDL_Color title = rgb(app.theme.panels == GUI_PANELS_FILL ? app.theme.page : app.theme.ink);
        size_t size = 0;
        int row = app.editor.row, had_games = snx_is_dir(app.games_dir); /* the tests after this count on its absence */
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(old_ini, game_ini, strlen(game_ini)));
        kept = (char*)snx_read_file(app.settings_file, &size);
        {
            char* more = snx_format("%s[Display]\nrenderer=opengl\n", kept ? kept : "");
            assert(!snx_write_file(app.settings_file, more, strlen(more)));
            free(more);
        }
        free(kept);
        assert(settings_unread(&app, unread, sizeof unread) == 1 && !strcmp(unread, "renderer=opengl"));
        assert(!snx_write_file(app.app_file, gone, strlen(gone)));
        app.editor.row = GUI_APP_CLEAN;
        key(&app, SDLK_RETURN);
        assert(app.clean_armed && !app.cleaned_shown && snx_is_file(old_ini));
        assert(!strcmp(app.editor.note, "Enter again to clean: [Deprecated] in 1 .ini, 2 old keys"));
        render(&app);
        shows(&app, "- Clean old settings", yellow);
        shows(&app, "Enter again to clean:", yellow);
        key(&app, SDLK_UP);
        key(&app, SDLK_DOWN);
        assert(!app.clean_armed && app.editor.row == GUI_APP_CLEAN);
        key(&app, SDLK_RETURN);
        key(&app, SDLK_RETURN);
        assert(!app.clean_armed && app.cleaned_shown && app.cleaned.count == 3 && app.cleaned.done == 3);
        assert(!strcmp(app.editor.note, "Cleaned: 3 done, 0 not"));
        kept = (char*)snx_read_file(old_ini, &size);
        assert(kept && !strcmp(kept, "[Other]\nlanguage=en\n"));
        free(kept);
        assert(!settings_unread(&app, unread, sizeof unread) && setting_is(&app, "fullscreen", "1"));
        snprintf(expected, sizeof expected,
                 "+\t%s\t[Deprecated] taken out\n+\t%s\tTook out renderer=opengl\n+\t%s\tTook out theme = gone\n",
                 old_ini, app.settings_file, app.app_file);
        assert(!strcmp(app.cleaned.text, expected));
        kept = (char*)snx_read_file(app.app_file, &size); /* the picks as the menu has them */
        snprintf(expected, sizeof expected, "\ntheme = %s\n", app.theme.id);
        assert(kept && strstr(kept, expected) && !strstr(kept, "gone"));
        free(kept);
        render(&app);
        shows(&app, "Clean old settings: 3 done", title);
        shows(&app, "[Deprecated] taken out", rgb(app.theme.soft));
        shows(&app, "- OK", yellow);
        key(&app, SDLK_DOWN); /* closes it, and does nothing else */
        assert(!app.cleaned_shown && app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_CLEAN);
        render(&app);
        hides(&app, "Clean old settings: 3 done", title);
        key(&app, SDLK_RETURN);
        assert(!app.clean_armed && !strncmp(app.editor.note, "Nothing to clean", 16));
        /* a runtime that listed no settings: every key of settings.ini would look old, and none is taken out */
        {
            GuiSettings listed = app.settings;
            app.settings = (GuiSettings){0};
            assert(!gui_settings_read(&app.settings, "") && app.settings.count == GUI_SETTINGS_OWN);
            assert(!settings_unread(&app, unread, sizeof unread) && !unread[0]);
            gui_settings_free(&app.settings);
            app.settings = listed;
            assert(setting_is(&app, "fullscreen", "1"));
        }
        remove(old_ini);
        if (!had_games) assert(!snx_remove_tree(app.games_dir));
        free(old_ini);
        app.editor.row = row;
        app.editor.note[0] = 0;
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT); /* the editor opens with Enter only */
    assert(app.form == GUI_FORM_APP);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_THEME && app.editing);
    key(&app, SDLK_ESCAPE);
    assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_EDITOR);
    /* the game folders: games/ first, always; one dropped is added and kept, once; the picker answers none here
       (SDL_FILE_DIALOG_DRIVER=none); Del or Enter twice takes one off, the folder staying */
    key(&app, SDLK_DOWN);
    render(&app);
    shows(&app, "- Game folders       games/", yellow);
    key(&app, SDLK_RETURN);
    assert(app.form == GUI_FORM_FOLDERS && gui_editor_rows(&app.editor) == 2 && app.editor.row == 0);
    /* Back over the rows, for a click or a finger: it does what ESC does, back on the App's row; the groups' list has
       none */
    {
        GuiFormBoxes f;
        render(&app);
        shows(&app, "- Back [ESC]", normal);
        form_boxes(&app, &f);
        assert(f.back.count == 1 &&
               list_box(&f.right, 0).y - (list_box(&f.back, 0).y + list_box(&f.back, 0).h) > 14 * app.scale);
        /* a row with no value has no padding to the widest label: short, it stays whole while a long one slides */
        {
            Uint32 kept = app.theme.scroll_ms;
            app.theme.scroll_ms = 100; /* the default theme slides nothing */
            for (app.clock = 1; gui_slide(1000, app.clock, app.theme.scroll_ms) != 3;) app.clock += 100;
            render(&app);
            shows(&app, "- Add a folder", normal);
            app.theme.scroll_ms = kept;
            app.clock = 0;
        }
        point(&app, list_box(&f.back, 0), true);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_FOLDERS);
        render(&app);
        shows(&app, "- Back [ESC]", normal);
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_SETTINGS);
        render(&app);
        hides(&app, "- Back [ESC]", normal);
        key(&app, SDLK_RETURN);
        app.editor.row = GUI_APP_FOLDERS;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_FOLDERS && app.editor.row == 0);
    }
    {
        char* more = snx_path_join(temporary, "more games");
        char* file = snx_path_join(more, "Zed");
        char* kept_as;
        char *kept, line[160];
        size_t size;
        SDL_Event e = {0};
        assert(!snx_mkdirs(more) && !snx_write_file(file, "fixture", 7));
        kept_as = snx_absolute(more); /* as the system writes the folder once it is there: backslashes on Windows */
        render(&app);
        shows(&app, "Game folders", normal);
        shows(&app, "The data folder's", normal);
        snprintf(line, sizeof line, "- %.20s", app.games_dir);
        shows(&app, line, yellow);
        shows(&app, "- Add a folder", normal);
        assert(!strcmp(app.app_form.folder[0], "not found")); /* games/ was taken away above */
        key(&app, SDLK_DELETE);
        assert(!strcmp(app.editor.note, "games/ is always listed"));
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = file;
        event(&app, &e);
        assert(app.folder_count == 0 && !strncmp(app.editor.note, "Not a folder: ", 14));
        e.drop.data = more;
        event(&app, &e);
        if (app.folder_count != 1 || !same_path(app.folders[0], kept_as) || strcmp(app.editor.note, "Added"))
            fprintf(stderr, "folders: %d, first \"%s\", expected \"%s\", note \"%s\"\n", app.folder_count,
                    app.folder_count ? app.folders[0] : "", kept_as, app.editor.note);
        assert(app.folder_count == 1 && same_path(app.folders[0], kept_as) && !strcmp(app.editor.note, "Added"));
        assert(app.form == GUI_FORM_FOLDERS && app.editor.row == 1 && gui_editor_rows(&app.editor) == 3);
        kept = (char*)snx_read_file(app.folders_file, &size);
        snprintf(line, sizeof line, "%s\n", kept_as);
        assert(kept && !strcmp(kept, line));
        free(kept);
        event(&app, &e);
        assert(app.folder_count == 1 && !strncmp(app.editor.note, "Already listed: ", 16));
        render(&app);
        snprintf(line, sizeof line, "- %.20s", kept_as);
        shows(&app, line, yellow);
        app.editor.row = 2;
        key(&app, SDLK_RETURN);
        {
            SDL_Event answer;
            Uint64 until = SDL_GetTicks() + 2000;
            while (!app.editor.note[0] && SDL_GetTicks() < until) /* its answer comes on the picker's thread */
                while (SDL_PollEvent(&answer)) event(&app, &answer);
        }
        assert(app.folder_count == 1 && (!strcmp(app.editor.note, "No folder was picked.") ||
                                         !strncmp(app.editor.note, "The folder picker could not open: ", 34)));
        app.editor.row = 1;
        key(&app, SDLK_RETURN);
        assert(app.folder_count == 1 && !strcmp(app.editor.note, "Enter again takes it off the list"));
        key(&app, SDLK_RETURN);
        assert(!app.folder_count && !strcmp(app.editor.note, "Taken off the list; the folder stays"));
        assert(snx_is_dir(more) && (kept = (char*)snx_read_file(app.folders_file, &size)) && !size);
        free(kept);
        /* a folder with folders in it asks whether their games are listed too: Cancel adds nothing, Only this folder
           its own games, With subfolders theirs as well, down GUI_FOLDER_DEPTH levels, a hidden one left alone; kept
           with GUI_FOLDER_DEEP after its path, and its row says so */
        {
#ifdef _WIN32
#define GAME(name) name ".exe" /* a game there is an .exe, listed by its file's name */
#else
#define GAME(name) name
#endif
            static const char* const files[] = {GAME("Top"), "a/" GAME("One"), "a/b/c/d/" GAME("Four"),
                                                "a/b/c/d/e/" GAME("Five"), ".hid/" GAME("Hidden")};
            char *deep = snx_path_join(temporary, "deep games"), *deep_as;
            GuiGames listed = {0};
            char said[sizeof app.message];
            size_t f, g;
            int found = 0;
            for (f = 0; f < sizeof files / sizeof files[0]; ++f) {
                char *path = snx_path_join(deep, files[f]), *dir = snx_dirname(path);
                assert(!snx_mkdirs(dir) && !snx_write_file(path, "fixture", 7) && !snx_make_executable(path));
                free(dir);
                free(path);
            }
            deep_as = snx_absolute(deep);
            e.drop.data = deep;
            event(&app, &e);
            assert(app.folder_asked && same_path(app.folder_asked, deep_as) && app.confirm.shown && !app.folder_count);
            render(&app);
            shows(&app, "List the games in its subfolders too?", rgb(app.theme.warning));
            key(&app, SDLK_ESCAPE);
            assert(!app.folder_asked && !app.confirm.shown && !app.folder_count &&
                   !strcmp(app.editor.note, "Not added"));
            event(&app, &e);
            key(&app, SDLK_RETURN); /* Only this folder */
            assert(app.folder_count == 1 && !app.folders_deep[0] && !strcmp(app.editor.note, "Added"));
            games_scan(&app, &listed, said, sizeof said);
            for (g = 0; g < listed.count; ++g)
                found |= !strcmp(listed.names[g], GAME("Top")) | !strcmp(listed.names[g], GAME("One")) << 1;
            assert(found == 1);
            gui_games_free(&listed);
            folder_remove(&app, 0);
            event(&app, &e);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN); /* With subfolders */
            assert(app.folder_count == 1 && app.folders_deep[0] &&
                   !strcmp(app.editor.note, "Added, with its subfolders"));
            kept = (char*)snx_read_file(app.folders_file, &size);
            snprintf(line, sizeof line, "%s" GUI_FOLDER_DEEP "\n", deep_as);
            assert(kept && !strcmp(kept, line));
            free(kept);
            assert(!strcmp(app.app_form.folder[1], "+ subfolders"));
            games_scan(&app, &listed, said, sizeof said);
            found = 0;
            for (g = 0; g < listed.count; ++g) {
                found |= !strcmp(listed.names[g], GAME("Top")) | !strcmp(listed.names[g], GAME("One")) << 1;
                found |= !strcmp(listed.names[g], GAME("Four")) << 2 | !strcmp(listed.names[g], GAME("Five")) << 3;
                found |= !strcmp(listed.names[g], GAME("Hidden")) << 4;
            }
            assert(found == 7); /* Top, One and Four; not Five, past four levels, nor Hidden */
            gui_games_free(&listed);
            /* read back as it was kept; a change below it changes the stamp */
            {
                uint64_t stamp = games_stamp(&app);
                char* more_deep = snx_path_join(deep, "a/b/Two");
                assert(!snx_write_file(more_deep, "fixture", 7));
                assert(games_stamp(&app) != stamp);
                free(more_deep);
            }
            free(app.folders[0]);
            app.folder_count = 0;
            folders_load(&app);
            assert(app.folder_count == 1 && app.folders_deep[0] && same_path(app.folders[0], deep_as));
            folder_remove(&app, 0);
            free(deep_as);
            free(deep);
#undef GAME
        }
        e.drop.data = more;
        event(&app, &e); /* back for the games below */
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_FOLDERS);
        assert(!strcmp(app.app_form.folders, "games/ and 1 more"));
        free(file);
        free(kept_as);
        free(more);
    }
    /* Folders: the data folder, games/ and the extractions, each its default or a folder picked, kept in app.ini's
       [folders]. A folder that cannot take what goes there is refused, never swapped for the default */
    {
        char *data = snx_strdup(app.data_dir), *games = snx_strdup(app.games_dir), *work = snx_strdup(app.work_dir);
        char* games_default = snx_path_join(data, "games"); /* the test's own games/ is elsewhere, set by hand */
        char *keys = snx_strdup(app.keys_path), *themes = snx_strdup(app.themes_dirs[0]); /* so are these */
        char *picked = snx_path_join(temporary, "picked games"), *file = snx_path_join(temporary, "not a folder.txt");
        char *kept, line[512];
        size_t size;
        key(&app, SDLK_DOWN);
        assert(app.editor.row == GUI_APP_PLACES && !strcmp(app.app_form.places, "Defaults"));
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_PLACES && app.editor.row == GUI_PLACE_DATA && gui_editor_rows(&app.editor) == 4);
        render(&app);
        shows(&app, "Folders", normal);
        shows(&app, "- Data", yellow);
        shows(&app, "default", yellow);
        /* under the rows, the folder in use: its start, as a long one wraps (Windows' %APPDATA% does) */
        snprintf(line, sizeof line, "%.24s", data);
        shows(&app, line, rgb(app.theme.soft));
        shows(&app, "- Extractions", normal);
        shows(&app, "- All back to the defaults", normal);
        assert(!strcmp(app.place_line[GUI_PLACE_GAMES], app.games_dir) && !strcmp(app.app_form.place[1], "default"));
        assert(strstr(app.place_about[GUI_PLACE_GAMES], "The recompiled games, ") &&
               strstr(app.place_about[GUI_PLACE_GAMES], " free. Enter: pick another. Del: the default"));
        /* refused: a folder not there, a file; nothing changes, nothing is kept */
        key(&app, SDLK_DOWN);
        place_set(&app, GUI_PLACE_GAMES, picked);
        snprintf(line, sizeof line, "Not changed: %s is not there", picked);
        assert(!app.place[GUI_PLACE_GAMES] && !strcmp(app.editor.note, line) && !strcmp(app.games_dir, games));
        assert(!snx_write_file(file, "x", 1));
        place_set(&app, GUI_PLACE_GAMES, file);
        assert(!app.place[GUI_PLACE_GAMES] && strstr(app.editor.note, " is not a folder"));
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(!kept || !strstr(kept, "[folders]"));
        free(kept);
        /* Enter opens the folder picker for the focused row; its answer is that row's folder at once, kept, and the
           folder before keeps what it has */
        assert(!snx_mkdirs(picked));
        key(&app, SDLK_RETURN);
        assert(app.place_row == GUI_PLACE_GAMES);
        {
            SDL_Event answer;
            SDL_zero(answer);
            answer.type = app.picked_folder;
            answer.user.data1 = SDL_strdup(picked);
            event(&app, &answer);
        }
        assert(app.place_row == -1 && app.place[GUI_PLACE_GAMES] && !strcmp(app.games_dir, app.place[GUI_PLACE_GAMES]));
        assert(snx_path_within(app.games_dir, picked) && !strcmp(app.data_dir, data) && !strcmp(app.work_dir, work));
        snprintf(line, sizeof line, "Games: %s. The folder before keeps what it has", app.games_dir);
        assert(!strcmp(app.editor.note, line) && app.editor.row == GUI_PLACE_GAMES);
        kept = (char*)snx_read_file(app.app_file, &size);
        snprintf(line, sizeof line, "[folders]\ndata = \ngames = %s\nwork = \n", app.games_dir);
        assert(kept && strstr(kept, "[app]\ntheme = ") && strstr(kept, line));
        free(kept);
        render(&app);
        shows(&app, "- Games", yellow);
        shows(&app, "picked", yellow);
        assert(!strcmp(app.place_line[GUI_PLACE_GAMES], app.games_dir));
        /* read back as the menu opens, a path not from the root left out and named */
        free(app.place[GUI_PLACE_GAMES]);
        app.place[GUI_PLACE_GAMES] = NULL;
        places_apply(&app);
        assert(!strcmp(app.games_dir, games_default));
        kept = (char*)snx_read_file(app.app_file, &size);
        {
            char* more_text = snx_format("%s[folders]\nwork = relative/work\nshaders = /x\n", kept);
            assert(!snx_write_file(app.app_file, more_text, strlen(more_text)));
            free(more_text);
        }
        free(kept);
        app.message[0] = 0;
        app_load(&app);
        snprintf(line, sizeof line, "Not used from %s: [folders] work = relative/work, [folders] shaders = /x",
                 app.app_file);
        assert(!strcmp(app.message, line) && app.place[GUI_PLACE_GAMES] && !strcmp(app.games_dir, app.place[1]));
        assert(!app.place[GUI_PLACE_WORK] && !strcmp(app.work_dir, work));
        app.message[0] = 0;
        /* a folder picked once and gone since: its row says so, as a run would refuse it */
        assert(!snx_remove_tree(picked));
        open_places(&app, GUI_PLACE_GAMES);
        snprintf(line, sizeof line, "%s is not there", app.games_dir);
        assert(!strcmp(app.app_form.place[GUI_PLACE_GAMES], "is not there") &&
               !strcmp(app.place_line[GUI_PLACE_GAMES], line));
        assert(!places_fit(&app, line, sizeof line) && strstr(line, "Games folder ") &&
               strstr(line, " is not there. "));
        /* Del: the default again, kept */
        key(&app, SDLK_DELETE);
        assert(!app.place[GUI_PLACE_GAMES] && !strcmp(app.games_dir, games_default) &&
               strstr(app.editor.note, ", the default"));
        kept = (char*)snx_read_file(app.app_file, &size);
        assert(kept && !strstr(kept, "[folders]"));
        free(kept);
        /* a disk that runs no programs takes the extractions, not the games */
#if defined(ST_NOEXEC) && !defined(SDL_PLATFORM_ANDROID)
        {
            struct statvfs st;
            if (!statvfs("/dev/shm", &st) && (st.f_flag & ST_NOEXEC)) {
                char* shm = snx_temp_dir("/dev/shm");
                assert(shm);
                place_set(&app, GUI_PLACE_GAMES, shm);
                assert(!app.place[GUI_PLACE_GAMES] && strstr(app.editor.note, "runs no programs (noexec)"));
                place_set(&app, GUI_PLACE_WORK, shm);
                assert(app.place[GUI_PLACE_WORK] && !strcmp(app.work_dir, app.place[GUI_PLACE_WORK]));
                assert(!snx_remove_tree(shm));
                free(shm);
            }
        }
#endif
        /* Android: a folder outside the app's own storage is asked about first, its Use it pressed twice; Back leaves
           the one in use. One inside is taken at once */
        {
            char *own = snx_path_join(temporary, "own"), *inside = snx_path_join(own, "games");
            int w, h;
            GuiRect r;
            GuiList l;
            assert(!snx_mkdirs(inside));
            app.own_dir = own;
            app.editor.row = GUI_PLACE_WORK;
            place_set(&app, GUI_PLACE_WORK, temporary);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_OUTSIDE && !app.place[GUI_PLACE_WORK]);
            /* the question wraps where it is wider than the screen, its choices under it */
            SDL_GetWindowSize(app.window, &w, &h);
            assert(SDL_SetWindowSize(app.window, 420, h));
            render(&app);
            shows(&app, "Outside the app,", rgb(app.theme.warning));
            shows(&app, "games may run slower.", rgb(app.theme.warning));
            ask_boxes(&app, &r, &l);
            assert(list_box(&l, 0).y >= r.y + 2 * cell_h(&app));
            assert(SDL_SetWindowSize(app.window, w, h));
            render(&app);
            shows(&app, "- Back", yellow);
            shows(&app, "- Use it", normal);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && !app.place[GUI_PLACE_WORK] && !strcmp(app.editor.note, "Not changed"));
            place_set(&app, GUI_PLACE_WORK, temporary);
            key(&app, SDLK_DOWN);
            key(&app, SDLK_RETURN);
            assert(app.confirm.shown && app.confirm.armed && !app.place[GUI_PLACE_WORK]);
            render(&app);
            shows(&app, "- Use it: press again", yellow);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && app.place[GUI_PLACE_WORK] && same_path(app.work_dir, temporary));
            assert(!strcmp(app.app_form.place[GUI_PLACE_WORK], "outside the app"));
            /* a finger: the first tap on Use it arms it, the second takes it */
            key(&app, SDLK_DELETE);
            assert(!app.place[GUI_PLACE_WORK]);
            place_set(&app, GUI_PLACE_WORK, temporary);
            ask_boxes(&app, &r, &l);
            touch_box(&app, list_box(&l, 1));
            assert(app.confirm.shown && app.confirm.armed && !app.place[GUI_PLACE_WORK]);
            touch_box(&app, list_box(&l, 1));
            assert(!app.confirm.shown && app.place[GUI_PLACE_WORK]);
            key(&app, SDLK_DELETE);
            place_set(&app, GUI_PLACE_GAMES, inside);
            assert(!app.confirm.shown && app.place[GUI_PLACE_GAMES] &&
                   !strcmp(app.app_form.place[GUI_PLACE_GAMES], "picked"));
            app.own_dir = NULL;
            free(inside);
            free(own);
        }
        /* the last row: every one back to its default */
        place_set(&app, GUI_PLACE_DATA, temporary);
        assert(app.place[GUI_PLACE_DATA] && snx_path_within(app.data_dir, temporary) &&
               snx_path_within(app.games_dir, app.data_dir));
        app.editor.row = GUI_PLACES;
        key(&app, SDLK_RETURN);
        assert(!app.place[0] && !app.place[1] && !app.place[2] && !strcmp(app.data_dir, data));
        assert(!strcmp(app.editor.note, "All back to the defaults. The folders before keep what they have"));
        free(app.games_dir); /* the test's own games/ and the rest, as before */
        app.games_dir = games;
        free(app.keys_path);
        app.keys_path = keys;
        free(app.themes_dirs[0]);
        app.themes_dirs[0] = themes;
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_APP && app.editor.row == GUI_APP_PLACES);
        free(file);
        free(picked);
        free(work);
        free(data);
        free(games_default);
    }
    key(&app, SDLK_ESCAPE);
    assert(app.form == GUI_FORM_SETTINGS && app.editor.row == 0);
    key(&app, SDLK_ESCAPE);
    assert(!app.editing && app.menu.selected == GUI_SETTINGS);
    /* kept in a file, the changed ones alone, they come back as the menu opens again; the runs below get them */
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.settings_file, &size);
        assert(kept && strstr(kept, "[Display]\nfullscreen=1\n") && !strstr(kept, "controls=") &&
               !strstr(kept, "gdbstub"));
        free(kept);
        gui_settings_free(&app.settings); /* as a new start has none read */
        assert(setting_is(&app, "fullscreen", NULL) && settings_of(&app) && setting_is(&app, "fullscreen", "1"));
    }
    key(&app, SDLK_UP);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    /* a load opens the screen its file is picked on at once: games/ is Games' now, not an option under it */
    assert(app.menu.selected == GUI_SWITCH);
    pump(&app);
    render(&app);
    shows(&app, "- Load Switch NSP/XCI/NRO", yellow);
    const char* screenshot = getenv("SNX_GUI_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_SWITCH);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_SWITCH);
    /* the activated item stays pink while the mouse hovers another in yellow; a click selects */
    point(&app, entry_box(&app, GUI_LINUX), false);
    assert(app.menu.selected == GUI_LINUX && app.menu.active == GUI_SWITCH && app.menu.view == GUI_LOAD_NONE);
    render(&app);
    shows(&app, "- Load Switch NSP/XCI/NRO", pink);
    shows(&app, "- Load Linux ARM ELF", yellow);
    point(&app, entry_box(&app, GUI_SWITCH), true);
    assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_SWITCH);
    {
        GuiPickBoxes k;
        app.clock = 1000 * app.theme.blink_ms * 2; /* the prompt shown, not blinked away */
        render(&app);
        shows(&app, "Load Switch NSP/XCI/NRO:", normal);
        shows(&app, "[ Pick or drop a file ]", yellow);
        app.clock = 0;
        /* the arrows have nothing to move to there; a click on the prompt, or Enter, opens the system's picker, which
           answers no file here (SDL_FILE_DIALOG_DRIVER=none) */
        key(&app, SDLK_DOWN);
        assert(app.menu.view == GUI_LOAD_DROP && !app.picked);
        pick_boxes(&app, &k);
        mouse_px(&app, k.place.list.x + 16, k.prompt + 4, true);
        assert(app.picked && app.menu.view == GUI_LOAD_DROP);
        key(&app, SDLK_RETURN);
        {
            SDL_Event answer; /* its answer comes in the next poll, as a frame's */
            while (SDL_PollEvent(&answer)) event(&app, &answer);
        }
        assert(app.menu.view == GUI_LOAD_DROP && !strcmp(app.message, "No file was picked."));
    }
    app.menu.selected = GUI_LINUX; /* a drop runs the item that opened the screen */
    SDL_Event e = {0};
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = "one\ntwo";
    event(&app, &e);
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = input;
    app.run_log = snx_path_join(temporary, "last-run.log"); /* the run's output, kept as it comes */
    event(&app, &e);
    assert(app.menu.running && app.menu.selected == GUI_SWITCH);
    finish(&app, 0);
    assert(!app.native && !gui_steps_last(&app.steps, "LAUNCH")); /* a driver that started no game stayed compiling */
    /* a menu that is no runtime's (a build without one) looks for scarletnx beside it, then on PATH */
    {
        char* own = app.self;
        char* path = SDL_strdup(SDL_getenv("PATH"));
        app.self = NULL;
        SDL_setenv_unsafe("PATH", "/nonexistent", 1);
        launch(&app, snx_strdup(input), false);
        assert(!app.menu.running && !strcmp(app.message, "Cannot find scarletnx beside the GUI or on PATH."));
        SDL_setenv_unsafe("PATH", path, 1);
        SDL_free(path);
        app.self = own;
        app.message[0] = 0;
    }
    {
        size_t size;
        char* kept = (char*)snx_read_file(app.run_log, &size);
        char* line = snx_format("game with spaces;literal.nro --fullscreen=1 --controls=0 --repo %s\n", app.data_dir);
        assert(kept && line && !app.run_out && !strncmp(kept, "$ ", 2) && strstr(kept, line));
        free(line);
        assert(strstr(kept, "driver stdout") && strstr(kept, "driver stderr") && strstr(kept, "\n[ended, status 0]\n"));
        free(kept);
    }
    key(&app, SDLK_DOWN);
    key(&app, SDLK_SPACE);
    assert(app.menu.selected == GUI_LINUX && app.menu.view == GUI_LOAD_DROP);
    char* quoted = snx_format("\"%s\"", input);
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = quoted;
    event(&app, &e);
    free(quoted);
    assert(app.menu.running);
    finish(&app, 7);
    size_t size;
    unsigned char* runs = snx_read_file(input, &size);
    assert(runs && size == 1 && runs[0] == '2');
    free(runs);
    char* flags_input = snx_path_join(temporary, "option flags.nro");
    assert(!snx_write_file(flags_input, "fixture", 7));
    app.menu.selected = GUI_SWITCH;
    launcher(&app, true);
    launch(&app, snx_strdup(flags_input), false);
    finish(&app, 0);
    assert(!app.native && gui_steps_last(&app.steps, "LAUNCH"));
    launcher(&app, false);
    app.menu.selected = GUI_LINUX;
    free(flags_input);
    /* the folders picked in Settings > App > Folders, passed to the driver: games/ for every input, the extraction for a
       dump, under the picked folder as the driver names it; one gone since stops the run, naming it */
    {
        char *games = snx_path_join(temporary, "picked games"), *work = snx_path_join(temporary, "picked work");
        char *dump = snx_path_join(temporary, "picked folders.nsp"),
             *nro = snx_path_join(temporary, "picked folders.nro");
        char *games_was = snx_strdup(app.games_dir), *keys_was = snx_strdup(app.keys_path), expected[512];
        assert(!snx_mkdirs(games) && !snx_mkdirs(work) && !snx_write_file(dump, "fixture", 7) &&
               !snx_write_file(nro, "fixture", 7));
        place_set(&app, GUI_PLACE_GAMES, games);
        place_set(&app, GUI_PLACE_WORK, work);
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        launch(&app, snx_strdup(dump), false);
        finish(&app, 0);
        launch(&app, snx_strdup(nro), false);
        finish(&app, 0);
        assert(!snx_remove_tree(work));
        launch(&app, snx_strdup(dump), false);
        assert(!app.menu.running && strstr(app.message, "Extractions folder ") &&
               strstr(app.message, " is not there. Settings > App > Folders picks another."));
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        places_reset(&app);
        snprintf(expected, sizeof expected,
                 "Folders: All back to the defaults. The folders before keep what they have");
        assert(!app.place[GUI_PLACE_GAMES] && !app.place[GUI_PLACE_WORK] && !strcmp(app.message, expected));
        app.message[0] = 0;
        free(app.games_dir);
        app.games_dir = games_was;
        free(app.keys_path);
        app.keys_path = keys_was;
        free(nro);
        free(dump);
        free(work);
        free(games);
    }

    /* a dump not extracted yet, with less room than its extraction and RomFS copy take: asked first, Run anyway
       pressed twice; Back runs nothing. With room, extracted already or an NRO, it runs at once */
    {
        char *dump = snx_path_join(temporary, "room dump.nsp"), *nro = snx_path_join(temporary, "room dump.nro");
        char *work_was = app.work_dir, *games_was = app.games_dir, *out;
        GuiRect r;
        GuiList l;
        void* zeros = calloc(1, 3u << 20);
        assert(zeros && !snx_write_file(dump, zeros, 3u << 20) && !snx_write_file(nro, zeros, 3u << 20));
        free(zeros);
        app.work_dir = snx_path_join(temporary, "room work"); /* one disk, as the defaults are */
        app.games_dir = snx_path_join(temporary, "room games");
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        app.room_free = 4u << 20; /* 6 MB to take */
        launch(&app, snx_strdup(dump), false);
        assert(!app.menu.running && app.confirm.shown && app.confirm.ask == GUI_ASK_ROOM && app.room_file);
        render(&app);
        shows(&app, "Needs about 6 MB, 4 MB free.", rgb(app.theme.warning));
        shows(&app, "- Back", yellow);
        shows(&app, "- Run anyway", normal);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && !app.menu.running && !app.room_file);
        launch(&app, snx_strdup(dump), false);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(app.confirm.shown && app.confirm.armed && !app.menu.running);
        render(&app);
        shows(&app, "- Run anyway: press again", yellow);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && app.menu.running && !app.room_ok && app.menu.selected == GUI_SWITCH);
        finish(&app, 0);
        /* R's recompile of a dump is asked about too, and Run anyway prepares it again, playing nothing */
        app.recompile = true;
        launch(&app, snx_strdup(dump), false);
        app.recompile = false;
        assert(app.confirm.shown && app.confirm.ask == GUI_ASK_ROOM && !app.menu.running);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        key(&app, SDLK_RETURN);
        assert(!app.confirm.shown && app.menu.running && !app.recompile);
        finish(&app, 0);
        assert(gui_steps_last(&app.steps, "RECOMPILE") && !gui_steps_last(&app.steps, "LAUNCH"));
        /* a finger: the first tap arms Run anyway, the second runs it */
        launch(&app, snx_strdup(dump), false);
        ask_boxes(&app, &r, &l);
        touch_box(&app, list_box(&l, 1));
        assert(app.confirm.shown && app.confirm.armed && !app.menu.running);
        touch_box(&app, list_box(&l, 1));
        assert(!app.confirm.shown && app.menu.running);
        finish(&app, 0);
        app.room_free = 6u << 20;
        launch(&app, snx_strdup(dump), false);
        assert(!app.confirm.shown && app.menu.running);
        finish(&app, 0);
        app.room_free = 1;
        launch(&app, snx_strdup(nro), false);
        assert(!app.confirm.shown && app.menu.running);
        finish(&app, 0);
        out = dump_out(&app, dump, NULL);
        assert(!snx_mkdirs(out));
        launch(&app, snx_strdup(dump), false);
        assert(!app.confirm.shown && app.menu.running);
        finish(&app, 0);
        assert(!snx_remove_tree(out));
        free(out);
        app.room_free = 0;
        free(app.work_dir);
        app.work_dir = work_was;
        free(app.games_dir);
        app.games_dir = games_was;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(nro);
        free(dump);
    }

    /* recent games: what the GUI ran, newest first in Games under Add a folder and kept in a file, named as the run
       named it; one runs again with the options the menu has now, which the fake driver checks */
    {
        GuiRecent kept;
        char* recent_file = snx_path_join(temporary, "prefs/recent.txt");
        char* again = snx_path_join(temporary, "option flags again.nro");
        SDL_Color soft = rgb(app.theme.soft);
        assert(!snx_write_file(again, "fixture", 7));
        recent_open(&app, snx_strdup(recent_file));
        assert(!app.recent.count && app.browser.head == 1);
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        launch(&app, snx_strdup(again), false);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.browser.head == 2 && !strcmp(app.recent.game[0].name, "option flags"));
        assert(app.recent.game[0].driver == GUI_SWITCH && !app.recent.game[0].prepared);
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && !strcmp(kept.game[0].name, "option flags"));
        assert(!strcmp(kept.game[0].path, app.recent.game[0].path));
        /* in Games, not the menu: Add a folder, a title and the game, then the games; down from Add a folder reaches
           it */
        render(&app);
        hides(&app, "Recent games", soft); /* the game itself shows in the last output, beside the items */
        go_tab(&app, GUI_TAB_GAMES);
        render(&app);
        shows(&app, "- Add a folder", yellow);
        shows(&app, "Recent games", soft);
        shows(&app, "- option flags", normal);
        key(&app, SDLK_DOWN);
        render(&app);
        assert(app.browser.selected == 1);
        shows(&app, "- option flags", yellow);
        hides(&app, "- Add a folder", yellow);
        {
            char start[21]; /* the tips: where it is */
            snprintf(start, sizeof start, "%s", app.recent.game[0].path);
            shows(&app, start, normal);
        }
        /* run again from there, whatever item the menu last had: its own driver, the options of now; the menu keeps
           its item */
        app.menu.selected = GUI_LINUX;
        key(&app, SDLK_RETURN);
        assert(app.menu.running && app.menu.selected == GUI_LINUX && app.recent.game[0].driver == GUI_SWITCH);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.menu.view == GUI_LOAD_NONE);
        /* the mouse: over the game it takes the focus, over Add a folder it gives it back */
        go_tab(&app, GUI_TAB_GAMES);
        point(&app, game_box(&app, 0, true), false);
        assert(app.browser.selected == 1);
        point(&app, game_box(&app, 0, false), false);
        assert(app.browser.selected == 0);
        point(&app, game_box(&app, 0, true), false);
        /* YoRHa: the focused game on the band, the title in its soft colour */
        app.theme = *gui_theme_builtin("yorha");
        render(&app);
        shows(&app, "Recent games", rgb(app.theme.soft));
        shows(&app, "\xfe option flags", rgb(app.theme.page));
        app.theme = *gui_theme_builtin("default");
        /* a game whose file went away leaves the list when chosen, and the file says so */
        assert(!remove(again));
        key(&app, SDLK_RETURN);
        assert(!app.menu.running && !app.recent.count && app.browser.head == 1 && !app.browser.selected);
        assert(strstr(app.message, "is gone"));
        gui_recent_load(&kept, recent_file);
        assert(!kept.count);
        render(&app);
        hides(&app, "Recent games", soft);
        /* five: three of them, and a row that shows the rest */
        {
            const char* names[] = {"e", "d", "c", "b", "a"}; /* added oldest first: a is the newest */
            for (int i = 0; i < 5; ++i) {
                char* path = snx_format("%s/%s.nro", temporary, names[i]);
                assert(!gui_recent_add(&app.recent, path, GUI_SWITCH, 0));
                free(path);
            }
            recent_changed(&app);
            assert(app.recent.count == 5 && app.browser.head == 5 && !app.recent_all);
            render(&app);
            shows(&app, "Recent games", soft);
            shows(&app, "- a", normal);
            shows(&app, "- c", normal);
            shows(&app, "- ...show more", normal);
            hides(&app, "- d", normal);
            app.browser.selected = 4; /* the row that shows the rest */
            render(&app);
            shows(&app, "- ...show more", yellow);
            shows(&app, "The recent games not shown", normal); /* beside the list */
            key(&app, SDLK_RETURN);                            /* all five, the focus on the first that was hidden */
            assert(!app.menu.running && app.recent_all && app.browser.head == 6 && app.browser.selected == 4);
            render(&app);
            shows(&app, "- d", yellow);
            shows(&app, "- e", normal);
            app.recent = (GuiRecent){0};
            app.recent_all = false;
            recent_changed(&app);
            go_tab(&app, GUI_TAB_MENU);
        }
        /* a list that cannot be written: the run's end says so, and the menu after it, until the list is kept */
        {
            char* blocker = snx_path_join(temporary, "not a folder");
            char* unsaved = snx_path_join(blocker, "recent.txt");
            char* game = snx_path_join(temporary, "option flags unsaved.nro");
            char note[512], expected[600];
            assert(!snx_write_file(blocker, "file", 4) && !snx_write_file(game, "fixture", 7));
            recent_open(&app, snx_strdup(unsaved));
            app.menu.selected = GUI_SWITCH;
            launcher(&app, true);
            launch(&app, game, false);
            for (Uint64 until = SDL_GetTicks() + 10000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5))
                pump(&app);
            assert(app.finished && !app.abnormal_exit);
            snprintf(note, sizeof note, "Recent games not kept: %s could not be written.", unsaved);
            snprintf(expected, sizeof expected, "Driver exited with status 0. %s", note);
            assert(app.recent_unsaved && app.recent.count == 1 && !strcmp(app.message, expected));
            render(&app);
            shows(&app, "Driver exited with status 0.", yellow);
            leave_run(&app);
            assert(!strcmp(app.message, note));
            /* written at last: the note goes, what the message said before it stays */
            assert(!remove(blocker));
            snprintf(app.message + strlen(app.message), sizeof app.message - strlen(app.message), " After.");
            recent_changed(&app);
            assert(!app.recent_unsaved && !strcmp(app.message, "After.") && snx_is_file(unsaved));
            snprintf(app.message, sizeof app.message, "Before. %s", note);
            recent_changed(&app);
            assert(!strcmp(app.message, "Before."));
            assert(!snx_remove_tree(blocker));
            free(unsaved);
            free(blocker);
            app.recent = (GuiRecent){0};
            recent_changed(&app);
        }
        /* the rest runs with no list */
        free(app.recent_file);
        app.recent_file = NULL;
        app.message[0] = 0;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(again);
        free(recent_file);
    }

    /* 3DS: its own driver for drops, CIA browser and recents, with no Switch
       flags or direct execution, even when another item is hovered. */
    {
        char* original_games = app.games_dir;
        char* recent_file = snx_path_join(temporary, "prefs/3ds-recent.txt");
        app.games_dir = snx_path_join(temporary, "cia-games");
        char* cia = snx_path_join(app.games_dir, "3ds input.CIA");
        assert(!snx_mkdirs(app.games_dir) && !snx_write_file(cia, "fixture", 7));
        recent_open(&app, snx_strdup(recent_file));
        launcher(&app, true);
        point(&app, entry_box(&app, GUI_3DS), true);
        assert(app.menu.view == GUI_LOAD_DROP && app.menu.active == GUI_3DS);
        point(&app, entry_box(&app, GUI_LINUX), false); /* where Linux's was: nothing there now */
        e.type = SDL_EVENT_DROP_FILE;
        e.drop.data = cia;
        event(&app, &e);
        assert(app.menu.running && app.menu.selected == GUI_3DS && !app.native);
        finish(&app, 3);
        assert(app.recent.count == 1 && app.recent.game[0].driver == GUI_3DS && !app.recent.game[0].prepared);
        GuiRecent kept;
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && kept.game[0].driver == GUI_3DS && !kept.game[0].prepared);
        open_recent(&app, 0);
        finish(&app, 3);
        /* a CIA in games/ is one of Games', which runs it with the 3DS driver */
        go_tab(&app, GUI_TAB_GAMES);
        assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 1);
        app.browser.selected = app.browser.head; /* the game, under Add a folder and the recent one */
        key(&app, SDLK_RETURN);
        assert(app.menu.running && !app.native && app.menu.selected == GUI_3DS);
        finish(&app, 3);
        app.recent = (GuiRecent){0};
        recent_changed(&app);
        free(app.recent_file);
        app.recent_file = NULL;
        launcher(&app, false);
        app.menu.selected = GUI_LINUX;
        free(app.games_dir);
        app.games_dir = original_games;
        app.games_stamp = games_stamp(&app);
        free(cia);
        free(recent_file);
    }

    char* hung = snx_path_join(temporary, "hang in join.elf");
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    Uint64 until = SDL_GetTicks() + 5000;
    bool ready = false;
    while (SDL_GetTicks() < until) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
        if (ready) break;
        SDL_Delay(5);
    }
    assert(ready && app.menu.running);
    render(&app);
    shows_foot(&app, SAY_ABORT, red);
    int selected = app.menu.selected;
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.running && app.menu.selected == selected);
    Uint64 started = SDL_GetTicks();
    key(&app, SDLK_ESCAPE);
    assert(app.aborting && SDL_GetTicks() - started < 1000);
    render(&app);
    shows(&app, "Aborting process tree...", red);
    app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
    app.theme.prompt.blink = false;
    render(&app);
    shows_foot(&app, "[ Aborting process tree... ]", red); /* where the keys go */
    app.theme.prompt = gui_theme_builtin(app.theme.id)->prompt;
    key(&app, SDLK_ESCAPE); /* Repeated ESC while reaping is harmless. */
    while (app.menu.running && SDL_GetTicks() - started < 2000) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && !app.finished && !app.aborting && !app.menu.quit);
    assert(app.menu.view == GUI_LOAD_NONE && app.abnormal_exit);
    assert(!strcmp(app.message, "Program ended abnormally (force-terminated by ESC)."));
    render(&app);
    shows(&app, "Program ended abnormally", red); /* in the tips */
    /* a finger on a running run's ESC line aborts it, anywhere else it does nothing */
    assert(!snx_write_file(hung, "", 0));
    launch(&app, snx_strdup(hung), false);
    for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
        unsigned char* data = snx_read_file(hung, &size);
        ready = data && size == 5 && !memcmp(data, "ready", 5);
        free(data);
        pump(&app);
    }
    assert(ready && app.menu.running);
    {
        GuiPlace p;
        int cw = 8 * app.scale, ch = 16 * app.scale;
        run_place(&app, &p);
        touch_box(&app, p.list); /* the output: nothing */
        assert(app.menu.running && !app.aborting);
        app.theme = *gui_theme_builtin("yorha"); /* a loading screen's ESC line is its last row */
        tap_px(&app, 20 * cw, 3 * ch + ch / 2);
        assert(!app.aborting);
        tap_px(&app, 20 * cw, (app.rows - 1) * ch + ch / 2);
        assert(app.aborting);
        app.theme = *gui_theme_builtin("default");
    }
    while (app.menu.running && SDL_GetTicks() < until) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && app.abnormal_exit);
    /* where ESC is a back gesture made by mistake (ask_abort, Android's) it asks first: ESC again, a click off the box
       or Keep going leave the run going; Abort aborts it. The ESC line's tap asks too, and a held key answers nothing */
    {
        int pass;
        GuiRect r;
        GuiList l;
        GuiPlace p;
        SDL_Event held;
        app.ask_abort = true;
        for (pass = 0; pass < 2; ++pass) {
            assert(!snx_write_file(hung, "", 0));
            launch(&app, snx_strdup(hung), false);
            for (until = SDL_GetTicks() + 5000, ready = false; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
                unsigned char* data = snx_read_file(hung, &size);
                ready = data && size == 5 && !memcmp(data, "ready", 5);
                free(data);
                pump(&app);
            }
            assert(ready && app.menu.running);
            key(&app, SDLK_ESCAPE);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_KEEP && !app.aborting && app.menu.running);
            if (pass) break;
            render(&app);
            ask_boxes(&app, &r, &l);
            run_place(&app, &p);
            shows(&app, "Abort the recompile? Its work is lost.", rgb(app.theme.warning));
            shows(&app, "- Keep going", yellow);
            shows(&app, "- Abort", normal);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && app.menu.running && !app.aborting);
            key(&app, SDLK_AC_BACK);
            key(&app, SDLK_DOWN);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            render(&app);
            shows(&app, "- Keep going", normal);
            shows(&app, "- Abort", yellow);
            key(&app, SDLK_ESCAPE); /* on Abort too */
            assert(!app.confirm.shown && !app.aborting);
            touch_box(&app, p.keys); /* the ESC line */
            assert(app.confirm.shown && !app.aborting);
            tap_px(&app, r.x + 16 * app.scale, r.y + 8 * app.scale); /* the question itself: nothing */
            assert(app.confirm.shown);
            tap_px(&app, r.x - 8, list_box(&l, 1).y + 4); /* beside Abort, off the box */
            assert(!app.confirm.shown && !app.aborting && app.menu.running);
            key(&app, SDLK_ESCAPE);
            point(&app, list_box(&l, 1), false); /* the pointer moves the focus */
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            point(&app, list_box(&l, 0), true);
            assert(!app.confirm.shown && !app.aborting && app.menu.running);
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_RETURN);
            assert(!app.confirm.shown && !app.aborting);
            SDL_zero(held);
            held.type = SDL_EVENT_KEY_DOWN;
            held.key.key = SDLK_ESCAPE;
            held.key.repeat = true;
            event(&app, &held);
            assert(!app.confirm.shown);
            /* over YoRHa's loading screen: its box, the focused choice on a band of its focus colour */
            app.theme = *gui_theme_builtin("yorha");
            app.fade_ms = 0;
            key(&app, SDLK_ESCAPE);
            render(&app);
            shows(&app, "Abort the recompile?", rgb(app.theme.warning));
            shows(&app, "\xfe Keep going", rgb(app.theme.page));
            shows(&app, "\xfe Abort", rgb(app.theme.ink));
            ask_boxes(&app, &r, &l);
            app.theme = *gui_theme_builtin("default");
            touch_box(&app, list_box(&l, 1));
            assert(app.aborting && !app.confirm.shown);
            while (app.menu.running && SDL_GetTicks() < until) {
                pump(&app);
                SDL_Delay(5);
            }
            assert(!app.menu.running && app.abnormal_exit && !app.finished);
            assert(!strcmp(app.message, "Program ended abnormally (force-terminated by ESC)."));
        }
        /* a run that ends on its own while asking takes the question with it */
        gui_process_terminate(app.process);
        while (app.menu.running && SDL_GetTicks() < until) {
            pump(&app);
            SDL_Delay(5);
        }
        assert(!app.menu.running && app.finished && app.abnormal_exit && !app.confirm.shown);
        key(&app, SDLK_ESCAPE);
        assert(!app.finished && !app.confirm.shown);
        app.ask_abort = false;
    }
    free(hung);

    /* Games: a tab of its own, not an item of the menu; its browser filters, pages and launches both ways. Reopening
       refreshes the folders rather than caching them */
    assert(!snx_mkdirs(app.games_dir));
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES && !app.games.count && app.browser.head == 1);
    render(&app);
    shows(&app, "- Add a folder", yellow);
    shows(&app, "Pick a folder of games", normal); /* beside it, what it does */
    /* it opens the folder picker of Settings > App > Game folders, whose answer (none here) the message says */
    key(&app, SDLK_RETURN);
    {
        SDL_Event answer;
        Uint64 until = SDL_GetTicks() + 2000;
        while (!app.message[0] && SDL_GetTicks() < until) /* its answer comes on the picker's thread */
            while (SDL_PollEvent(&answer)) event(&app, &answer);
    }
    assert(app.menu.view == GUI_LOAD_GAMES && !app.editing &&
           (!strcmp(app.message, "Game folders: No folder was picked.") ||
            !strncmp(app.message, "Game folders: The folder picker could not open: ", 48)));
    app.message[0] = 0;
    for (int i = 0; i < 35; ++i) {
        char name[40];
#ifdef _WIN32
        snprintf(name, sizeof(name), "Game%02d.exe", i);
#else
        snprintf(name, sizeof(name), "Game%02d", i);
#endif
        char* path = snx_path_join(app.games_dir, name);
        assert(!(i ? snx_write_file(path, "fixture", 7) : snx_copy_file(self, path)));
        assert(!snx_make_executable(path));
        free(path);
    }
    const char* ignored[] = {"Game_code.so", "Game_code.dll", ".hidden.exe", "notes.ini"};
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
        char* path = snx_path_join(app.games_dir, ignored[i]);
        assert(!snx_write_file(path, "fixture", 7));
        if (i < 3) assert(!snx_make_executable(path));
        free(path);
    }
    char* directory = snx_path_join(app.games_dir, "directory.exe");
    assert(!snx_mkdirs(directory));
    free(directory);
    /* Games finds them itself, looking once a second while it is open */
    app.clock = 50000;
    app.games_checked = app.clock;
    pump(&app);
    assert(!app.games.count);
    app.clock += 1000;
    pump(&app);
    assert(app.games.count == 35 && app.browser.count == 35);
    app.clock = 0;
    app.games_checked = 0;
    /* the menu: the loads, the keys, Settings, Share log and Exit, a box each with none between; no games in it */
    go_tab(&app, GUI_TAB_MENU);
    render(&app);
    hides(&app, "- Games", normal);
    hides(&app, "Recent games", rgb(app.theme.soft));
    {
        GuiMenuBoxes m;
        menu_boxes(&app, &m);
        assert(m.list.count == GUI_ITEM_COUNT && GUI_SWITCH == 0);
    }
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    { /* fixed: the list's column as wide on the menu as on Games, however long their entries; adaptive: each its own */
        GuiMenuBoxes m;
        GuiGamesBoxes g;
        app.menu.view = GUI_LOAD_NONE;
        menu_boxes(&app, &m);
        app.menu.view = GUI_LOAD_GAMES;
        games_boxes(&app, &g);
        assert(app.theme.sizing == GUI_SIZING_FIXED && g.place.list.w == m.place.list.w);
        app.theme.sizing = GUI_SIZING_ADAPTIVE;
        app.menu.view = GUI_LOAD_NONE;
        menu_boxes(&app, &m);
        app.menu.view = GUI_LOAD_GAMES;
        games_boxes(&app, &g);
        assert(g.place.list.w != m.place.list.w);
        app.theme.sizing = GUI_SIZING_FIXED;
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 1 && app.browser.selected == 0);
    /* Add a folder over every page, a wider gap under it than between the games, and no Back: ESC is the menu's */
    render(&app);
    shows(&app, "- Add a folder", yellow);
    hides(&app, "Back [ESC]", normal);
    {
        GuiGamesBoxes g;
        games_boxes(&app, &g);
        assert(list_box(&g.list, 0).y - (list_box(&g.add, 0).y + list_box(&g.add, 0).h) > 14 * app.scale &&
               g.list.pitch - g.list.item <= 14 * app.scale); /* the list's widest */
    }
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    assert(!strncmp(app.games.names[0], "Game00", 6));
    /* the player's folders' games after games/'s, each run from where it is; a folder gone is named */
    {
#ifdef _WIN32
        char* zed = snx_path_join(app.folders[0], "Zed.exe");
#else
        char* zed = snx_path_join(app.folders[0], "Zed");
#endif
        assert(app.folder_count == 1 && !snx_write_file(zed, "fixture", 7) && !snx_make_executable(zed));
        list_games(&app);
        assert(app.games.count == 36 && !strcmp(app.games.paths[35], zed) && !app.message[0]);
        app.folders[1] = snx_strdup("/nonexistent/snx-games");
        app.folder_count = 2;
        list_games(&app);
        assert(app.games.count == 36 && strstr(app.message, "/nonexistent/snx-games") &&
               strstr(app.message, "Settings > App > Game folders"));
        free(app.folders[1]);
        app.folder_count = 1;
        assert(!snx_remove_tree(zed));
        list_games(&app);
        assert(app.games.count == 35 && !app.message[0]);
        free(zed);
    }
    assert(gui_browser_pages(&app.browser) > 1);
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_DOWN);
    assert(app.browser.page == 1 && app.browser.selected == 1);
    /* a game put in while the list shows joins it, the list staying on its page and file; taken out, it leaves */
    {
#ifdef _WIN32
        char* late = snx_path_join(app.games_dir, "Game35.exe");
#else
        char* late = snx_path_join(app.games_dir, "Game35");
#endif
        assert(!snx_write_file(late, "fixture", 7) && !snx_make_executable(late));
        app.clock = 60000;
        app.games_checked = 59000;
        pump(&app);
        assert(app.games.count == 36 && app.browser.page == 1 && app.browser.selected == 1);
        assert(!snx_remove_tree(late));
        app.clock += 1000;
        pump(&app);
        assert(app.games.count == 35 && app.browser.page == 1 && app.browser.selected == 1);
        app.clock = 0;
        app.games_checked = 0;
        free(late);
    }
    render(&app);
    shows(&app, "\x1e", rgb(app.theme.soft)); /* games on the pages before and after */
    shows(&app, "\x1f", rgb(app.theme.soft));
    screenshot = getenv("SNX_GUI_BROWSER_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 2 && gui_browser_items(&app.browser) == 35 - 2 * app.browser.page_size);
    render(&app);
    shows(&app, "\x1e", rgb(app.theme.soft));
    hides(&app, "\x1f", rgb(app.theme.soft));
    key(&app, SDLK_PAGEUP);
    key(&app, SDLK_LEFT);
    assert(app.browser.page == 0 && app.browser.selected == 1);
    render(&app);
    hides(&app, "\x1e", rgb(app.theme.soft));
    shows(&app, "\x1f", rgb(app.theme.soft));
    point(&app, game_box(&app, 2, false), false);
    assert(app.browser.selected == 2 && !app.menu.running && !app.browser.activated);
    render(&app);
    shows(&app, app.games.names[1], yellow);
    point(&app, game_box(&app, 1, false), true); /* native prepared game, not another extraction */
    assert(app.menu.running && app.native && screen(&app) == GUI_SCREEN_LOADING);
    finish(&app, 0);
    /* a game in a folder of its own beside its data goes through the menu's driver with the settings and the data
       folder (Game00 above, an older version's beside the others, runs as it is): the driver plays it, recompiled
       first when another build made it, which the run shows as compiling until the game loads */
    {
        char *folder = snx_path_join(app.games_dir, "folder game"), *data = snx_path_join(folder, "folder game_data");
#ifdef _WIN32
        char* program = snx_path_join(folder, "folder game.exe");
#else
        char* program = snx_path_join(folder, "folder game");
#endif
        int item = app.menu.selected;
        assert(!snx_mkdirs(data) && !snx_copy_file(self, program) && !snx_make_executable(program));
        leave_run(&app);
        app.menu.selected = GUI_SWITCH;
        launch(&app, snx_strdup(program), true);
        assert(app.menu.running && app.native && screen(&app) == GUI_SCREEN_LOADING);
        {
            char name[64];
            run_name(&app, name, sizeof name);
            assert(!strcmp(name, "FOLDER GAME")); /* its file's, until its recompile names it */
            gui_steps_feed(&app.steps, "Recompiling Folder Quest 2.0 (1 modules)\n", 41, 0);
            run_name(&app, name, sizeof name);
            assert(!strcmp(name, "FOLDER QUEST 2.0") && screen(&app) == GUI_SCREEN_COMPILING);
        }
        gui_steps_feed(&app.steps, "ScarletNX - folder game\n", 24, 0);
        assert(screen(&app) == GUI_SCREEN_LOADING);
        gui_steps_reset(&app.steps);
        finish(&app, 0);
        assert(gui_steps_last(&app.steps, "RECOMPILE") && gui_steps_last(&app.steps, "LAUNCH"));
        app.menu.selected = item;
        /* R on it recompiles it through the driver with the backend picked and no option of a run (-S -s, the
           settings), playing nothing; R on Add a folder or a CIA says why it does nothing. The foot says so, and its
           word presses R as the key does */
        {
            char* cia = snx_path_join(app.games_dir, "r test.cia");
            assert(!snx_write_file(cia, "fixture", 7));
            go_tab(&app, GUI_TAB_GAMES);
            render(&app);
            shows_foot(&app, "R", yellow); /* its key, then what it does */
            shows_foot(&app, "recompile", normal);
            assert(hint_keys[hint_pick("R", 0)].key == SDLK_R && hint_keys[hint_pick("R", 5)].key == SDLK_R);
            assert(hint_keys[hint_pick("Recompile", 3)].key == SDLK_R); /* Android's: no R to name */
            app.browser.selected = 0;
            key(&app, SDLK_R);
            assert(!app.menu.running && !strcmp(app.message, "No game in focus to recompile."));
            focus_game(&app, "r test.cia");
            key(&app, SDLK_R);
            assert(!app.menu.running && !strcmp(app.message, "R recompiles Switch games only."));
            focus_game(&app, "folder game");
            launcher(&app, true);
            key(&app, SDLK_R);
            assert(app.menu.running && app.native && !app.recompile);
            finish(&app, 0);
            assert(gui_steps_last(&app.steps, "BUILT") && !gui_steps_last(&app.steps, "LAUNCH"));
            launcher(&app, false);
            app.menu.selected = item;
            assert(!snx_remove_tree(cia));
            free(cia);
        }
        assert(!snx_remove_tree(folder));
        free(program);
        free(data);
        free(folder);
    }
    go_tab(&app, GUI_TAB_GAMES); /* back from the run on the menu */
    key(&app, SDLK_DOWN);
    char* removed = snx_path_join(app.games_dir, app.games.names[0]);
    assert(!snx_remove_tree(removed));
    free(removed);
    key(&app, SDLK_RETURN);
    assert(!app.menu.running && app.menu.view == GUI_LOAD_GAMES && app.message[0]);
    point(&app, game_box(&app, 2, false), false);
    render(&app);
    shows(&app, app.games.names[0], pink);
    shows(&app, app.games.names[1], yellow);
    key(&app, SDLK_ESCAPE);
    go_tab(&app, GUI_TAB_GAMES);
    assert(app.games.count == 34);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE && !app.confirm.shown);
    /* Compact and spacious layouts share the renderer's mouse hit coordinates. Text size Auto keeps the letters of
       the window it opened in (1280x800: 2x), however the window is resized, and the window keeps 16 rows of them */
    const int sizes[][4] = {{2560, 1440, 45, 160}, {1280, 960, 30, 80}, {800, 480, 16, 50}, {1280, 800, 25, 80}};
    assert(app.auto_scale == 2);
    /* Auto is twice the display's scale, as far as 16 rows fit on a computer: 100%, 125%, 150% and 200%; a phone keeps
       20, so the Poco F8 (408 dpi, 1156 rows of pixels) and the Pixel 9 (420 dpi, 1080) get 3x, with 24 and 22 rows */
    assert(pick_auto_scale(1.0f, 800, 16) == 2 && pick_auto_scale(1.25f, 1000, 16) == 2);
    assert(pick_auto_scale(1.5f, 1200, 16) == 3 && pick_auto_scale(2.0f, 1600, 16) == 4);
    assert(pick_auto_scale(408.0f / 160, 1156, 20) == 3 && pick_auto_scale(420.0f / 160, 1080, 20) == 3);
    assert(pick_auto_scale(408.0f / 160, 1156, 16) == 4);
    assert(pick_auto_scale(1.0f, 480, 16) == 1 && pick_auto_scale(0.5f, 800, 16) == 1 &&
           pick_auto_scale(3.0f, 2400, 16) == 6);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert(SDL_SetWindowSize(app.window, sizes[i][0], sizes[i][1]));
        SDL_SyncWindow(app.window);
        render(&app);
        assert(app.scale == 2 && app.rows == sizes[i][2] && app.cols == sizes[i][3]);
        app.menu.selected = app.rows == 16 ? GUI_LOG : GUI_SWITCH; /* 16 rows: the list scrolls to its focus */
        render(&app);
        point(&app, entry_box(&app, GUI_LOG), false);
        assert(app.menu.selected == GUI_LOG && !app.menu.text_size);
        app.clock = 1; /* a description too long for its box rests before it slides */
        render(&app);
        shows(&app, "- Share log", yellow);
        shows(&app, "Saves the last run", normal);
        app.clock = 0;
        {
            GuiMenuBoxes m;
            menu_boxes(&app, &m);
            assert(m.list.focus >= m.list.first && m.list.focus < m.list.first + m.list.shown);
        }
    }
    /* a text size too big for the window: the list scrolls as the focus goes down, then back, an arrow where boxes
       are hidden, and a size that would leave fewer than 16 rows falls back */
    {
        GuiMenuBoxes m;
        SDL_Color soft = rgb(app.theme.soft);
        assert(SDL_SetWindowSize(app.window, 1280, 1024));
        SDL_SyncWindow(app.window);
        app.menu.text_size = GUI_TEXT_MAX;
        app.menu.selected = GUI_SWITCH;
        /* too narrow for the side box at that size: fixed shows the list alone, its whole height */
        render(&app);
        menu_boxes(&app, &m);
        assert(app.theme.sizing == GUI_SIZING_FIXED && !m.place.side.h && !m.place.status.h && !m.place.stacked);
        assert(m.place.list.w == m.place.head.w);
        app.theme.sizing = GUI_SIZING_ADAPTIVE; /* the side box under the list, which leaves room for fewer */
        render(&app);
        menu_boxes(&app, &m);
        assert(m.place.stacked && m.place.side.h);
        assert(app.scale == GUI_TEXT_MAX && app.rows == 16 && m.list.shown < m.list.count && !m.list.first);
        hides(&app, "\x1e", soft);
        shows(&app, "\x1f", soft); /* items below the last box */
        app.menu.selected = GUI_LOG;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.list.first > 0 && m.list.first + m.list.shown < m.list.count);
        shows(&app, "\x1e", soft);
        shows(&app, "\x1f", soft);
        app.menu.selected = GUI_EXIT;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.list.first + m.list.shown == m.list.count && m.list.first > 0);
        shows(&app, "\x1e", soft);
        hides(&app, "\x1f", soft);
        app.menu.selected = GUI_SWITCH;
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.list.first);
        /* a drag down moves the focus down row by row while the finger goes, not all at once when it lifts */
        {
            SDL_Event e = {0};
            float row = 1.0f / (float)app.rows;
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            e.type = SDL_EVENT_FINGER_DOWN;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 1.5f * row;
            event(&app, &e);
            assert(app.menu.selected == GUI_LINUX);
            e.tfinger.y = 0.2f + 3.2f * row;
            event(&app, &e);
            assert(app.menu.selected == GUI_KEYS);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            assert(app.menu.selected == GUI_KEYS && !app.finger);
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        /* the wheel too: a notch is a row, down three and back up */
        {
            SDL_Event e = {0};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.integer_y = -3;
            event(&app, &e);
            assert(app.menu.selected == GUI_KEYS);
            e.wheel.integer_y = 3;
            event(&app, &e);
            assert(app.menu.selected == GUI_SWITCH);
            /* both stop at the menu's ends, where the keys go round */
            app.menu.selected = GUI_EXIT;
            e.wheel.integer_y = -2;
            event(&app, &e);
            assert(app.menu.selected == GUI_EXIT);
            app.menu.selected = 0;
            e.wheel.integer_y = 2;
            event(&app, &e);
            assert(app.menu.selected == 0);
            app.menu.selected = GUI_EXIT;
            e.type = SDL_EVENT_FINGER_DOWN;
            e.tfinger.fingerID = 7;
            e.tfinger.x = 0.5f;
            e.tfinger.y = 0.2f;
            event(&app, &e);
            e.type = SDL_EVENT_FINGER_MOTION;
            e.tfinger.y = 0.2f + 2.5f / (float)app.rows;
            event(&app, &e);
            assert(app.menu.selected == GUI_EXIT);
            e.type = SDL_EVENT_FINGER_UP;
            event(&app, &e);
            app.menu.selected = GUI_SWITCH;
            render(&app);
        }
        assert(SDL_SetWindowSize(app.window, 800, 480)); /* 800x512 at least: 4x would leave 8 rows */
        SDL_SyncWindow(app.window);
        render(&app);
        assert(app.scale == 2 && app.rows == 16);
        app.menu.text_size = 0;
        app.theme.sizing = GUI_SIZING_FIXED;
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        render(&app);
    }
    {
        SDL_Color half;
        char row_text[160];
        int last_row;
        float x, y;
        SDL_Color page, ink, lp, li, ls, dimmed, lf;
        const GuiPalette* run;
        char* steps_input;
        Uint64 until, shown, faded, later, entered, launch_shown, first_shown, second_shown;
        float cell;
        GuiSteps kept;
        const char* detail;
        const char* live;
        const char* wide;
        const char* wide_name;
        int i;
        /* Themes: the menu item goes through the built-ins, then each themes/<id>/theme.ini */
        char* themes = snx_path_join(temporary, "themes");
        char* custom = snx_path_join(themes, "custom");
        char* custom_ini = snx_path_join(custom, "theme.ini");
        const char* ini = "[theme]\nbase = yorha\nname = Custom test\n[colors]\npage = #102030\nink = not a colour\n"
                          "[head]\nmenu = TEST  /  MENU\n[body]\nmarker = \"> \"\n";
        assert(!snx_mkdirs(custom) && !snx_write_file(custom_ini, ini, strlen(ini)));
        free(app.themes_dirs[0]);
        app.themes_dirs[0] = snx_strdup(themes);
        assert(!strcmp(app.theme.id, "default"));
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "yorha") && !app.editing && app.theme.panels == GUI_PANELS_FILL);
        app.menu.selected = GUI_SETTINGS;
        render(&app);
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        shows_big(&app, "SCARLETNX  /  MENU", ink, 2); /* its head plain, as NieR's titles */
        shows(&app, "\xfe Settings", page);            /* the focused box on a band */
        shows(&app, "\xfe Exit", ink);
        shows(&app, "CWD: ", ink);
        {
            GuiMenuBoxes
                m; /* the foot on a band of ink, the keys' box's bar in the page's colour, in the foot's size */
            menu_boxes(&app, &m);
            assert(same_colour(pixel(&app, (float)(m.place.keys.x + m.place.keys.w - 2), (float)(m.place.keys.y + 1)),
                               ink));
            assert(same_colour(pixel(&app, (float)(m.place.keys.x + 3 * foot_scale(&app)),
                                     (float)(m.place.keys.y + m.place.keys.h / 2)),
                               page));
        }
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "custom") && !strcmp(app.theme.name, "Custom test"));
        assert(app.theme.panels == GUI_PANELS_FILL && !app.editor.note[0]); /* its base's boxes */
        render(&app);
        page.r = 0x10; /* its own page; its ink, unreadable, is YoRHa's */
        page.g = 0x20;
        page.b = 0x30;
        shows_big(&app, "TEST  /  MENU", ink, 2);
        shows(&app, "> Settings", page);
        shows(&app, "> Exit", ink);
        app_select(&app, GUI_APP_THEME);
        assert(!strcmp(app.theme.id, "default") && !strcmp(app.app_form.theme, "Default"));
        free(custom_ini);
        free(custom);
        free(themes);

        /* The theme editor: its screen in the menu's theme, a preview the menu's own drawing makes in the theme being
           edited, a typed value, the preview's switch, and Save making it the menu's theme */
        {
            int r;
            GuiFormBoxes f;
            GuiRect preview;
            SDL_Color night = {0x10, 0x20, 0x30, 255}, soft = rgb(app.theme.soft);
            char* saved = snx_format("%s/themes/mytheme/theme.ini", temporary);
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && app.form == GUI_FORM_THEME && !strcmp(app.editor.id, "mytheme") &&
                   !strcmp(app.editor.theme.id, "default"));
            render(&app);
            shows(&app, "THEME EDITOR  /  mytheme", normal);
            shows(&app, "- Save", yellow); /* its first row */
            /* more rows than the box holds: an arrow down at first, both in the middle, up at the end */
            form_boxes(&app, &f);
            assert(app.editor.row == 0 && f.left.shown < f.left.count);
            hides(&app, "\x1e", soft);
            shows(&app, "\x1f", soft);
            app.editor.row = gui_editor_rows(&app.editor) / 2;
            render(&app);
            form_boxes(&app, &f);
            assert(f.left.first > 0);
            shows(&app, "\x1e", soft);
            shows(&app, "\x1f", soft);
            app.editor.row = gui_editor_rows(&app.editor) - 1;
            render(&app);
            shows(&app, "\x1e", soft);
            hides(&app, "\x1f", soft);
            app.editor.row = 0;
            render(&app);
            form_boxes(&app, &f);
            preview = inner(&app, f.place.side);
            preview.y += head_h(&app) + 2 * app.scale; /* under the pane's head */
            /* page = #102030, picked: Enter opens the picker over the keys, Left/Right and the pages step a channel,
               a click or a drag on a bar sets it, its last row types the colour; the preview's page takes it, the
               editor's own stays the menu's */
            for (r = 0; !gui_editor_key_of(&app.editor, r) || strcmp(gui_editor_key_of(&app.editor, r)->key, "page");
                 ++r) {}
            app.editor.row = r;
            app.editor.theme.page = (GuiRgb){0x80, 0x40, 0x20}; /* the bars' marks off their ends */
            key(&app, SDLK_RETURN);
            assert(app.editor.picking && !app.editor.typing && app.editor.channel == GUI_PICK_RED);
            {
                GuiPicker pk;
                GuiRgb was = app.editor.theme.page;
                int s = app.scale, w;
                SDL_Color c;
                render(&app);
                shows_foot(&app, "put back", normal); /* the picker's keys */
                picker_place(&app, &pk);
                assert(pk.box.x + pk.box.w <= f.place.list.x + f.place.list.w); /* over the keys: the preview shows */
                /* each bar the colours its channel goes through, the red one's ends black and red */
                w = pk.bars[0].w - 2 * s;
                c = pixel(&app, (float)(pk.bars[0].x + s), (float)(pk.bars[0].y + pk.bars[0].h / 2));
                assert(c.r == 0 && c.g == was.g && c.b == was.b);
                c = pixel(&app, (float)(pk.bars[0].x + s + w - 1), (float)(pk.bars[0].y + pk.bars[0].h / 2));
                assert(c.r == 255 && c.g == was.g && c.b == was.b);
                key(&app, SDLK_RIGHT);
                assert(app.editor.theme.page.r == was.r + 1 && app.editor.dirty);
                key(&app, SDLK_PAGEUP);
                assert(app.editor.theme.page.r == was.r + 17);
                key(&app, SDLK_PAGEDOWN);
                key(&app, SDLK_LEFT);
                assert(app.editor.theme.page.r == was.r);
                /* the mouse: a press at the green bar's start, a drag to its end, and the lift lets it go */
                mouse_px(&app, pk.bars[1].x + s, pk.bars[1].y + 2, true);
                assert(app.editor.channel == GUI_PICK_GREEN && app.editor.theme.page.g == 0 && app.pick_press == 2);
                mouse_px(&app, pk.bars[1].x + pk.bars[1].w + 40, pk.bars[0].y, false); /* past it, on another row */
                assert(app.editor.theme.page.g == 255 && app.editor.theme.page.r == was.r);
                mouse_px(&app, pk.bars[1].x + s + (w - 1) / 2, pk.bars[1].y + 2, false); /* the middle's strip */
                assert(app.editor.theme.page.g >= 126 && app.editor.theme.page.g <= 129);
                {
                    SDL_Event up = {0};
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    event(&app, &up);
                }
                mouse_px(&app, pk.bars[1].x + s, pk.bars[1].y + 2, false);
                assert(!app.pick_press && app.editor.theme.page.g >= 126 && app.editor.theme.page.g <= 129);
                /* a finger on the blue bar's end: set, and its lift taps nothing under it */
                tap_px(&app, pk.bars[2].x + pk.bars[2].w - s - 1, pk.bars[2].y + 2);
                assert(app.editor.picking && app.editor.channel == GUI_PICK_BLUE && app.editor.theme.page.b == 255);
                assert(!app.pick_press && !app.editor.typing);
                /* ESC puts it back; the wheel does not move the rows under it */
                {
                    SDL_Event wheel = {0};
                    wheel.type = SDL_EVENT_MOUSE_WHEEL;
                    wheel.wheel.integer_y = -1;
                    event(&app, &wheel);
                    assert(app.editor.row == r && app.editor.channel == GUI_PICK_BLUE);
                }
                key(&app, SDLK_ESCAPE);
                assert(!app.editor.picking && !memcmp(&app.editor.theme.page, &was, sizeof was) && !app.editor.dirty);
                assert(app.editing && app.form == GUI_FORM_THEME && app.editor.row == r);
                /* a click on the focused row opens it again, a click on its last row types */
                key(&app, SDLK_RETURN);
                mouse_px(&app, pk.rows[GUI_PICK_HEX].x + 4, pk.rows[GUI_PICK_HEX].y + 4, true);
                assert(app.editor.typing && app.editor.channel == GUI_PICK_HEX && SDL_TextInputActive(app.window));
                for (int i = 0; i < 7; ++i) key(&app, SDLK_BACKSPACE);
                typed(&app, "#102030");
                key(&app, SDLK_RETURN);
                assert(app.editor.picking && !app.editor.typing && !SDL_TextInputActive(app.window) &&
                       app.editor.dirty);
                assert(!strcmp(app.editor.text, "#102030") && app.editor.theme.page.r == 0x10);
                render(&app);
                c = pixel(&app, (float)(pk.rows[GUI_PICK_HEX].x + 8 * s / 2 + 10 * 8 * s + 4 * s),
                          (float)(pk.rows[GUI_PICK_HEX].y + 3 * s + 8 * s));
                assert(same_colour(c, night)); /* the colour now, beside the one it had */
                /* a click off the box keeps it, and does nothing else: the preview stays the menu */
                mouse_px(&app, preview.x + preview.w / 2, preview.y + 8 * s, true);
                assert(!app.editor.picking && app.editor.preview == GUI_SCREEN_MENU && app.editor.theme.page.g == 0x20);
                {
                    SDL_Event up = {0};
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    event(&app, &up);
                    assert(!app.pick_press);
                }
                /* a mouse's press on a bar whose lift never came: a finger after it still taps */
                key(&app, SDLK_RETURN);
                mouse_px(&app, pk.bars[0].x + pk.bars[0].w / 2, pk.bars[0].y + 2, true);
                key(&app, SDLK_ESCAPE);
                assert(!app.editor.picking && app.pick_press == 1);
                touch_box(&app, row_box(&app, r + 1, false));
                assert(app.editor.row == r + 1 && !app.pick_press);
                app.editor.row = r;
            }
            render(&app);
            { /* the preview's page, beside the boxes of the menu it draws */
                int w = SDL_min(preview.w, (f.info.y - f.place.side.y - 2 * 16 * app.scale) * 16 / 10);
                assert(same_colour(
                    pixel(&app, (float)(preview.x + (preview.w - w) / 2 + w / 160), (float)(preview.y + w * 5 / 16)),
                    night));
            }
            /* Ctrl+P, or a tap on the preview: a run in the theme, then the menu again; Tab, which goes to the next
               screen, keeps a theme not saved, saying so */
            key(&app, SDLK_TAB);
            assert(app.editing && app.form == GUI_FORM_THEME && !strncmp(app.editor.note, "Not saved: ", 11));
            key_with(&app, SDLK_P, SDL_KMOD_LCTRL);
            assert(app.editor.preview == GUI_SCREEN_COMPILING);
            render(&app);
            point(&app, f.place.side, true);
            assert(app.editor.preview == GUI_SCREEN_MENU);
            /* a space typed arrives as text: the key itself, Select elsewhere, does not take a text value */
            app.editor.row = 2; /* [theme] name, after Save and the folder */
            key(&app, SDLK_RETURN);
            typed(&app, "x");
            key(&app, SDLK_SPACE);
            assert(app.editor.typing);
            key(&app, SDLK_ESCAPE);
            assert(!app.editor.typing && strcmp(app.editor.theme.name, "Defaultx"));
            /* a click focuses a row, a click on the focused one changes it */
            point(&app, row_box(&app, 3, false), true);
            assert(app.editor.row == 3 && !app.editor.typing);
            /* ESC over changes asks first; Ctrl+S saves in themes/mytheme, and the menu's theme is it */
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.editor.leaving);
            key_with(&app, SDLK_S, SDL_KMOD_LCTRL);
            assert(snx_is_file(saved) && !app.editor.dirty && !strcmp(app.theme.id, "mytheme"));
            assert(same_colour(rgb(app.theme.page), night));
            {
                size_t size = 0;
                char* kept = (char*)snx_read_file(app.app_file, &size);
                assert(kept && strstr(kept, "\ntheme = mytheme\n")); /* the next start opens on it too */
                free(kept);
            }
            key(&app, SDLK_ESCAPE); /* nothing left unsaved: it leaves at once, back to App */
            assert(app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_EDITOR);
            leave_editor(&app);
            render(&app);
            shows(&app, "- Share log", normal); /* on its own page */
            assert(same_colour(pixel(&app, 2, 2), night));
            /* opened on a theme of themes/, it saves there under its own name */
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && !strcmp(app.editor.id, "mytheme") && !strcmp(app.editor.file.base, "default"));
            key(&app, SDLK_ESCAPE);
            assert(app.form == GUI_FORM_APP);
            leave_editor(&app);
            free(saved);
        }

        /* YoRHa loading: a run as steps, typed in, with the ring, the dots and the bar; the run fades in, and out */
        steps_input = snx_path_join(temporary, "option flags steps.nro");
        assert(!snx_write_file(steps_input, "fixture", 7));
        gui_theme_load(&app.theme, (const char* const*)app.themes_dirs, 2, "yorha");
        app.menu.selected = GUI_SWITCH;
        launcher(&app, true);
        launch(&app, snx_strdup(steps_input), false);
        assert(app.menu.running && app.fade_ms);
        for (until = SDL_GetTicks() + 10000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) {
            pump(&app);
            render(&app);
        }
        assert(app.finished && !app.abnormal_exit && app.steps.count == 2 && screen(&app) == GUI_SCREEN_LOADING);
        run = &app.theme.run_colors;
        lp = rgb(run->page);
        li = rgb(run->ink);
        ls = rgb(run->soft);
        shown = app.steps.step[1].shown_ms;
        faded = app.fade_ms;
        later = (shown + 60000) / 1600 * 1600; /* where turning dots would show none: the "..." is the end's */
        /* entering: the menu stays and fades out over fade_out_ms, then the run fades in */
        entered = app.entering_ms;
        assert(entered && faded == entered + app.theme.fade_out_ms);
        app.clock = entered;
        render(&app);
        shows_big(&app, "SCARLETNX  /  MENU", rgb(app.theme.ink), 2);
        hides(&app, "Last output", rgb(app.theme.soft)); /* the run's output waits for the run */
        app.clock = entered + app.theme.fade_out_ms / 2;
        render(&app);
        dimmed = pixel(&app, 4, 4); /* the page, half way to black */
        assert(!same_colour(dimmed, rgb(app.theme.ink)) && !same_colour(dimmed, black));
        app.clock = entered + app.theme.fade_out_ms;
        render(&app);
        assert(same_colour(pixel(&app, 4, 4), black));
        app.theme.texture = false; /* exact backgrounds first */
        app.clock = later;
        render(&app);
        colored_text_on(&app, 1, 1, "BOOTING", li, lp);
        colored_text_on(&app, 9, 1, "OPTION FLAGS...", ls, lp);
        colored_text_on(&app, 1, 3, "RECOMPILE: OPTION FLAGS", ls, lp);
        colored_text_on(&app, 24, 3, "...OK", li, lp);
        colored_text_on(&app, 1, 4, "LAUNCH: OPTION FLAGS...OK", li, lp);
        colored_text_on(&app, 1, app.rows - 1, "Press [ESC] to return to menu", ls, lp);
        /* the run's last line as a prompt, when the theme says so: done, failed and aborting each on its own switch */
        app.theme.prompt.blink = false;
        app.theme.prompt.lines[GUI_PROMPT_DONE] = true;
        render(&app);
        shows(&app, "[ Press [ESC] to return to menu ]", ls); /* in the middle of its row */
        app.abnormal_exit = true;
        render(&app);
        colored_text_on(&app, 1, app.rows - 1, "Press [ESC] to return to menu    ", ls, lp);
        app.theme.prompt.lines[GUI_PROMPT_FAILED] = true;
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        colored_text_on(&app, app.cols - 34, app.rows - 1, "[ Press [ESC] to return to menu ]", ls, lp);
        app.abnormal_exit = app.finished = false;
        app.menu.running = app.aborting = true;
        app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
        render(&app);
        colored_text_on(&app, app.cols - 29, app.rows - 1, "[ Aborting process tree... ]", rgb(run->warning), lp);
        app.menu.running = app.aborting = false;
        app.finished = true;
        app.theme.prompt = gui_theme_builtin("yorha")->prompt;
        cell = 16.0f * (float)app.scale;
        /* the menu's footer over the ESC line, in the run's colours: YoRHa's bar style, a band of its ink */
        colored_text_on(&app, 1, app.rows - 3, "GUI + children: ", lp, li);
        colored_text_on(&app, 1, app.rows - 2, "CWD: ", lp, li);
        app.theme.footer = GUI_STYLE_PLAIN;
        render(&app);
        colored_text_on(&app, 1, app.rows - 2, "CWD:", ls, lp);
        app.theme.footer = GUI_STYLE_NONE;
        render(&app);
        colored_text_on(&app, 1, app.rows - 2, "    ", ls, lp);
        app.theme.footer = GUI_STYLE_BAR;
        colored_text_on(&app, app.cols - 6, app.rows - 5, "    ", li, lp); /* YoRHa has no bar unless asked */
        colored_text_on(&app, 1, app.rows - 4, "            ", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 5) + cell / 2), lp));
        app.theme.bar = true;
        render(&app);
        colored_text_on(&app, app.cols - 6, app.rows - 5, "100%", li, lp); /* above the footer */
        colored_text_on(&app, 1, app.rows - 4, "OPTION FLAGS", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 5) + cell / 2), li));
        colored_text_on(&app, 1, app.rows - 2, "CWD: ", lp, li);
        /* typing: five characters in, the cursor after them; async, each line from when it came. The run is over, so
           a row types whole in typing_ms: 25 characters in 10 ms */
        app.theme.typing_sync = false;
        app.theme.typing_ms = 10;
        app.theme.fade_in_ms = app.theme.fade_out_ms = 0;
        app.theme.blink_ms = 0;
        launch_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = entered + 1; /* it came while the menu still showed: it types once the run is in */
        app.clock = faded + 2;
        render(&app);
        colored_text_on(&app, 1, 4, "LAUNC_", li, lp);
        app.steps.step[1].shown_ms = launch_shown;
        app.fade_ms = 0; /* in view from the start, for the lines below */
        app.clock = shown + 2;
        render(&app);
        colored_text_on(&app, 1, 4, "LAUNC_", li, lp);
        /* sync: one line at a time; the second waits for the first */
        app.theme.typing_sync = true;
        first_shown = app.steps.step[0].shown_ms;
        second_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = first_shown; /* both lines came at once */
        app.clock = first_shown + 2;
        render(&app);
        colored_text_on(&app, 1, 3, "RECOM", ls, lp); /* not the last line: soft, with the cursor typing it */
        colored_text_on(&app, 6, 3, "_", li, lp);
        colored_text_on(&app, 1, 4, "    ", li, lp);
        app.theme.typing_sync = false;
        app.steps.step[1].shown_ms = second_shown;
        /* the newest row of a step still running types a character every typing_ms; a done step's, or one a newer
           row of its step passed, whole in typing_ms; one above the screen at once, the visible ones from when they
           came: a fast log keeps the cursor on its newest lines */
        {
            static size_t total[200], typed[200];
            static Uint64 came[200];
            static int slow[200];
            GuiTheme t = *gui_theme_builtin("yorha");
            int k;
            t.typing_ms = 10;
            for (k = 0; k < 200; ++k) {
                total[k] = 50;
                came[k] = 1000;
                slow[k] = 0;
            }
            slow[199] = 1; /* 200 rows at once, the last its running step's newest; rows 180 to 199 on the screen */
            assert(t.typing_sync && t.line_in == GUI_LINE_TYPING);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 5, typed) == 180);
            assert(typed[0] == 50 && typed[179] == 50 && typed[180] == 25 && typed[181] == 0);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 19 * 10 - 1, typed) == 198);
            assert(typed[180] == 50 && typed[198] == 45 && typed[199] == 0);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 19 * 10 + 30, typed) == 199 && typed[199] == 3);
            slow[199] = 0; /* the step is done: its 20 rows on the screen take 200 ms */
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 20 * 10 - 1, typed) == 199 && typed[199] == 45);
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 20 * 10, typed) == -1 && typed[199] == 50);
            slow[199] = 1;
            t.typing_sync = 0; /* async: each row from when it came, at its own speed */
            assert(type_rows(&t, 200, 180, total, came, slow, 1000 + 30, typed) == 199);
            assert(typed[180] == 50 && typed[198] == 50 && typed[199] == 3);
            t.line_in = GUI_LINE_NONE;
            assert(type_rows(&t, 200, 180, total, came, slow, 1000, typed) == -1 && typed[180] == 50 &&
                   typed[199] == 50);
        }
        /* detailed: a step's lines under it, indented; collapse folds a done step's lines, never a failed one's */
        kept = app.steps;
        lf = rgb(run->failure);
        gui_steps_reset(&app.steps);
        detail = "Recompiling Game (1 modules)\n  compiled block 1/2: code_000 (1.0 s)\n"
                 "  compiled block 2/2: code_001 (1.0 s)\nBuilt games/Game\n"
                 "ScarletNX - Game\nfs: 3 files mapped\n";
        gui_steps_feed(&app.steps, detail, strlen(detail), shown);
        gui_steps_finish(&app.steps, false);
        app.clock = later;
        render(&app);
        colored_text_on(&app, 1, 3, "RECOMPILE: GAME", ls, lp);
        colored_text_on(&app, 16, 3, "...OK", li, lp);
        colored_text_on(&app, 3, 4, "compiled block 1/2: code_000 (1.0 s)", ls, lp);
        colored_text_on(&app, 3, 5, "compiled block 2/2: code_001 (1.0 s)", ls, lp);
        colored_text_on(&app, 1, 6, "BUILT: GAMES/GAME", ls, lp);
        colored_text_on(&app, 1, 7, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 8, "fs: 3 files mapped", ls, lp);
        app.theme.collapse = true;
        render(&app);
        colored_text_on(&app, 1, 4, "BUILT: GAMES/GAME", ls, lp);
        colored_text_on(&app, 1, 5, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 6, "fs: 3 files mapped", ls, lp);
        app.theme.detailed = false;
        render(&app);
        colored_text_on(&app, 1, 5, "LAUNCH: GAME...FAILED", lf, lp);
        colored_text_on(&app, 3, 6, "                  ", ls, lp);
        /* a step still running keeps its lines, folded or not; the dots and the cursor stay on the step's own row */
        app.theme.detailed = true;
        gui_steps_reset(&app.steps);
        live = "Recompiling Live (1 modules)\n";
        gui_steps_feed(&app.steps, live, strlen(live), shown);
        gui_steps_feed(&app.steps, "Functions: 2\n", 13, shown + 1000);
        app.clock = shown + 1000 + 3 * app.theme.typing_ms + 1; /* a line types from when it came */
        render(&app);
        colored_text_on(&app, 3, 4, "Fun", ls, lp);
        colored_text_on(&app, 6, 4, "_         ", li, lp);
        app.fade_ms = shown + 2000; /* or once the run is in, when it came before */
        app.clock = shown + 2000 + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 3, 4, "Fun", ls, lp);
        colored_text_on(&app, 6, 4, "_         ", li, lp);
        app.fade_ms = 0;
        app.clock = later + 1200;
        render(&app);
        colored_text_on(&app, 16, 3, "..._", li, lp);
        colored_text_on(&app, 3, 4, "Functions: 2    ", ls, lp);
        /* two lines at once: the one the newer passed types whole, the newer a character every typing_ms */
        gui_steps_reset(&app.steps);
        gui_steps_feed(&app.steps, live, strlen(live), shown);
        gui_steps_feed(&app.steps, "Functions: 2\nBlocks: 3\n", 23, shown + 1000);
        app.clock = shown + 1000 + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 3, 4, "Functions: 2", ls, lp);
        colored_text_on(&app, 3, 5, "Blo", ls, lp);
        /* every line a step kept, down the screen; a row too wide for it ends at the edge */
        app.theme.collapse = app.theme.bar = false;
        gui_steps_reset(&app.steps);
        wide_name = "Recompiling A Game Whose Name Runs Far Past The Right Edge Of This Small Window (1 modules)\n";
        gui_steps_feed(&app.steps, wide_name, strlen(wide_name), shown);
        for (i = 0; i < 9; ++i) {
            char line[32];
            snprintf(line, sizeof line, "line %d\n", i);
            gui_steps_feed(&app.steps, line, strlen(line), shown);
        }
        wide = "fs: a line long enough to pass the right edge of a window eighty columns wide, and on, and on\n";
        gui_steps_feed(&app.steps, wide, strlen(wide), shown);
        gui_steps_finish(&app.steps, true);
        app.clock = later;
        render(&app);
        /* of the row's cols - 6, "RECOMPILE: ", the detail's space and "...OK" take 16 */
        snprintf(row_text, sizeof row_text, "RECOMPILE: %.*s", app.cols - 6 - 16,
                 "A GAME WHOSE NAME RUNS FAR PAST THE RIGHT EDGE OF THIS SMALL WINDOW");
        colored_text_on(&app, 1, 3, row_text, li, lp); /* the last step, in ink */
        colored_text_on(&app, app.cols - 10, 3, "...OK ", li, lp);
        colored_text_on(&app, 3, 4, "line 0", ls, lp);
        colored_text_on(&app, 3, 12, "line 8", ls, lp);
        snprintf(row_text, sizeof row_text, "%.*s...", app.cols - 8, wide);
        colored_text_on(&app, 3, 13, row_text, ls, lp);
        colored_text_on(&app, app.cols - 2, 13, " ", ls, lp);
        for (i = 9; i < 20; ++i) { /* past the bottom, the latest rows stay */
            char line[32];
            snprintf(line, sizeof line, "line %d\n", i);
            gui_steps_feed(&app.steps, line, strlen(line), shown);
        }
        render(&app);
        last_row = app.rows - 5; /* a blank row above the footer */
        colored_text_on(&app, 3, last_row, "line 19", ls, lp);
        colored_text_on(&app, 3, last_row - 1, "line 18", ls, lp);
        app.steps = kept;
        /* the page's faint lines, one pixel row in four */
        app.theme = *gui_theme_builtin("yorha");
        app.fade_ms = faded;
        app.clock = later;
        render(&app);
        x = 8.0f * (float)app.scale * (float)(app.cols - 10);
        y = cell * 10;
        assert(!same_colour(pixel(&app, x, y + 3), lp) && same_colour(pixel(&app, x, y + 2), lp));
        /* the fade in from black, over fade_in_ms */
        app.clock = faded;
        render(&app);
        assert(same_colour(pixel(&app, x, y + 2), black));
        app.clock = faded + app.theme.fade_in_ms / 2;
        render(&app);
        half = pixel(&app, x, y + 2);
        assert(!same_colour(half, black) && !same_colour(half, lp));
        /* ESC: the run fades out to black over fade_out_ms, then the menu fades in */
        app.clock = later;
        key(&app, SDLK_ESCAPE);
        assert(app.finished && app.leaving_ms == app.clock);
        app.clock += app.theme.fade_out_ms / 2;
        pump(&app);
        render(&app);
        assert(app.finished && !same_colour(pixel(&app, x, y + 2), lp));
        app.clock += app.theme.fade_out_ms / 2;
        pump(&app);
        assert(!app.finished && app.menu.view == GUI_LOAD_NONE && app.fade_ms == app.clock);
        render(&app);
        assert(same_colour(pixel(&app, x, y + 2), black));
        app.clock += app.theme.fade_in_ms;
        render(&app);
        shows_big(&app, "SCARLETNX  /  MENU", rgb(app.theme.ink), 2); /* the menus' own */
        app.clock = 0;
        app.theme = *gui_theme_builtin("default");
        launcher(&app, false);
        free(steps_input);
    }
    /* The menu: each entry a box of its own, the focused one framed in the focus colour (outline) or inverted with the
       cursor in the margin (fill); beside the list what the focus is about, the status under it; at the foot the tips,
       the same or the screen's message, beside the keys. A box under the pointer takes the focus and a click selects
       it; many entries scroll, and a narrow window puts the side box under the list */
    {
        GuiMenuBoxes m;
        GuiRect b, r;
        int s, cw, focus, exit_entry;
        SDL_Color dim, page, ink;
        char* keys_folder = snx_dirname(app.keys_path);
        app.theme = *gui_theme_builtin("default");
        assert(app.theme.panels == GUI_PANELS_OUTLINE && !app.theme.cursor[0] && !app.theme.tips_box);
        /* keys in use, so Setup device keys has its usual look, not the red of a start without them */
        assert(keys_folder && !snx_mkdirs(keys_folder) && !snx_write_file(app.keys_path, "header_key = 00\n", 16));
        free(keys_folder);
        app.menu.selected = GUI_KEYS;
        app.message[0] = 0;
        app.abnormal_exit = false;
        app.fade_ms = 0; /* the last run's fade in, on the clock it had */
        render(&app);
        menu_boxes(&app, &m);
        s = app.scale;
        cw = 8 * s;
        dim = rgb(app.theme.dim);
        focus = GUI_KEYS;
        exit_entry = GUI_EXIT;
        assert(focus >= 0 && m.list.focus == focus && exit_entry > focus && !m.place.stacked);
        b = list_box(&m.list, focus);
        assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), yellow)); /* framed in the focus colour */
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Setup device keys", yellow, black);
        b = list_box(&m.list, exit_entry);
        assert(same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), dim)); /* the others in the dim one */
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Exit", normal, black);
        /* the side box: the focus's name over a rule, what it is about and the keys in use; the status box under it */
        r = m.place.side;
        assert(r.x >= m.place.list.x + m.place.list.w && r.h > 0 && m.place.status.y >= r.y + r.h);
        text_px_is(&app, r.x + cw, r.y + 2 * s, "Setup device keys", normal, black);
        text_px_is(&app, r.x + cw, r.y + head_h(&app) + 2 * s, "Installs prod.keys", normal, black);
        text_px_is(&app, m.place.status.x + cw, m.place.status.y + 2 * s, "Status", normal, black);
        /* the tips on a row of their own under the head, what the focus is about or the screen's message, a failure's
           in its colour; the keys alone at the foot, the whole width */
        r = m.place.tips;
        assert(app.theme.tips_place == GUI_TIPS_TOP && r.x == m.place.head.x && r.w == m.place.head.w);
        assert(r.y > m.place.tabs.y + m.place.tabs.h && r.y + r.h < m.place.list.y);
        assert(m.place.keys.x == m.place.head.x && m.place.keys.w == m.place.head.w && m.place.keys.y > r.y);
        text_px_is(&app, r.x, r.y + 2 * s, "Installs prod.keys", normal, black);
        snprintf(app.message, sizeof app.message, "Program ended abnormally (status 7).");
        app.abnormal_exit = true;
        render(&app);
        text_px_is(&app, r.x, r.y + 2 * s, "Program ended abnormally", red, black);
        app.message[0] = 0;
        app.abnormal_exit = false;
        /* in the foot with [tips] place = bottom: under the list, the keys beside them, against the right edge, each key
           in the focus colour; the default's on the page, no box */
        app.theme.tips_place = GUI_TIPS_BOTTOM;
        render(&app);
        menu_boxes(&app, &m);
        r = m.place.tips;
        assert(r.y == m.place.keys.y && r.x == m.place.list.x && r.w == m.place.list.w &&
               m.place.keys.x == m.place.side.x);
        assert(r.y > m.place.list.y + m.place.list.h && r.y + r.h <= 800 - 8 * s);
        { /* the tips and the keys in the foot's letters, a size under the rest's; Tab's the last key, against the right
             edge of the keys' last row */
            int fs = foot_scale(&app), rows;
            assert(fs == s - 1);
            foot_px_is(&app, r.x, r.y + 2 * fs, "Installs prod.keys", normal, black);
            r = m.place.keys;
            rows = (r.h - 4 * fs) / (16 * fs);
            assert(rows >= 1 && r.h == rows * 16 * fs + 4 * fs);
            shows_foot(&app, "ESC", yellow);
            shows_foot(&app, "exit", normal);
            hides(&app, "ESC", yellow);
            foot_px_is(&app, r.x + r.w - 10 * 8 * fs, r.y + 2 * fs + (rows - 1) * 16 * fs, "Tab", yellow, black);
            app.menu.text_size = 1; /* 1x: the foot's too, never under it */
            render(&app);
            assert(app.scale == 1 && foot_scale(&app) == 1);
            shows(&app, "ESC", yellow);
            app.menu.text_size = 0;
            render(&app);
            menu_boxes(&app, &m);
        }
        /* the screens as tabs over it all, the one shown framed in the focus colour */
        r = m.place.tabs;
        assert(r.h && r.y + r.h <= m.place.list.y);
        text_px_is(&app, tab_rect(&app, r, GUI_TAB_MENU).x + cw, r.y + 2 * s, "Menu", yellow, black);
        text_px_is(&app, tab_rect(&app, r, GUI_TAB_GAMES).x + cw, r.y + 2 * s, "Games", normal, black);
        assert(same_colour(pixel(&app, (float)tab_rect(&app, r, GUI_TAB_MENU).x, (float)(r.y + r.h / 2)), yellow));
        /* Tab goes round them, Shift+Tab back; a click on one opens it, as a pad's shoulders do (screen_tab) */
        key(&app, SDLK_TAB);
        assert(app.menu.view == GUI_LOAD_GAMES && !app.editing);
        key(&app, SDLK_TAB);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        key(&app, SDLK_TAB);
        assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
        key_with(&app, SDLK_TAB, SDL_KMOD_LSHIFT);
        assert(app.editing && app.form == GUI_FORM_SETTINGS);
        screen_tab(&app, -1);
        assert(!app.editing && app.menu.view == GUI_LOAD_GAMES);
        render(&app);
        r = tab_rect(&app, m.place.tabs, GUI_TAB_MENU);
        mouse_px(&app, r.x + 4, r.y + 4, true);
        assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
        app.menu.selected = GUI_KEYS;
        render(&app);
        r = m.place.tips;
        /* the screen's message takes the tips, a failure's in its colour */
        snprintf(app.message, sizeof app.message, "Program ended abnormally (status 7).");
        app.abnormal_exit = true;
        render(&app);
        foot_px_is(&app, r.x, r.y + 2 * foot_scale(&app), "Program ended abnormally", red, black);
        app.message[0] = 0;
        app.abnormal_exit = false;
        /* the pointer: over Exit's box it takes the focus, a click on Setup device keys opens its screen */
        b = list_box(&m.list, exit_entry);
        mouse_px(&app, b.x + 4, b.y + 4, false);
        assert(app.menu.selected == GUI_EXIT);
        b = list_box(&m.list, focus);
        mouse_px(&app, b.x + 4, b.y + 4, true);
        assert(app.menu.view == GUI_LOAD_KEYS);
        key(&app, SDLK_ESCAPE);
        assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_KEYS);
        /* Games: Add a folder, then as many of a page's games as the list's box holds, the focused one's path beside
           them and in the bar; Right turns the page, ESC goes back */
        {
            GuiGamesBoxes g;
            size_t page;
            go_tab(&app, GUI_TAB_GAMES);
            assert(app.menu.view == GUI_LOAD_GAMES && !app.browser.selected && app.games.count > 12);
            render(&app);
            games_boxes(&app, &g);
            b = list_box(&g.add, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Add a folder", yellow,
                       black); /* the focus, first */
            key(&app, SDLK_DOWN);
            render(&app);
            games_boxes(&app, &g);
            page = games_page(&app);
            assert(app.browser.page_size == page && g.list.count == g.lead + (int)page && g.list.shown == g.list.count);
            b = list_box(&g.add, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Add a folder", normal, black);
            b = list_box(&g.list, g.lead);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- ", yellow, black);
            text_px_is(&app, b.x + cw / 2 + 2 * cw, item_text_y(&app, b), app.games.names[0], yellow, black);
            text_px_is(&app, g.place.side.x + cw, g.place.side.y + 2 * s, app.games.names[0], normal, black);
            {
                char start[9]; /* its path, from a drive's letter on Windows */
                snprintf(start, sizeof start, "%s", app.games.paths[0]);
                foot_px_is(&app, g.place.tips.x, g.place.tips.y + 2 * foot_scale(&app), start, normal, black);
            }
            key(&app, SDLK_RIGHT);
            assert(app.browser.page == 1);
            render(&app);
            games_boxes(&app, &g);
            assert(g.start == page);
            key(&app, SDLK_ESCAPE);
            assert(app.menu.view == GUI_LOAD_NONE && app.menu.selected == GUI_KEYS);
        }
        /* the keys' screen: one box with where they go, the line that waits in its middle and Back at its foot; a click
           in the box opens the picker, whose answer (none here) the bar says; a click on Back goes back */
        {
            GuiPickBoxes k;
            SDL_Event answer;
            Uint64 until;
            const char* waiting = "[ Pick or drop prod.keys ]";
            app.menu.selected = GUI_KEYS;
            key(&app, SDLK_RETURN);
            assert(app.menu.view == GUI_LOAD_KEYS);
            app.clock = 1000 * app.theme.blink_ms * 2; /* the line shown, not blinked away */
            render(&app);
            pick_boxes(&app, &k);
            r = inner(&app, k.place.list);
            text_px_is(&app, r.x, k.place.list.y + 2 * s, "Setup device keys", normal, black);
            text_px_is(&app, r.x + (r.w - (int)strlen(waiting) * cw) / 2, k.prompt, waiting, yellow, black);
            foot_px_is(&app, k.place.tips.x, k.place.tips.y + 2 * foot_scale(&app), "Setup device keys:", normal,
                       black);
            app.clock = 0;
            mouse_px(&app, r.x + 10, k.prompt + 4, true);
            for (until = SDL_GetTicks() + 2000; !app.message[0] && SDL_GetTicks() < until;)
                while (SDL_PollEvent(&answer)) event(&app, &answer);
            assert(app.menu.view == GUI_LOAD_KEYS && !strcmp(app.message, "No file was picked."));
            render(&app);
            foot_px_is(&app, k.place.tips.x, k.place.tips.y + 2 * foot_scale(&app), "No file was picked.", yellow,
                       black);
            b = list_box(&k.back, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.menu.view == GUI_LOAD_NONE);
            app.message[0] = 0;
        }
        /* the Settings: the groups on the left, beside them the focused group's rows; Enter opens it, its rows taking
           the focus and the group marked chosen; a click on another group opens that one, one on a row focuses it and
           a second changes it; the bar says what the focused row is */
        {
            GuiFormBoxes f;
            int launch, row;
            open_settings(&app);
            assert(app.editing && app.form == GUI_FORM_SETTINGS && !app.editor.row && app.groups > 3);
            render(&app);
            form_boxes(&app, &f);
            assert(f.place.side.x > f.place.list.x + f.place.list.w && f.left.count == app.groups + 1);
            b = list_box(&f.left, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- App", yellow, black);
            b = list_box(&f.left, app.groups); /* the last row, whole */
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Reset all to the defaults", normal, black);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "App", normal, black);
            /* the note at the foot of the rows whole, wrapped to the box: at 2x and at 1x */
            for (int size = 2; size >= 1; --size) {
                const char* note = form_note(&app);
                int y, room;
                app.menu.text_size = size == 1 ? 1 : 0; /* Auto is 2x here */
                render(&app);
                form_boxes(&app, &f);
                assert(app.scale == size && note);
                room = inner(&app, f.place.side).w / (8 * app.scale);
                assert(size == 1 || (int)strlen(note) > room); /* two rows at 2x */
                for (y = f.info.y; *note; y += 16 * app.scale) {
                    char row[96];
                    size_t n = wrap_at(note, room, 0);
                    snprintf(row, sizeof row, "%.*s", (int)n, note);
                    text_px_is(&app, f.info.x + 8 * app.scale, y, row, rgb(app.theme.soft), black);
                    for (note += n; *note == ' ';) ++note;
                }
                assert(y <= f.info.y + f.info.h && f.info.y >= list_box(&f.right, f.right.first + f.right.shown - 1).y);
            }
            app.menu.text_size = 0;
            render(&app);
            form_boxes(&app, &f);
            b = list_box(&f.right, GUI_APP_THEME);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Theme", normal,
                       black); /* no focus on the right yet */
            for (launch = 0; launch < app.groups && strcmp(app.group_keys[launch].key, "Launch"); ++launch) {}
            assert(launch < app.groups);
            app.editor.row = launch;
            render(&app);
            form_boxes(&app, &f);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "Launch", normal, black);
            text_px_is(&app, list_box(&f.right, 0).x + cw / 2, item_text_y(&app, list_box(&f.right, 0)), "- libgccjit",
                       normal, black);
            key(&app, SDLK_RETURN);
            assert(app.form == GUI_FORM_GROUP && app.group == launch && !app.editor.row);
            render(&app);
            form_boxes(&app, &f);
            b = list_box(&f.left, launch);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- Launch", yellow, black); /* the group open */
            b = list_box(&f.right, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "- libgccjit", yellow, black);
            {
                char start[16]; /* in the tips, its first row: in the foot here, in its letters */
                snprintf(start, sizeof start, "%s", app.settings.keys[0].about);
                shows_foot(&app, start, normal);
            }
            /* a click on the second row focuses it, a second click changes it */
            launcher(&app, false);
            b = list_box(&f.right, 1);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.form == GUI_FORM_GROUP && app.editor.row == 1 && setting_is(&app, "gdbstub", NULL));
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(setting_is(&app, "gdbstub", "on"));
            key(&app, SDLK_LEFT); /* back as it was */
            assert(setting_is(&app, "gdbstub", NULL));
            /* a click on App in the list opens it, from a group's rows */
            b = list_box(&f.left, 0);
            mouse_px(&app, b.x + 4, b.y + 4, true);
            assert(app.editing && app.form == GUI_FORM_APP && app.editor.row == GUI_APP_THEME);
            /* the theme editor: its keys on the left, the theme's menu in the preview on the right, which a click, or
               Ctrl+P, turns to a run; a long value being typed shows its end */
            app.editor.row = GUI_APP_EDITOR;
            key(&app, SDLK_RETURN);
            assert(app.form == GUI_FORM_THEME);
            render(&app);
            form_boxes(&app, &f);
            assert(f.left.count == gui_editor_rows(&app.editor) && f.left.shown < f.left.count);
            text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, "Preview", normal, black);
            mouse_px(&app, f.place.side.x + f.place.side.w / 2, f.place.side.y + f.place.side.h / 2, true);
            assert(app.editor.preview == GUI_SCREEN_COMPILING);
            key_with(&app, SDLK_P, SDL_KMOD_LCTRL);
            assert(app.editor.preview == GUI_SCREEN_MENU);
            /* the colour picker over the keys' box, under a head naming the key; a drag on a bar sets it, ESC puts
               it back */
            {
                GuiPicker pk;
                GuiRgb was;
                for (row = 0;
                     !gui_editor_key_of(&app.editor, row) || strcmp(gui_editor_key_of(&app.editor, row)->key, "ink");
                     ++row) {}
                app.editor.row = row;
                was = app.editor.theme.ink;
                key(&app, SDLK_RETURN);
                assert(app.editor.picking);
                render(&app);
                picker_place(&app, &pk);
                assert(pk.box.x >= f.place.list.x && pk.box.x + pk.box.w <= f.place.list.x + f.place.list.w);
                text_px_is(&app, pk.box.x + cw, pk.box.y + 2 * s, "[colors] ink", normal, black);
                text_px_is(&app, pk.rows[0].x + cw / 2, pk.rows[0].y + 3 * s, "R", yellow, black);
                text_px_is(&app, pk.rows[1].x + cw / 2, pk.rows[1].y + 3 * s, "G", normal, black);
                mouse_px(&app, pk.bars[2].x + s, pk.bars[2].y + 2, true);
                assert(app.editor.channel == GUI_PICK_BLUE && !app.editor.theme.ink.b);
                {
                    SDL_Event up = {0};
                    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
                    up.button.button = SDL_BUTTON_LEFT;
                    event(&app, &up);
                }
                render(&app);
                text_px_is(&app, pk.rows[2].x + cw / 2, pk.rows[2].y + 3 * s, "B", yellow, black);
                key(&app, SDLK_ESCAPE);
                assert(!app.editor.picking && !memcmp(&app.editor.theme.ink, &was, sizeof was));
                assert(app.editing && app.form == GUI_FORM_THEME);
            }
            for (row = 0;
                 !gui_editor_key_of(&app.editor, row) || strcmp(gui_editor_key_of(&app.editor, row)->key, "name");
                 ++row) {}
            app.editor.row = row;
            key(&app, SDLK_RETURN);
            assert(app.editor.typing);
            typed(&app, "a theme name long enough to run past its box's right edge");
            render(&app);
            form_boxes(&app, &f);
            b = list_box(&f.left, row);
            {
                int room = (b.w - cw * 3 / 2) / cw, end = b.x + cw / 2 + (room - 2) * cw;
                text_px_is(&app, end, item_text_y(&app, b), "e_", yellow, black); /* its end, and the cursor */
            }
            key(&app, SDLK_ESCAPE);
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.form == GUI_FORM_APP);
            leave_editor(&app);
        }
        /* a box over the screen is a box on the screen darkened: the exit question, its choices as boxes, a click on
           one taking it, a click off the box keeping on; the files an older version left, OK focused, a key closing it */
        {
            GuiList choices;
            app.menu.selected = GUI_EXIT;
            render(&app);
            menu_boxes(&app, &m);
            b = list_box(&m.list, m.list.focus);
            key(&app, SDLK_ESCAPE);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            render(&app);
            ask_boxes(&app, &r, &choices);
            text_px_is(&app, r.x + cw, r.y + 4 * s, "Exit ScarletNX?", rgb(app.theme.warning), black);
            text_px_is(&app, list_box(&choices, 0).x + cw / 2, item_text_y(&app, list_box(&choices, 0)), "- Stay",
                       yellow, black);
            assert(b.x < r.x && /* its left edge, off the question's box, darkened */
                   same_colour(pixel(&app, (float)b.x, (float)(b.y + b.h / 2)), (SDL_Color){0x7F, 0x7F, 0, 255}));
            mouse_px(&app, list_box(&choices, 1).x + 4, list_box(&choices, 1).y + 4, false);
            assert(app.confirm.shown && app.confirm.selected == GUI_CONFIRM_ABORT);
            mouse_px(&app, r.x + r.w + 8, r.y + r.h + 8, true);
            assert(!app.confirm.shown && !app.menu.quit);
            key(&app, SDLK_ESCAPE);
            render(&app);
            mouse_px(&app, list_box(&choices, 1).x + 4, list_box(&choices, 1).y + 4, true);
            assert(!app.confirm.shown && app.menu.quit);
            app.menu.quit = false;
            {
                char* elsewhere[] = {"/elsewhere"}; /* a count for another list is not this one's */
                space_count(&app.left_space, &app.left_counter, elsewhere, 1, true);
                while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
            }
            app.left = snx_format("/old/recent.txt" SAY_LEFT_BY "/new/recent.txt\n");
            app.lefts = 1;
            app.left_shown = true;
            app.left_at = SDL_GetTicks(); /* not looked for again, as there is no such file */
            render(&app);
            {
                int width, height;
                area(&app, &width, &height);
                r = over_rect(&app, width / cw - 6, head_h(&app) + 2 * 16 * s + item_h(&app) + 16 * s);
                text_px_is(&app, r.x + cw, r.y + 2 * s, "Left by older versions", normal, black);
                text_px_is(&app, r.x + cw, r.y + head_h(&app) + 4 * s, "- /old/recent.txt", red, black);
                /* its space, not counted: the menu counts what it looked for itself */
                text_px_is(&app, r.x + cw + 2 * cw, r.y + head_h(&app) + 4 * s + 16 * s, "counting",
                           rgb(app.theme.soft), black);
            }
            key(&app, SDLK_DOWN);
            assert(!app.left_shown && app.menu.selected == GUI_EXIT); /* the key closed it and did nothing else */
            free(app.left);
            app.left = NULL;
            app.lefts = 0;
            app.menu.selected = GUI_KEYS;
        }
        /* a run of the console layout: the driver's output in its box, sized to it, beside it what runs and how it
           ended, the ESC line in the bar, where a finger's tap aborts a run */
        {
            GuiPlace p;
            Uint64 until;
            int cols, rows;
            char* hung = snx_path_join(temporary, "hang in join boxed.elf");
            bool ready = false;
            assert(!snx_write_file(hung, "", 0));
            app.menu.selected = GUI_LINUX;
            launch(&app, snx_strdup(hung), false);
            for (until = SDL_GetTicks() + 5000; !ready && SDL_GetTicks() < until; SDL_Delay(5)) {
                size_t got = 0;
                unsigned char* data = snx_read_file(hung, &got);
                ready = data && got == 5 && !memcmp(data, "ready", 5);
                free(data);
                pump(&app);
            }
            assert(ready && app.menu.running);
            render(&app);
            run_place(&app, &p);
            run_console(&app, &p, &cols, &rows);
            assert(app.console.cols == cols && app.console.rows == rows && !p.tabs.h);
            text_px_is(&app, p.list.x + cw, p.list.y + 2 * s, "Output", normal, black);
            text_px_is(&app, p.side.x + cw, p.side.y + 2 * s, "Load Linux ARM ELF", normal, black);
            r = foot_room(&app, p.keys, app.theme.footer_box);
            foot_px_is(&app, r.x + r.w - (int)strlen(SAY_ABORT) * 8 * foot_scale(&app), r.y, SAY_ABORT, red, black);
            touch_box(&app, p.keys);
            assert(app.aborting);
            for (until = SDL_GetTicks() + 3000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
            assert(!app.menu.running && !app.finished);
            free(hung);
            /* one that ends: its message beside the output, the ESC line to go back */
            app.menu.selected = GUI_SWITCH;
            launch(&app, snx_strdup(input), false);
            for (until = SDL_GetTicks() + 5000; app.menu.running && SDL_GetTicks() < until; SDL_Delay(5)) pump(&app);
            assert(app.finished && !app.abnormal_exit);
            render(&app);
            run_place(&app, &p);
            text_px_is(&app, p.list.x + cw, p.list.y + head_h(&app) + 2 * s, "driver std", normal, black);
            text_px_is(&app, p.side.x + cw,
                       p.side.y + head_h(&app) + 2 * s +
                           wrap_rows(&app, app.file, (p.side.w - 2 * cw) / cw, 1) * 16 * s + 8 * s,
                       "Driver exited with status 0.", yellow, black);
            r = foot_room(&app, p.keys, app.theme.footer_box);
            foot_px_is(&app, r.x, r.y, SAY_DONE, red, black);
            key(&app, SDLK_ESCAPE);
            assert(!app.finished && app.menu.view == GUI_LOAD_NONE);
            pump(&app);
            app.menu.selected = GUI_KEYS;
        }
        /* each key the foot shows is a button: a click or a tap on its word presses it as the keyboard does, each word
           between slashes its own; its text, when its words all press one key, that key. A word of every screen's keys
           presses one, or is one that names none */
        {
            static const char* const says[] = {SAY_MENU_KEYS, SAY_GAMES,    SAY_KEYS_HOW,   SAY_EDITOR,
                                               SAY_PICKER,    SAY_SETTINGS, SAY_GROUPS,     SAY_APP,
                                               SAY_FOLDERS,   SAY_PAD,      " Tab: screen."};
            static const char* const none[] = {"Arrows", "mouse", "pad"};
            char key_of[128], what[128];
            Uint8 was;
            int launch;
            for (size_t i = 0; i < SDL_arraysize(says); ++i)
                for (const char* at = says[i]; hint_next(&at, key_of, what, sizeof key_of);)
                    for (const char *word = key_of, *end; *word; word = *end ? end + 1 : end) {
                        size_t n, k;
                        end = strchr(word, '/');
                        if (!end) end = word + strlen(word);
                        n = (size_t)(end - word);
                        for (k = 0; k < SDL_arraysize(none) && (strlen(none[k]) != n || strncmp(none[k], word, n));
                             ++k) {}
                        if (hint_word(word, n) < 0 && k == SDL_arraysize(none))
                            fprintf(stderr, "gui_sdl.c: the foot's key \"%.*s\" presses nothing\n", (int)n, word);
                        assert(hint_word(word, n) >= 0 || k < SDL_arraysize(none));
                    }
            app.theme = *gui_theme_builtin("default");
            app.menu.view = GUI_LOAD_NONE;
            app.menu.selected = GUI_SETTINGS;
            app.message[0] = 0;
            /* the menu: ESC asks before closing, the pointer over it presses nothing; Arrows/mouse name no one key;
               select, the text of three words that all select, opens the Settings, where ESC comes back */
            press_key(&app, "ESC", 0, false, yellow);
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            key(&app, SDLK_ESCAPE);
            assert(!app.confirm.shown && !app.menu.quit);
            {
                int x, y, scale = app.scale;
                render(&app);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "ESC", yellow, 1, &x, &y));
                app.scale = scale;
                mouse_px(&app, x + 4, y + 4, false);
                assert(!app.confirm.shown);
            }
            press_key(&app, "Arrows/mouse", 1, false, yellow);
            assert(app.menu.selected == GUI_SETTINGS && !app.editing);
            press_key(&app, "Space/Enter/click", 20, false, yellow);
            assert(app.editing && app.form == GUI_FORM_SETTINGS);
            press_key(&app, "ESC", 0, false, yellow);
            assert(!app.editing && app.menu.view == GUI_LOAD_NONE);
            { /* the keys' box's whole height is their rows': a click on its top edge, over Tab, opens Games */
                GuiMenuBoxes mb;
                int x, y, scale = app.scale;
                render(&app);
                menu_boxes(&app, &mb);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "Tab", yellow, 1, &x, &y));
                app.scale = scale;
                assert(y > mb.place.keys.y);
                mouse_px(&app, x + 4, mb.place.keys.y, true);
                assert(app.menu.view == GUI_LOAD_GAMES);
                go_tab(&app, GUI_TAB_MENU);
            }
            { /* the mouse over a key of the foot lights its sentence, inverted, when a click there presses something;
                 not over Arrows/mouse, which names no one key, and not once it leaves the window */
                SDL_Event left = {0};
                int x, y, scale = app.scale;
                SDL_Color page = rgb(app.theme.page);
                render(&app);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "Tab", yellow, 1, &x, &y));
                app.scale = scale;
                shows_foot(&app, "screen", normal);
                mouse_px(&app, x + 4, y + 4, false);
                assert(app.menu.view == GUI_LOAD_NONE); /* a hover presses nothing */
                render(&app);
                shows_foot(&app, "Tab", page);
                shows_foot(&app, "screen", page);
                hides_foot(&app, "Tab", yellow);
                render(&app);
                app.scale = foot_scale(&app);
                assert(find_text(&app, "Arrows/mouse", yellow, 1, &x, &y));
                app.scale = scale;
                mouse_px(&app, x + 4, y + 4, false);
                render(&app);
                shows_foot(&app, "Arrows/mouse", yellow);
                shows_foot(&app, "Tab", yellow);
                left.type = SDL_EVENT_WINDOW_MOUSE_LEAVE;
                app.hovering = true;
                event(&app, &left);
                assert(!app.hovering);
            }
            /* a tap on Tab opens Games, where each word of Left/Right/PgUp/PgDown turns the page its way, and its text,
               of words two ways, none; Up/Down's each move the focus */
            press_key(&app, "Tab", 0, true, yellow);
            assert(app.menu.view == GUI_LOAD_GAMES && app.browser.page == 0 && gui_browser_pages(&app.browser) > 1);
            press_key(&app, "Left/Right/PgUp/PgDown", 18, false, yellow); /* PgDown */
            assert(app.browser.page == 1);
            press_key(&app, "Left/Right/PgUp/PgDown", 24, false, yellow); /* page */
            assert(app.browser.page == 1);
            press_key(&app, "Left/Right/PgUp/PgDown", 12, false, yellow); /* PgUp */
            assert(app.browser.page == 0);
            press_key(&app, "Left/Right/PgUp/PgDown", 10, false, yellow); /* Right's slash */
            assert(app.browser.page == 1);
            press_key(&app, "Left/Right/PgUp/PgDown", -1, false, yellow); /* the margin before Left */
            assert(app.browser.page == 0);
            app.browser.selected = 0;
            press_key(&app, "Up/Down", 3, false, yellow);
            assert(app.browser.selected == 1);
            press_key(&app, "Up/Down", 0, false, yellow);
            assert(app.browser.selected == 0);
            /* Android's words: a swipe across turns the page, Back goes back as ESC, a tap selects; Tap outside, the
               picker's, presses nothing (a tap off the picker keeps the colour) */
            assert(hint_press(&app, "Swipe across", 0) && app.browser.page == 1);
            assert(!hint_press(&app, "Tap outside", 0));
            go_tab(&app, GUI_TAB_MENU);
            assert(hint_press(&app, "Back", 0) && app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            key(&app, SDLK_ESCAPE);
            app.menu.selected = GUI_KEYS;
            assert(hint_press(&app, "Tap", 0) && app.menu.view == GUI_LOAD_KEYS);
            /* the keys' screen: ESC goes back */
            press_key(&app, "ESC", 0, false, yellow);
            assert(app.menu.view == GUI_LOAD_NONE && !app.confirm.shown);
            /* a group's settings: Up/Down, Left/Right change the row, Del puts it back, ESC leaves the rows, then the
               Settings */
            open_settings(&app);
            for (launch = 0; launch < app.groups && strcmp(app.group_keys[launch].key, "Launch"); ++launch) {}
            assert(launch < app.groups);
            app.editor.row = launch;
            press_key(&app, "Enter", 0, false, yellow);
            assert(app.form == GUI_FORM_GROUP && app.group == launch && !app.editor.row);
            press_key(&app, "Up/Down", 5, false, yellow);
            assert(app.editor.row == 1 && !strcmp(app.settings.keys[app.first + 1].key, "gdbstub"));
            assert(gui_settings_set(&app.settings, "gdbstub", "off"));
            press_key(&app, "Left/Right", 6, false, yellow);
            assert(setting_is(&app, "gdbstub", "on"));
            press_key(&app, "Del", 0, false, yellow);
            assert(setting_is(&app, "gdbstub", NULL));
            press_key(&app, "ESC", 0, false, yellow);
            assert(app.editing && app.form == GUI_FORM_SETTINGS);
            press_key(&app, "ESC", 0, false, yellow);
            assert(!app.editing);
            /* the colour picker, its keys over the foot as ever: Right steps the channel, ESC puts the colour back; a
               finger on Enter keeps it as it goes down, its lift no tap that would open the picker again */
            app_select(&app, GUI_APP_EDITOR);
            assert(app.editing && app.form == GUI_FORM_THEME);
            for (app.editor.row = 0; !gui_editor_key_of(&app.editor, app.editor.row) ||
                                     strcmp(gui_editor_key_of(&app.editor, app.editor.row)->key, "page");
                 ++app.editor.row) {}
            key(&app, SDLK_RETURN);
            assert(app.editor.picking && app.editor.channel == GUI_PICK_RED && app.editor.theme.page.r < 255);
            was = app.editor.theme.page.r;
            press_key(&app, "Left/Right", 6, false, yellow);
            assert(app.editor.picking && app.editor.theme.page.r == was + 1);
            press_key(&app, "ESC", 0, false, yellow);
            assert(!app.editor.picking && app.editor.theme.page.r == was && app.form == GUI_FORM_THEME);
            key(&app, SDLK_RETURN);
            assert(app.editor.picking);
            press_key(&app, "Enter", 0, true, yellow);
            assert(!app.editor.picking && app.editing && app.form == GUI_FORM_THEME && !app.pick_press);
            key(&app, SDLK_ESCAPE);
            assert(app.editing && app.form == GUI_FORM_APP);
            leave_editor(&app);
            /* the game folders: Add a folder opens the folder picker, which answers none here */
            app_select(&app, GUI_APP_FOLDERS);
            assert(app.editing && app.form == GUI_FORM_FOLDERS && !app.editor.note[0]);
            press_key(&app, "Add a folder", 0, false, yellow);
            {
                SDL_Event answer;
                Uint64 until = SDL_GetTicks() + 2000;
                while (!app.editor.note[0] && SDL_GetTicks() < until) /* its answer comes on the picker's thread */
                    while (SDL_PollEvent(&answer)) event(&app, &answer);
            }
            assert(!strcmp(app.editor.note, "No folder was picked.") ||
                   !strncmp(app.editor.note, "The folder picker could not open: ", 34));
            leave_editor(&app);
            /* with the tips beside them, and in YoRHa's box on the band, in its page colour */
            app.theme.tips_place = GUI_TIPS_BOTTOM;
            press_key(&app, "Tab", 0, false, yellow);
            assert(app.menu.view == GUI_LOAD_GAMES);
            go_tab(&app, GUI_TAB_MENU);
            app.theme = *gui_theme_builtin("yorha");
            app.theme.grid = false;
            press_key(&app, "ESC", 0, false, rgb(app.theme.page));
            assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT);
            key(&app, SDLK_ESCAPE);
            app.theme = *gui_theme_builtin("default");
            app.menu.selected = GUI_KEYS;
        }
        /* YoRHa: the boxes on its dim colour, the focused one on its focus colour with the cursor in the margin, the
           side box's name on a band of its ink */
        app.theme = *gui_theme_builtin("yorha");
        assert(app.theme.panels == GUI_PANELS_FILL && !strcmp(app.theme.cursor, "\xe2\x96\xba "));
        /* the page behind the boxes: its grid in the dim colour under them */
        {
            SDL_Color plain = rgb(app.theme.page), c;
            int width, height;
            render(&app);
            area(&app, &width, &height);
            c = pixel(&app, 0, (float)(height / 2 + 1)); /* the grid's first column, beside the boxes */
            assert(!same_colour(c, plain) && c.r < plain.r && c.r > app.theme.dim.r);
            app.theme.grid = false; /* exact colours below */
        }
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        app.clock = 1; /* the tips at their start, before they slide */
        render(&app);
        menu_boxes(&app, &m);
        b = list_box(&m.list, m.list.focus);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe Setup device keys", page, rgb(app.theme.focus));
        text_px_is(&app, m.list.at.x, item_text_y(&app, b), "\x10", rgb(app.theme.focus), page);
        b = list_box(&m.list, GUI_EXIT);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe Exit", ink, dim = rgb(app.theme.dim));
        r = m.place.side;
        text_px_is(&app, r.x + cw, r.y + 2 * s, "Setup device keys", page, ink);
        r = foot_text(&app, m.place.tips, app.theme.tips_box); /* the tips in their box under the head, off the band */
        assert(m.place.tips.y < m.place.list.y);
        text_px_is(&app, r.x, r.y, "Installs prod.keys", ink, page);
        app.theme.tips_place = GUI_TIPS_BOTTOM; /* in the foot: on its band, in the page's colour */
        render(&app);
        menu_boxes(&app, &m);
        r = foot_room(&app, m.place.tips, app.theme.tips_box);
        foot_px_is(&app, r.x, r.y, "Installs prod.keys", page, ink);
        /* on one row of the foot, the rest of a long one out of it until it slides there; [tips] wrap puts the rest on
           the foot's next rows */
        snprintf(app.message, sizeof app.message,
                 "The tips take a row of the foot: a long line slides, or wraps over its rows.");
        assert((int)strlen(app.message) > r.w / (8 * foot_scale(&app)));
        render(&app);
        foot_px_is(&app, r.x, r.y, "The tips take", page, ink);
        hides_foot(&app, "its rows.", page);
        for (app.clock = 1; gui_slide(1000, app.clock, app.theme.scroll_ms) != 2;) app.clock += app.theme.scroll_ms;
        render(&app);
        foot_px_is(&app, r.x, r.y, "e tips take", page, ink);
        app.clock = 1;
        app.theme.tips_wrap = true;
        render(&app);
        shows_foot(&app, "its rows.", page);
        app.theme.tips_wrap = false;
        app.message[0] = 0;
        app.clock = 0;
        /* [body] text: a text too long for its box over its rows, or on one row that slides */
        {
            const char* many = "one two three four five six seven eight";
            GuiRect box = gui_rect(0, 0, 10 * cw, 4 * 16 * s);
            int rows = wrap_rows(&app, many, 10, 0);
            assert(app.theme.text == GUI_TEXT_WRAP && rows > 1 &&
                   wrap_in(&app, box, 0, 0, many, 0, ink) == SDL_min(rows, 4));
            app.theme.text = GUI_TEXT_SLIDE;
            assert(wrap_rows(&app, many, 10, 0) == 1 && wrap_in(&app, box, 0, 0, many, 0, ink) == 1);
            assert(!wrap_in(&app, box, 0, 0, "", 0, ink) &&
                   !wrap_in(&app, gui_rect(0, 0, 10 * cw, 8), 0, 0, many, 0, ink));
            app.theme.text = GUI_TEXT_WRAP;
        }
        /* its title twice the text's size, in the row of tabs against its right; with its shadow, in the dim colour a big
           pixel down and right */
        r = m.place.title;
        assert(r.h == 2 * 16 * s && r.x + r.w == m.place.head.x + m.place.head.w && r.y < m.place.tabs.y);
        assert(m.place.tabs.y + m.place.tabs.h == r.y + r.h && r.y + r.h <= m.place.list.y);
        for (int pass = 0; pass < 2; ++pass) {
            const unsigned char* g = &gui_vga_font['S' * 16];
            int big = 2 * s, shadowed = 0;
            app.theme.title_shadow = pass;
            render(&app);
            menu_boxes(&app, &m);
            r = m.place.title;
            for (int fy = 0; fy < 15; ++fy)
                for (int fx = 0; fx < 7; ++fx) {
                    int here = g[fy] & (0x80 >> fx), under = g[fy + 1] & (0x80 >> (fx + 1));
                    if (here) assert(same_colour(pixel(&app, (float)(r.x + fx * big), (float)(r.y + fy * big)), ink));
                    if (here && !under)
                        shadowed +=
                            same_colour(pixel(&app, (float)(r.x + (fx + 1) * big), (float)(r.y + (fy + 1) * big)), dim);
                }
            assert(pass ? shadowed > 4 : !shadowed);
        }
        app.theme.title_shadow = false;
        /* the head on a band of ink with its bar style, the title and the tabs in the page's colour; a rule under it */
        app.theme.head = GUI_STYLE_BAR;
        render(&app);
        menu_boxes(&app, &m);
        assert(same_colour(pixel(&app, (float)(m.place.head.x + m.place.head.w / 2), (float)(m.place.head.y)), ink));
        shows(&app, "Games", page);
        shows_big(&app, "SCARLETNX  /  MENU", page, 2);
        app.theme.head = GUI_STYLE_PLAIN;
        render(&app);
        assert(same_colour(pixel(&app, (float)(m.place.head.x + m.place.head.w / 2),
                                 (float)(m.place.head.y + m.place.head.h + 16 * s / 4 - s / 2)),
                           dim));
        /* the title under the tabs and the tips on a row of their own under the head, in their box with its bar; the
           keys alone at the foot, a rule over it; the footer's none leaves no Status box */
        app.theme.head_place = GUI_HEAD_BELOW;
        app.theme.tips_place = GUI_TIPS_TOP;
        app.theme.footer_rule = true;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.place.title.y >= m.place.tabs.y + m.place.tabs.h && m.place.title.x == m.place.head.x);
        assert(m.place.tips.y > m.place.title.y + m.place.title.h && m.place.list.y > m.place.tips.y + m.place.tips.h);
        assert(m.place.tips.w == m.place.head.w && m.place.keys.x == m.place.head.x &&
               m.place.keys.w == m.place.head.w);
        r = foot_text(&app, m.place.tips, app.theme.tips_box);
        text_px_is(&app, r.x, r.y, "Installs prod.keys", ink, page);
        assert(same_colour(pixel(&app, (float)(m.place.tips.x + 3 * s), (float)(m.place.tips.y + m.place.tips.h / 2)),
                           ink));
        assert(same_colour(
            pixel(&app, (float)(m.place.keys.x + m.place.keys.w / 2), (float)(m.place.keys.y - 16 * s / 4 - s / 2)),
            rgb(app.theme.soft)));
        assert(m.place.status.h);
        app.theme.footer = GUI_STYLE_NONE;
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.place.status.h);
        app.theme = *gui_theme_builtin("yorha");
        app.theme.grid = false;
        render(&app);
        menu_boxes(&app, &m);
        /* with its tabs and big title, a 1280x800 window still holds the whole menu, and Games Add a folder, three
           recent games, the row for the rest and a page of games */
        {
            GuiRecent kept = app.recent;
            int n;
            app.recent.count = 4;
            for (n = 0; n < 4; ++n) {
                snprintf(app.recent.game[n].name, sizeof app.recent.game[n].name, "Game %d", n);
                snprintf(app.recent.game[n].path, sizeof app.recent.game[n].path, "/games/Game %d", n);
                app.recent.game[n].driver = GUI_SWITCH;
            }
            app.recent_all = false;
            recent_changed(&app);
            render(&app);
            menu_boxes(&app, &m);
            assert(m.list.count == GUI_ITEM_COUNT && m.list.shown == m.list.count);
            go_tab(&app, GUI_TAB_GAMES);
            {
                GuiGamesBoxes g;
                render(&app);
                games_boxes(&app, &g);
                assert(g.lead == 5 && g.list.shown == g.list.count && g.list.count > g.lead);
                shows(&app, "Recent games", rgb(app.theme.soft));
                shows(&app, "\xfe ...show more", rgb(app.theme.ink));
            }
            go_tab(&app, GUI_TAB_MENU);
            app.recent = kept;
            recent_changed(&app);
        }
        /* letters too big for the screen: the list scrolls, the focus in view, an arrow where entries are hidden. At
           800 high the whole menu fits 3x, its foot's letters 2x: 768 keeps 3x and one entry fewer */
        assert(SDL_SetWindowSize(app.window, 1280, 768));
        SDL_SyncWindow(app.window);
        app.menu.text_size = GUI_TEXT_MAX;
        app.menu.selected = GUI_EXIT;
        render(&app);
        menu_boxes(&app, &m);
        assert(app.scale == 3 && foot_scale(&app) == 2);
        assert(m.list.shown < m.list.count && m.list.first > 0 && m.list.focus < m.list.first + m.list.shown);
        assert(menu_scrolls(&app));
        b = list_box(&m.list, m.list.first);
        text_px_is(&app, m.list.at.x + m.list.at.w - 8 * app.scale, item_text_y(&app, b), "\x1e", rgb(app.theme.soft),
                   page);
        assert(!m.place.stacked);
        /* and on a window too narrow for both, the list alone (fixed), or the side box under it (adaptive) */
        assert(SDL_SetWindowSize(app.window, 800, 800));
        SDL_SyncWindow(app.window);
        render(&app);
        menu_boxes(&app, &m);
        assert(!m.place.stacked && !m.place.side.h && m.place.list.w == m.place.head.w);
        app.theme.sizing = GUI_SIZING_ADAPTIVE;
        render(&app);
        menu_boxes(&app, &m);
        assert(m.place.stacked && m.place.side.y > m.place.list.y + m.place.list.h && m.place.side.h > 0);
        app.theme.sizing = GUI_SIZING_FIXED;
        assert(SDL_SetWindowSize(app.window, 1280, 800));
        SDL_SyncWindow(app.window);
        app.menu.text_size = 0;
        app.theme = *gui_theme_builtin("default");
        render(&app);
    }
    /* ESC on the menu's first screen asks before the app closes: ESC again stays, Exit closes */
    assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished && !app.confirm.shown);
    key(&app, SDLK_ESCAPE);
    assert(app.confirm.shown && app.confirm.ask == GUI_ASK_EXIT && app.confirm.selected == GUI_CONFIRM_KEEP &&
           !app.menu.quit);
    render(&app);
    key(&app, SDLK_ESCAPE);
    assert(!app.confirm.shown && !app.menu.quit);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(!app.confirm.shown && app.menu.quit);
    app.menu.quit = false;
    /* fingers: a tap takes an entry anywhere across the list, the cursor's margin too, which a mouse does on the box
       only; a drag is no tap, nor a second finger; a swipe across turns the games' pages; back is ESC */
    {
        GuiMenuBoxes m;
        GuiRect b;
        SDL_Event second = {0};
        int margin, y, ch = 16 * app.scale;
        app.theme = *gui_theme_builtin("yorha"); /* a cursor's margin before the boxes */
        app.theme.grid = false;
        assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished && !app.editing);
        app.menu.selected = GUI_KEYS;
        render(&app);
        menu_boxes(&app, &m);
        b = entry_box(&app, GUI_SETTINGS);
        margin = m.list.at.x + 2;
        y = b.y + b.h / 2;
        assert(margin < b.x);
        mouse_px(&app, margin, y, true);
        assert(!app.editing);
        tap_px(&app, margin, y);
        assert(app.editing && app.menu.selected == GUI_SETTINGS);
        key(&app, SDLK_ESCAPE);
        swipe_px(&app, b.x + 10, y, b.x + 10, y + 4 * ch);
        assert(!app.editing);
        second.type = SDL_EVENT_FINGER_DOWN;
        second.tfinger.fingerID = 8;
        event(&app, &second); /* the first finger is still down */
        assert(app.finger == 8);
        tap_px(&app, b.x + 10, y); /* finger 9: no finger of the screen's */
        assert(!app.editing);
        second.type = SDL_EVENT_FINGER_UP;
        second.tfinger.x = app.finger_x;
        second.tfinger.y = app.finger_y;
        event(&app, &second); /* finger 8 lifts where it went down, at the corner: nothing there */
        assert(!app.finger && !app.editing);
        tap_px(&app, b.x + 10, y);
        assert(app.editing);
        key(&app, SDLK_ESCAPE);
        touch_box(&app, entry_box(&app, GUI_SWITCH));
        assert(app.menu.view == GUI_LOAD_DROP);
        key(&app, SDLK_AC_BACK);
        assert(app.menu.view == GUI_LOAD_NONE);
        app.menu.view = GUI_LOAD_GAMES;
        app.browser = (GuiBrowser){.count = 1000};
        layout(&app);
        swipe_px(&app, 60 * 8 * app.scale, 10 * ch, 10 * 8 * app.scale, 11 * ch);
        assert(app.browser.page == 1);
        swipe_px(&app, 10 * 8 * app.scale, 10 * ch, 60 * 8 * app.scale, 10 * ch);
        assert(app.browser.page == 0);
        app.menu.view = GUI_LOAD_NONE;
        app.browser = (GuiBrowser){0};
        app.theme = *gui_theme_builtin("default");
    }
    /* Share log off Android: scarletnx.log with the message and the last run's output, in the state folder of a data
       folder that keeps its kinds apart, else in the working folder */
    {
        char *saved = snx_path_join(temporary, "scarletnx.log"), *installed_dir = app.data_dir;
        char *portable = snx_path_join(temporary, ".scarletnx"), *kept = snx_path_join(portable, "state/scarletnx.log");
        size_t size = 0;
        char* text;
        SDL_free(app.cwd);
        app.cwd = SDL_strdup(temporary);
        snprintf(app.message, sizeof(app.message), "before the log");
        app.menu.selected = GUI_LOG;
#ifdef SNX_XDG
        /* an installed build's in its state folder, as the working folder is the home folder from the desktop */
        {
            char *state = snx_path_join(temporary, "state"), *logged = snx_path_join(state, "scarletnx/scarletnx.log");
            SDL_setenv_unsafe("XDG_STATE_HOME", state, 1);
            key(&app, SDLK_RETURN);
            assert(strstr(app.message, logged) && !snx_path_exists(saved));
            text = (char*)snx_read_file(logged, &size);
            assert(text && strstr(text, "Message: before the log"));
            free(text);
            free(logged);
            free(state);
        }
#endif
        /* a portable copy's .scarletnx/: its state/, on every system */
        app.data_dir = snx_strdup(portable);
        snprintf(app.message, sizeof(app.message), "before the log");
        key(&app, SDLK_RETURN);
        assert(strstr(app.message, "scarletnx.log") && snx_is_file(kept) && !snx_path_exists(saved));
        text = (char*)snx_read_file(kept, &size);
        assert(text && strstr(text, "Message: before the log"));
        free(text);
        /* any other build's (a checkout) in the working folder */
        free(app.data_dir);
        app.data_dir = snx_strdup(temporary);
        snprintf(app.message, sizeof(app.message), "before the log");
        key(&app, SDLK_RETURN);
        assert(!strncmp(app.message, "Log saved to ", 13) && strstr(app.message, "scarletnx.log"));
        text = (char*)snx_read_file(saved, &size);
        assert(text && strstr(text, "Message: before the log") && strstr(text, "== the last run: "));
        free(text);
        free(app.data_dir);
        app.data_dir = installed_dir;
        assert(!snx_remove_tree(portable));
        free(kept);
        free(portable);
        free(saved);
    }
#ifdef SNX_XDG
    /* the menu's recent.txt and last-run.log in the state folder; an older build's in the data folder's gui/ stay */
    {
        char *data = snx_path_join(temporary, "data"), *state = snx_path_join(temporary, "state");
        char *old = snx_path_join(data, "scarletnx/gui"), *recent = snx_path_join(old, "recent.txt");
        char *run = snx_path_join(old, "last-run.log"), *folder, *expected = snx_path_join(state, "scarletnx"), *now;
        assert(!snx_mkdirs(old) && !snx_write_file(recent, "recent", 6) && !snx_write_file(run, "run", 3));
        SDL_setenv_unsafe("XDG_DATA_HOME", data, 1);
        folder = menu_folder(&app, SNX_USER_STATE, true);
        assert(folder && !strcmp(folder, expected) && snx_file_size(recent) == 6 && snx_file_size(run) == 3);
        now = snx_path_join(folder, "recent.txt");
        assert(!snx_path_exists(now) && snx_left_behind(recent, now));
        free(now);
        free(folder);
        /* the menu says, a folder at a time, what an older build left where this one no longer looks: its own files'
           gui/, work/, each game laid out beside the others (its folder now by its title ID, else by its name), until
           they are gone */
#define LONG "Foo, a game whose name is longer than a row of the box has room for"
        {
            char *installed_dir = app.data_dir, *games = app.games_dir, *loose, *loose_data, *named, *npdm, *work,
                 *line;
            char* config = snx_path_join(temporary, "config");
            char* cache = snx_path_join(temporary, "cache");
            char* lines[5][2];
            unsigned char meta[0xa0];
            const char* at;
            int i;
            SDL_setenv_unsafe("XDG_CONFIG_HOME", config, 1);
            SDL_setenv_unsafe("XDG_CACHE_HOME", cache, 1);
            app.data_dir = snx_path_join(data, "scarletnx");
            app.games_dir = snx_path_join(app.data_dir, "games");
            app.recent_file = snx_path_join(expected, "recent.txt");
            loose = snx_path_join(app.games_dir, LONG);
            loose_data = snx_format("%s_data", loose);
            named = snx_path_join(app.games_dir, "Tunic");
            npdm = snx_path_join(app.games_dir, "Tunic_data/exefs/main.npdm");
            work = snx_path_join(app.data_dir, "work");
            memset(meta, 0, sizeof meta); /* an NPDM whose ACI0 at 0x80 gives the game its title ID */
            memcpy(meta, "META", 4);
            meta[0x70] = 0x80;
            meta[0x74] = 0x20;
            memcpy(meta + 0x80, "ACI0", 4);
            for (i = 0; i < 8; ++i) meta[0x90 + i] = (unsigned char)(0x0100DA801624E000ull >> (8 * i));
            line = snx_dirname(npdm);
            assert(!snx_mkdirs(work) && !snx_mkdirs(loose_data) && !snx_mkdirs(line) && !snx_write_file(loose, "", 0));
            {
                char* big = snx_path_join(work, "big");
                void* zeros = calloc(1, 3u << 20);
                assert(zeros && !snx_write_file(big, zeros, 3u << 20));
                free(zeros);
                free(big);
            }
            assert(!snx_write_file(named, "", 0) && !snx_write_file(npdm, meta, sizeof meta));
            free(line);
            lines[0][0] = snx_strdup(old); /* the menu's gui/, its recent games and last run in the state folder */
            lines[0][1] = snx_strdup(expected);
            lines[1][0] = snx_strdup(work);
            lines[1][1] = snx_format("%s/scarletnx/work", cache);
            lines[2][0] = snx_strdup(loose);
            lines[2][1] = snx_format("%s/" LONG "/", app.games_dir);
            lines[3][0] = snx_strdup(named);
            lines[3][1] = snx_format("%s/0100DA801624E000/", app.games_dir);
            lines[4][0] = snx_path_join(app.drivers, "games"); /* a portable copy's games/ beside the program */
            lines[4][1] = snx_format("%s/.scarletnx/games", app.drivers);
            left_behind(&app);
            assert(app.lefts == 5 && snx_is_dir(lines[4][0]));
            for (i = 0, at = app.left; i < 5; ++i, at = strchr(at, '\n') + 1) {
                line =
                    snx_format("%s was left by an older version: this one keeps it in %s\n", lines[i][0], lines[i][1]);
                assert(!strncmp(at, line, strlen(line)));
                free(line);
                free(lines[i][0]);
                free(lines[i][1]);
            }
            /* gui/'s gamecontrollerdb.txt, where #276 read it, goes to the config folder as settings.ini does */
            {
                char *db = snx_path_join(old, "gamecontrollerdb.txt"),
                     *said = snx_format("%s" SAY_LEFT_BY "%s/scarletnx\n", old, config);
                assert(!snx_write_file(db, "", 0));
                left_behind(&app);
                assert(app.lefts == 6 && strstr(app.left, said));
                assert(!snx_remove_tree(db));
                left_behind(&app);
                assert(app.lefts == 5);
                free(said);
                free(db);
            }
            /* a build tree's menu: its checkout's games/, work/ and keys, where an older version kept them, each where
               this one does */
            {
                char *checkout = snx_path_join(temporary, "checkout"), *old_games = snx_path_join(checkout, "games");
                char *old_work = snx_path_join(checkout, "work"), *keys = snx_path_join(checkout, "prod.keys"),
                     *said[3];
                assert(!snx_mkdirs(old_games) && !snx_mkdirs(old_work) && !snx_write_file(keys, "", 0));
                said[0] = snx_format("%s" SAY_LEFT_BY "%s\n", old_games, app.games_dir);
                said[1] = snx_format("%s" SAY_LEFT_BY "%s/scarletnx/work\n", old_work, cache);
                said[2] = snx_format("%s" SAY_LEFT_BY "%s/prod.keys\n", keys, app.data_dir);
                app.checkout = checkout;
                left_behind(&app);
                assert(app.lefts == 8);
                /* a list that changed is counted again */
                while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
                assert(app.left_space->count == 8 && !strcmp(app.left_space->dirs[7], keys));
                for (i = 0; i < 3; ++i) {
                    assert(strstr(app.left, said[i]));
                    free(said[i]);
                }
                app.checkout = NULL;
                left_behind(&app);
                assert(app.lefts == 5);
                assert(!snx_remove_tree(checkout));
                free(keys);
                free(old_work);
                free(old_games);
                free(checkout);
            }
            /* in a box over the menu as it opens: each where it is (-), a path too long for a row going on in the next,
               and under it the space it takes, counted on a thread (a game laid out beside the others with its files
               beside it), as many as fit */
            {
                int width, wraps = 0, selected = app.menu.selected, more, n;
                SDL_Color ink = rgb(app.theme.ink), added = rgb(app.theme.added), failure = rgb(app.theme.failure);
                SDL_Color soft = rgb(app.theme.soft);
                static char paths[2][4200]; /* where each file is, and where it goes */
                while (!__atomic_load_n(&app.left_space->done, __ATOMIC_ACQUIRE)) SDL_Delay(1);
                assert(app.left_space->count == 5 && app.left_space->bytes[0] == 9 &&
                       app.left_space->bytes[1] == 3u << 20 && app.left_space->bytes[2] == 0 &&
                       app.left_space->bytes[3] == sizeof meta);
                int window_w, window_h, text_size = app.menu.text_size;
                app.left_shown = true;
                app.clock = 1;
                /* tall enough for all five, whatever the temporary folder's path, at the letters' size of now */
                assert(SDL_GetWindowSize(app.window, &window_w, &window_h));
                app.menu.text_size = app.scale;
                assert(SDL_SetWindowSize(app.window, window_w, 30 * 16 * app.scale));
                SDL_SyncWindow(app.window);
                render(&app);
                width = app.cols - 6; /* the box's columns, as render_left takes them */
                assert(app.rows >= 28 && width >= 57 && !same_colour(added, ink));
                shows(&app, SAY_LEFT_TITLE, ink);
                for (at = app.left; *at; at = strchr(at, '\n') + 1) {
                    left_paths(at, paths[0], paths[1], sizeof paths[0]);
                    for (const char* rest = paths[0]; *rest;) { /* the whole path, a piece a row */
                        size_t cut = gui_wrap(rest, width - 2);
                        line = snx_format("%.*s", (int)cut, rest);
                        shows(&app, line, failure);
                        wraps += rest != paths[0];
                        free(line);
                        rest += cut;
                    }
                    hides(&app, paths[1], ink); /* where it goes is Share log's */
                }
                assert(wraps >= 1); /* the long game's path wraps */
                shows(&app, "1 KB", soft);
                shows(&app, "3 MB", soft);
                shows(&app, "0 MB", soft);
                shows(&app, "- OK", yellow);
                hides(&app, "which Share log lists", rgb(app.theme.soft));
                /* a window too short for them all: as many as fit, then how many more */
                assert(SDL_SetWindowSize(app.window, window_w, 14 * 16 * app.scale));
                SDL_SyncWindow(app.window);
                render(&app);
                for (more = 0, n = 1; n < 5; ++n) {
                    char said[64];
                    int x, y;
                    snprintf(said, sizeof said, "and %d more, which Share log lists", n);
                    if (find_text(&app, said, rgb(app.theme.soft), 1, &x, &y)) more = n;
                }
                assert(more > 0);
                shows(&app, "- OK", yellow);
                app.menu.text_size = text_size;
                assert(SDL_SetWindowSize(app.window, window_w, window_h));
                SDL_SyncWindow(app.window);
                layout(&app);
                app.clock = 0;
                /* the wheel moves nothing under it, and leaves it open */
                {
                    SDL_Event wheel = {0};
                    wheel.type = SDL_EVENT_MOUSE_WHEEL;
                    wheel.wheel.integer_y = -2;
                    event(&app, &wheel);
                    assert(app.left_shown && app.menu.selected == selected);
                }
                /* a key closes it and does nothing else; nothing was moved */
                key(&app, SDLK_DOWN);
                assert(!app.left_shown && app.menu.selected == selected && snx_is_file(loose));
                /* Clean old settings cleans settings only: what an older version left stays where it is */
                clean_old(&app);
                assert(snx_is_file(loose) && snx_is_dir(loose_data) && snx_is_file(npdm) && snx_is_dir(work));
                left_behind(&app);
                assert(app.lefts == 5);
                gui_cleaned_free(&app.cleaned);
                app.cleaned_shown = false;
                /* gone while it shows, it closes: the data folder's, and the program in a folder with no games/ */
                {
                    char* drivers = app.drivers;
                    app.drivers = snx_path_join(temporary, "program alone");
                    app.left_shown = true;
                    assert(!snx_remove_tree(app.data_dir)); /* its gui/ too */
                    app.left_at = 0;
                    pump(&app);
                    assert(!app.left_shown && !app.lefts && !app.left);
                    free(app.drivers);
                    app.drivers = drivers;
                }
            }
            SDL_setenv_unsafe("XDG_CONFIG_HOME", "/nonexistent/snx-gui-test/config", 1);
            SDL_setenv_unsafe("XDG_CACHE_HOME", "/nonexistent/snx-gui-test/cache", 1);
            free(app.recent_file);
            app.recent_file = NULL;
            free(app.games_dir);
            app.games_dir = games;
            free(app.data_dir);
            app.data_dir = installed_dir;
            free(work);
            free(npdm);
            free(named);
            free(loose_data);
            free(loose);
            free(cache);
            free(config);
        }
#undef LONG
        /* and an empty state folder is made for the first run */
        assert(!snx_remove_tree(state));
        folder = menu_folder(&app, SNX_USER_STATE, true);
        assert(folder && snx_is_dir(folder));
        free(folder);
        SDL_setenv_unsafe("XDG_DATA_HOME", "/nonexistent/snx-gui-test", 1);
        free(expected);
        free(run);
        free(recent);
        free(old);
        free(state);
        free(data);
    }
#endif
    /* the system's picker, as the Android menu opens it for a file to load or for the keys: a document with no path
       stays on the screen, and so does none, saying so, for the screen's messages to be read; a path loads */
    app.picked = SDL_RegisterEvents(1);
    assert(app.picked);
    app.menu.selected = GUI_KEYS;
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    pick(&app, NULL);
    assert(app.menu.view == GUI_LOAD_KEYS && !strcmp(app.message, "No file was picked."));
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    app.menu.selected = GUI_SWITCH;
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_DROP);
    pick(&app, "content://com.android.providers.downloads.documents/document/msf%3A12");
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running && app.message[0]);
    pick(&app, NULL);
    assert(app.menu.view == GUI_LOAD_DROP && !strcmp(app.message, "No file was picked."));
    pick(&app, input);
    assert(app.menu.running && app.menu.selected == GUI_SWITCH);
    finish(&app, 0);
    button_map(&app);
    /* the button map in the theme's boxes: the tabs, the title big, the pad drawn in the box between it and the bar,
       which says the note a map it cannot read leaves */
    {
        GuiPlace p;
        GuiRect r;
        char keys[256];
        const SnxPadShape* first;
        int s = app.scale, cw = 8 * s, page = -1;
        app.theme = *gui_theme_builtin("default");
        for (int i = 0; i < app.settings.count; ++i)
            if (app.settings.values[i].kind == GUI_SETTING_PAGE) page = i;
        open_settings(&app);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        app.editor.row = page - app.first;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_PAD && app.editor.note[0]); /* the map it cannot read, still set */
        render(&app);
        place_screen(&app, &p, screen_keys(&app, SAY_PAD, keys, sizeof keys), 0, GUI_SHOWS_TABS);
        r = inner(&app, p.list);
        first = drawn(&app, 0, SNX_PAD_SHAPE_BOX, -1);
        assert(first && first->x >= (float)r.x && first->y >= (float)r.y && first->y < (float)(r.y + r.h));
        text_px_is(&app, tab_rect(&app, p.tabs, GUI_TAB_SETTINGS).x + cw, p.tabs.y + 2 * s, "Settings", yellow, black);
        {
            char start[24];
            snprintf(start, sizeof start, "%s", app.editor.note); /* as far as the tips' first row shows it */
            text_px_is(&app, p.tips.x, p.tips.y + 2 * s, start, red, black);
        }
        key(&app, SDLK_ESCAPE);
        assert(app.form == GUI_FORM_GROUP);
        leave_editor(&app);
    }
    players_view(&app);
    step_line();
    /* YoRHa's Settings: its groups' column wide enough for the last row with its cursor; a page of a group open, its
       name over its rows */
    {
        static const char list[] = "Display\tfullscreen\t0\tint\t0\t1\tStart in fullscreen\tFullscreen\t0=Off|1=On\n"
                                   "Display/Window\tscale\t1\tint\t1\t4\tThe window's scale\tScale\t\n";
        GuiFormBoxes f;
        GuiRect b;
        int s = app.scale, cw = 8 * s, row;
        gui_settings_free(&app.settings);
        memset(&app.settings, 0, sizeof app.settings);
        assert(!gui_settings_read(&app.settings, list));
        gui_settings_mark(&app.settings);
        app.theme = *gui_theme_builtin("yorha");
        app.theme.grid = false;
        open_settings(&app);
        app.editor.row = app.groups; /* the last row */
        render(&app);
        form_boxes(&app, &f);
        b = list_box(&f.left, app.groups);
        text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), "\xfe Reset all to the defaults", rgb(app.theme.page),
                   rgb(app.theme.focus));
        assert(!gui_editor_rows(f.rows) && !f.right.shown); /* no rows beside it: not a theme's Folder and Save */
        for (row = 0; row < app.groups && strcmp(app.group_keys[row].key, "Display"); ++row) {}
        app.editor.row = row;
        key(&app, SDLK_RETURN);
        assert(app.form == GUI_FORM_GROUP && app.editor.row == 0);
        /* a setting not picked: its default and [default], as the lines show it */
        {
            char shown[96], line[200];
            render(&app);
            form_boxes(&app, &f);
            gui_settings_shown(&app.settings, app.first, shown, sizeof shown);
            assert(!strcmp(shown, "Off [default]"));
            snprintf(line, sizeof line, "\xfe %-*s %s", form_labels(f.rows), "Fullscreen", shown);
            b = list_box(&f.right, 0);
            text_px_is(&app, b.x + cw / 2, item_text_y(&app, b), line, rgb(app.theme.page), rgb(app.theme.focus));
        }
        key(&app, SDLK_DOWN);
        key(&app, SDLK_RETURN);
        assert(app.page >= 0);
        render(&app);
        form_boxes(&app, &f);
        text_px_is(&app, f.place.side.x + cw, f.place.side.y + 2 * s, app.settings.keys[app.page].key,
                   rgb(app.theme.page), rgb(app.theme.ink));
        leave_editor(&app);
        app.theme = *gui_theme_builtin("default");
    }
    e.type = SDL_EVENT_QUIT;
    event(&app, &e);
    assert(app.menu.quit);
    cleanup(&app);
    assert(!snx_remove_tree(temporary));
    free(temporary);
    free(self);
    free(input);
    puts("SDL3 GUI: keyboard, font rendering, drops, dispatch and return-to-menu passed");
    return 0;
}
