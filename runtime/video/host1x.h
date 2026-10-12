/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* host1x (host1x.c): NVDEC and VIC, reached through nvdrv */
#ifndef SNX_HOST1X_H
#define SNX_HOST1X_H
#include "abi/cpu.h"

/* one channel per nvdrv fd of /dev/nvhost-nvdec or /dev/nvhost-vic: its registers, syncpoints and (NVDEC) decoder.
   NULL when out of memory */
void* host1x_open(int is_vic);
/* the fd closed: drops the decoder with its pending pictures and gives the syncpoints back */
void host1x_close(void* ch);
u32 host1x_ioctl(void* ch, u32 cmd, u8* d, u32 n);
/* nvmap freed handle h: drops its NVDEC/VIC mapping so the IOVA range can be used again */
void host1x_forget_handle(u32 h);
/* FFmpeg's messages into the game log, warnings and above, a limited number (nvdec.c, in builds with FFmpeg). Every
   user of libavcodec calls it before opening a decoder */
void ffmpeg_log_init(void);

#endif
