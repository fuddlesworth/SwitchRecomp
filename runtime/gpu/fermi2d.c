/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the 2D engine (Fermi 2D): blits between surfaces, on the renderer or in guest memory */
#include "gpu/engines_int.h"
/* ============================================================ Fermi 2D (blits) */
static void surf2d(u32* r, int b, Surf2D* s) {
    s->format = r[b];
    s->linear = r[b + 1];
    s->bh = (r[b + 2] >> 4) & 15;
    s->depth = r[b + 3];
    s->layer = r[b + 4];
    s->pitch = r[b + 5];
    s->width = r[b + 6];
    s->height = r[b + 7];
    s->addr = ((u64)r[b + 8] << 32) | r[b + 9];
}
u32 fmt_bpp_2d(u32 f) {
    switch (f) {
        case 0xE6:
        case 0xE9:
        case 0xD5:
        case 0xCF:
        case 0xD8:
        case 0xD9:
        case 0xDF:
        case 0xC2:
        case 0xCA:
        case 0xDE:
        case 0xC8:
        case 0xE0:
        case 0xE2: return 4;
        case 0xF3:
        case 0xE8:
        case 0xF8:
        case 0xEA:
        case 0xEB:
        case 0xEC:
        case 0xFE:
        case 0xFF: return 1;
        case 0xC0:
        case 0xC1:
        case 0xC3:
        case 0xC4:
        case 0xC5:
        case 0xC6:
        case 0xC7: return 16;
        case 0xCB:
        case 0xCC:
        case 0xCE: return 8;
        default: return 4;
    }
}
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x3FF;
    ch->r2d[m] = v;
    if (m == 0x237) { /* src_y0 write kicks the blit */
        u32* r = ch->r2d;
        Surf2D dst, src;
        surf2d(r, 0x80, &dst);
        surf2d(r, 0x8C, &src);
        i32 dx0 = (i32)r[0x22C], dy0 = (i32)r[0x22D], dw = (i32)r[0x22E], dh = (i32)r[0x22F];
        /* 32.32 fixed point, frac reg comes before int reg (DU_DX_FRAC, DU_DX_INT...) */
        i64 dudx = (i64)(((u64)r[0x231] << 32) | r[0x230]);
        i64 dvdy = (i64)(((u64)r[0x233] << 32) | r[0x232]);
        i64 sx0 = (i64)(((u64)r[0x235] << 32) | r[0x234]);
        i64 sy0 = (i64)(((u64)r[0x237] << 32) | r[0x236]);
        double sx = (double)sx0 / 4294967296.0, sy = (double)sy0 / 4294967296.0, sdx = (double)dudx / 4294967296.0,
               sdy = (double)dvdy / 4294967296.0;
        /* CENTER origin (sample_mode bit0 == 0): coords address texel centers */
        if (!(r[0x223] & 1)) {
            sx -= sdx * 0.5;
            sy -= sdy * 0.5;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
        }
        if (r3d_blit(&src, &dst, dx0, dy0, dw, dh, sx, sy, sdx, sdy)) return;
        if (g_gl) {
            g_renderer->rt_flush(src.addr);
            g_renderer->rt_flush(dst.addr);
        }
        u32 bpp = fmt_bpp_2d(dst.format), sbpp = fmt_bpp_2d(src.format);
        if (bpp != sbpp) bpp = sbpp;
        for (i32 y = 0; y < dh; y++) {
            u32 syy = (u32)(sy + sdy * y);
            for (i32 x = 0; x < dw; x++) {
                u32 sxx = (u32)(sx + sdx * x);
                u8 px[16];
                u64 so = src.linear ? (u64)syy * src.pitch + sxx * bpp
                                    : bl_offset(sxx * bpp, syy, 0, src.width * bpp, src.height, src.bh, 0);
                u64 dof = dst.linear ? (u64)(dy0 + y) * dst.pitch + (dx0 + x) * bpp
                                     : bl_offset((dx0 + x) * bpp, dy0 + y, 0, dst.width * bpp, dst.height, dst.bh, 0);
                gmem_read(src.addr + so, px, bpp);
                gmem_write(dst.addr + dof, px, bpp);
            }
        }
        if (g_gl) g_renderer->rt_reload(dst.addr);
    }
}
