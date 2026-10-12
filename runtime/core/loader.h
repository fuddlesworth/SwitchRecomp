/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the game module (<Game>_code.so, its descriptor SwrModule is in abi/cpu.h) and loading it (loader.c) */
#ifndef SWR_LOADER_H
#define SWR_LOADER_H
#include "abi/cpu.h"

extern const SwrModule* g_mod;
extern GuestFn* g_disp;
extern int g_code_tcc; /* the module was built with TCC */
int loader_load(void);
extern u64 g_code_lo, g_code_hi;

#endif
