/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the copy engine: DMA between linear and block-linear surfaces in guest memory */
#include "gpu/engines_int.h"
/* ============================================================ DMA */
volatile u64 g_dma_copies;
/* host pointer to a whole bl surface (slice dz, block depth bd) if it's contiguous, so DMA can memcpy
   instead of translating + tracking every 16 bytes */
static u8* bl_span_ptr(u64 base, u32 wbytes, u32 h, u32 bh, u32 z, u32 bd, u64* span) {
    static int off = -1;
    if (off < 0) off = getenv("SWR_DMA_SLOW") != NULL;
    if (off) return NULL;
    u64 gh = 8ull << bh, gd = 1ull << bd;
    u64 sz = (((u64)wbytes + 63) & ~63ull) * ((h + gh - 1) / gh * gh) * ((z / gd + 1) * gd);
    if (!sz || sz > (256ull << 20)) return NULL;
    u8* p = gmem_ptr(base, sz);
    if (p) *span = sz;
    return p;
}
static void bl_span_done(u64 base, u64 span) {
    if (g_ww_touch) {
        u8* p = gmem_ptr(base, span);
        if (p)
            for (u64 o = 0; o < span; o += 0x10000) g_ww_touch((u64)(p - g_mem) + o);
    }
    gpu_mem_written(base, span);
}
void dma_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x1FF;
    ch->rdma[m] = v;
    if (m != 0xC0) return; /* launch_dma */
    u32* r = ch->rdma;
    u64 src = ((u64)r[0x100] << 32) | r[0x101], dst = ((u64)r[0x102] << 32) | r[0x103];
    u32 pitch_in = r[0x104], pitch_out = r[0x105], line_len = r[0x106], lines = r[0x107];
    int src_linear = (v >> 7) & 1, dst_linear = (v >> 8) & 1, multi = (v >> 9) & 1, remap = (v >> 10) & 1;
    if (!multi) lines = 1;
    u32 bpp = 1;
    if (remap) {
        u32 rc = r[0x1C2];
        u32 comp = ((rc >> 16) & 3) + 1, csz = ((rc >> 20) & 3) + 1;
        bpp = comp * csz;
        line_len *= bpp;
    }
    /* bl params: dst 0x1C3.., src 0x1CA.. */
    u32 dbh = (r[0x1C3] >> 4) & 15, dw = r[0x1C4], dh = r[0x1C5], dd = r[0x1C6];
    (void)dd;
    u32 sbh = (r[0x1CA] >> 4) & 15, sw = r[0x1CB], sh = r[0x1CC];
    u32 sx = r[0x1CF] & 0xFFFF, sy = r[0x1CF] >> 16;
    u32 dx = r[0x1C8] & 0xFFFF, dy = r[0x1C8] >> 16;
    /* array layer = whole 2D bl surface stride */
    /* 3D (depth>1): layer reg is the z slice inside the volume, block depth at 11:8 */
    u32 dz = 0, sz_ = 0, dbd = 0, sbd = 0;
    if (!dst_linear && r[0x1C6] > 1) {
        dz = r[0x1C7];
        dbd = (r[0x1C3] >> 8) & 15;
    }
    if (!src_linear && r[0x1CD] > 1) {
        sz_ = r[0x1CE];
        sbd = (r[0x1CA] >> 8) & 15;
    }
    {
        u32 dl = dz ? 0 : r[0x1C7], sl = sz_ ? 0 : r[0x1CE];
        if (r[0x1C6] > 1) dl = 0;
        if (r[0x1CD] > 1) sl = 0;
        if (!dst_linear && dl) {
            u32 wbb = dw * (remap ? bpp : 1);
            u64 gh = 8u << dbh;
            u64 lsz = (((u64)wbb + 63) & ~63ull) * ((dh + gh - 1) / gh * gh);
            dst += lsz * dl;
        }
        if (!src_linear && sl) {
            u32 wbb = sw * (remap ? bpp : 1);
            u64 gh = 8u << sbh;
            u64 lsz = (((u64)wbb + 63) & ~63ull) * ((sh + gh - 1) / gh * gh);
            src += lsz * sl;
        }
    }
    {
        static u64 ndma;
        ndma++;
        g_dma_copies = ndma;
    }
    {
        static int said;
        if (said < 6) {
            said++;
            LOG("dma: %s->%s src=%llx dst=%llx pitch %u/%u len=%u lines=%u multi=%d remap=%d(%x) dst[bh=%u w=%u h=%u d=%u l=%u o=%x] src[bh=%u w=%u h=%u o=%x] launch=%x\n",
                src_linear ? "lin" : "bl", dst_linear ? "lin" : "bl", (unsigned long long)src, (unsigned long long)dst,
                pitch_in, pitch_out, line_len, lines, multi, remap, r[0x1C2], dbh, dw, dh, r[0x1C6], r[0x1C7], r[0x1C8],
                sbh, sw, sh, r[0x1CF], v);
        }
    }
    if (g_gl) {
        char tb[256];
        snprintf(tb, sizeof tb, "DMA %s %llx -> %s %llx len=%u lines=%u remap=%d\n", src_linear ? "lin" : "bl",
                 (unsigned long long)src, dst_linear ? "lin" : "bl", (unsigned long long)dst, line_len, lines, remap);
        g_renderer->trace(tb);
        g_renderer->rt_flush(src);
        g_renderer->rt_flush(dst);
    }
    u8* dpp = NULL;
    u8* spp = NULL;
    u64 dspan = 0, sspan = 0;
    (void)spp;
    (void)sspan;
    if (src_linear && dst_linear) {
        if (!multi) {
            u8* tmp = malloc(line_len);
            gmem_read(src, tmp, line_len);
            gmem_write(dst, tmp, line_len);
            free(tmp);
        } else {
            u8* tmp = malloc(line_len);
            for (u32 y = 0; y < lines; y++) {
                gmem_read(src + (u64)y * pitch_in, tmp, line_len);
                gmem_write(dst + (u64)y * pitch_out, tmp, line_len);
            }
            free(tmp);
        }
    } else if (src_linear && !dst_linear &&
               (dpp = bl_span_ptr(dst, dw * (remap ? bpp : 1), dh, dbh, dz, dbd, &dspan))) {
        /* contiguous dst: copy directly, one write record at the end */
        u32 wb = dw * (remap ? bpp : 1);
        u8* buf = malloc(line_len);
        for (u32 y = 0; y < lines; y++) {
            gmem_read(src + (u64)y * pitch_in, buf, line_len);
            for (u32 x = 0; x < line_len;) {
                u32 xb = dx * (remap ? bpp : 1) + x, n = 16 - (xb & 15);
                if (n > line_len - x) n = line_len - x;
                u64 o = bl_offset(xb, dy + y, dz, wb, dh, dbh, dbd);
                if (o + n <= dspan)
                    memcpy(dpp + o, buf + x, n);
                else
                    gmem_write(dst + o, buf + x, n);
                x += n;
            }
        }
        free(buf);
        bl_span_done(dst, dspan);
    } else if (src_linear && !dst_linear) {
        u32 wb = dw * (remap ? bpp : 1);
        u8* buf = malloc(line_len);
        for (u32 y = 0; y < lines; y++) {
            gmem_read(src + (u64)y * pitch_in, buf, line_len);
            /* bl keeps 16 byte groups contiguous, so copy per group not per byte (was ~11% in profiles) */
            for (u32 x = 0; x < line_len;) {
                u32 xb = dx * (remap ? bpp : 1) + x, n = 16 - (xb & 15);
                if (n > line_len - x) n = line_len - x;
                u64 o = bl_offset(xb, dy + y, dz, wb, dh, dbh, dbd);
                gmem_write(dst + o, buf + x, n);
                x += n;
            }
        }
        free(buf);
    } else if (!src_linear && dst_linear) {
        u32 wb = sw * (remap ? bpp : 1);
        u8* buf = malloc(line_len);
        for (u32 y = 0; y < lines; y++) {
            for (u32 x = 0; x < line_len;) {
                u32 xb = sx * (remap ? bpp : 1) + x, n = 16 - (xb & 15);
                if (n > line_len - x) n = line_len - x;
                u64 o = bl_offset(xb, sy + y, sz_, wb, sh, sbh, sbd);
                gmem_read(src + o, buf + x, n);
                x += n;
            }
            gmem_write(dst + (u64)y * pitch_out, buf, line_len);
        }
        free(buf);
    } else if (!src_linear && !dst_linear &&
               (dpp = bl_span_ptr(dst, dw * (remap ? bpp : 1), dh, dbh, dz, dbd, &dspan)) &&
               (spp = bl_span_ptr(src, sw * (remap ? bpp : 1), sh, sbh, sz_, sbd, &sspan))) {
        u32 swb = sw * (remap ? bpp : 1), dwb = dw * (remap ? bpp : 1);
        for (u32 y = 0; y < lines; y++)
            for (u32 x = 0; x < line_len;) {
                u32 xs = sx * (remap ? bpp : 1) + x, xd = dx * (remap ? bpp : 1) + x, n = 16 - (xs & 15),
                    m = 16 - (xd & 15);
                if (m < n) n = m;
                if (n > line_len - x) n = line_len - x;
                u64 so = bl_offset(xs, sy + y, sz_, swb, sh, sbh, sbd),
                    dof = bl_offset(xd, dy + y, dz, dwb, dh, dbh, dbd);
                if (so + n <= sspan && dof + n <= dspan)
                    memmove(dpp + dof, spp + so, n);
                else {
                    u8 b[16];
                    gmem_read(src + so, b, n);
                    gmem_write(dst + dof, b, n);
                }
                x += n;
            }
        bl_span_done(dst, dspan);
    } else {
        u32 swb = sw * (remap ? bpp : 1), dwb = dw * (remap ? bpp : 1);
        for (u32 y = 0; y < lines; y++)
            for (u32 x = 0; x < line_len;) {
                u32 xs = sx * (remap ? bpp : 1) + x, xd = dx * (remap ? bpp : 1) + x, n = 16 - (xs & 15),
                    m = 16 - (xd & 15);
                if (m < n) n = m;
                if (n > line_len - x) n = line_len - x;
                u8 b[16];
                gmem_read(src + bl_offset(xs, sy + y, sz_, swb, sh, sbh, sbd), b, n);
                gmem_write(dst + bl_offset(xd, dy + y, dz, dwb, dh, dbh, dbd), b, n);
                x += n;
            }
    }
    if (g_gl) g_renderer->rt_reload(dst);
    /* completion semaphore */
    u32 sem = (v >> 3) & 3;
    if (sem) {
        u64 sa = ((u64)r[0x90] << 32) | r[0x91];
        if (sem == 1 || sem == 2) gmem_w32(sa, r[0x92]);
        if (sem == 2) {
            u64 t = gpu_ticks();
            gmem_w64(sa + 8, t);
        }
    }
}

void gpu_flush(GpuChannel* ch) { r3d_flush(ch); }
