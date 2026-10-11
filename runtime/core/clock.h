/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the guest clock (clock.c). Normally real time. In smooth recording mode (F7) it runs slower than real time so that
   every frame spans 1/recording_fps of guest time, and the vsync events follow it */
#ifndef SWR_CLOCK_H
#define SWR_CLOCK_H
#include "abi/cpu.h"

void clock_start(void);
u64 g_t0_ns(void);  /* plat_ns() at startup */
u64 guest_ns(void); /* the guest's clock */
extern volatile int g_vt_on, g_rec_active;
void vt_set(int on);      /* smooth recording on or off */
void vt_frame(void);      /* a frame was presented */
void vsync_tick_vt(void); /* signals vi's vsync event when a frame's worth of guest time has passed */

#endif
