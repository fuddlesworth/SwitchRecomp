/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the actual renderer and SDL event handlers without production test flags. */
#define SNX_GUI_NO_MAIN
#include "../src/gui/main.c"
#include <assert.h>
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
static void mouse(App* app, int col, int row, bool click) {
    layout(app);
    SDL_Event e = {0};
    float x, y;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (col + 0.5f) * 8 * app->scale,
                                        (row + 0.5f) * 16 * app->scale, &x, &y));
    if (click) {
        e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.x = x;
        e.button.y = y;
    } else {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.x = x;
        e.motion.y = y;
    }
    event(app, &e);
}
/* a finger down on one cell and lifted on another, as SDL gives it: 0 to 1 across the window */
static void swipe(App* app, int col, int row, int to_col, int to_row) {
    SDL_Event e = {0};
    float x, y;
    int w, h;
    layout(app);
    SDL_GetWindowSize(app->window, &w, &h);
    e.tfinger.fingerID = 7;
    e.type = SDL_EVENT_FINGER_DOWN;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (col + 0.5f) * 8 * app->scale, (row + 0.5f) * 16 * app->scale,
                                        &x, &y));
    e.tfinger.x = x / (float)w;
    e.tfinger.y = y / (float)h;
    event(app, &e);
    e.type = SDL_EVENT_FINGER_UP;
    assert(SDL_RenderCoordinatesToWindow(app->renderer, (to_col + 0.5f) * 8 * app->scale,
                                        (to_row + 0.5f) * 16 * app->scale, &x, &y));
    e.tfinger.x = x / (float)w;
    e.tfinger.y = y / (float)h;
    event(app, &e);
}
static void tap(App* app, int col, int row) { swipe(app, col, row, col, row); }
static void colored_text_on(App* app, int col, int row, const char* prompt, SDL_Color color, SDL_Color back) {
    /* Verify the exact text, its glyph color and what is behind it in the real renderer. */
    SDL_Surface* image = SDL_RenderReadPixels(app->renderer, NULL);
    assert(image);
    for (int i = 0; prompt[i]; ++i)
        for (int y = 0; y < 16; ++y)
            for (int x = 0; x < 8; ++x) {
                Uint8 r, g, b, a;
                assert(SDL_ReadSurfacePixel(image, ((i + col) * 8 + x) * app->scale, (row * 16 + y) * app->scale, &r, &g,
                                            &b, &a));
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
static void colored_text_at(App* app, int col, int row, const char* prompt, SDL_Color color) {
    colored_text_on(app, col, row, prompt, color, black);
}
static void colored_text(App* app, int row, const char* prompt, SDL_Color color) {
    colored_text_at(app, 1, row, prompt, color);
}
static void red_text(App* app, int row, const char* prompt) { colored_text(app, row, prompt, red); }
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
    assert(!strcmp(app->message, expected) && app->abnormal_exit == (status != 0));
    assert(app->has_output && app->log_top == 4);
    red_text(app, 3, "Press [ESC] to return to menu");
    if (status) red_text(app, 1, expected);
    /* a theme can make that line a prompt: framed and placed, the done and the failed one apart */
    GuiPromptLine line = status ? GUI_PROMPT_FAILED : GUI_PROMPT_DONE;
    app->theme.prompt.lines[line] = true;
    app->theme.prompt.blink = false;
    render(app);
    colored_text_at(app, (app->cols - 33) / 2, 3, "[ Press [ESC] to return to menu ]", red);
    app->theme.prompt.lines[line] = false;
    app->theme.prompt.lines[status ? GUI_PROMPT_DONE : GUI_PROMPT_FAILED] = true;
    render(app);
    red_text(app, 3, "Press [ESC] to return to menu");
    app->theme.prompt = gui_theme_builtin(app->theme.id)->prompt;
    for (int i = 0; i < 3; ++i) pump(app);
    mouse(app, 2, 4, true); /* A completed process still requires ESC acknowledgement. */
    key(app, SDLK_DOWN);
    key(app, SDLK_RETURN);
    key(app, SDLK_SPACE);
    assert(!app->menu.running && app->finished && app->menu.selected == selected);
    key(app, SDLK_ESCAPE);
    assert(!app->menu.running && !app->finished && app->menu.view == GUI_LOAD_NONE);
    assert(!app->menu.quit && !app->message[0] && app->has_output);
    assert(app->menu.selected == selected && app->menu.fullscreen);
    render(app);
    assert(app->log_top == menu_row(app, GUI_EXIT) + 6);
}
static int SDLCALL hang(void* unused) {
    (void)unused;
    for (;;) SDL_Delay(100);
}
int main(int argc, char** argv) {
    int row;
    if (argc > 1 && strstr(argv[1], "hang in join")) {
        SDL_Thread* thread = SDL_CreateThread(hang, "hung worker", NULL);
        assert(thread && !snx_write_file(argv[1], "ready", 5));
        SDL_WaitThread(thread, NULL); /* Never returns. ESC must kill, not request a graceful exit. */
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "-f")) {
        puts("prepared game stdout");
        fputs("prepared game stderr\n", stderr);
        return 0;
    }
    if (argc > 1 && strstr(argv[1], "option flags")) {
        int i = 2;
        assert(argc == 5 + SNX_GUI_HAS_GCCJIT && !strcmp(argv[i++], "-f"));
        if (SNX_GUI_HAS_GCCJIT) assert(!strcmp(argv[i++], "--backend=gcc"));
        assert(!strcmp(argv[i++], "-S") && !strcmp(argv[i++], "-s") && i == argc);
        puts("Recompiling option flags\nScarletNX - option flags"); /* the runner's banner: now loading */
        return 0;
    }
    if (argc > 1) {
        assert(argc == 2 || argc == 3);
        assert(snx_is_file(argv[1]));
        if (strstr(argv[1], "Game00")) {
            assert(argc == 2 && strstr(argv[0], "gnuabi"));
            puts("Linux file selected from games/");
            return 0;
        }
        assert(strstr(argv[1], "game with spaces"));
        bool nx = strstr(argv[0], "mariko") != NULL;
        if (nx)
            assert(argc == 3 && !strcmp(argv[2], "-f"));
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
    assert(initialize(&app));
    /* the window says which build it is: commit, compiler, platform */
    assert(!strcmp(SDL_GetWindowTitle(app.window), snx_build_info()) && !strncmp(snx_build_info(), "ScarletNX ", 10));
    char* temporary = snx_temp_dir(NULL);
    char* self = snx_executable(argv[0]);
    char* input = snx_path_join(temporary, "game with spaces;literal.nro");
    assert(!snx_write_file(input, "0", 1));
    const char* names[] = {"scarletnx-mariko", "scarletnx-gnuabi"};
    for (int i = 0; i < 2; ++i) {
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
    app.drivers = temporary;
    free(app.games_dir);
    app.games_dir = snx_path_join(temporary, "games");
    render(&app);
    colored_text(&app, menu_row(&app, GUI_SWITCH), "- Load Switch NSP/XCI/NRO", yellow);
    assert(!app.menu.activated);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_KEYS);
    /* Never touch the real user's keys: exercise installation in a private home. */
    free(app.keys_path);
    char* switch_dir = snx_path_join(temporary, ".switch");
    app.keys_path = snx_path_join(switch_dir, "prod.keys");
    char* source_keys = snx_path_join(temporary, "prod.keys");
    assert(!snx_write_file(source_keys, "first keys", 10));
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_KEYS);
    render(&app);
    /* the drop line waits on the user: framed, in the middle of the screen, blinking at blink_ms */
    {
        const char* waiting = "[ Drop prod.keys here ]";
        int middle = SDL_max(app.rows / 2, 10), width = (int)strlen(waiting);
        SDL_Color yellow = rgb(app.theme.focus);
        app.clock = 1000 * app.theme.blink_ms * 2;
        render(&app);
        colored_text_at(&app, (app.cols - width) / 2, middle, waiting, yellow);
        app.clock += app.theme.blink_ms;
        render(&app);
        colored_text_at(&app, (app.cols - width) / 2, middle, "                       ", yellow);
        app.theme.prompt.blink = false; /* steady */
        render(&app);
        colored_text_at(&app, (app.cols - width) / 2, middle, waiting, yellow);
        app.theme.prompt.align = GUI_ALIGN_RIGHT;
        render(&app);
        colored_text_at(&app, app.cols - 1 - width, middle, waiting, yellow);
        app.theme.prompt.align = GUI_ALIGN_LEFT;
        render(&app);
        colored_text_at(&app, 1, middle, waiting, yellow);
        app.theme.prompt.lines[GUI_PROMPT_KEYS] = false; /* plain */
        render(&app);
        colored_text_at(&app, 1, middle, "Drop prod.keys here    ", yellow);
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
    assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running);
    size_t keys_size;
    unsigned char* keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 10 && !memcmp(keys_data, "first keys", 10));
    free(keys_data);
#ifndef _WIN32
    struct stat keys_stat;
    assert(!stat(app.keys_path, &keys_stat) && (keys_stat.st_mode & 0777) == 0600);
#endif
    key(&app, SDLK_RETURN);
    render(&app);
    red_text(&app, 4, "Keys already setup. This will overwrite the previous ones");
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
    key(&app, SDLK_RETURN);
    drop(&app, app.keys_path, false); /* Dropping the destination itself is safe. */
    keys_data = snx_read_file(app.keys_path, &keys_size);
    assert(keys_data && keys_size == 11 && !memcmp(keys_data, "replacement", 11));
    free(keys_data);
    key(&app, SDLK_RETURN);
    assert(!snx_remove_tree(source_keys));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_file_size(app.keys_path) == 11);
    /* Failed replacement must leave the destination untouched and permit retry. */
    assert(!snx_remove_tree(app.keys_path) && !snx_mkdirs(app.keys_path));
    assert(!snx_write_file(source_keys, "replacement", 11));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_KEYS && snx_is_dir(app.keys_path) && app.message[0]);
    assert(!snx_remove_tree(app.keys_path));
    drop(&app, source_keys, false);
    assert(app.menu.view == GUI_LOAD_NONE && snx_is_file(app.keys_path));
    free(switch_dir);
    free(source_keys);
    key(&app, SDLK_RIGHT);
    assert(app.menu.selected == GUI_FULLSCREEN);
    key(&app, SDLK_SPACE);
    assert(app.menu.fullscreen);
    mouse(&app, 2, menu_row(&app, GUI_GCCJIT), false);
    assert(app.menu.selected == GUI_GCCJIT && !app.menu.gccjit);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_GCCJIT), "- [OFF] Enable libgccjit", yellow);
    colored_text(&app, menu_row(&app, GUI_FULLSCREEN), "- [ON] Fullscreen", pink);
    colored_text(&app, app.rows - 4, SNX_GUI_HAS_GCCJIT ? "libgccjit will be used instead of llvm"
                                                      : "Program was not compiled with libgccjit", normal);
    mouse(&app, 2, menu_row(&app, GUI_GCCJIT), true);
    assert(app.menu.gccjit == !!SNX_GUI_HAS_GCCJIT);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_GCCJIT),
                 SNX_GUI_HAS_GCCJIT ? "- [ON] Enable libgccjit" : "- [OFF] Enable libgccjit", pink);
    mouse(&app, 2, menu_row(&app, GUI_GCCJIT), true);
    assert(!app.menu.gccjit);
    mouse(&app, 2, menu_row(&app, GUI_GDBSTUB), false);
    assert(app.menu.selected == GUI_GDBSTUB && !app.menu.gdbstub);
    render(&app);
    colored_text(&app, app.rows - 4, "Enables gdbstub on port 1234", normal);
    mouse(&app, 2, menu_row(&app, GUI_GDBSTUB), true);
    assert(app.menu.gdbstub);
    mouse(&app, 2, menu_row(&app, GUI_GDBSTUB), true);
    assert(!app.menu.gdbstub);
    mouse(&app, 2, menu_row(&app, GUI_FULLSCREEN), false);
    assert(app.menu.selected == GUI_FULLSCREEN && app.menu.fullscreen);
    render(&app);
    colored_text(&app, app.rows - 4, "Enable fullscreen at startup", normal);
    mouse(&app, 2, menu_row(&app, GUI_FULLSCREEN), true);
    assert(!app.menu.fullscreen);
    mouse(&app, 0, 0, true); /* Blank space must not activate the current selection. */
    assert(!app.menu.fullscreen);
    mouse(&app, 2, menu_row(&app, GUI_FULLSCREEN), true);
    assert(app.menu.fullscreen);
    key(&app, SDLK_UP);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_LEFT);
    key(&app, SDLK_RETURN);
    assert(app.menu.selected == GUI_SWITCH && app.menu.view == GUI_LOAD_OPTIONS);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_SWITCH), "- Load Switch NSP/XCI/NRO", pink);
    colored_text_at(&app, 3, menu_row(&app, GUI_SWITCH) + 1, "-> Select via drag and drop", yellow);
    /* Leaving a submenu, including crossing another main option, must neither
       close it nor move its rows or change its activated owner. */
    int exit_row = menu_row(&app, GUI_EXIT);
    mouse(&app, 2, menu_row(&app, GUI_LINUX), false);
    assert(app.menu.selected == GUI_LINUX && app.menu.active == GUI_SWITCH);
    assert(app.menu.view == GUI_LOAD_OPTIONS && !app.menu.focus_load);
    assert(menu_row(&app, GUI_EXIT) == exit_row);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_SWITCH), "- Load Switch NSP/XCI/NRO", pink);
    colored_text(&app, menu_row(&app, GUI_LINUX), "- Load Linux ARM ELF", yellow);
    mouse(&app, 0, 0, false);
    SDL_Event leave = {.type = SDL_EVENT_WINDOW_MOUSE_LEAVE};
    event(&app, &leave);
    assert(app.menu.view == GUI_LOAD_OPTIONS && app.menu.active == GUI_SWITCH);
    /* Enter explicitly activates the hovered main item; clicking another main
       item also replaces the submenu, unlike merely hovering over it. */
    key(&app, SDLK_RETURN);
    assert(app.menu.active == GUI_LINUX && app.menu.view == GUI_LOAD_OPTIONS);
    render(&app);
    colored_text(&app, menu_row(&app, GUI_LINUX), "- Load Linux ARM ELF", pink);
    mouse(&app, 2, menu_row(&app, GUI_SWITCH), true);
    assert(app.menu.active == GUI_SWITCH && app.menu.view == GUI_LOAD_OPTIONS);
    mouse(&app, 4, menu_row(&app, GUI_SWITCH) + 2, false);
    assert(app.menu.load_selected == GUI_LOAD_DIRECTORY && app.menu.view == GUI_LOAD_OPTIONS);
    mouse(&app, 4, menu_row(&app, GUI_SWITCH) + 2, true);
    assert(app.menu.view == GUI_LOAD_GAMES);
    mouse(&app, 2, 3, true);
    assert(app.menu.view == GUI_LOAD_OPTIONS);
    mouse(&app, 4, menu_row(&app, GUI_SWITCH) + 1, false);
    assert(app.menu.load_selected == GUI_LOAD_DRAG);
    pump(&app);
    render(&app);
    const char* screenshot = getenv("SNX_GUI_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_NONE);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_RETURN); /* activate Select via drag and drop */
    render(&app);
    colored_text_at(&app, 3, menu_row(&app, GUI_SWITCH) + 1, "-> Select via drag and drop", pink);
    mouse(&app, 4, menu_row(&app, GUI_SWITCH) + 2, false);
    render(&app);
    colored_text_at(&app, 3, menu_row(&app, GUI_SWITCH) + 1, "-> Select via drag and drop", pink);
    colored_text_at(&app, 3, menu_row(&app, GUI_SWITCH) + 2, "-> Select file from games/ instead", yellow);
    mouse(&app, 2, menu_row(&app, GUI_LINUX), false);
    assert(app.menu.active == GUI_SWITCH && app.menu.selected == GUI_LINUX && app.menu.view == GUI_LOAD_DROP);
    SDL_Event e = {0};
    e.type = SDL_EVENT_DROP_TEXT;
    e.drop.data = "one\ntwo";
    event(&app, &e);
    assert(app.menu.view == GUI_LOAD_DROP && !app.menu.running);
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.data = input;
    event(&app, &e);
    assert(app.menu.running && app.menu.selected == GUI_SWITCH);
    finish(&app, 0);
    assert(!app.native && !app.banner.seen); /* a driver that started no game stayed compiling */
    key(&app, SDLK_DOWN);
    key(&app, SDLK_SPACE);
    assert(app.menu.selected == GUI_LINUX && app.menu.view == GUI_LOAD_OPTIONS);
    key(&app, SDLK_RETURN);
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
    app.menu.gccjit = !!SNX_GUI_HAS_GCCJIT;
    app.menu.gdbstub = true;
    launch(&app, snx_strdup(flags_input), false);
    finish(&app, 0);
    assert(!app.native && app.banner.seen);
    app.menu.gccjit = app.menu.gdbstub = false;
    app.menu.selected = GUI_LINUX;
    free(flags_input);

    /* recent games: what the GUI ran, newest first above the items and kept in a file, named as the run named it;
       one runs again with the options the menu has now, which the fake driver checks */
    {
        GuiRecent kept;
        char* recent_file = snx_path_join(temporary, "prefs/recent.txt");
        char* again = snx_path_join(temporary, "option flags again.nro");
        SDL_Color soft = rgb(app.theme.soft);
        assert(!snx_write_file(again, "fixture", 7));
        recent_open(&app, snx_strdup(recent_file));
        assert(!app.recent.count && !app.menu.recent);
        app.menu.selected = GUI_SWITCH;
        app.menu.gccjit = !!SNX_GUI_HAS_GCCJIT;
        app.menu.gdbstub = true;
        launch(&app, snx_strdup(again), false);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.menu.recent == 1 && !strcmp(app.recent.game[0].name, "option flags"));
        assert(app.recent.game[0].driver == GUI_SWITCH && !app.recent.game[0].prepared);
        gui_recent_load(&kept, recent_file);
        assert(kept.count == 1 && !strcmp(kept.game[0].name, "option flags"));
        assert(!strcmp(kept.game[0].path, app.recent.game[0].path));
        /* a title, the game and a blank row, then the items; up from the first item reaches the game */
        render(&app);
        colored_text(&app, 4, "Recent games", soft);
        colored_text(&app, 5, "- option flags", normal);
        assert(menu_row(&app, GUI_SWITCH) == 7);
        colored_text(&app, 7, "- Load Switch NSP/XCI/NRO", yellow);
        key(&app, SDLK_UP);
        render(&app);
        assert(app.menu.recent_focus == 1);
        colored_text(&app, 5, "- option flags", yellow);
        colored_text(&app, 7, "- Load Switch NSP/XCI/NRO", /* the item gave the focus up */
                     app.menu.activated && app.menu.active == GUI_SWITCH ? pink : normal);
        colored_text(&app, app.rows - 4, app.recent.game[0].path, normal); /* the description: where it is */
        /* run again from there, whatever item the menu last had: its own driver, the options of now */
        app.menu.selected = GUI_LINUX;
        key(&app, SDLK_RETURN);
        assert(app.menu.running && app.menu.selected == GUI_SWITCH);
        finish(&app, 0);
        assert(app.recent.count == 1 && app.menu.recent_focus == 1);
        /* the mouse: over the game it takes the focus, over an item it gives it back */
        mouse(&app, 3, menu_row(&app, GUI_KEYS), false);
        assert(!app.menu.recent_focus && app.menu.selected == GUI_KEYS);
        mouse(&app, 3, 5, false);
        assert(app.menu.recent_focus == 1);
        /* YoRHa: the focused game on the band, the title in its soft colour */
        app.theme = *gui_theme_builtin("yorha");
        app.theme.texture = 0;
        render(&app);
        colored_text_on(&app, 1, 4, "Recent games", rgb(app.theme.soft), rgb(app.theme.page));
        colored_text_on(&app, 3, 5, "option flags", rgb(app.theme.page), rgb(app.theme.focus));
        app.theme = *gui_theme_builtin("default");
        /* a game whose file went away leaves the list when chosen, and the file says so */
        assert(!remove(again));
        key(&app, SDLK_RETURN);
        assert(!app.menu.running && !app.recent.count && !app.menu.recent && !app.menu.recent_focus);
        assert(strstr(app.message, "is gone"));
        gui_recent_load(&kept, recent_file);
        assert(!kept.count);
        render(&app);
        assert(menu_row(&app, GUI_SWITCH) == 4);
        /* the rest runs with no list */
        free(app.recent_file);
        app.recent_file = NULL;
        app.message[0] = 0;
        app.menu.gccjit = app.menu.gdbstub = false;
        app.menu.selected = GUI_LINUX;
        free(again);
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
    red_text(&app, 3, "Press [ESC] to abort and return to menu");
    int selected = app.menu.selected;
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.running && app.menu.selected == selected);
    Uint64 started = SDL_GetTicks();
    key(&app, SDLK_ESCAPE);
    assert(app.aborting && SDL_GetTicks() - started < 1000);
    render(&app);
    red_text(&app, 3, "Aborting process tree...");
    app.theme.prompt.lines[GUI_PROMPT_ABORTING] = true;
    app.theme.prompt.blink = false;
    render(&app);
    colored_text_at(&app, (app.cols - 28) / 2, 3, "[ Aborting process tree... ]", red);
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
    red_text(&app, app.log_top - 4, app.message);
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
    assert(escape_row(&app) == 3); /* the console's ESC line, under the head */
    tap(&app, 20, 5);
    assert(app.menu.running && !app.aborting);
    app.theme = *gui_theme_builtin("yorha"); /* a loading screen's is the last row */
    tap(&app, 20, 3);
    assert(!app.aborting);
    tap(&app, 20, app.rows - 1);
    assert(app.aborting);
    app.theme = *gui_theme_builtin("default");
    while (app.menu.running && SDL_GetTicks() < until) {
        pump(&app);
        SDL_Delay(5);
    }
    assert(!app.menu.running && app.abnormal_exit);
    free(hung);

    /* Browser: missing/empty directories, filtering, page controls and both
       launch paths. Reopening refreshes the directory rather than caching it. */
    key(&app, SDLK_UP);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 0 && app.message[0]);
    key(&app, SDLK_ESCAPE);
    assert(app.menu.view == GUI_LOAD_OPTIONS && app.menu.load_selected == GUI_LOAD_DIRECTORY);
    assert(!snx_mkdirs(app.games_dir));
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 0 && !app.message[0]);
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 0 && app.browser.selected == 0);
    mouse(&app, 2, 3, true); /* Back [ESC] is a selectable item too */
    assert(app.menu.view == GUI_LOAD_OPTIONS);
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
    key(&app, SDLK_RETURN);
    assert(app.menu.view == GUI_LOAD_GAMES && app.games.count == 35 && !app.message[0]);
    assert(!strncmp(app.games.names[0], "Game00", 6));
    assert(gui_browser_pages(&app.browser) > 1);
    key(&app, SDLK_RIGHT);
    key(&app, SDLK_DOWN);
    assert(app.browser.page == 1 && app.browser.selected == 1);
    render(&app);
    screenshot = getenv("SNX_GUI_BROWSER_SCREENSHOT");
    if (screenshot) {
        SDL_Surface* image = SDL_RenderReadPixels(app.renderer, NULL);
        assert(image && SDL_SaveBMP(image, screenshot));
        SDL_DestroySurface(image);
    }
    key(&app, SDLK_PAGEDOWN);
    assert(app.browser.page == 2);
    key(&app, SDLK_PAGEUP);
    key(&app, SDLK_LEFT);
    assert(app.browser.page == 0 && app.browser.selected == 1);
    mouse(&app, 2, 6, false);
    assert(app.browser.selected == 2 && !app.menu.running && !app.browser.activated);
    render(&app);
    colored_text(&app, 6, app.games.names[1], yellow);
    mouse(&app, 2, 5, true); /* native prepared game, not another extraction */
    assert(app.menu.running && app.native && screen(&app) == GUI_SCREEN_LOADING);
    finish(&app, 0);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN); /* Linux selection still uses the GNUABI driver */
    assert(app.menu.running);
    finish(&app, 0);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    key(&app, SDLK_RETURN);
    key(&app, SDLK_DOWN);
    char* removed = snx_path_join(app.games_dir, app.games.names[0]);
    assert(!snx_remove_tree(removed));
    free(removed);
    key(&app, SDLK_RETURN);
    assert(!app.menu.running && app.menu.view == GUI_LOAD_GAMES && app.message[0]);
    mouse(&app, 2, 6, false);
    render(&app);
    colored_text(&app, 5, app.games.names[0], pink);
    colored_text(&app, 6, app.games.names[1], yellow);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_RETURN);
    assert(app.games.count == 34);
    key(&app, SDLK_ESCAPE);
    key(&app, SDLK_ESCAPE);
    /* Compact and spacious layouts share the renderer's mouse hit coordinates. */
    const int sizes[][2] = {{800, 480}, {1280, 960}, {1280, 800}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        assert(SDL_SetWindowSize(app.window, sizes[i][0], sizes[i][1]));
        SDL_SyncWindow(app.window);
        render(&app);
        mouse(&app, 2, menu_row(&app, GUI_GDBSTUB), false);
        assert(app.menu.selected == GUI_GDBSTUB && !app.menu.gdbstub);
        render(&app);
        colored_text(&app, menu_row(&app, GUI_GDBSTUB), "- [OFF] Enable gdbstub", yellow);
        colored_text(&app, app.rows - 4, "Enables gdbstub on port 1234", normal);
        assert(menu_row(&app, GUI_EXIT) < app.rows - 4);
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
        render(&app);
        colored_text(&app, menu_row(&app, GUI_THEME), "- Theme: Default", normal);
        mouse(&app, 2, menu_row(&app, GUI_THEME), true);
        assert(!strcmp(app.theme.id, "yorha"));
        render(&app);
        page = rgb(app.theme.page);
        ink = rgb(app.theme.ink);
        colored_text_on(&app, 1, 0, "SCARLETNX  /  MENU", page, ink);
        colored_text_on(&app, 3, menu_row(&app, GUI_THEME), "Theme: YoRHa", page, ink); /* the focused line's band */
        colored_text_on(&app, 3, menu_row(&app, GUI_EXIT), "Exit", ink, page);
        colored_text_on(&app, 1, app.rows - 2, "CWD:", page, ink);
        mouse(&app, 2, menu_row(&app, GUI_THEME), true);
        assert(!strcmp(app.theme.id, "custom") && !strcmp(app.theme.name, "Custom test"));
        render(&app);
        page.r = 0x10; /* its own page; its ink, unreadable, is YoRHa's */
        page.g = 0x20;
        page.b = 0x30;
        colored_text_on(&app, 1, 0, "TEST  /  MENU", page, ink);
        colored_text_on(&app, 1, menu_row(&app, GUI_THEME), "> Theme: Custom test", page, ink);
        colored_text_on(&app, 1, menu_row(&app, GUI_EXIT), "> Exit", ink, page);
        mouse(&app, 2, menu_row(&app, GUI_THEME), true);
        assert(!strcmp(app.theme.id, "default"));
        render(&app);
        colored_text(&app, menu_row(&app, GUI_THEME), "- Theme: Default", pink); /* activated, as any option */
        free(custom_ini);
        free(custom);
        free(themes);

        /* YoRHa loading: a run as steps, typed in, with the ring, the dots and the bar; the run fades in, and out */
        steps_input = snx_path_join(temporary, "option flags steps.nro");
        assert(!snx_write_file(steps_input, "fixture", 7));
        gui_theme_load(&app.theme, (const char* const*)app.themes_dirs, 2, "yorha");
        app.menu.selected = GUI_SWITCH;
        app.menu.gccjit = !!SNX_GUI_HAS_GCCJIT;
        app.menu.gdbstub = true;
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
        colored_text_on(&app, 1, 0, "SCARLETNX  /  MENU", rgb(app.theme.page), rgb(app.theme.ink));
        for (row = app.log_top - 2; row <= app.log_top; ++row) /* the run's output waits for the run */
            colored_text_on(&app, 1, row, "                            ", rgb(app.theme.ink), rgb(app.theme.page));
        app.clock = entered + app.theme.fade_out_ms / 2;
        render(&app);
        dimmed = pixel(&app, 4, 4); /* the head's bar, half way to black */
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
        colored_text_on(&app, (app.cols - 33) / 2, app.rows - 1, "[ Press [ESC] to return to menu ]", ls, lp);
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
        colored_text_on(&app, app.cols - 6, app.rows - 3, "    ", li, lp); /* YoRHa has no bar unless asked */
        colored_text_on(&app, 1, app.rows - 2, "            ", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 3) + cell / 2), lp));
        app.theme.bar = true;
        render(&app);
        colored_text_on(&app, app.cols - 6, app.rows - 3, "100%", li, lp);
        colored_text_on(&app, 1, app.rows - 2, "OPTION FLAGS", li, lp);
        assert(same_colour(pixel(&app, 8.0f * (float)app.scale + 2, cell * (float)(app.rows - 3) + cell / 2), li));
        /* typing: three characters in, the cursor after them */
        app.theme.fade_in_ms = app.theme.fade_out_ms = 0;
        app.theme.blink_ms = 0;
        launch_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = entered + 1; /* it came while the menu still showed: it types once the run is in */
        app.clock = faded + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 1, 4, "LAU_", li, lp);
        app.steps.step[1].shown_ms = launch_shown;
        app.fade_ms = 0; /* in view from the start, for the lines below */
        app.clock = shown + 3 * app.theme.typing_ms + 1;
        render(&app);
        colored_text_on(&app, 1, 4, "LAU_", li, lp);
        /* sync: one line at a time, a done one three times faster; the second waits for the first */
        app.theme.typing_sync = true;
        first_shown = app.steps.step[0].shown_ms;
        second_shown = app.steps.step[1].shown_ms;
        app.steps.step[1].shown_ms = first_shown;          /* both lines came at once */
        app.clock = first_shown + app.theme.typing_ms + 1; /* three characters at a third of typing_ms each */
        render(&app);
        colored_text_on(&app, 1, 3, "REC", ls, lp); /* not the last line: soft, with the cursor typing it */
        colored_text_on(&app, 4, 3, "_", li, lp);
        colored_text_on(&app, 1, 4, "    ", li, lp);
        app.theme.typing_sync = false;
        app.steps.step[1].shown_ms = second_shown;
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
        last_row = app.rows - 3;
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
        colored_text_on(&app, 1, 0, "SCARLETNX  /  MENU", rgb(app.theme.page), rgb(app.theme.ink)); /* the menus' own */
        app.clock = 0;
        app.theme = *gui_theme_builtin("default");
        app.menu.gccjit = app.menu.gdbstub = false;
        free(steps_input);
    }
    /* fingers: a tap clicks anywhere on a row, which a mouse does on the label only; a drag is no tap, nor a second
       finger; a swipe across turns the games' pages; back is ESC */
    {
        bool fullscreen = app.menu.fullscreen;
        int row = menu_row(&app, GUI_FULLSCREEN);
        SDL_Event second = {0};
        assert(app.menu.view == GUI_LOAD_NONE && !app.menu.running && !app.finished);
        mouse(&app, app.cols - 3, row, true);
        assert(app.menu.fullscreen == fullscreen);
        tap(&app, app.cols - 3, row);
        assert(app.menu.fullscreen != fullscreen && app.menu.selected == GUI_FULLSCREEN);
        swipe(&app, 10, row, 10, row + 4);
        assert(app.menu.fullscreen != fullscreen);
        second.type = SDL_EVENT_FINGER_DOWN;
        second.tfinger.fingerID = 8;
        event(&app, &second); /* the first finger is still down */
        assert(app.finger == 8);
        tap(&app, 2, row); /* finger 7: no finger of the screen's */
        assert(app.menu.fullscreen != fullscreen);
        second.type = SDL_EVENT_FINGER_UP;
        second.tfinger.x = app.finger_x;
        second.tfinger.y = app.finger_y;
        event(&app, &second); /* finger 8 lifts where it went down, on the title: nothing there */
        assert(!app.finger && app.menu.fullscreen != fullscreen);
        tap(&app, 2, row);
        assert(app.menu.fullscreen == fullscreen);
        tap(&app, 2, menu_row(&app, GUI_SWITCH));
        assert(app.menu.view == GUI_LOAD_OPTIONS);
        key(&app, SDLK_AC_BACK);
        assert(app.menu.view == GUI_LOAD_NONE);
        app.menu.view = GUI_LOAD_GAMES;
        app.browser = (GuiBrowser){.count = 1000};
        layout(&app);
        swipe(&app, 60, 10, 10, 11);
        assert(app.browser.page == 1);
        swipe(&app, 10, 10, 60, 10);
        assert(app.browser.page == 0);
        app.menu.view = GUI_LOAD_NONE;
        app.browser = (GuiBrowser){0};
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
