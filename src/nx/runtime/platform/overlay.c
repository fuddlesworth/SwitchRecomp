/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/platform/overlay.h"
#include "font.h"
#include <stdio.h>
#include <string.h>

enum { COLS = OVERLAY_W / 8, FIRST = 3 }; /* the items' first row */
static const char* const items[] = {"Exit"};
static const int actions[] = {OVERLAY_EXIT};
#define ITEMS ((int)(sizeof items / sizeof items[0]))
static u32 pixels[OVERLAY_W * OVERLAY_H];
static int shown, focus;

static u32 rgb(u32 r, u32 g, u32 b) { return r | g << 8 | b << 16 | 0xFF000000u; }

static void band(int row, u32 color) {
    int i;
    for (i = 0; i < OVERLAY_W * 16; ++i) pixels[row * 16 * OVERLAY_W + i] = color;
}
/* ASCII, and 0xFE for the font's square */
static void text(int col, int row, const char* s, u32 ink) {
    int x, y;
    for (; *s && col < COLS; ++s, ++col)
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 8; ++x)
                if (gui_vga_font[(u8)*s * 16 + y] & (0x80 >> x)) pixels[(row * 16 + y) * OVERLAY_W + col * 8 + x] = ink;
}
/* as the YoRHa menu: the title on a band of ink, the focused item on one too */
static void draw(void) {
    const u32 page = rgb(0xDA, 0xD6, 0xBE), ink = rgb(0x45, 0x41, 0x3A), soft = rgb(0x8A, 0x86, 0x76);
    char label[COLS + 1];
    int i;
    for (i = 0; i < OVERLAY_W * OVERLAY_H; ++i) pixels[i] = page;
    band(0, ink);
    text(1, 0, "SCARLETNX  /  MENU", page);
    text(1, 1, "Back or Home: back to the game.", soft);
    for (i = 0; i < ITEMS; ++i) {
        snprintf(label, sizeof label, "\xFE %s", items[i]);
        if (i == focus) band(FIRST + i, ink);
        text(1, FIRST + i, label, i == focus ? page : ink);
    }
}

int overlay_shown(void) { return __atomic_load_n(&shown, __ATOMIC_ACQUIRE); }
void overlay_toggle(void) {
    if (!shown) {
        focus = 0;
        draw();
    }
    __atomic_store_n(&shown, !shown, __ATOMIC_RELEASE);
}
const u32* overlay_pixels(void) { return pixels; }
void overlay_place(int sw, int sh, int* x, int* y, int* scale) {
    int s = sw / 2 / OVERLAY_W < sh / 2 / OVERLAY_H ? sw / 2 / OVERLAY_W : sh / 2 / OVERLAY_H;
    *scale = s < 1 ? 1 : s;
    *x = (sw - OVERLAY_W * *scale) / 2;
    *y = (sh - OVERLAY_H * *scale) / 2;
}
int overlay_select(void) { return actions[focus]; }
void overlay_move(int rows) {
    focus = ((focus + rows) % ITEMS + ITEMS) % ITEMS;
    draw();
}
int overlay_tap(float x, float y, int sw, int sh) {
    int px, py, s, row;
    overlay_place(sw, sh, &px, &py, &s);
    if (x < px || y < py || x >= px + OVERLAY_W * s || y >= py + OVERLAY_H * s) return OVERLAY_CLOSE;
    row = (int)(y - py) / (16 * s) - FIRST;
    if (row < 0 || row >= ITEMS) return OVERLAY_NONE;
    focus = row;
    draw();
    return actions[row];
}
