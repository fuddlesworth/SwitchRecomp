/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* host1x (host1x.c): NVDEC and VIC, reached through nvdrv */
#ifndef SWR_HOST1X_H
#define SWR_HOST1X_H
#include "abi/cpu.h"

u32 host1x_ioctl(int is_vic, u32 cmd, u8* d, u32 n);

#endif
