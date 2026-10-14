/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "model.h"
#include "font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gui_menu_key(GuiMenu* menu, GuiKey key) {
    if (menu->running) return;
    if (menu->view == GUI_LOAD_GAMES) return; /* the paginated browser owns its keys */
    if (menu->view == GUI_LOAD_KEYS) {
        if (key == GUI_CANCEL) menu->view = GUI_LOAD_NONE;
        return;
    }
    if (menu->view == GUI_LOAD_OPTIONS || menu->view == GUI_LOAD_DROP) {
        if (key == GUI_CANCEL) {
            menu->view = menu->view == GUI_LOAD_DROP ? GUI_LOAD_OPTIONS : GUI_LOAD_NONE;
            menu->selected = menu->active;
            menu->focus_load = menu->view == GUI_LOAD_OPTIONS;
            menu->load_activated = false;
            return;
        }
        if (menu->focus_load) {
            if (key == GUI_UP || key == GUI_DOWN || key == GUI_LEFT || key == GUI_RIGHT)
                menu->load_selected ^= 1;
            else if (key == GUI_SELECT) {
                menu->selected = menu->active;
                menu->load_active = menu->load_selected;
                menu->load_activated = true;
                menu->view = menu->load_selected == GUI_LOAD_DRAG ? GUI_LOAD_DROP : GUI_LOAD_GAMES;
            }
            return;
        }
        /* Main-menu focus may move while the submenu stays anchored to active.
           Only explicit activation below replaces/closes that submenu. */
    } else {
        menu->focus_load = false;
    }
    /* the recent games and the items are one ring: the last game leads to the first item, Exit to the first game */
    switch (key) {
        case GUI_UP:
        case GUI_LEFT:
            if (menu->recent_focus > 1)
                menu->recent_focus--;
            else if (menu->recent_focus) {
                menu->recent_focus = 0;
                menu->selected = GUI_ITEM_COUNT - 1;
            } else if (menu->selected == 0 && menu->recent)
                menu->recent_focus = menu->recent;
            else
                menu->selected = (menu->selected + GUI_ITEM_COUNT - 1) % GUI_ITEM_COUNT;
            break;
        case GUI_DOWN:
        case GUI_RIGHT:
            if (menu->recent_focus && menu->recent_focus < menu->recent)
                menu->recent_focus++;
            else if (menu->recent_focus) {
                menu->recent_focus = 0;
                menu->selected = 0;
            } else if (menu->selected == GUI_ITEM_COUNT - 1 && menu->recent)
                menu->recent_focus = 1;
            else
                menu->selected = (menu->selected + 1) % GUI_ITEM_COUNT;
            break;
        case GUI_SELECT:
            if (menu->recent_focus) {
                menu->recent_open = menu->recent_focus;
                menu->view = GUI_LOAD_NONE;
                menu->focus_load = menu->load_activated = false;
                break;
            }
            menu->active = menu->selected;
            menu->activated = true;
            menu->load_activated = false;
            menu->view = GUI_LOAD_NONE;
            if (menu->selected == GUI_FULLSCREEN)
                menu->fullscreen = !menu->fullscreen;
            else if (menu->selected == GUI_GCCJIT) {
                if (SNX_GUI_HAS_GCCJIT) menu->gccjit = !menu->gccjit;
            } else if (menu->selected == GUI_GDBSTUB)
                menu->gdbstub = !menu->gdbstub;
            else if (menu->selected == GUI_THEME)
                menu->theme_next = 1;
#ifdef __ANDROID__
            else if (menu->selected == GUI_LOG)
                menu->share_log = 1;
#endif
            else if (menu->selected == GUI_EXIT)
                menu->quit = true;
            else if (menu->selected == GUI_KEYS)
                menu->view = GUI_LOAD_KEYS;
            else {
                menu->view = GUI_LOAD_OPTIONS;
                menu->load_selected = GUI_LOAD_DRAG;
                menu->focus_load = true;
            }
            break;
        default: break;
    }
}
const char* gui_setting_description(const GuiMenu* menu) {
    switch (menu->selected) {
        case GUI_FULLSCREEN:
            return "Enable fullscreen at startup";
        case GUI_GCCJIT:
            return SNX_GUI_HAS_GCCJIT
                ? "libgccjit will be used instead of llvm"
                : "Program was not compiled with libgccjit";
        case GUI_GDBSTUB:
            return "Enables gdbstub on port 1234";
        case GUI_THEME:
            return "Built-in themes, then each themes/<name>/theme.ini";
#ifdef __ANDROID__
        case GUI_LOG:
            return "Saves the last run's output and game log to Download/ScarletNX, then shares them";
#endif
        default: return "";
    }
}
size_t gui_launch_options(const GuiMenu* menu, bool prepared, const char** argv) {
    size_t n = 0;
    /* These are Switch options, never guest argv for the CPU-only Linux driver. */
    if (menu->selected != GUI_SWITCH) return 0;
    if (menu->fullscreen) argv[n++] = "-f";
    if (menu->gccjit && SNX_GUI_HAS_GCCJIT && !prepared) argv[n++] = "--backend=gcc";
    if (menu->gdbstub) {
        argv[n++] = "-S";
        argv[n++] = "-s";
    }
    return n;
}
size_t gui_browser_pages(const GuiBrowser* browser) {
    if (!browser->count || !browser->page_size) return 1;
    return 1 + (browser->count - 1) / browser->page_size;
}
size_t gui_browser_items(const GuiBrowser* browser) {
    if (!browser->page_size || browser->page >= gui_browser_pages(browser)) return 0;
    size_t remaining = browser->count - browser->page * browser->page_size;
    return remaining < browser->page_size ? remaining : browser->page_size;
}
void gui_browser_resize(GuiBrowser* browser, size_t page_size) {
    if (!page_size) page_size = 1;
    /* Preserve the selected file across a resize, not merely its row number. */
    size_t anchor = browser->page * browser->page_size;
    if (browser->selected) anchor += browser->selected - 1;
    browser->page_size = page_size;
    browser->page = anchor / page_size;
    size_t pages = gui_browser_pages(browser);
    if (browser->page >= pages) browser->page = pages - 1;
    if (browser->selected) browser->selected = anchor % page_size + 1;
    size_t items = gui_browser_items(browser);
    if (browser->selected > items) browser->selected = items;
}
GuiBrowserAction gui_browser_key(GuiBrowser* browser, GuiKey key) {
    size_t items = gui_browser_items(browser);
    switch (key) {
        case GUI_CANCEL: return GUI_BROWSER_BACK;
        case GUI_SELECT:
            if (!browser->selected) return GUI_BROWSER_BACK;
            browser->active = browser->page * browser->page_size + browser->selected - 1;
            browser->activated = true;
            return GUI_BROWSER_OPEN;
        case GUI_UP: browser->selected = browser->selected ? browser->selected - 1 : items; break;
        case GUI_DOWN: browser->selected = browser->selected < items ? browser->selected + 1 : 0; break;
        case GUI_LEFT:
        case GUI_PAGE_UP:
            if (browser->page) --browser->page;
            break;
        case GUI_RIGHT:
        case GUI_PAGE_DOWN:
            if (browser->page + 1 < gui_browser_pages(browser)) ++browser->page;
            break;
        default: break;
    }
    items = gui_browser_items(browser);
    if (browser->selected > items) browser->selected = items;
    return GUI_BROWSER_NONE;
}
unsigned char gui_glyph(uint32_t codepoint) {
    if (codepoint < 128) return (unsigned char)codepoint;
    for (int i = 128; i < 256; ++i)
        if (gui_codepage[i] == codepoint) return (unsigned char)i;
    return '?';
}
static int clamp(int n, int maximum) { return n < 1 ? 1 : n > maximum ? maximum : n; }
void gui_console_init(GuiConsole* console, int cols, int rows) {
    memset(console, 0, sizeof(*console));
    memset(console->cells, ' ', sizeof(console->cells));
    console->cols = clamp(cols, GUI_MAX_COLS);
    console->rows = clamp(rows, GUI_MAX_ROWS);
}
void gui_console_resize(GuiConsole* console, int cols, int rows) {
    cols = clamp(cols, GUI_MAX_COLS);
    rows = clamp(rows, GUI_MAX_ROWS);
    /* Only the visible screen is retained. Resizing is not a scrollback buffer. */
    if (console->row >= rows) {
        int drop = console->row - rows + 1;
        memmove(console->cells, console->cells[drop], (size_t)rows * GUI_MAX_COLS);
        memset(console->cells[rows], ' ', (size_t)(GUI_MAX_ROWS - rows) * GUI_MAX_COLS);
        console->row = rows - 1;
    }
    if (cols < console->cols)
        for (int i = 0; i < GUI_MAX_ROWS; ++i) memset(console->cells[i] + cols, ' ', GUI_MAX_COLS - (size_t)cols);
    console->cols = cols;
    console->rows = rows;
    if (console->col > cols) console->col = cols;
}
static void newline(GuiConsole* console) {
    console->col = 0;
    if (++console->row < console->rows) return;
    memmove(console->cells, console->cells[1], (size_t)(console->rows - 1) * GUI_MAX_COLS);
    memset(console->cells[console->rows - 1], ' ', GUI_MAX_COLS);
    console->row = console->rows - 1;
}
static void put_glyph(GuiConsole* console, unsigned char glyph) {
    if (console->col >= console->cols) newline(console);
    console->cells[console->row][console->col++] = glyph;
}
static void feed_byte(GuiConsole* console, unsigned char c) {
    if (console->escape) {
        if (console->escape == 1)
            console->escape = c == '[' ? 2 : c == ']' ? 3 : 0;
        else if (console->escape == 2 && c >= 0x40 && c <= 0x7e)
            console->escape = 0;
        else if (console->escape == 3 && c == 7)
            console->escape = 0;
        else if (console->escape == 3 && c == 27)
            console->escape = 4;
        else if (console->escape == 4)
            console->escape = c == '\\' ? 0 : 3;
        return;
    }
    if (console->utf8_left) {
        if ((c & 0xc0) == 0x80) {
            console->codepoint = (console->codepoint << 6) | (c & 63);
            if (!--console->utf8_left) {
                uint32_t cp = console->codepoint;
                put_glyph(console, cp < console->utf8_min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)
                                       ? '?'
                                       : gui_glyph(cp));
            }
            return;
        }
        console->utf8_left = 0;
        put_glyph(console, '?');
    }
    if (c == 27)
        console->escape = 1;
    else if (c == '\n')
        newline(console);
    else if (c == '\r')
        console->col = 0;
    else if (c == '\b') {
        if (console->col) --console->col;
    } else if (c == '\t') {
        int spaces = 8 - (console->col % 8);
        while (spaces--) put_glyph(console, ' ');
    } else if (c >= 32 && c < 127)
        put_glyph(console, c);
    else if (c >= 0xc2 && c <= 0xf4) {
        console->utf8_left = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        console->utf8_min = c < 0xe0 ? 0x80 : c < 0xf0 ? 0x800 : 0x10000;
        console->codepoint = c & (console->utf8_left == 1 ? 31 : console->utf8_left == 2 ? 15 : 7);
    } else if (c >= 128)
        put_glyph(console, '?');
}
void gui_console_feed(GuiConsole* console, const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) feed_byte(console, (unsigned char)data[i]);
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
char* gui_drop_path(const char* text, bool text_drop) {
    if (!text) return NULL;
    const char* end = text + strlen(text);
    bool uri = false;
    if (text_drop) {
        while (text < end && (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')) ++text;
        while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
        if (end - text >= 2 && ((*text == '"' && end[-1] == '"') || (*text == '\'' && end[-1] == '\''))) {
            ++text;
            --end;
        }
        if (end - text >= 7 && !memcmp(text, "file://", 7)) {
            text += 7;
            if (end - text >= 10 && !memcmp(text, "localhost/", 10)) text += 9;
            if (text == end || *text != '/') return NULL; /* no remote URI authorities */
            uri = true;
        }
    }
    if (text == end) return NULL;
    char* result = malloc((size_t)(end - text) + 1);
    if (!result) return NULL;
    size_t n = 0;
    while (text < end) {
        unsigned char c = (unsigned char)*text++;
        if (uri && c == '%') {
            if (end - text < 2 || hex(text[0]) < 0 || hex(text[1]) < 0) goto invalid;
            c = (unsigned char)((hex(text[0]) << 4) | hex(text[1]));
            text += 2;
        }
        if (!c || (text_drop && (c == '\r' || c == '\n'))) goto invalid;
        result[n++] = (char)c;
    }
    result[n] = 0;
#ifdef _WIN32
    if (uri && n >= 3 && result[0] == '/' && result[2] == ':') memmove(result, result + 1, n);
#endif
    return result;
invalid:
    free(result);
    return NULL;
}
/* text from at, percent-decoded; NULL when a % is not followed by two hex digits */
static char* decoded(const char* at) {
    size_t n = 0;
    char* out = malloc(strlen(at) + 1);
    if (!out) return NULL;
    while (*at) {
        if (*at != '%') {
            out[n++] = *at++;
            continue;
        }
        if (hex(at[1]) < 0 || hex(at[2]) < 0) {
            free(out);
            return NULL;
        }
        out[n++] = (char)(hex(at[1]) << 4 | hex(at[2]));
        at += 3;
    }
    out[n] = 0;
    return out;
}
char* gui_document_path(const char* uri) {
    static const char storage[] = "content://com.android.externalstorage.documents/document/";
    static const char downloads[] = "content://com.android.providers.downloads.documents/document/";
    int download = !strncmp(uri, downloads, sizeof downloads - 1);
    char *id, *colon, *path = NULL;
    size_t n;
    if (!download && strncmp(uri, storage, sizeof storage - 1)) /* other systems' pickers: a path or a file:// URI */
        return !strstr(uri, "://")           ? gui_drop_path(uri, false)
               : !strncmp(uri, "file://", 7) ? gui_drop_path(uri, true)
                                             : NULL;
    id = decoded(uri + (download ? sizeof downloads : sizeof storage) - 1);
    colon = id ? strchr(id, ':') : NULL;
    if (colon && download && !strncmp(id, "raw:/", 5)) { /* a download by path; "msf:12" is by number */
        path = malloc(strlen(colon + 1) + 1);
        if (path) strcpy(path, colon + 1);
    } else if (colon && !download) {
        /* "primary:Download/x.xci" is the device's own storage (its first user's), "1234-5678:x.xci" a card */
        *colon = 0;
        n = strlen(id) + strlen(colon + 1) + 32;
        path = malloc(n);
        if (path) snprintf(path, n, "/storage/%s/%s", strcmp(id, "primary") ? id : "emulated/0", colon + 1);
    }
    free(id);
    return path;
}

static const char banner_text[] = "\nScarletNX - ";
void gui_banner_reset(GuiBanner* banner) {
    banner->matched = 1; /* the output starts at the start of a line */
    banner->seen = 0;
}
int gui_banner_feed(GuiBanner* banner, const char* data, size_t size) {
    size_t i;
    for (i = 0; i < size && !banner->seen; ++i) {
        if (data[i] == banner_text[banner->matched]) {
            if (++banner->matched == sizeof banner_text - 1) banner->seen = 1;
        } else
            banner->matched = data[i] == '\n' ? 1 : 0; /* only a newline starts the text again */
    }
    return banner->seen;
}
