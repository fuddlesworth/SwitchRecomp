/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* ASTC decoding on the CPU (astc.c) */
#ifndef SWR_ASTC_H
#define SWR_ASTC_H
#include "abi/cpu.h"

/* a w x h image in bw x bh blocks, to RGBA8 */
void astc_decode_image(const u8* src, u32 w, u32 h, u32 bw, u32 bh, u8* dst);

#endif
