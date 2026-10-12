/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the nvhost devices (nvdrv.c): nvmap, the GPU address space, syncpoints */
#ifndef SWR_NVDRV_H
#define SWR_NVDRV_H
#include "abi/cpu.h"

void nv_init(void);
u64 nvmap_addr(u32 handle);
u64 nvmap_addr_by_id(u32 id);
u32 nvmap_size(u32 h);
u64 gmmu_translate(u64 gva);
u32 syncpt_alloc(void);   /* fatal past the 128 ids NVIDIA's stream library has room for */
void syncpt_free(u32 id); /* the id can be handed out again */
u32 syncpt_read(u32 id);
void syncpt_incr(u32 id);
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns);

#endif
