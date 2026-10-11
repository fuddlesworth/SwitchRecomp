/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* input (hid.c) */
#ifndef SWR_HID_H
#define SWR_HID_H
#include "abi/cpu.h"

typedef struct {
    u64 buttons;
    i32 lx, ly, rx, ry;
    int touch, tx, ty;
} PadState;
extern PadState g_pad;
void hid_update(void);

#endif
