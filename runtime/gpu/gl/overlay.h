/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the touch overlay (overlay.c): on-screen controls drawn with GL, for the touch backends (Android, x11es) */
#ifndef SWR_OVERLAY_H
#define SWR_OVERLAY_H
#include "abi/cpu.h"

extern int g_overlay_visible;
void overlay_touch(int id, int action, float x, float y);
void overlay_state(u64* buttons, i32* lx, i32* ly, i32* rx, i32* ry); /* the pad state the touches make */
void overlay_set_opacity(float o);
void overlay_message(const char* m);
void overlay_draw(int w, int h);

#endif
