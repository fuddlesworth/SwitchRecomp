/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* ASTC decoding on the CPU (astc.c) */
#ifndef SNX_ASTC_H
#define SNX_ASTC_H
#include "abi/cpu.h"

/* a w x h image in bw x bh blocks, to RGBA8. srgb: the texture is sRGB, which expands endpoints differently */
void astc_decode_image(const u8* src, u32 w, u32 h, u32 bw, u32 bh, u8* dst, int srgb);

#endif
