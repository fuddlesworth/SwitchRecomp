/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Textures from the guest's texture headers (TIC, 32 bytes each), and samplers from its sampler headers (TSC). A texture whose address holds a render target is that render
   target (a copy of its top-left corner when the header is smaller). Others are loaded from guest memory, deswizzled
   on the CPU, and checked for changes now and then: often at first, rarely once they have stayed the same. */
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu.h"
#include "gpu/texture/astc.h"
#include "platform/log.h"
#include "gpu/vk/vk.h"

/* bytes of one mip level of a block-linear texture, block height and depth shrunk for small levels as the hardware
   does; out_bh gets the level's block height */
static u64 bl_level_size(u32 w, u32 h, u32 d, const VkFmt* f, u32 bh, u32 bd, u32 level, u32* out_bh) {
    u32 lw = w >> level, lh = h >> level, ld = d >> level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (!ld) ld = 1;
    u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
    u32 rowb = tw * f->bpp;
    u32 gx = (rowb + 63) / 64, gy = (th + 7) / 8;
    u32 b = bh;
    while (b > 0 && gy <= (1u << (b - 1))) b--;
    u32 bdd = bd;
    while (bdd > 0 && ld <= (1u << (bdd - 1))) bdd--;
    if (out_bh) *out_bh = b;
    u64 gys = ((gy + (1u << b) - 1) >> b) << b;
    u64 lds = ((ld + (1u << bdd) - 1) >> bdd) << bdd;
    return (u64)gx * gys * lds * 512;
}

/* one level of one layer, from guest bytes to tightly packed rows, converted when the format needs it */
static void level_convert(const VkFmt* f, const u8* lin, u32 lw, u32 lh, u32 ld, u32 rowb, u8* out) {
    u32 th = (lh + f->bh - 1) / f->bh;
    switch (f->conv) {
        case VKC_ASTC: astc_decode_image(lin, lw, lh, f->bw, f->bh, out); break;
        case VKC_RGBA4:
        case VKC_RGB5A1:
            for (u64 i = 0; i < (u64)lw * lh * ld; i++) {
                u32 v = ((const u16*)lin)[i], r, g, b, a;
                if (f->conv == VKC_RGBA4) {
                    r = (v & 15) * 17;
                    g = ((v >> 4) & 15) * 17;
                    b = ((v >> 8) & 15) * 17;
                    a = ((v >> 12) & 15) * 17;
                } else {
                    r = (v & 31) * 255 / 31;
                    g = ((v >> 5) & 31) * 255 / 31;
                    b = ((v >> 10) & 31) * 255 / 31;
                    a = (v >> 15) ? 255 : 0;
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
        case VKC_RGB5A1: return (u64)lw * lh * ld * 4;
        case VKC_RGB32F: return (u64)lw * lh * ld * 16;
    }
    return (u64)rowb * th * ld;
}

static void tex_upload(Surf* t) {
    const VkFmt* f = &t->f;
    if (f->depth) {
        static int said;
        if (!said) {
            said = 1;
            LOG("vulkan: depth textures from memory aren't loaded yet (only depth render targets are sampled)\n");
        }
        return;
    }
    int is3d = t->type == VK_IMAGE_VIEW_TYPE_3D;
    u32 layers = t->layers, d3 = is3d ? t->depth3d : 1;
    u64 layer_size = 0;
    for (u32 l = 0; l < t->levels; l++) layer_size += bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, l, NULL);
    if (t->bl && layers > 1) {
        u64 al = 512ull << t->bh;
        layer_size = (layer_size + al - 1) / al * al;
    }
    VkBufferImageCopy* reg = calloc((size_t)layers * t->levels, sizeof *reg);
    u32 nreg = 0;
    VkBuffer buf = VK_NULL_HANDLE, regbuf = VK_NULL_HANDLE;
    u64 off = 0;
    vk_render_end(); /* copies happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (u32 layer = 0; layer < layers; layer++) {
        u64 at = t->bl ? layer * layer_size : 0;
        for (u32 l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l, lh = t->h >> l, ld = d3 >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (!ld) ld = 1;
            u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
            u32 rowb = tw * f->bpp, lbh = 0;
            u64 lsz = t->bl ? bl_level_size(t->w, t->h, d3, f, t->bh, t->bd, l, &lbh) : (u64)t->pitch * th;
            if (!t->bl) at = (u64)layer * t->pitch * th; /* pitch textures have one level */
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
                u32 lbd = t->bd;
                while (lbd > 0 && ld <= (1u << (lbd - 1))) lbd--;
                for (u32 z = 0; z < ld; z++)
                    for (u32 y = 0; y < th; y++) {
                        u8* dr = lin + ((u64)z * th + y) * rowb;
                        for (u32 x = 0; x < rowb; x += 16) {
                            u64 o = bl_offset(x, y, z, rowb, th, lbh, lbd);
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
            if (t->bl) at += lsz;
        }
    }
    if (nreg) vkCmdCopyBufferToImage(cb, regbuf, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, reg);
    free(reg);
}

/* change checks: a hash of the whole texture, or for big long-stable ones a sample of it */
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
static u64 sample_hash(u64 gva, u64 len) {
    u8 tmp[2048 + 128];
    if (len <= sizeof tmp) {
        gmem_read(gva, tmp, len);
        return vk_hash(tmp, len);
    }
    u32 n = 0;
    gmem_read(gva, tmp, 64);
    n = 64;
    gmem_read(gva + len - 64, tmp + n, 64);
    n += 64;
    u64 step = (len - 32) / 62;
    for (int i = 0; i < 62; i++) {
        gmem_read(gva + (u64)i * step, tmp + n, 32);
        n += 32;
    }
    return vk_hash(tmp, n);
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
                        u32 bl, u32 bh, u32 bd, u32 pitch) {
    Surf* t = calloc(1, sizeof *t);
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
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateFlags fl = f.depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (type != VK_IMAGE_VIEW_TYPE_3D && layers >= 6 && w == h) fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = fl,
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
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &t->img, &t->mem, NULL))) {
        free(t);
        return NULL;
    }
    t->last_used = g_vk_present;
    vk_surf_add(t);
    return t;
}

/* a render target read through a smaller header: a copy of that region, refreshed when the target is drawn to */
static Surf* rt_crop(Surf* rt, u32 w, u32 h) {
    Surf* c = rt->crop;
    if (c && (c->w != w || c->h != h)) {
        for (int i = 0; i < c->nsv; i++) vkDestroyImageView(g_vk.dev, c->sv[i].v, NULL);
        vk_defer_image(c->img, c->mem, VK_NULL_HANDLE, VK_NULL_HANDLE);
        free(c);
        c = rt->crop = NULL;
    }
    if (!c) {
        c = calloc(1, sizeof *c);
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
                                .flags = rt->f.depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = rt->f.vk,
                                .extent = {c->sw, c->sh, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &c->img, &c->mem, NULL))) {
            free(c);
            return rt;
        }
        c->crop_gen = ~0u;
        rt->crop = c;
        static int said;
        if (said < 10) {
            said++;
            LOG("vulkan: %ux%u texture reads the %ux%u render target at %llx: only that region is used\n", w, h, rt->w,
                rt->h, (unsigned long long)rt->gva);
        }
    }
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
    u32 levels = ((tic[3] >> 28) & 0xF) + 1;
    u32 bh = (tic[3] >> 3) & 7, bd = (tic[3] >> 6) & 7, pitch = bl ? 0 : ((tic[3] & 0xFFFF) << 5);
    if (type == 0 || type == 4) h = 1;
    if (type == 3)
        d = 6;
    else if (type == 8)
        d *= 6;
    VkFmt f = vk_tic_format(tic[0], srgb);
    if (!f.bpp || !gva || w > 16384 || h > 16384 || d > 2048 || levels > 15) return NULL;
    if ((u64)w * h * d * (f.bw > 1 ? 1 : f.bpp) > (512ull << 20)) return NULL;
    /* a render target at that address: read it on the GPU */
    Surf* rt = vk_rt_find(gva);
    if (rt && f.bw == 1 && !f.conv && type != 2 && rt->w >= w && rt->h >= h && (d == 1 || type == 1 || type == 7)) {
        rt->last_used = vk_frame_number();
        return rt->w == w && rt->h == h ? rt : rt_crop(rt, w, h);
    }
    VkImageViewType vt = tic_view_type(type, d);
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE && d != 6) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (f.conv == VKC_ASTC && levels > 1) levels = 1;
    if (!bl) levels = 1;
    u32 layers = vt == VK_IMAGE_VIEW_TYPE_3D ? 1 : d, d3 = vt == VK_IMAGE_VIEW_TYPE_3D ? d : 1;
    u32 fkey = (tic[0] & 0x7FFFF) | ((u32)srgb << 31);
    for (Surf* t = vk_surf_first(gva); t; t = t->next) {
        if (t->is_rt || t->gva != gva || t->w != w || t->h != h || t->layers != layers || t->depth3d != d3 ||
            t->fkey != fkey || t->levels != levels || t->type != vt)
            continue;
        t->last_used = g_vk_present;
        /* with write tracking, checked once a frame only when its memory was written */
        if (t->checked != g_vk_present) {
            u32 wnow = vk_ww_now();
            int w = vk_written_since(gmem_ptr(gva, t->size), t->size, t->wseen);
            if (w >= 0) {
                t->checked = g_vk_present;
                if (w) {
                    u64 hh = mem_hash(gva, t->size);
                    if (hh != t->hash) {
                        t->hash = hh;
                        tex_upload(t);
                    }
                }
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
                if (sample_hash(gva, t->size) == t->shash) {
                    t->next_check = g_vk_present + 20 + ((u32)(gva >> 9) & 7);
                    return t;
                }
                t->stable = 0;
            }
            if (g_bud > 0 || t->stable < 4) {
                t->checked = g_vk_present;
                g_bud -= (i64)(t->size >> (t->size > 65536 ? 1 : 0));
                u64 hh = mem_hash(gva, t->size);
                if (hh != t->hash) {
                    t->hash = hh;
                    tex_upload(t);
                    t->stable = 0;
                } else if (t->stable < 100000)
                    t->stable++;
                if (t->stable >= 39 && t->size >= 16384) {
                    t->shash = sample_hash(gva, t->size);
                    t->tfull = g_vk_present;
                }
                t->next_check = g_vk_present + (t->stable < 4 ? 4 : t->stable < 12 ? 8 : 20) + ((u32)(gva >> 9) & 3);
            }
        }
        return t;
    }
    if (!format_samples(f.vk)) return NULL;
    Surf* t = tex_create(gva, w, h, layers, d3, levels, vt, f, fkey, bl, bh, bd, pitch);
    if (!t) return NULL;
    u64 sz = 0;
    for (u32 l = 0; l < levels; l++)
        sz += bl ? bl_level_size(w, h, d3, &f, bh, bd, l, NULL) : (u64)pitch * ((h + f.bh - 1) / f.bh);
    if (bl && layers > 1) {
        u64 al = 512ull << bh;
        sz = (sz + al - 1) / al * al;
    }
    t->size = sz * layers;
    t->wseen = vk_ww_now();
    vk_written_since(gmem_ptr(gva, t->size), t->size, 0); /* starts tracking it */
    t->wseen = vk_ww_now();
    t->hash = mem_hash(gva, t->size);
    t->checked = g_vk_present;
    t->next_check = g_vk_present + 4;
    tex_upload(t);
    return t;
}

/* sRGB and linear views of the same bytes */
static VkFormat srgb_as(VkFormat f, int srgb) {
    static const VkFormat pairs[][2] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB},
                                        {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB},
                                        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
                                        {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK},
                                        {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK},
                                        {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK}};
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        if (pairs[i][0] == f || pairs[i][1] == f) return pairs[i][srgb ? 1 : 0];
    return f;
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
    VkFormat fmt = s->f.depth ? s->f.vk : srgb_as(s->f.vk, srgb);
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

/* empty textures (all zero) for slots whose texture is missing or doesn't match the shader */
VkImageView vk_null_view(VkImageViewType type, int num) {
    static VkImageView views[8][3];
    static Surf* imgs[2][3]; /* [3D][num] */
    if ((unsigned)type >= 8 || num < 0 || num > 2) return VK_NULL_HANDLE;
    if (views[type][num]) return views[type][num];
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) return VK_NULL_HANDLE;
    int is3d = type == VK_IMAGE_VIEW_TYPE_3D;
    Surf* s = imgs[is3d][num];
    if (!s) {
        static const VkFormat fm[3] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT};
        s = calloc(1, sizeof *s);
        s->w = s->h = s->sw = s->sh = s->levels = s->depth3d = 1;
        s->layers = is3d ? 1 : 6;
        s->type = is3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        s->f = (VkFmt){fm[num], 4, 1, 1, 0, 0, (u8)(num != 0), 0, (u8)(num == 2)};
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
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue z = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, s->layers};
        vkCmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &all);
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
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
        case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/* tsc[0] bit 9 compare, bits 10-12 the compare function; tsc[1] filters (gpu/vk/render.c forces nearest for integer
   textures and the compare mode the shader declares) */
VkSampler vk_sampler(const u32* tsc) {
    static struct {
        u32 tsc[8];
        VkSampler s;
    } tab[4096];
    u32 hk = 2166136261u;
    for (int i = 0; i < 8; i++) hk = (hk ^ tsc[i]) * 16777619u;
    hk &= 4095;
    u32 j = hk;
    for (; tab[j].s; j = (j + 1) & 4095)
        if (!memcmp(tab[j].tsc, tsc, 32)) return tab[j].s;
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3;
    float minl = (float)(tsc[2] & 0xFFF) / 256.f, maxl = (float)((tsc[2] >> 12) & 0xFFF) / 256.f;
    i32 bias = (i32)((tsc[1] >> 12) & 0x1FFF);
    if (bias & 0x1000) bias |= ~0x1FFF;
    u32 aniso = (tsc[0] >> 20) & 7;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = mag == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .minFilter = min == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = mip == 3 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = wrap(tsc[0] & 7),
                              .addressModeV = wrap((tsc[0] >> 3) & 7),
                              .addressModeW = wrap((tsc[0] >> 6) & 7),
                              .mipLodBias = (float)bias / 256.f,
                              .anisotropyEnable = aniso && g_vk.feat.samplerAnisotropy,
                              .maxAnisotropy = (float)(1 << aniso),
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
    return s;
}

void vk_tex_frame(void) {
    /* textures not drawn for ten seconds go; they come back from memory if they are needed again */
    if (g_vk_present % 60 == 0 && g_vk_present > 600) vk_surf_evict(g_vk_present - 600);
}
