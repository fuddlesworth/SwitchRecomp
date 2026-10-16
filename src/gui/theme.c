/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "theme.h"
#include "common.h"
#include "ini_lines.h"
#include "gui_themes.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the built-ins: each folder of src/gui/themes, its theme.ini compiled in (cmake/gui_themes.cmake) and read the
   first time one is asked for. Their files are their only source: nothing here holds a value of theirs */
static GuiTheme builtins[SNX_GUI_THEME_COUNT];
/* a theme's id fits a slot of the theme list, and a word's field is an int: an array of negative size otherwise */
typedef char snx_gui_id_slot[sizeof(((GuiThemes*)0)->ids[0]) == sizeof(((GuiTheme*)0)->id) ? 1 : -1];
typedef char snx_gui_word_int[sizeof(GuiStyle) == sizeof(int) && sizeof(GuiRunLayout) == sizeof(int) &&
                                      sizeof(GuiRing) == sizeof(int) && sizeof(GuiLineIn) == sizeof(int) &&
                                      sizeof(GuiAlign) == sizeof(int)
                                  ? 1
                                  : -1];

static const char* const styles[] = {"plain", "bar", "none", NULL};
static const char* const focuses[] = {"color", "bar", NULL};
static const char* const layouts[] = {"console", "steps", NULL};
static const char* const rings[] = {"none", "left", "right", "right-near-text", NULL};
static const char* const aligns[] = {"left", "center", "right", NULL};
static const char* const entrances[] = {"none", "typing", "slide", "fade", NULL};
static const char* const typings[] = {"async", "sync", NULL};
#define FIELD(f)                    offsetof(GuiTheme, f), sizeof(((GuiTheme*)0)->f)
#define TEXT(s, k, f, about)        {s, k, GUI_KEY_TEXT, FIELD(f), NULL, 0, about}
#define COLOUR(s, k, f, about)      {s, k, GUI_KEY_COLOUR, FIELD(f), NULL, 0, about}
#define FLAG(s, k, f, about)        {s, k, GUI_KEY_FLAG, FIELD(f), NULL, 0, about}
#define NUMBER(s, k, f, max, about) {s, k, GUI_KEY_NUMBER, FIELD(f), NULL, max, about}
#define WORD(s, k, f, words, about) {s, k, GUI_KEY_WORD, FIELD(f), words, 0, about}
const GuiThemeKey gui_theme_keys[] = {
    TEXT("theme", "name", name, "the name the menu's Theme item shows"),
    TEXT("theme", "author", author, "who made it"),
    COLOUR("colors", "page", page, "the background"),
    COLOUR("colors", "ink", ink, "text; the head's and footer's bar"),
    COLOUR("colors", "soft", soft, "hints, secondary text, rules"),
    COLOUR("colors", "focus", focus, "the focused line, or its band"),
    COLOUR("colors", "active", active, "the item chosen, the run's head"),
    COLOUR("colors", "failure", failure, "warnings, the ESC line, a failed run"),
    COLOUR("colors", "dim", dim, "unused by the menus; [run] dim is the loading screen's"),
    COLOUR("colors", "warning", warning, "the abort question in the console layout"),
    WORD("head", "style", head, styles, "bar: the title on a band of ink; none: no title and no hints"),
    FLAG("head", "rule", head_rule, "a line under the head"),
    TEXT("head", "menu", title[GUI_SCREEN_MENU], "the menu's title"),
    TEXT("head", "games", title[GUI_SCREEN_GAMES], "the games/ list's title"),
    TEXT("head", "keys", title[GUI_SCREEN_KEYS], "the keys screen's title"),
    TEXT("head", "compiling", title[GUI_SCREEN_COMPILING], "a run's title until the game loads"),
    TEXT("head", "loading", title[GUI_SCREEN_LOADING], "a run's title once the runner's banner came"),
    TEXT("body", "marker", marker, "before each menu item"),
    WORD("body", "focus", focus_bar, focuses, "the focused line in the focus colour, or on a band of it"),
    WORD("footer", "style", footer, styles, "bar: on a band of ink"),
    FLAG("footer", "rule", footer_rule, "a line over the footer"),
    WORD("run", "layout", run, layouts, "steps: the loading screen; the keys below but colours are its"),
    COLOUR("run", "page", run_colors.page, "the loading screen's background"),
    COLOUR("run", "ink", run_colors.ink, "its text, the cursor, the ring's turning bar"),
    COLOUR("run", "soft", run_colors.soft, "older steps, the name, the inner ring"),
    COLOUR("run", "dim", run_colors.dim, "the outer ring, the bar's empty segments"),
    COLOUR("run", "failure", run_colors.failure, "a failed step, a failed run"),
    COLOUR("run", "warning", run_colors.warning, "a run being aborted, the question"),
    FLAG("run", "texture", texture, "faint lines over the page"),
    FLAG("run", "bar", bar, "the progress bar and its percentage"),
    FLAG("run", "detailed", detailed, "each step's other lines under it"),
    FLAG("run", "collapse", collapse, "a done step's lines folded away"),
    WORD("run", "ring", ring, rings, "the turning ring"),
    WORD("prompt", "align", prompt.align, aligns, "on its row"),
    TEXT("prompt", "open", prompt.open, "before the line"),
    TEXT("prompt", "close", prompt.close, "after it"),
    FLAG("prompt", "blink", prompt.blink, "blinking at blink_ms"),
    FLAG("prompt", "keys", prompt.lines[GUI_PROMPT_KEYS], "the keys screen's drop line"),
    FLAG("prompt", "done", prompt.lines[GUI_PROMPT_DONE], "the ESC line after a run"),
    FLAG("prompt", "failed", prompt.lines[GUI_PROMPT_FAILED], "the ESC line after a failed run"),
    FLAG("prompt", "aborting", prompt.lines[GUI_PROMPT_ABORTING], "a run being aborted"),
    NUMBER("transitions", "fade_in_ms", fade_in_ms, 10000, "what comes after a fade out, from black"),
    NUMBER("transitions", "fade_out_ms", fade_out_ms, 10000, "the menu as a run starts, a run ESC leaves"),
    WORD("transitions", "line_in", line_in, entrances, "how a step's line comes in"),
    NUMBER("transitions", "line_in_ms", line_in_ms, 10000, "slide and fade's length"),
    NUMBER("transitions", "typing_ms", typing_ms, 10000, "a character of the running line, any other line whole"),
    WORD("transitions", "typing", typing_sync, typings, "sync: one line at a time"),
    NUMBER("transitions", "blink_ms", blink_ms, 10000, "the cursor's and the prompt's blink"),
    NUMBER("transitions", "scroll_ms", scroll_ms, 10000, "a line too long for its row slides a column each"),
    COLOUR("splash", "page", splash_page, "behind the icon"),
    NUMBER("splash", "ms", splash_ms, 10000, "how long; 0: none"),
    NUMBER("splash", "chance", splash_chance, 100, "the boots in 100 that show this theme's, the default's the others"),
    {NULL, NULL, GUI_KEY_TEXT, 0, 0, NULL, 0, NULL}};
static const char* const sections[][2] = {
    {"theme", "Who made it. The theme's id is its folder's name."},
    {"colors", "The menus' colours, #RRGGBB."},
    {"head", "The top: a title per screen and the hints under it."},
    {"body", "The menu's lines."},
    {"footer", "The bottom: resource use and the working folder."},
    {"run", "A run: the driver's output as it comes, or a loading screen in its own colours."},
    {"prompt", "A line that waits on the user: framed, placed and blinking when its key is true."},
    {"transitions", "Milliseconds, 0 to 10000; 0 turns the thing off."},
    {"splash", "The boot's: the program's icon on a page."}};

/* "quoted" keeps its spaces */
static char* unquote(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        s[n - 1] = 0;
        return s + 1;
    }
    return s;
}
static int copy(char* out, size_t size, const char* value) {
    if (strlen(value) >= size) return 0;
    strcpy(out, value);
    return 1;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int colour(GuiRgb* out, const char* value) {
    int i;
    uint8_t v[3];
    if (strlen(value) != 7 || value[0] != '#') return 0;
    for (i = 0; i < 3; ++i) {
        int hi = hex(value[1 + 2 * i]), lo = hex(value[2 + 2 * i]);
        if (hi < 0 || lo < 0) return 0;
        v[i] = (uint8_t)(hi * 16 + lo);
    }
    out->r = v[0];
    out->g = v[1];
    out->b = v[2];
    return 1;
}
/* a whole number, 0 to 10000 */
static int integer(uint32_t* out, const char* value) {
    char* end;
    unsigned long n = strtoul(value, &end, 10);
    if (!*value || *end || n > 10000) return 0;
    *out = (uint32_t)n;
    return 1;
}
const char* gui_theme_section_about(const char* section) {
    size_t i;
    for (i = 0; i < sizeof sections / sizeof *sections; ++i)
        if (!strcmp(sections[i][0], section)) return sections[i][1];
    return "";
}
const GuiThemeKey* gui_theme_key(const char* section, const char* key) {
    const GuiThemeKey* k;
    for (k = gui_theme_keys; k->section; ++k)
        if (!strcmp(k->section, section) && !strcmp(k->key, key)) return k;
    return NULL;
}
int gui_theme_set(GuiTheme* theme, const GuiThemeKey* key, const char* value) {
    char* field = (char*)theme + key->at;
    uint32_t n;
    int i;
    switch (key->type) {
        case GUI_KEY_TEXT: return !strchr(value, '\n') && copy(field, key->size, value); /* a line of the file */
        case GUI_KEY_COLOUR: return colour((GuiRgb*)field, value);
        case GUI_KEY_FLAG:
            if (strcmp(value, "true") && strcmp(value, "false")) return 0;
            *(int*)field = value[0] == 't';
            return 1;
        case GUI_KEY_NUMBER:
            if (!integer(&n, value)) return 0;
            *(uint32_t*)field = n < key->max ? n : key->max;
            return 1;
        case GUI_KEY_WORD:
            for (i = 0; key->words[i]; ++i)
                if (!strcmp(value, key->words[i])) {
                    *(int*)field = i;
                    return 1;
                }
            return 0;
    }
    return 0;
}
void gui_theme_get(const GuiTheme* theme, const GuiThemeKey* key, char* out, size_t size) {
    const char* field = (const char*)theme + key->at;
    const GuiRgb* c = (const GuiRgb*)field;
    int i, n;
    switch (key->type) {
        case GUI_KEY_TEXT: snprintf(out, size, "%s", field); return;
        case GUI_KEY_COLOUR: snprintf(out, size, "#%02X%02X%02X", c->r, c->g, c->b); return;
        case GUI_KEY_FLAG: snprintf(out, size, "%s", *(const int*)field ? "true" : "false"); return;
        case GUI_KEY_NUMBER: snprintf(out, size, "%lu", (unsigned long)*(const uint32_t*)field); return;
        case GUI_KEY_WORD:
            i = *(const int*)field;
            for (n = 0; key->words[n]; ++n) {}
            snprintf(out, size, "%s", key->words[i >= 0 && i < n ? i : 0]);
            return;
    }
}
/* run_colors: an ErET loading theme's [colors] are its loading screen's, here the run's */
static void apply(GuiTheme* t, const char* section, const char* key, const char* value, int run_colors) {
    const GuiThemeKey* k;
    uint32_t number;
    if (!strcmp(section, "theme") && !strcmp(key, "format")) {
        if (integer(&number, value) && number) t->format = number;
        return;
    }
    /* ErET's places: [colors] texture, and with its loading layout the run's colours; [header] ring_side in its
       words, none being ours; [loading] title, its boot word */
    if (!strcmp(section, "colors") && (run_colors || !strcmp(key, "texture"))) {
        k = gui_theme_key("run", key);
        if (k && (k->type == GUI_KEY_COLOUR || !strcmp(key, "texture"))) gui_theme_set(t, k, value);
        return;
    }
    if (!strcmp(section, "header") && !strcmp(key, "ring_side")) {
        if (strcmp(value, "none")) gui_theme_set(t, gui_theme_key("run", "ring"), value);
        return;
    }
    if (!strcmp(section, "loading") && !strcmp(key, "title")) {
        gui_theme_set(t, gui_theme_key("head", "loading"), value);
        return;
    }
    k = gui_theme_key(section, key);
    if (k) gui_theme_set(t, k, value);
}
/* each key = value of text, with its section, to visit; false when out of memory */
/* ErET's layout as the built-in to start from, when no base names one */
static const GuiTheme* layout_base(const char* layout) {
    if (!strcmp(layout, "menu")) return gui_theme_builtin("yorha");
    if (!strcmp(layout, "loading")) return gui_theme_builtin("yorha");
    if (!strcmp(layout, "plain") || !strcmp(layout, "debug") || !strcmp(layout, "skip"))
        return gui_theme_builtin("default");
    return NULL;
}
static int visit(const char* text, size_t size, GuiTheme* theme, const GuiTheme** bases, int base_only,
                 int run_colors) {
    SnxIni ini;
    char* key;
    char* value;
    char* copy_of = malloc(size + 1);
    if (!copy_of) return 0;
    memcpy(copy_of, text, size);
    copy_of[size] = 0;
    snx_ini_start(&ini, copy_of);
    while (snx_ini_next(&ini, &key, &value)) {
        const char* section = ini.section;
        value = unquote(value);
        if (base_only) {
            if (!strcmp(section, "theme") && !strcmp(key, "base") && gui_theme_builtin(value))
                bases[0] = gui_theme_builtin(value);
            else if (!strcmp(section, "theme") && !strcmp(key, "layout") && layout_base(value)) {
                bases[1] = layout_base(value);
                bases[2] = !strcmp(value, "loading") ? bases[1] : NULL;
            }
        } else
            apply(theme, section, key, value, run_colors);
    }
    free(copy_of);
    return 1;
}
/* the built-ins, read from their files the first time: each one over nothing, as its file sets every key */
static const GuiTheme* builtins_read(void) {
    size_t i;
    if (builtins[0].id[0]) return builtins;
    for (i = 0; i < SNX_GUI_THEME_COUNT; ++i) {
        GuiTheme* t = &builtins[i];
        memset(t, 0, sizeof *t);
        copy(t->id, sizeof t->id, snx_gui_theme_ids[i]);
        if (!visit(snx_gui_theme_texts[i], strlen(snx_gui_theme_texts[i]), t, NULL, 0, 0)) t->id[0] = 0;
    }
    return builtins;
}
const GuiTheme* gui_splash_theme(const GuiTheme* theme, int roll) {
    return roll < (int)theme->splash_chance ? theme : gui_theme_builtin("default");
}
const GuiTheme* gui_theme_builtin(const char* id) {
    size_t i;
    const GuiTheme* all = builtins_read();
    for (i = 0; id && i < SNX_GUI_THEME_COUNT; ++i)
        if (all[i].id[0] && !strcmp(all[i].id, id)) return &all[i];
    return NULL;
}

int gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size) {
    const GuiTheme* bases[3] = {NULL, NULL, NULL}; /* the base, ErET's layout, set when that is loading */
    if (!visit(text, size, theme, bases, 1, 0)) return 0;
    *theme = bases[0] ? *bases[0] : bases[1] ? *bases[1] : builtins_read()[0];
    copy(theme->id, sizeof theme->id, id);
    copy(theme->name, sizeof theme->name, id);
    return visit(text, size, theme, bases, 0, !bases[0] && bases[2]);
}
void gui_theme_load(GuiTheme* theme, const char* const* dirs, size_t count, const char* id) {
    size_t i;
    const GuiTheme* builtin;
    for (i = 0; i < count; ++i) {
        char* folder = dirs[i] ? snx_path_join(dirs[i], id) : NULL;
        char* path = folder ? snx_path_join(folder, "theme.ini") : NULL;
        size_t size = 0;
        unsigned char* text = path ? snx_read_file(path, &size) : NULL;
        int read = text && gui_theme_parse(theme, id, (const char*)text, size);
        free(text);
        free(path);
        free(folder);
        if (read) return;
    }
    builtin = gui_theme_builtin(id);
    *theme = builtin ? *builtin : builtins_read()[0];
}

void gui_themes_free(GuiThemes* themes) {
    free(themes->ids);
    themes->ids = NULL;
    themes->count = 0;
}
static int listed(const GuiThemes* themes, const char* id) {
    size_t i;
    for (i = 0; i < themes->count; ++i)
        if (!strcmp(themes->ids[i], id)) return 1;
    return 0;
}
void gui_themes_scan(GuiThemes* themes, const char* const* dirs, size_t count) {
    size_t total, capacity, i, d;
    gui_themes_free(themes);
    total = SNX_GUI_THEME_COUNT;
    capacity = total;
    themes->ids = calloc(capacity, sizeof *themes->ids);
    if (!themes->ids) return;
    for (i = 0; i < total; ++i) memcpy(themes->ids[themes->count++], builtins_read()[i].id, sizeof *themes->ids);
    for (d = 0; d < count; ++d) {
        char** names = NULL;
        size_t found = 0;
        char (*grown)[sizeof *themes->ids];
        if (!dirs[d] || snx_list_dir(dirs[d], &names, &found)) continue; /* no such folder: nothing from it */
        grown = realloc(themes->ids, (capacity + found) * sizeof *themes->ids);
        if (!grown) {
            snx_free_strings(names, found);
            continue;
        }
        themes->ids = grown;
        capacity += found;
        for (i = 0; i < found; ++i) {
            char* folder = snx_path_join(dirs[d], names[i]);
            char* file = folder ? snx_path_join(folder, "theme.ini") : NULL;
            int usable = names[i][0] != '.' && !listed(themes, names[i]) && strlen(names[i]) < sizeof *themes->ids &&
                         file && snx_is_file(file);
            free(file);
            free(folder);
            if (usable) snprintf(themes->ids[themes->count++], sizeof *themes->ids, "%s", names[i]);
        }
        snx_free_strings(names, found);
    }
}

/* text that grows as it is written; failed once out of memory */
typedef struct {
    char* p;
    size_t n, room;
    int failed;
} Out;
static void put(Out* o, const char* format, ...) {
    va_list ap;
    int n;
    for (;;) {
        char* grown;
        if (o->failed) return;
        if (o->room) {
            va_start(ap, format);
            n = vsnprintf(o->p + o->n, o->room - o->n, format, ap);
            va_end(ap);
            if (n < 0) o->failed = 1;
            if (n < 0 || o->n + (size_t)n < o->room) {
                o->n += n < 0 ? 0 : (size_t)n;
                return;
            }
        }
        o->room = o->room ? o->room * 2 : 4096;
        grown = realloc(o->p, o->room);
        if (!grown) {
            free(o->p);
            o->p = NULL;
            o->failed = 1;
        }
        o->p = grown;
    }
}
/* quotes keep white space at either end, and a value that is itself quoted */
static void put_key(Out* o, const char* key, const char* value) {
    size_t n = strlen(value);
    int quoted = n && (value[0] == ' ' || value[0] == '\t' || value[n - 1] == ' ' || value[n - 1] == '\t' ||
                       value[n - 1] == '\r' || (n >= 2 && value[0] == '"' && value[n - 1] == '"'));
    put(o, quoted ? "%s = \"%s\"\n" : "%s = %s\n", key, value);
}
static const char* const eret_layouts[] = {"plain", "debug", "loading", "menu", "skip", NULL};
static int is_word(const char* const* words, const char* w) {
    for (; *words; ++words)
        if (!strcmp(*words, w)) return 1;
    return 0;
}

int gui_theme_file_read(GuiThemeFile* file, const char* text, size_t size) {
    Out others = {NULL, 0, 0, 0};
    SnxIni ini;
    char *key, *value, last[sizeof ini.section] = "";
    char* copy_of = malloc(size + 1);
    memset(file, 0, sizeof *file);
    if (!copy_of) return 0;
    memcpy(copy_of, text, size);
    copy_of[size] = 0;
    snx_ini_start(&ini, copy_of);
    while (snx_ini_next(&ini, &key, &value)) {
        value = unquote(value);
        if (!strcmp(ini.section, "theme") && !strcmp(key, "base"))
            copy(file->base, sizeof file->base, value);
        else if (!strcmp(ini.section, "theme") && !strcmp(key, "layout"))
            copy(file->layout, sizeof file->layout, value);
        else if (!strcmp(ini.section, "header") && !strcmp(key, "ring_side"))
            file->ring_side = 1;
        else if (!strcmp(ini.section, "loading") && !strcmp(key, "title"))
            file->loading_title = 1;
        else if (!strcmp(ini.section, "colors") && !strcmp(key, "texture"))
            file->colors_texture = 1;
        else if (ini.section[0] && key[0] && !gui_theme_key(ini.section, key) &&
                 (strcmp(ini.section, "theme") || strcmp(key, "format"))) {
            if (strcmp(last, ini.section)) put(&others, "[%s]\n", strcpy(last, ini.section));
            put_key(&others, key, value);
        }
    }
    free(copy_of);
    file->others = others.p;
    return !others.failed;
}
void gui_theme_file_free(GuiThemeFile* file) {
    free(file->others);
    file->others = NULL;
}
/* the [theme] layout a built-in's file names: ErET's drawing for a theme that starts from it */
static const char* builtin_layout(const char* id) {
    static GuiThemeFile files[SNX_GUI_THEME_COUNT];
    size_t i;
    for (i = 0; i < SNX_GUI_THEME_COUNT; ++i)
        if (!strcmp(snx_gui_theme_ids[i], id)) {
            if (!files[i].layout[0])
                gui_theme_file_read(&files[i], snx_gui_theme_texts[i], strlen(snx_gui_theme_texts[i]));
            return is_word(eret_layouts, files[i].layout) ? files[i].layout : "debug";
        }
    return "debug";
}
char* gui_theme_write(const GuiTheme* theme, const GuiThemeFile* file) {
    Out o = {NULL, 0, 0, 0};
    const char* base = file && gui_theme_builtin(file->base) ? file->base : "default";
    const char* layout = file && is_word(eret_layouts, file->layout) ? file->layout : builtin_layout(base);
    int loading = !strcmp(layout, "loading"); /* ErET's: no base, it starts from YoRHa, [colors] are the run's */
    char value[80];
    size_t s;
    put(&o,
        "; ScarletNX GUI theme, format %d: a folder of themes/ holding this file, the folder's name its id. A\n"
        "; folder named like a built-in replaces it. Keys left out, or that cannot be read, keep the base's value;\n"
        "; keys this GUI does not know are left for others (rEden's ErET reads the same file). A comment takes a\n"
        "; line of its own. docs/GUI.md describes every key. Made with the theme editor in ScarletNX's menu.\n",
        GUI_THEME_FORMAT);
    for (s = 0; s < sizeof sections / sizeof *sections; ++s) {
        const char* name = sections[s][0];
        int run_colors = loading && !strcmp(name, "colors");
        const GuiThemeKey* k;
        put(&o, "%s[%s]\n; %s\n", s ? "\n" : "", name,
            run_colors ? "The loading screen's colours (a run's), as [run] has them in other layouts."
                       : sections[s][1]);
        /* a newer format's file keeps its number: the keys of it this GUI does not know are kept as they are */
        if (!s)
            put(&o, "format = %lu\n",
                (unsigned long)(theme->format > GUI_THEME_FORMAT ? theme->format : GUI_THEME_FORMAT));
        for (k = gui_theme_keys; k->section; ++k) {
            if (strcmp(k->section, run_colors ? "run" : name)) continue;
            if (run_colors && k->type != GUI_KEY_COLOUR && strcmp(k->key, "texture")) continue;
            if (loading && !strcmp(name, "run") && k->type == GUI_KEY_COLOUR) continue;
            gui_theme_get(theme, k, value, sizeof value);
            put(&o, "; %s\n", k->about);
            put_key(&o, k->key, value);
        }
        if (s) continue;
        put(&o, "; ErET's drawing for it (plain | debug | loading | menu | skip); a base when no base names one\n");
        put_key(&o, "layout", layout);
        if (loading)
            put(&o, "; no base: ErET's loading layout starts from yorha, the menus in its colours\n");
        else {
            put(&o, "; the built-in it starts from: default | yorha\n");
            put_key(&o, "base", base);
        }
    }
    if (file && ((file->ring_side && theme->ring != GUI_RING_NONE) || file->loading_title ||
                 (file->colors_texture && !loading))) {
        put(&o, "\n; ErET's places for the ring, the boot word and the texture, the same as above\n");
        if (file->ring_side && theme->ring != GUI_RING_NONE) {
            put(&o, "[header]\n");
            put_key(&o, "ring_side", rings[theme->ring]);
        }
        if (file->loading_title) {
            put(&o, "[loading]\n");
            put_key(&o, "title", theme->title[GUI_SCREEN_LOADING]);
        }
        if (file->colors_texture && !loading) {
            put(&o, "[colors]\n");
            put_key(&o, "texture", theme->texture ? "true" : "false");
        }
    }
    if (file && file->others)
        put(&o, "\n; keys this GUI does not read, kept for the programs that do\n%s", file->others);
    if (o.failed) {
        free(o.p);
        return NULL;
    }
    return o.p;
}
