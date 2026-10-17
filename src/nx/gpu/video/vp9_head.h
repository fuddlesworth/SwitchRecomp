/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "nx/runtime/platform/plat.h"

typedef struct {
    u8 y_mode[36];
    u8 partition[64];
    u8 coef[1728];
    u8 switchable[8];
    u8 inter_mode[28];
    u8 intra_inter[4];
    u8 comp_inter[5];
    u8 single_ref[10];
    u8 comp_ref[5];
    u8 tx32[6];
    u8 tx16[4];
    u8 tx8[2];
    u8 skip[3];
    u8 joints[3];
    u8 sign[2];
    u8 classes[20];
    u8 class0[2];
    u8 prob_bits[20];
    u8 class0_fr[12];
    u8 fr[6];
    u8 class0_hp[2];
    u8 highprec[2];
} Vp9Ent;

typedef struct {
    Vp9Ent prev;
    Vp9Ent ctxs[4];
    int8_t loop_ref[4];
    int8_t loop_mode[2];
    u8 last_seg[0x64];
    int swap_ref;
    int inited;
} Vp9HeadCtx;

void vp9_head_reset(Vp9HeadCtx* c);
/* compose headers + bitstream. curr_off/next_off are the 4 surface luma addresses (Last,Golden,AltRef,Current).
   has_next 0 on flush (refresh Last only for inter). last_shown is the next job's setup bit 4: 0 means this
   picture was hidden (alt-ref, not for display); ignored when has_next is 0 (no next job, assume shown).
   Returns total bytes, 0 on failure */
u32 vp9_compose_frame(Vp9HeadCtx* c, const u8* pic, const u8* entropy_g, const u8* bs, u32 bs_len, u8* out, u32 out_cap,
                      const u64 curr_off[4], const u64 next_off[4], int has_next, int last_shown);
