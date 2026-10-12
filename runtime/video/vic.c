/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* VIC: the video image compositor. Here it converts the last picture NVDEC decoded (not the input slot the job
   names) into the game's surface, without scaling: a picture larger than the surface is cropped */
#include "video/host1x_int.h"
/* VIC output surface config, u64 at config(0x708)+0x20:
   0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8, 0x44 NV12), 11-14 bl kind, 15-18 block height log2,
   32-45 width-1, 46-59 height-1 */
static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }
static void vic_put(u8* base, u64 off, const u8* src, u32 n, u64 lim) {
    if (off + n <= lim) memcpy(base + off, src, n);
}
void vic_execute(u32* r) {
    if (!g_vframes || !g_fy) return;
    {
        static u32 nv;
        nv++;
        if (nv % 120 == 0) LOG("video: %u VIC jobs so far\n", nv);
    }
    u64 ly = (u64)r[0x720 / 4] << 8, lc = (u64)r[0x724 / 4] << 8, cfg_iova = (u64)r[0x708 / 4] << 8;
    u8* cs = iova_ptr(cfg_iova, 0x28);
    if (!cs) {
        /* without the config the surface's format, size and layout are unknown: writing a guess corrupts it */
        static int said;
        if (said < 4) {
            said++;
            LOG("video: VIC config at iova %llx not mapped, job skipped\n", (unsigned long long)cfg_iova);
        }
        return;
    }
    u64 cfg;
    memcpy(&cfg, cs + 0x20, 8);
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
        static int said;
        if (said < 4) {
            said++;
            LOG("video: VIC output format %x isn't handled, job skipped\n", fmt);
        }
        return;
    }
    u32 w = g_fw < sw ? g_fw : sw, h = g_fh < sh ? g_fh : sh;
    {
        static int said;
        if (said < 3) {
            said++;
            LOG("video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u, picture %ux%u\n",
                (unsigned long long)cfg, fmt, blk ? "block linear" : "pitch linear", 1u << bh, sw, sh, g_fw, g_fh);
        }
    }
    if (fmt == 0x1F || fmt == 0x20) {
        /* pitch linear rows are 256 aligned, as in the NV12 path below. A block linear surface spans whole GOBs:
           64 bytes wide (bl_offset rounds the width up the same way), 8 << bh rows high */
        u32 pitch = blk ? sw * 4 : (sw * 4 + 255) & ~255u;
        u64 lim =
            blk ? (u64)((pitch + 63) & ~63u) * (((sh + (8u << bh) - 1) / (8u << bh)) * (8u << bh)) : (u64)pitch * sh;
        u8* py = iova_ptr(ly, lim);
        if (!py) {
            py = iova_ptr(ly, (u64)pitch * h);
            lim = (u64)pitch * h;
        }
        if (!py) {
            static int said;
            if (said < 4) {
                said++;
                LOG("video: VIC output at iova %llx not mapped\n", (unsigned long long)ly);
            }
            return;
        }
        static u8* row;
        static u32 rcap;
        if (rcap < pitch) {
            u8* nr = realloc(row, pitch);
            if (!nr) {
                WARN("video: out of memory for a %u byte VIC row\n", pitch);
                return;
            }
            row = nr;
            rcap = pitch;
        }
        int ri = fmt == 0x1F ? 0 : 2, bi = fmt == 0x1F ? 2 : 0;
        for (u32 y = 0; y < h; y++) {
            const u8* Y = g_fy + (u64)y * g_fw;
            const u8* UV = g_fuv + (u64)(y / 2) * g_fw;
            for (u32 x = 0; x < w; x++) {
                /* BT.601, limited range */
                int c = ((int)Y[x] - 16) * 298, d = (int)UV[x & ~1u] - 128, e = (int)UV[(x & ~1u) + 1] - 128;
                u8* o = row + x * 4;
                o[ri] = clamp8((c + 409 * e + 128) >> 8);
                o[1] = clamp8((c - 100 * d - 208 * e + 128) >> 8);
                o[bi] = clamp8((c + 516 * d + 128) >> 8);
                o[3] = 255;
            }
            if (!blk)
                vic_put(py, (u64)y * pitch, row, w * 4, lim);
            else
                for (u32 xb = 0; xb < w * 4; xb += 16) {
                    u32 n = w * 4 - xb < 16 ? w * 4 - xb : 16;
                    vic_put(py, bl_offset(xb, y, 0, pitch, sh, bh, 0), row + xb, n, lim);
                }
        }
        {
            static int said;
            if (said < 2) {
                said++;
                LOG("video: VIC wrote a %ux%u RGBA picture at iova %llx\n", w, h, (unsigned long long)ly);
            }
        }
        return;
    }
    /* NV12. pitch linear rows are 256 aligned */
    u32 pitch = blk ? sw : (sw + 255) & ~255u;
    u64 ylim = (u64)pitch * sh, clim = (u64)pitch * ((sh + 1) / 2);
    if (blk) {
        /* whole GOBs: 64 bytes wide (bl_offset rounds the width up the same way), 8 << bh rows high */
        u32 gy = 8u << bh;
        u64 gw = (pitch + 63) & ~63u;
        ylim = gw * ((sh + gy - 1) / gy * gy);
        clim = gw * (((sh + 1) / 2 + gy - 1) / gy * gy);
    }
    u8* py = iova_ptr(ly, ylim);
    u8* pc = lc ? iova_ptr(lc, clim) : NULL;
    if (!py) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: VIC output luma at iova %llx not mapped\n", (unsigned long long)ly);
        }
        return;
    }
    if (!pc) {
        static int said;
        if (said < 4) {
            said++;
            LOG("video: VIC output chroma at iova %llx not mapped, the picture has no color\n", (unsigned long long)lc);
        }
    }
    for (u32 y = 0; y < h; y++) {
        const u8* s = g_fy + (u64)y * g_fw;
        if (!blk)
            vic_put(py, (u64)y * pitch, s, w, ylim);
        else
            for (u32 x = 0; x < w; x += 16) {
                u32 n = w - x < 16 ? w - x : 16;
                vic_put(py, bl_offset(x, y, 0, pitch, sh, bh, 0), s + x, n, ylim);
            }
    }
    if (pc)
        for (u32 y = 0; y < h / 2; y++) {
            const u8* s = g_fuv + (u64)y * g_fw;
            if (!blk)
                vic_put(pc, (u64)y * pitch, s, w, clim);
            else
                for (u32 x = 0; x < w; x += 16) {
                    u32 n = w - x < 16 ? w - x : 16;
                    vic_put(pc, bl_offset(x, y, 0, pitch, (sh + 1) / 2, bh, 0), s + x, n, clim);
                }
        }
    {
        static int said;
        if (said < 2) {
            said++;
            LOG("video: VIC wrote a %ux%u NV12 picture (luma iova %llx, chroma %llx, pitch %u)\n", w, h,
                (unsigned long long)ly, (unsigned long long)lc, pitch);
        }
    }
}
