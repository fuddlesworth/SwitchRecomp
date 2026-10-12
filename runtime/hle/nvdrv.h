/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the nvhost devices (nvdrv.c): nvmap, the GPU address space, syncpoints */
#ifndef SNX_NVDRV_H
#define SNX_NVDRV_H
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
/* queues fn(arg) on the GPU thread after every submission made before it, for vi's presents. A fence a queued
   submission promised is reached by then unless a stream fell short (fn checks it). Any other fence is waited for here,
   1 s at most, and its id set to ~0 (none) so fn doesn't wait for it again */
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg);

#endif
