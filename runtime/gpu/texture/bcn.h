/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* BCn decoding on the CPU, for GPUs without it (third_party/bcn/bcdec.cpp) */
#ifndef SWR_BCN_H
#define SWR_BCN_H
#include "abi/cpu.h"

int swr_bc_decode(int kind, const u8* src, u32 w, u32 h, u32* out);

#endif
