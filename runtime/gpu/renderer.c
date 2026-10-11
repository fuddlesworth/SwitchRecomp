/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the state the GPU front end and the renderers share (gpu/renderer.h) */
#include "gpu/renderer.h"

int g_renderer_up;
u64 g_draws;
volatile int g_cs_any;
