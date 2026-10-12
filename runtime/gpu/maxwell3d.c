/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the 3D engine's side of the renderer: clears, draws, constant buffers and blits go to it, with clears done in guest
   memory when no renderer runs (the headless build). Also the log of what the GPU wrote to memory */
#include <stdlib.h>
#include <string.h>
#include "gpu/renderer.h"
#include "hle/nvdrv.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "gpu/gpu.h"
void r3d_clear(GpuChannel* ch, u32 v) {
    if (g_renderer_up) {
        g_renderer->clear(ch, v);
        return;
    }
    u32* r = ch->r3d;
    u32 rt = (v >> 6) & 15;
    if (!(v & 0x3C)) return;
    u32* t = &r[0x200 + rt * 0x10];
    u64 addr = ((u64)t[0] << 32) | t[1];
    u32 w = t[2], h = t[3], fmt = t[4], tm = t[5];
    if (!addr || !w || !h) return;
    float cc[4];
    memcpy(cc, &r[0x360], 16);
    u32 bpp = gpu_color_bpp(fmt);
    u8 px[16];
    if (bpp == 4) {
        u32 c = 0;
        for (int i = 0; i < 4; i++) {
            float f = cc[i] < 0 ? 0 : cc[i] > 1 ? 1 : cc[i];
            c |= (u32)(f * 255.f + 0.5f) << (i * 8);
        }
        if (fmt == 0xCF || fmt == 0xD0) c = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        memcpy(px, &c, 4);
    } else
        memset(px, 0, 16);
    int linear = (tm >> 12) & 1;
    u32 bh = (tm >> 4) & 15;
    /* a pitch-linear target holds its row pitch in bytes where the width is (as gpu/vk/render.c reads it) */
    u32 pitch = linear ? w : w * bpp, wb = linear ? w / bpp * bpp : w * bpp;
    if (!wb) return;
    u8* row = malloc(wb);
    if (!row) {
        WARN("gpu: no memory to clear a %ux%u target\n", w, h);
        return;
    }
    for (u32 x = 0; x < wb; x += bpp) memcpy(row + x, px, bpp);
    if (linear) {
        for (u32 y = 0; y < h; y++) gmem_write(addr + (u64)y * pitch, row, wb);
    } else { /* 16 bytes at a time, logged once as the whole range */
        u64 hi = 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < wb; x += 16) {
                u64 o = bl_offset(x, y, 0, wb, h, bh, 0), cpu = gmmu_translate(addr + o);
                u32 n = wb - x < 16 ? wb - x : 16;
                if (cpu) memcpy(GP(cpu), row + x, n);
                if (o + n > hi) hi = o + n;
            }
        gpu_mem_written(addr, hi);
    }
    free(row);
}
void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances) {
    if (g_renderer_up) {
        g_renderer->draw(ch, indexed, topology, first, count, base_vertex, base_instance, instances);
        return;
    }
    (void)ch;
    (void)indexed;
    (void)topology;
    (void)first;
    (void)count;
    (void)base_vertex;
    (void)base_instance;
    (void)instances;
    g_draws++;
}
/* constant buffer data comes a word per method: the words of a method batch are collected and written at once, when
   the batch ends or before any other method (maxwell3d_method, call_method) and at the end of a submission */
void r3d_const_upload(GpuChannel* ch, u32 v, int last) {
    u32* r = ch->r3d;
    if (ch->cb_n == sizeof ch->cb_words / sizeof ch->cb_words[0]) r3d_const_flush(ch);
    if (!ch->cb_n) ch->cb_addr = (((u64)r[0x8E1] << 32) | r[0x8E2]) + r[0x8E3];
    ch->cb_words[ch->cb_n++] = v;
    r[0x8E3] += 4;
    if (last) r3d_const_flush(ch);
}
void r3d_const_flush(GpuChannel* ch) {
    if (!ch->cb_n) return;
    gmem_write(ch->cb_addr, ch->cb_words, (u64)ch->cb_n * 4);
    ch->cb_n = 0;
}
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v) {
    if (g_renderer_up) g_renderer->cb_bind(ch, stage, v);
}
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy) {
    if (g_renderer_up) return g_renderer->blit(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    return 0;
}

/* write log for GPU-side writes (cb_data, I2M, DMA, 2D, reports) so we can tell a range changed
   without rehashing it. Back-to-back writes (cb_data goes word by word) get merged. */
#define GW_N 1024
u64 g_gw_gen; /* entries in the log */
static u64 gw_a[GW_N], gw_e[GW_N], gw_g0[GW_N], gw_g1[GW_N];
static u64 gw_n;
/* last write gen per 4K block (hashed) so dirty_since doesn't have to walk the log.
   cb checks run thousands of times per frame, the walk showed up high in profiles */
#define GWB_N 65536
static u64 gw_blk[GWB_N];
void gpu_mem_written(u64 gva, u64 size) {
    if (!size) return;
    g_gw_gen++;
    {
        u64 b0 = gva >> 12, b1 = (gva + size - 1) >> 12;
        if (b1 - b0 >= GWB_N) b1 = b0 + GWB_N - 1;
        for (u64 b = b0; b <= b1; b++) gw_blk[b & (GWB_N - 1)] = g_gw_gen;
    }
    if (gw_n) {
        u32 k = (u32)((gw_n - 1) % GW_N);
        if (gw_e[k] == gva || (gva >= gw_a[k] && gva + size <= gw_e[k])) {
            if (gva + size > gw_e[k]) gw_e[k] = gva + size;
            gw_g1[k] = g_gw_gen;
            return;
        }
    }
    u32 k = (u32)(gw_n % GW_N);
    gw_a[k] = gva;
    gw_e[k] = gva + size;
    gw_g0[k] = gw_g1[k] = g_gw_gen;
    gw_n++;
}
/* nonzero if [a,a+sz) may have been written after gen 'since' */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz) {
    if (since >= g_gw_gen) return 0;
    if (sz && sz <= (1u << 20)) {
        for (u64 b = a >> 12, e = (a + sz - 1) >> 12; b <= e; b++)
            if (gw_blk[b & (GWB_N - 1)] > since) return 1;
        return 0;
    }
    u64 lim = gw_n > GW_N ? gw_n - GW_N : 0;
    for (u64 n = gw_n; n > lim; n--) {
        u32 k = (u32)((n - 1) % GW_N);
        if (gw_g1[k] <= since) return 0;
        if (gw_a[k] < a + sz && a < gw_e[k]) return 1;
        if (gw_g0[k] <= since) return 0;
    }
    return 1;
} /* older than the log, assume dirty */
