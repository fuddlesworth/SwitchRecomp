/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu: Android's back button or a gamepad's Home opens it over the game, in the menu's look (src/gui, the
   YoRHa theme), as a panel the renderer lays over each frame */
#ifndef SNX_OVERLAY_H
#define SNX_OVERLAY_H
#include "cpu/aarch64/cpu.h"

#define OVERLAY_W 320 /* 40 columns of the 8x16 font */
#define OVERLAY_H 80  /* 5 rows */
enum { OVERLAY_NONE, OVERLAY_CLOSE, OVERLAY_EXIT };

/* whether it is open, for the renderer's thread too */
int overlay_shown(void);
void overlay_toggle(void);
/* OVERLAY_W x OVERLAY_H pixels, R G B A bytes */
const u32* overlay_pixels(void);
/* where it goes on a screen of sw x sh pixels: its corner and the whole number it is scaled by */
void overlay_place(int sw, int sh, int* x, int* y, int* scale);
/* the focused item's action; the focus moved by a row (up -1, down 1) */
int overlay_select(void);
void overlay_move(int rows);
/* a tap at x, y on a screen of sw x sh: an item's action, OVERLAY_CLOSE outside the panel */
int overlay_tap(float x, float y, int sw, int sh);
#endif
