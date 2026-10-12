/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the GPU engines' files share */
#ifndef SWR_ENGINES_INT_H
#define SWR_ENGINES_INT_H
#include <string.h>
#include "core/main.h"
#include "core/options.h"
#include "gpu/renderer.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include "gpu/gpu.h"
#include "core/clock.h"

/* engines.c (gpu_ticks is in gpu.h) */
void query_get(GpuChannel* ch, u32* r, int base, u32 v);

/* mme.c */
void mme_run(GpuChannel* ch, u32 idx, const u32* params, u32 np);

#endif
