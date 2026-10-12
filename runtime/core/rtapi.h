/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the game module calls through RtAPI (rtapi.c), and the hints it records */
#ifndef SWR_RTAPI_H
#define SWR_RTAPI_H
#include "abi/cpu.h"

extern const RtAPI g_rtapi;
void hints_init(void);
void load_hints(void);
void save_hints(void); /* <Game>.hints: code addresses the recompiler should know about, from this run */
void swr_fpcr_apply(CPU* c);

#endif
