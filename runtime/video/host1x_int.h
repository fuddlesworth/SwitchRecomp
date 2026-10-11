/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the host1x files share: the IOVA mapping the video engines see, and the engines' entry points */
#ifndef SWR_HOST1X_INT_H
#define SWR_HOST1X_INT_H
#include "video/host1x.h"
#include "gpu/gpu.h"
#include "hle/nvdrv.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdlib.h>
#include <string.h>
#ifndef SWR_NO_H264
#include "wels/codec_api.h"
#endif

/* host1x.c */
u8* iova_ptr(u64 iova, u64 len);

/* nvdec.c */
u32 rd32(const u8* p, u32 off);
extern u8* g_fy;
extern u8* g_fuv;
extern u32 g_fw, g_fh, g_fcap;
extern u32 g_vframes;
void nvdec_execute(u32* r);

/* vic.c */
void vic_execute(u32* r);

#endif
