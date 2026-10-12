/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the guest clock (clock.c): real time since startup */
#ifndef SNX_CLOCK_H
#define SNX_CLOCK_H
#include "abi/cpu.h"

void clock_start(void);
u64 g_t0_ns(void);     /* plat_ns() at startup */
u64 guest_ns(void);    /* the guest's clock */
u64 guest_ticks(void); /* the same clock in system ticks (svcGetSystemTick) */

#endif
