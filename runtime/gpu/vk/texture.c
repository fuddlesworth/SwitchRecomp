/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Textures from the guest's texture headers (TIC, 32 bytes each), and samplers from its sampler headers (TSC). A
   texture whose address holds a render target is that render target (a copy of its top-left corner when the header
   is smaller). Others are loaded from guest memory, deswizzled on the CPU, and checked for changes now and then:
   often at first, rarely once they have stayed the same. */
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu.h"
#include "gpu/texture/astc.h"
#include "platform/log.h"
#include "gpu/vk/vk.h"

/* the block height and depth (log2 GOBs) of a mip level of a block-linear texture. The hardware shrinks them for
   small levels: the height from the base level on, the depth only from level 1 on and by one step a level
   (Ryujinx's SizeCalculator) */
static void bl_level_blocks(u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* lbh, u32* lbd) {
    u32 lh = h >> level ? h >> level : 1, gy = ((lh + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    for (u32 l = 1; l <= level; l++) {
        u32 ld = d >> l ? d >> l : 1;
        if (bd > 0 && ld <= (1u << (bd - 1))) bd--;
    }
    *lbh = bh;
    *lbd = bd;
}
/* bytes of one mip level of a block-linear texture; lbh and lbd (may be NULL) get its block height and depth */
static u64 bl_level_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* lbh, u32* lbd) {
    u32 lw = w >> level ? w >> level : 1, lh = h >> level ? h >> level : 1, ld = d >> level ? d >> level : 1;
    u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
    u32 gx = (tw * f->bpp + 63) / 64, gy = (th + 7) / 8, b, bdd;
    bl_level_blocks(h, d, f, bh, bd, level, &b, &bdd);
    if (lbh) *lbh = b;
    if (lbd) *lbd = bdd;
    u64 gys = ((gy + (1u << b) - 1) >> b) << b;
    u64 lds = ((ld + (1u << bdd) - 1) >> bdd) << bdd;
    return (u64)gx * gys * lds * 512;
}
/* bytes from one layer of a block-linear texture to the next: all its levels, aligned to a block of GOBs that is
   shrunk for a small base level (Ryujinx's AlignLayerSize) */
static u64 bl_layer_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 levels) {
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl_level_size(w, h, d, f, bh, bd, l, NULL, NULL);
    u32 gy = ((h + f->bh - 1) / f->bh + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    while (bd > 0 && d <= (1u << (bd - 1))) bd--;
    u64 al = 512ull << (bh + bd);
    return (sz + al - 1) / al * al;
}

/* one level of one layer, from guest bytes to tightly packed rows, converted when the format needs it */
static void level_convert(const VkFmt* f, const u8* lin, u32 lw, u32 lh, u32 ld, u32 rowb, u8* out) {
    u32 th = (lh + f->bh - 1) / f->bh;
    switch (f->conv) {
        case VKC_ASTC: /* decoded to sRGB storage when the texture header says sRGB (format.c's TA) */
            astc_decode_image(lin, lw, lh, f->bw, f->bh, out, f->vk == VK_FORMAT_R8G8B8A8_SRGB);
            break;
        case VKC_RGBA4:
        case VKC_RGB5A1:
        case VKC_A5B5G5R1:
            for (u64 i = 0; i < (u64)lw * lh * ld; i++) {
                u32 v = ((const u16*)lin)[i], r, g, b, a;
                if (f->conv == VKC_RGBA4) {
                    r = (v & 15) * 17;
                    g = ((v >> 4) & 15) * 17;
                    b = ((v >> 8) & 15) * 17;
                    a = ((v >> 12) & 15) * 17;
                } else if (f->conv == VKC_RGB5A1) {
                    r = (v & 31) * 255 / 31;
                    g = ((v >> 5) & 31) * 255 / 31;
                    b = ((v >> 10) & 31) * 255 / 31;
                    a = (v >> 15) ? 255 : 0;
                } else { /* A5B5G5R1: one bit of red at the bottom, then green, blue and alpha (Ryujinx's swizzle) */
                    r = (v & 1) ? 255 : 0;
                    g = ((v >> 1) & 31) * 255 / 31;
                    b = ((v >> 6) & 31) * 255 / 31;
                    a = ((v >> 11) & 31) * 255 / 31;
                }
                ((u32*)out)[i] = r | (g << 8) | (b << 16) | (a << 24);
            }
            break;
        case VKC_RGB32F:
            for (u64 i = 0; i < (u64)lw * lh * ld; i++) {
                float* o = (float*)out + i * 4;
                memcpy(o, lin + i * 12, 12);
                o[3] = 1.0f;
            }
            break;
        default: memcpy(out, lin, (u64)rowb * th * ld);
    }
}
static u64 level_out_size(const VkFmt* f, u32 lw, u32 lh, u32 ld, u32 rowb) {
    u32 th = (lh + f->bh - 1) / f->bh;
    switch (f->conv) {
        case VKC_ASTC:
        case VKC_RGBA4:
        case VKC_RGB5A1:
        case VKC_A5B5G5R1: return (u64)lw * lh * ld * 4;
        case VKC_RGB32F: return (u64)lw * lh * ld * 16;
    }
    return (u64)rowb * th * ld;
}

static void tex_upload(Surf* t) {
    const VkFmt* f = &t->f;
    vk_render_end(); /* copies and clears happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (f->depth) {
        LOG_LIMIT(1,
                  "vulkan: depth textures from memory aren't loaded yet, they read as 1.0 (only depth render targets "
                  "are sampled)\n");
        /* defined contents rather than whatever the image's memory held: far, so shadow tests pass */
        VkClearDepthStencilValue far = {1.0f, 0};
        VkImageSubresourceRange all = {vk_aspect(f), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &all);
        return;
    }
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    u32 layers = t->layers, d3 = is3d ? t->depth3d : 1;
    VkBufferImageCopy* reg = calloc((size_t)layers * t->levels, sizeof *reg);
    if (!reg) return;
    u32 nreg = 0;
    VkBuffer buf = VK_NULL_HANDLE, regbuf = VK_NULL_HANDLE;
    u64 off = 0;
    for (u32 layer = 0; layer < layers; layer++) {
        u64 at = layer * t->layer_size;
        for (u32 l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l, lh = t->h >> l, ld = d3 >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (!ld) ld = 1;
            u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
            u32 rowb = tw * f->bpp, lbh = 0, lbd = 0;
            u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, l, &lbh, &lbd) : (u64)t->pitch * th;
            u8* src = gmem_ptr(t->gva + at, lsz);
            u8* tmp = NULL;
            if (!src) {
                tmp = malloc(lsz ? lsz : 1);
                gmem_read(t->gva + at, tmp, lsz);
                src = tmp;
            }
            u64 lin_size = (u64)rowb * th * ld;
            u8* lin = malloc(lin_size ? lin_size : 1);
            if (t->bl) {
                /* a row's 16-byte pieces: the row's place (bl_offset at x 0), then a block of GOBs per 64 bytes, and
                   within a GOB's row 32-byte halves 256 bytes apart and 16-byte quarters 32 apart */
                u64 bsz = 512ull << (lbh + lbd);
                for (u32 z = 0; z < ld; z++)
                    for (u32 y = 0; y < th; y++) {
                        u8* dr = lin + ((u64)z * th + y) * rowb;
                        u64 o0 = bl_offset(0, y, z, rowb, th, lbh, lbd);
                        for (u32 x = 0; x < rowb; x += 16) {
                            u64 o = o0 + (u64)(x >> 6) * bsz + ((x & 63) >> 5) * 256 + ((x & 31) >> 4) * 32;
                            u32 n = rowb - x < 16 ? rowb - x : 16;
                            if (o + n <= lsz)
                                memcpy(dr + x, src + o, n);
                            else
                                memset(dr + x, 0, n);
                        }
                    }
            } else
                for (u32 y = 0; y < th; y++) memcpy(lin + (u64)y * rowb, src + (u64)y * t->pitch, rowb);
            free(tmp);
            u64 osz = level_out_size(f, lw, lh, ld, rowb);
            u8* p = vk_stage(osz, 16, &buf, &off);
            if (p) {
                /* the staging ring grew into a new buffer: copy what went to the old one first */
                if (nreg && buf != regbuf) {
                    vkCmdCopyBufferToImage(cb, regbuf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, reg);
                    nreg = 0;
                }
                regbuf = buf;
                level_convert(f, lin, lw, lh, ld, rowb, p);
                reg[nreg++] = (VkBufferImageCopy){.bufferOffset = off,
                                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1},
                                                  .imageExtent = {lw, lh, ld}};
            }
            free(lin);
            at += lsz;
        }
    }
    if (nreg) vkCmdCopyBufferToImage(cb, regbuf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, reg);
    free(reg);
}

/* change checks: a hash of the whole texture when it is contiguous in host memory, else of 64 bytes of each page
   (the sampled check for big long-stable textures is vk_sample_hash) */
static u64 mem_hash(u64 gva, u64 size) {
    u8* p = gmem_ptr(gva, size);
    if (p) return vk_hash(p, size);
    u64 h = 1469598103934665603ull;
    for (u64 o = 0; o < size; o += 4096) {
        u64 v[8] = {0};
        gmem_read(gva + o, v, size - o < 64 ? size - o : 64);
        for (int i = 0; i < 8; i++) h = (h ^ v[i]) * 1099511628211ull;
    }
    return h;
}
/* the texture's memory hashed again, and loaded again when that changed: 1 if it did */
static int tex_rehash(Surf* t) {
    t->gw = g_gw_gen;
    u64 hh = mem_hash(t->gva, t->size);
    if (hh == t->hash) return 0;
    t->hash = hh;
    tex_upload(t);
    return 1;
}

static int format_samples(VkFormat f) {
    static VkFormat ok[64], bad[64];
    static int nok, nbad;
    for (int i = 0; i < nok; i++)
        if (ok[i] == f) return 1;
    for (int i = 0; i < nbad; i++)
        if (bad[i] == f) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    int r = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    if (r && nok < 64) ok[nok++] = f;
    if (!r && nbad < 64) {
        bad[nbad++] = f;
        WARN("vulkan: the GPU can't sample format %d (textures in it aren't drawn)\n", (int)f);
    }
    return r;
}

static Surf* tex_create(u64 gva, u32 w, u32 h, u32 layers, u32 d3, u32 levels, VkImageViewType type, VkFmt f, u32 fkey,
                        u32 bl, u32 bh, u32 bd, u32 pitch, u64 layer_size) {
    Surf* t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->gva = gva;
    t->w = t->sw = w;
    t->h = t->sh = h;
    t->layers = layers;
    t->depth3d = d3;
    t->levels = levels;
    t->type = type;
    t->f = f;
    t->fkey = fkey;
    t->bl = bl;
    t->bh = bh;
    t->bd = bd;
    t->pitch = pitch;
    t->layer_size = layer_size;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {w, h, type == VK_IMAGE_VIEW_TYPE_3D ? d3 : 1},
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (type != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h) ii.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkViewFormats vf;
    vk_view_formats(&ii, &vf);
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &t->img, &t->mem, NULL))) {
        free(t);
        return NULL;
    }
    t->last_used = g_vk_present;
    vk_surf_add(t);
    return t;
}

/* a render target read through a smaller header: a copy of that region, refreshed when the target is drawn to. One
   copy per size, in a list through next: one draw can hold copies of a target at two sizes (two headers, or two
   stages), so a copy isn't replaced while a draw may hold it. Copies not read for a second go; their views and image
   when the frame being recorded is done, as descriptors may still name them */
static Surf* rt_crop(Surf* rt, u32 w, u32 h) {
    Surf* c = NULL;
    for (Surf** pp = &rt->crop; *pp;) {
        Surf* x = *pp;
        if (x->w == w && x->h == h)
            c = x;
        else if ((i32)(g_vk_present - (u32)x->last_used) > 60) {
            *pp = x->next;
            for (int i = 0; i < x->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, x->sv[i].v, VK_NULL_HANDLE);
            vk_defer_image(x->img, x->mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
            free(x);
            continue;
        }
        pp = &x->next;
    }
    if (!c) {
        c = calloc(1, sizeof *c);
        if (!c) return rt;
        c->gva = rt->gva;
        c->w = w;
        c->h = h;
        c->sw = vk_sc(w) < rt->sw ? vk_sc(w) : rt->sw; /* the same region of the scaled target */
        c->sh = vk_sc(h) < rt->sh ? vk_sc(h) : rt->sh;
        c->layers = c->levels = c->depth3d = 1;
        c->f = rt->f;
        c->fkey = rt->fkey;
        c->type = VK_IMAGE_VIEW_TYPE_2D;
        c->is_rt = 1; /* sampled like one; not in the table, so never found as one */
        c->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = rt->f.vk,
                                .extent = {c->sw, c->sh, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VkViewFormats vf;
        vk_view_formats(&ii, &vf);
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &c->img, &c->mem, NULL))) {
            free(c);
            return rt;
        }
        c->crop_gen = ~0u;
        c->next = rt->crop;
        rt->crop = c;
        LOG_LIMIT(10, "vulkan: %ux%u texture reads the %ux%u render target at %llx: only that region is used\n", w, h,
                  rt->w, rt->h, (unsigned long long)rt->gva);
    }
    c->last_used = g_vk_present;
    if (c->crop_gen != rt->wgen) {
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        vk_surf_use(cb, c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageAspectFlags a = vk_aspect(&rt->f);
        VkImageCopy r = {.srcSubresource = {a, 0, 0, 1}, .dstSubresource = {a, 0, 0, 1}, .extent = {c->sw, c->sh, 1}};
        vkCmdCopyImage(cb, rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &r);
        c->crop_gen = rt->wgen;
    }
    return c;
}

/* texture header type -> what the image is created as */
static VkImageViewType tic_view_type(u32 type, u32 d) {
    switch (type) {
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return VK_IMAGE_VIEW_TYPE_CUBE;
        case 8: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case 4:
        case 5: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 1:
        case 7: return d > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
    return VK_IMAGE_VIEW_TYPE_2D; /* 1D textures are 2D images one texel high */
}

static u32 g_bud_frame;
static i64 g_bud;
Surf* vk_tex_from_tic(const u32* tic) {
    u64 gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    u32 hv = (tic[2] >> 21) & 7;
    int bl = hv == 3 || hv == 4;
    u32 w = (tic[4] & 0xFFFF) + 1, h = (tic[5] & 0xFFFF) + 1, d = ((tic[5] >> 16) & 0x3FFF) + 1;
    u32 type = (tic[4] >> 23) & 0xF;
    int srgb = (tic[4] >> 22) & 1;
    u32 glevels = ((tic[3] >> 28) & 0xF) + 1;
    u32 bh = (tic[3] >> 3) & 7, bd = (tic[3] >> 6) & 7, pitch = bl ? 0 : ((tic[3] & 0xFFFF) << 5);
    if (!gva) return NULL;
    if (hv == 0 || type == 6) { /* a buffer texture: its width spans words 3 and 4, it has no pitch or blocks */
        LOG_LIMIT(1, "vulkan: buffer textures aren't supported (not drawn)\n");
        return NULL;
    }
    if ((tic[7] >> 8) & 0xF) LOG_LIMIT(1, "vulkan: multisampled textures are read as single-sampled\n");
    if (type == 0 || type == 4) h = 1;
    if (type == 3)
        d = 6;
    else if (type == 8)
        d *= 6;
    VkFmt f = vk_tic_format(tic[0], srgb);
    if (!f.bpp || w > 16384 || h > 16384 || d > 2048) return NULL;
    if ((u64)w * h * d * (f.bw > 1 ? 1 : f.bpp) > (512ull << 20)) return NULL;
    if (!bl && pitch < (w + f.bw - 1) / f.bw * f.bpp) {
        LOG_LIMIT(1, "vulkan: a pitch-linear texture's pitch (%u bytes) is shorter than its rows (not drawn)\n", pitch);
        return NULL;
    }
    VkImageViewType vt = tic_view_type(type, d);
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE && d != 6) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    u32 layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : d, d3 = vt == VK_IMAGE_VIEW_TYPE_3D ? d : 1;
    /* bytes from one layer to the next: all the levels the header has, whichever of them it shows */
    u64 stride = bl ? bl_layer_size(w, h, d3, &f, bh, bd, glevels) : (u64)pitch * ((h + f.bh - 1) / f.bh);
    /* the levels the shader sees, base level to max level (word 7), as a texture of their own the way Ryujinx makes
       it: the base level's address, size and blocks. Pitch-linear textures have one level */
    u32 levels = bl ? glevels : 1;
    u32 base = tic[7] & 0xF, maxl = (tic[7] >> 4) & 0xF;
    if (bl && base < glevels && (base || maxl + 1 != glevels)) {
        for (u32 l = 0; l < base; l++) gva += bl_level_size(w, h, d3, &f, bh, bd, l, NULL, NULL);
        bl_level_blocks(h, d3, &f, bh, bd, base, &bh, &bd);
        w = w >> base ? w >> base : 1;
        h = h >> base ? h >> base : 1;
        d3 = d3 >> base ? d3 >> base : 1;
        levels = maxl >= base ? maxl - base + 1 : 1;
        if (levels > glevels - base) levels = glevels - base;
    }
    /* no more levels than the size has (Vulkan requires it) */
    u32 maxdim = w > h ? w : h, maxlev = 1;
    if (d3 > maxdim) maxdim = d3;
    while (maxdim >> maxlev) maxlev++;
    if (levels > maxlev) levels = maxlev;
    /* a render target at that address: read it on the GPU, unless its contents are undefined (never drawn, or memory
       it can't load was written over it): then memory has them */
    Surf* rt = vk_rt_find(gva);
    if (rt && rt->layout != VK_IMAGE_LAYOUT_UNDEFINED && f.bw == 1 && !f.conv && type != 2 && rt->w >= w &&
        rt->h >= h && (d == 1 || type == 1 || type == 7)) {
        /* a header of another texel size covers other memory than the target's pixels (depth is read through its
           own aspect, whatever the host format's size) */
        if (f.bpp == rt->f.bpp || f.depth || rt->f.depth) {
            vk_rt_note_read(rt);
            return rt->w == w && rt->h == h ? rt : rt_crop(rt, w, h);
        }
        LOG_LIMIT(1,
                  "vulkan: a texture with %u-byte texels reads the render target at %llx, which has %u-byte pixels: it "
                  "is loaded from memory\n",
                  f.bpp, (unsigned long long)gva, rt->f.bpp);
    }
    /* what the device can make */
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    int cube = vt != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h; /* created cube compatible */
    if (vt == VK_IMAGE_VIEW_TYPE_3D
            ? w > lim->maxImageDimension3D || h > lim->maxImageDimension3D || d3 > lim->maxImageDimension3D
            : w > lim->maxImageDimension2D || h > lim->maxImageDimension2D || layers > lim->maxImageArrayLayers ||
                  (cube && w > lim->maxImageDimensionCube)) {
        LOG_LIMIT(1, "vulkan: a %ux%ux%u texture is bigger than the GPU's images can be (not drawn)\n", w, h, d);
        return NULL;
    }
    u32 ilevels = f.conv == VKC_ASTC ? 1 : levels; /* ASTC is decoded on the CPU: the base level only */
    u32 fkey = (tic[0] & 0x7FFFF) | ((u32)srgb << 31);
    for (Surf* t = vk_surf_first(gva); t; t = t->next) {
        if (t->is_rt || t->gva != gva || t->w != w || t->h != h || t->layers != layers || t->depth3d != d3 ||
            t->fkey != fkey || t->levels != ilevels || t->type != vt || t->bl != (u32)bl || t->bh != bh ||
            t->bd != bd || t->pitch != pitch || t->layer_size != stride)
            continue;
        t->last_used = g_vk_present;
        /* the GPU front end wrote its memory (DMA, I2M, 2D, compute results): checked now, whatever the schedule
           below says, as writes within a frame have to show in the same frame. Not written: it still matches as of
           now, which keeps the next check's walk of the write log short */
        int fresh = 0;
        if (gpu_mem_dirty_since(t->gw, gva, t->size)) {
            fresh = 1;
            if (tex_rehash(t)) t->stable = 0;
        } else
            t->gw = g_gw_gen;
        /* with write tracking, checked once a frame only when its memory was written */
        if (t->checked != g_vk_present) {
            u32 wnow = vk_ww_now();
            int wr = vk_written_since(gmem_ptr(gva, t->size), t->size, t->wseen);
            if (wr >= 0) {
                t->checked = g_vk_present;
                if (wr && !fresh) tex_rehash(t);
                t->wseen = wnow + 1; /* this scan's marks are stamped with the clock after wnow */
                return t;
            }
        }
        /* without it: often at first, backing off while the texture stays the same, within a per-frame budget */
        if (t->checked != g_vk_present && (i32)(g_vk_present - t->next_check) >= 0) {
            if (g_bud_frame != g_vk_present) {
                g_bud_frame = g_vk_present;
                g_bud = 24 << 20;
            }
            if (t->stable >= 40 && t->size >= 16384 && (i32)(g_vk_present - t->tfull) < 120) {
                t->checked = g_vk_present;
                if (vk_sample_hash(gva, t->size) == t->shash) {
                    t->next_check = g_vk_present + 20 + ((u32)(gva >> 9) & 7);
                    return t;
                }
                t->stable = 0;
            }
            if (g_bud > 0 || t->stable < 4) {
                t->checked = g_vk_present;
                g_bud -= (i64)(t->size >> (t->size > 65536 ? 1 : 0));
                if (tex_rehash(t))
                    t->stable = 0;
                else if (t->stable < 100000)
                    t->stable++;
                if (t->stable >= 39 && t->size >= 16384) {
                    t->shash = vk_sample_hash(gva, t->size);
                    t->tfull = g_vk_present;
                }
                t->next_check = g_vk_present + (t->stable < 4 ? 4 : t->stable < 12 ? 8 : 20) + ((u32)(gva >> 9) & 3);
            }
        }
        return t;
    }
    if (!format_samples(f.vk)) return NULL;
    Surf* t = tex_create(gva, w, h, layers, d3, ilevels, vt, f, fkey, bl, bh, bd, pitch, stride);
    if (!t) return NULL;
    /* this texture's levels in one layer; the layers before the last one take their whole stride */
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++) sz += bl ? bl_level_size(w, h, d3, &f, bh, bd, l, NULL, NULL) : stride;
    t->size = stride * (layers - 1) + sz;
    t->wseen = vk_ww_now();
    vk_written_since(gmem_ptr(gva, t->size), t->size, 0); /* starts tracking it */
    t->wseen = vk_ww_now();
    t->gw = g_gw_gen;
    t->hash = mem_hash(gva, t->size);
    t->checked = g_vk_present;
    t->next_check = g_vk_present + 4;
    tex_upload(t);
    return t;
}

VkImageView vk_tex_view(Surf* s, VkImageViewType type, int srgb, const VkComponentMapping* map) {
    /* what a view of each image type can be */
    u32 layers;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_2D:
            if (s->type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = 1;
            break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            if (s->type == VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = s->layers;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
            if (s->layers < 6 || s->w != s->h || s->type == VK_IMAGE_VIEW_TYPE_3D || s->is_rt) return VK_NULL_HANDLE;
            layers = 6;
            break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            if (s->layers < 6 || s->w != s->h || s->type == VK_IMAGE_VIEW_TYPE_3D || s->is_rt ||
                !g_vk.feat.imageCubeArray)
                return VK_NULL_HANDLE;
            layers = s->layers / 6 * 6;
            break;
        case VK_IMAGE_VIEW_TYPE_3D:
            if (s->type != VK_IMAGE_VIEW_TYPE_3D) return VK_NULL_HANDLE;
            layers = 1;
            break;
        default: return VK_NULL_HANDLE;
    }
    VkFormat fmt = s->f.depth ? s->f.vk : vk_srgb_as(s->f.vk, srgb);
    u64 key = (u64)type | (u64)map->r << 4 | (u64)map->g << 8 | (u64)map->b << 12 | (u64)map->a << 16 | (u64)fmt << 20;
    for (int i = 0; i < s->nsv; i++)
        if (s->sv[i].key == key) return s->sv[i].v;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = type,
                                .format = fmt,
                                .components = *map,
                                .subresourceRange = {s->f.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                                     0, s->levels, 0, layers}};
    VkImageView v;
    if (!VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &v))) return VK_NULL_HANDLE;
    if (s->nsv == (int)(sizeof s->sv / sizeof s->sv[0])) { /* full: drop the oldest */
        vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[0].v, VK_NULL_HANDLE);
        memmove(s->sv, s->sv + 1, sizeof s->sv - sizeof s->sv[0]);
        s->nsv--;
    }
    s->sv[s->nsv++] = (SurfView){key, v};
    return v;
}

/* empty textures for slots whose texture is missing or doesn't match the shader: zero, or depth 1.0 for shadow
   samplers, which compare against depth formats only */
VkImageView vk_null_view(VkImageViewType type, int num) {
    static VkImageView views[8][4];
    static Surf* imgs[2][4]; /* [3D][num] */
    if ((unsigned)type >= 8 || num < 0 || num > 3) return VK_NULL_HANDLE;
    if (num == 3 && type == VK_IMAGE_VIEW_TYPE_3D) num = 0; /* no 3D depth images (nor 3D shadow samplers) */
    if (views[type][num]) return views[type][num];
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) return VK_NULL_HANDLE;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D;
    Surf* s = imgs[is3d][num];
    if (!s) {
        static const VkFormat fm[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
                                       VK_FORMAT_D16_UNORM};
        s = calloc(1, sizeof *s);
        if (!s) return VK_NULL_HANDLE;
        s->w = s->h = s->sw = s->sh = s->levels = s->depth3d = 1;
        s->layers = is3d ? 1 : 6;
        s->type = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        s->f = num == 3 ? (VkFmt){.vk = fm[3], .bpp = 2, .bw = 1, .bh = 1, .depth = 1}
                        : (VkFmt){fm[num], 4, 1, 1, 0, 0, (u8)(num != 0), 0, (u8)(num == 2)};
        s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = is3d ? 0 : VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
                                .imageType = is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                .format = fm[num],
                                .extent = {1, 1, 1},
                                .mipLevels = 1,
                                .arrayLayers = s->layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &s->img, &s->mem, NULL))) {
            free(s);
            return VK_NULL_HANDLE;
        }
        vk_render_end(); /* clears happen outside rendering passes */
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageSubresourceRange all = {vk_aspect(&s->f), 0, 1, 0, s->layers};
        if (num == 3) {
            VkClearDepthStencilValue far = {1.0f, 0};
            vkCmdClearDepthStencilImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &all);
        } else {
            VkClearColorValue z = {{0, 0, 0, 0}};
            vkCmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        }
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        imgs[is3d][num] = s;
    }
    VkComponentMapping id = {0};
    views[type][num] = vk_tex_view(s, type, 0, &id);
    return views[type][num];
}

/* ------------------------------------------------------------ samplers */
static VkSamplerAddressMode wrap(u32 m) {
    switch (m) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2:
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3:
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case 5:
        case 7:
            /* an optional feature: without it mirrored repeat, the same over the first mirror */
            return g_vk.has_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                         : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/* tsc[0] bit 9 compare, bits 10-12 the compare function; tsc[1] filters (gpu/vk/render.c forces nearest for integer
   textures and the compare mode the shader declares) */
VkSampler vk_sampler(const u32* tsc) {
    /* samplers are kept for good, and a device allows at least 4000 (maxSamplerAllocationCount): past 3072, a quarter
       of the table left free for probing, new sampler states get the first sampler made */
    enum { TAB = 4096, MAX = 3072 };
    static struct {
        u32 tsc[8];
        VkSampler s;
    } tab[TAB];
    static u32 n;
    static VkSampler first;
    u32 hk = 2166136261u;
    for (int i = 0; i < 8; i++) hk = (hk ^ tsc[i]) * 16777619u;
    u32 j = hk & (TAB - 1);
    for (u32 probe = 0; probe < TAB && tab[j].s; probe++, j = (j + 1) & (TAB - 1))
        if (!memcmp(tab[j].tsc, tsc, 32)) return tab[j].s;
    if (n >= MAX || tab[j].s) {
        LOG_LIMIT(1, "vulkan: more than %d sampler states: the others use the first one\n", MAX);
        return first;
    }
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3;
    float minl = (float)(tsc[2] & 0xFFF) / 256.f, maxl = (float)((tsc[2] >> 12) & 0xFFF) / 256.f;
    i32 bias = (i32)((tsc[1] >> 12) & 0x1FFF);
    if (bias & 0x1000) bias |= ~0x1FFF;
    float lb = (float)bias / 256.f, lbmax = g_vk.props.limits.maxSamplerLodBias;
    if (lb > lbmax) lb = lbmax;
    if (lb < -lbmax) lb = -lbmax;
    static const float aniso_lut[8] = {1, 2, 4, 6, 8, 10, 12, 16}; /* Ryujinx's SamplerDescriptor */
    u32 aniso = (tsc[0] >> 20) & 7;
    if ((tsc[1] >> 10) & 3)
        LOG_LIMIT(1, "vulkan: min/max sampler reduction isn't supported (filtered as an average)\n");
    if ((tsc[0] >> 13) & 1)
        LOG_LIMIT(1, "vulkan: a sampler's sRGB conversion flag (TSC word 0 bit 13) isn't applied\n");
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = wrap(tsc[0] & 7),
                              .addressModeV = wrap((tsc[0] >> 3) & 7),
                              .addressModeW = wrap((tsc[0] >> 6) & 7),
                              .mipLodBias = lb,
                              .anisotropyEnable = aniso && g_vk.feat.samplerAnisotropy,
                              .maxAnisotropy = aniso_lut[aniso],
                              .compareEnable = (tsc[0] >> 9) & 1,
                              .compareOp = (VkCompareOp)((tsc[0] >> 10) & 7),
                              .minLod = minl,
                              .maxLod = mip <= 1 ? minl + 0.25f : maxl, /* no mip filter: the base level only */
                              .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    if (si.maxAnisotropy > g_vk.props.limits.maxSamplerAnisotropy)
        si.maxAnisotropy = g_vk.props.limits.maxSamplerAnisotropy;
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    float bc[4];
    memcpy(bc, &tsc[4], 16);
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType =
                                                      VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 0)
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 1)
        si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (bc[0] == 1 && bc[1] == 1 && bc[2] == 1 && bc[3] == 1)
        si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (g_vk.has_border_color) {
        memcpy(cb.customBorderColor.float32, bc, 16);
        si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        si.pNext = &cb;
    }
    VkSampler s;
    if (!VK_OK(vkCreateSampler(g_vk.dev, &si, NULL, &s))) return VK_NULL_HANDLE;
    memcpy(tab[j].tsc, tsc, 32);
    tab[j].s = s;
    if (!n++) first = s;
    return s;
}

void vk_tex_frame(void) {
    /* textures and render targets not used for ten seconds go; textures come back from memory if they are needed
       again */
    if (g_vk_present % 60 == 0 && g_vk_present > 600) vk_surf_evict(g_vk_present - 600);
}
