/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* VIC: the video image compositor. Here it copies each enabled slot's NV12 surface (the picture NVDEC wrote there)
   into the game's surface, converted to its format, without scaling: a picture larger than its target rectangle or
   the surface is cropped */
#include "video/host1x_int.h"
/* The config struct (its iova in register 0x708) as the game's TVMR and NvDdkVic libraries build it: 0x610 bytes,
   the output surface config at +0x20, 8 slots of 0xB0 bytes from +0x90.
   Surface config u64 (output at +0x20, a slot's at slot+0x40): 0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8, 0x44 NV12),
   11-14 block kind (0 pitch linear), 15-18 block height log2, 32-45 width-1, 46-59 height-1.
   A slot's config u64 at slot+0: bit 0 enable. Its source rectangle is 16.16 fixed point, right and bottom inclusive:
   left bits 0-29 and right 32-61 of the u64 at slot+0x20, top and bottom of the one at +0x28. Its target rectangle,
   u64 at slot+0x30: left 0-13, right 16-29, top 32-45, bottom 48-61, also inclusive.
   A slot's surface 0 (the current frame) is at registers 0x400 + slot * 0x60 (luma) and +4 (chroma), iova >> 8 */
#define VIC_CFG_SIZE 0x610
static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }
/* A surface plane as VIC addresses it: pitch linear rows (256 byte aligned) or block linear GOBs */
typedef struct {
    u8* p;
    u64 lim; /* bytes p may touch */
    u32 blk, bh, pitch, h;
} Plane;
static int plane_map(Plane* pl, u64 iova, u32 blk, u32 bh, u32 wbytes, u32 h) {
    pl->blk = blk;
    pl->bh = bh;
    pl->h = h;
    if (blk) {
        /* whole GOBs: 64 bytes wide (bl_offset rounds the width up the same way), 8 << bh rows high */
        u32 gy = 8u << bh;
        pl->pitch = wbytes;
        pl->lim = (u64)((wbytes + 63) & ~63u) * ((h + gy - 1) / gy * gy);
    } else {
        pl->pitch = (wbytes + 255) & ~255u;
        pl->lim = (u64)pl->pitch * h;
    }
    pl->p = iova ? iova_ptr(iova, pl->lim) : NULL;
    if (!pl->p && iova && blk) {
        /* a buffer that ends inside the last block row: the rows past its end are left out */
        pl->lim = (u64)wbytes * h;
        pl->p = iova_ptr(iova, pl->lim);
    }
    return pl->p != NULL;
}
/* n bytes of row y from byte xb, between the plane and buf: block linear moves at most up to the next 16 byte
   boundary at a time, the run bl_offset keeps contiguous */
static void plane_row(const Plane* pl, u32 xb, u32 y, u8* buf, u32 n, int write) {
    for (u32 k = 0; k < n;) {
        u32 x = xb + k, run = pl->blk ? 16 - (x & 15) : n - k;
        if (run > n - k) run = n - k;
        u64 off = pl->blk ? bl_offset(x, y, 0, pl->pitch, pl->h, pl->bh, 0) : (u64)y * pl->pitch + x;
        if (off + run <= pl->lim) {
            if (write)
                memcpy(pl->p + off, buf + k, run);
            else
                memcpy(buf + k, pl->p + off, run);
        } else if (!write)
            memset(buf + k, 0, run);
        k += run;
    }
}
/* one row of 14 bit widths: the largest a surface config names */
static u8 g_rowy[0x4000], g_rowc[0x4000], g_rowo[0x4000 * 4];
/* slot s of config cs into the output surface. 1 when it wrote anything */
static int compose_slot(const u32* r, const u8* cs, int s, u32 fmt, const Plane* oy, const Plane* oc, u32 sw, u32 sh) {
    const u8* sl = cs + 0x90 + s * 0xB0;
    u64 sc = rd64(sl, 0x40);
    u32 ifmt = (u32)(sc & 0x7F), iblk = (u32)((sc >> 11) & 15), ibh = (u32)((sc >> 15) & 15);
    u32 iw = (u32)((sc >> 32) & 0x3FFF) + 1, ih = (u32)((sc >> 46) & 0x3FFF) + 1;
    if (ifmt != 0x44) {
        LOG_LIMIT(4, "video: VIC slot %d input format %x isn't handled (NV12 is), slot skipped\n", s, ifmt);
        return 0;
    }
    u64 src_x = rd64(sl, 0x20), src_y = rd64(sl, 0x28), dst = rd64(sl, 0x30);
    u32 sx0 = (u32)(src_x & 0x3FFFFFFF) >> 16, sx1 = (u32)((src_x >> 32) & 0x3FFFFFFF) >> 16;
    u32 sy0 = (u32)(src_y & 0x3FFFFFFF) >> 16, sy1 = (u32)((src_y >> 32) & 0x3FFFFFFF) >> 16;
    u32 dx0 = (u32)(dst & 0x3FFF), dx1 = (u32)((dst >> 16) & 0x3FFF);
    u32 dy0 = (u32)((dst >> 32) & 0x3FFF), dy1 = (u32)((dst >> 48) & 0x3FFF);
    if (sx1 < sx0 || sy1 < sy0 || dx1 < dx0 || dy1 < dy0 || sx0 >= iw || sy0 >= ih || dx0 >= sw || dy0 >= sh) {
        LOG_LIMIT(4, "video: VIC slot %d rectangles are empty or outside the surfaces, slot skipped\n", s);
        return 0;
    }
    u32 w = sx1 - sx0 + 1, h = sy1 - sy0 + 1;
    if (w != dx1 - dx0 + 1 || h != dy1 - dy0 + 1)
        LOG_LIMIT(2, "video: VIC slot %d scales %ux%u to %ux%u, which isn't done: the picture is cropped\n", s, w, h,
                  dx1 - dx0 + 1, dy1 - dy0 + 1);
    if (w > dx1 - dx0 + 1) w = dx1 - dx0 + 1;
    if (h > dy1 - dy0 + 1) h = dy1 - dy0 + 1;
    if (w > iw - sx0) w = iw - sx0;
    if (h > ih - sy0) h = ih - sy0;
    if (w > sw - dx0) w = sw - dx0;
    if (h > sh - dy0) h = sh - dy0;
    /* 4:2:0 chroma covers pixel pairs: the rectangles start on even pixels */
    sx0 &= ~1u;
    sy0 &= ~1u;
    dx0 &= ~1u;
    dy0 &= ~1u;
    Plane iy, ic;
    u64 ly = (u64)r[(0x400 + s * 0x60) / 4] << 8, lc = (u64)r[(0x404 + s * 0x60) / 4] << 8;
    if (!plane_map(&iy, ly, iblk, ibh, iw, ih) || !plane_map(&ic, lc, iblk, ibh, iw, (ih + 1) / 2)) {
        LOG_LIMIT(4, "video: VIC slot %d input (luma iova %llx, chroma %llx) not mapped, slot skipped\n", s,
                  (unsigned long long)ly, (unsigned long long)lc);
        return 0;
    }
    LOG_LIMIT(3, "video: VIC slot %d: NV12 %ux%u %s (block height %u), source %u,%u, target %u,%u, %ux%u copied\n", s,
              iw, ih, iblk ? "block linear" : "pitch linear", 1u << ibh, sx0, sy0, dx0, dy0, w, h);
    u32 wc = (w + 1) & ~1u; /* chroma bytes: whole U, V pairs */
    if (fmt == 0x1F || fmt == 0x20) {
        int ri = fmt == 0x1F ? 0 : 2, bi = fmt == 0x1F ? 2 : 0;
        for (u32 y = 0; y < h; y++) {
            plane_row(&iy, sx0, sy0 + y, g_rowy, w, 0);
            plane_row(&ic, sx0, (sy0 + y) / 2, g_rowc, wc, 0);
            for (u32 x = 0; x < w; x++) {
                /* BT.601, limited range */
                int c = ((int)g_rowy[x] - 16) * 298, d = (int)g_rowc[x & ~1u] - 128, e = (int)g_rowc[x | 1] - 128;
                u8* o = g_rowo + x * 4;
                o[ri] = clamp8((c + 409 * e + 128) >> 8);
                o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
                o[bi] = clamp8((c + 516 * d + 128) >> 8);
                o[3] = 255;
            }
            plane_row(oy, dx0 * 4, dy0 + y, g_rowo, w * 4, 1);
        }
        return 1;
    }
    for (u32 y = 0; y < h; y++) {
        plane_row(&iy, sx0, sy0 + y, g_rowy, w, 0);
        plane_row(oy, dx0, dy0 + y, g_rowy, w, 1);
    }
    if (oc->p)
        for (u32 y = 0; y < h / 2; y++) {
            plane_row(&ic, sx0, sy0 / 2 + y, g_rowc, wc, 0);
            plane_row(oc, dx0, dy0 / 2 + y, g_rowc, wc, 1);
        }
    return 1;
}
void vic_execute(const u32* r) {
    {
        static u32 nv;
        nv++;
        if (nv % 120 == 0) LOG("video: %u VIC jobs so far\n", nv);
    }
    u64 ly = (u64)r[0x720 / 4] << 8, lc = (u64)r[0x724 / 4] << 8, cfg_iova = (u64)r[0x708 / 4] << 8;
    u8* cs = iova_ptr(cfg_iova, VIC_CFG_SIZE);
    if (!cs) {
        /* without the config the surface's format, size and layout are unknown: writing a guess corrupts it */
        LOG_LIMIT(4, "video: VIC config at iova %llx not mapped, job skipped\n", (unsigned long long)cfg_iova);
        return;
    }
    u64 cfg = rd64(cs, 0x20);
    u32 fmt = (u32)(cfg & 0x7F), blk = (u32)((cfg >> 11) & 15), bh = (u32)((cfg >> 15) & 15);
    u32 sw = (u32)((cfg >> 32) & 0x3FFF) + 1, sh = (u32)((cfg >> 46) & 0x3FFF) + 1;
    /* SWR_VIC_FMT=n overrides the output format the config names, to try another one while debugging */
    static int fmt_env = -1;
    if (fmt_env == -1) {
        const char* e = getenv("SWR_VIC_FMT");
        fmt_env = e ? (int)(strtoul(e, 0, 0) & 0x7F) : -2;
    }
    if (fmt_env >= 0) fmt = (u32)fmt_env;
    if (fmt != 0x1F && fmt != 0x20 && fmt != 0x44) {
        LOG_LIMIT(4, "video: VIC output format %x isn't handled, job skipped\n", fmt);
        return;
    }
    LOG_LIMIT(3, "video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u\n", (unsigned long long)cfg,
              fmt, blk ? "block linear" : "pitch linear", 1u << bh, sw, sh);
    Plane oy, oc = {0};
    if (!plane_map(&oy, ly, blk, bh, fmt == 0x44 ? sw : sw * 4, sh)) {
        LOG_LIMIT(4, "video: VIC output at iova %llx not mapped\n", (unsigned long long)ly);
        return;
    }
    if (fmt == 0x44 && !plane_map(&oc, lc, blk, bh, sw, (sh + 1) / 2))
        LOG_LIMIT(4, "video: VIC output chroma at iova %llx not mapped, the picture has no color\n",
                  (unsigned long long)lc);
    /* the slots in order, a later one over an earlier one (no blending) */
    int any = 0;
    for (int s = 0; s < 8; s++)
        if (rd64(cs, 0x90 + s * 0xB0) & 1) any |= compose_slot(r, cs, s, fmt, &oy, &oc, sw, sh);
    if (!any) LOG_LIMIT(4, "video: VIC job with no slot it could compose\n");
}
