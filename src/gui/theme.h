/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GUI_THEME_H
#define SNX_GUI_THEME_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t r, g, b;
} GuiRgb;
typedef enum { GUI_STYLE_PLAIN, GUI_STYLE_BAR, GUI_STYLE_NONE } GuiStyle;
/* a run's screens: the driver's output as it comes, or its steps as a loading screen draws them */
typedef enum { GUI_RUN_CONSOLE, GUI_RUN_STEPS } GuiRunLayout;
typedef enum { GUI_LINE_NONE, GUI_LINE_TYPING, GUI_LINE_SLIDE, GUI_LINE_FADE } GuiLineIn;
typedef enum { GUI_RING_NONE, GUI_RING_LEFT, GUI_RING_RIGHT, GUI_RING_NEAR_TEXT } GuiRing;
typedef enum { GUI_ALIGN_LEFT, GUI_ALIGN_CENTER, GUI_ALIGN_RIGHT } GuiAlign;
/* the lines that wait on the user: the keys screen's drop line, the ESC line after a run, a run being aborted */
typedef enum {
    GUI_PROMPT_KEYS,
    GUI_PROMPT_DONE,
    GUI_PROMPT_FAILED,
    GUI_PROMPT_ABORTING,
    GUI_PROMPT_COUNT
} GuiPromptLine;
/* how such a line is drawn when lines[] says so: framed, placed on its row, blinking at blink_ms; else plain */
typedef struct {
    GuiAlign align;
    char open[16], close[16];
    int blink;
    int lines[GUI_PROMPT_COUNT];
} GuiPrompt;
/* the theme.ini format this GUI writes and reads; a newer file's keys this one knows still apply */
#define GUI_THEME_FORMAT 1
typedef enum {
    GUI_SCREEN_MENU,
    GUI_SCREEN_GAMES,
    GUI_SCREEN_KEYS,
    GUI_SCREEN_COMPILING,
    GUI_SCREEN_LOADING,
    GUI_SCREEN_COUNT
} GuiScreen;
typedef struct {
    GuiRgb page, ink, soft, dim, failure, warning;
} GuiPalette;
typedef struct {
    char id[64], name[64], author[64];
    uint32_t format;
    GuiRgb page, ink, soft, focus, active, failure, dim, warning;
    GuiStyle head, footer;
    int head_rule, footer_rule;
    int focus_bar; /* the focused line as a band of ink, its text in the page colour */
    char marker[16];
    char title[GUI_SCREEN_COUNT][64];
    GuiRunLayout run;
    GuiPalette run_colors;  /* the steps screen's own: a loading screen, darker than the menus */
    int texture, bar;       /* faint lines over the page; the progress bar and its percentage */
    int detailed, collapse; /* each step's lines under it; folded once the step is done */
    GuiRing ring;
    GuiLineIn line_in;
    int typing_sync; /* one line typed at a time, those already done three times faster */
    GuiPrompt prompt;
    uint32_t fade_in_ms, fade_out_ms, line_in_ms, typing_ms, blink_ms;
    uint32_t scroll_ms; /* a line too long for its row slides a column every scroll_ms, and back; 0 cuts it */
    /* the boot's splash: the program's icon on splash_page for splash_ms (0: none), this theme's in splash_chance
       boots of 100, the default theme's in the others */
    GuiRgb splash_page;
    uint32_t splash_ms, splash_chance;
} GuiTheme;
typedef struct {
    char (*ids)[64]; /* the built-ins, then each folder of themes/ with a theme.ini, in one block */
    size_t count;
} GuiThemes;

/* A key of theme.ini: where a GuiTheme keeps it and what it takes. gui_theme_keys lists every one the GUI writes, by
   section, the editor's form and the file in the same order */
typedef enum { GUI_KEY_TEXT, GUI_KEY_COLOUR, GUI_KEY_FLAG, GUI_KEY_NUMBER, GUI_KEY_WORD } GuiKeyType;
typedef struct {
    const char* section;
    const char* key;
    GuiKeyType type;
    size_t at, size;          /* the field's offset in GuiTheme and its bytes: a text takes size - 1 at most */
    const char* const* words; /* GUI_KEY_WORD: its words, NULL-ended; the field an int, the word's index */
    uint32_t max;             /* GUI_KEY_NUMBER: 0 to 10000 read, then at most max */
    const char* about;
} GuiThemeKey;
extern const GuiThemeKey gui_theme_keys[]; /* a NULL section ends it */
/* what a section of the file holds, for the editor's and the file's heading */
const char* gui_theme_section_about(const char* section);
const GuiThemeKey* gui_theme_key(const char* section, const char* key);
/* value into the key's field: 1 when the GUI takes it, 0 leaves the field as it was */
int gui_theme_set(GuiTheme* theme, const GuiThemeKey* key, const char* value);
/* the key's value as the file writes it */
void gui_theme_get(const GuiTheme* theme, const GuiThemeKey* key, char* out, size_t size);

/* What a theme's file says beyond the theme, to be written back: its base and ErET's layout, ErET's places for the
   ring, the boot word and the texture (written again with the theme's values), and the keys the GUI does not read,
   as "[section]\nkey = value\n" lines kept for the programs that do */
typedef struct {
    char base[64], layout[16];
    int ring_side, loading_title, colors_texture;
    char* others;
} GuiThemeFile;
/* text's: a built-in's to start from it. False when out of memory */
int gui_theme_file_read(GuiThemeFile* file, const char* text, size_t size);
void gui_theme_file_free(GuiThemeFile* file);
/* the theme as a theme.ini of the newest format, every key in it so it reads the same over any base; owned, NULL
   when out of memory */
char* gui_theme_write(const GuiTheme* theme, const GuiThemeFile* file);

/* "default" and "yorha"; NULL for any other id */
const GuiTheme* gui_theme_builtin(const char* id);
/* the theme whose splash a boot shows: this one when roll (0 to 99) is under its chance, else the default */
const GuiTheme* gui_splash_theme(const GuiTheme* theme, int roll);
/* A theme.ini's text over the built-in its [theme] base names (default when absent): a key it lacks, or cannot
   read, keeps the base's value. Returns false only when out of memory. */
int gui_theme_parse(GuiTheme* theme, const char* id, const char* text, size_t size);
/* The first dirs[i]/id/theme.ini that can be read, else the built-in of that id, else the default theme: a folder
   named like a built-in replaces it */
void gui_theme_load(GuiTheme* theme, const char* const* dirs, size_t count, const char* id);
/* the built-ins, then each other folder of dirs with a theme.ini, once */
void gui_themes_scan(GuiThemes* themes, const char* const* dirs, size_t count);
void gui_themes_free(GuiThemes* themes);
#endif
