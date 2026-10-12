/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* host1x (host1x.c): NVDEC and VIC, reached through nvdrv */
#ifndef SWR_HOST1X_H
#define SWR_HOST1X_H
#include "abi/cpu.h"

u32 host1x_ioctl(int is_vic, u32 cmd, u8* d, u32 n);
/* nvmap freed handle h: drops its NVDEC/VIC mapping so the IOVA range can be used again */
void host1x_forget_handle(u32 h);

#endif
