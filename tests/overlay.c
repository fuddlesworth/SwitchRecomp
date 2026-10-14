/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (runtime/platform/overlay.c): where it goes, what a tap or a move does, and its look */
#include "runtime/platform/overlay.h"
#include <assert.h>
#include <stdio.h>

static u32 at(int x, int y) { return overlay_pixels()[y * OVERLAY_W + x]; }

int main(void) {
    const u32 page = 0xFFBED6DAu, ink = 0xFF3A4145u; /* the YoRHa menu's, R G B A */
    int x, y, s;
    assert(!overlay_shown());
    overlay_toggle();
    assert(overlay_shown());
    /* the title on a band of ink, the hint on the page, the focused item on a band */
    assert(at(0, 0) == ink && at(0, 16) == page && at(0, 3 * 16) == ink && at(0, 4 * 16) == page);
    /* half the screen at most, in whole steps, in the middle: a phone held wide, a small window */
    overlay_place(2424, 1080, &x, &y, &s);
    assert(s == 3 && x == (2424 - 960) / 2 && y == (1080 - 240) / 2);
    overlay_place(500, 100, &x, &y, &s);
    assert(s == 1 && x == 90 && y == 10);
    /* a tap on Exit exits; on the title, nothing; outside the panel, it closes */
    overlay_place(2424, 1080, &x, &y, &s);
    assert(overlay_tap((float)(x + 30), (float)(y + 3 * 48 + 5), 2424, 1080) == OVERLAY_EXIT);
    assert(overlay_tap((float)(x + 30), (float)(y + 5), 2424, 1080) == OVERLAY_NONE);
    assert(overlay_tap(5.0f, 5.0f, 2424, 1080) == OVERLAY_CLOSE);
    /* the focus goes round the items */
    overlay_move(1);
    overlay_move(-2);
    assert(overlay_select() == OVERLAY_EXIT);
    overlay_toggle();
    assert(!overlay_shown());
    puts("overlay: placing, taps, focus and look passed");
    return 0;
}
