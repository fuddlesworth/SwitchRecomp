/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the guest clock: real time since startup */
#include "core/clock.h"
#include "platform/plat.h"

static u64 g_t0;
void clock_start(void) { g_t0 = plat_ns(); }
u64 g_t0_ns(void) { return g_t0; }
u64 guest_ns(void) { return plat_ns() - g_t0; }
