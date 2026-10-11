/* Render targets (and later textures) by guest GPU address. A render target lives on the GPU; vk_rt_flush writes it
   back to guest memory when the CPU side needs it (DMA and 2D copies), vk_rt_reload reads it again after the CPU
   side wrote that memory. Games reuse one address for render targets of different sizes and formats within a frame:
   a target whose address is taken by another is parked and reused when its size and format come back. */
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu.h"
#include "hle/nvdrv.h"
#include "gpu/vk/vk.h"

#define SURFH 4096
static Surf* g_surfh[SURFH];
static u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (SURFH - 1); }

/* bytes of a block-linear surface of w_bytes x h, block height 2^bh GOBs (shrunk for small surfaces, as the
   hardware does) */
u64 vk_bl_size(u32 w_bytes, u32 h, u32 bh) {
    u32 gx = (w_bytes + 63) / 64, gy = (h + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    u64 gys = ((u64)(gy + (1u << bh) - 1) >> bh) << bh;
    return (u64)gx * gys * 512;
}
static u32 bl_height(u32 h, u32 bh) {
    u32 gy = (h + 7) / 8;
    while (bh > 0 && gy <= (1u << (bh - 1))) bh--;
    return bh;
}
VkImageAspectFlags vk_aspect(const VkFmt* f) {
    if (!f->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (f->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

void vk_surf_use(VkCommandBuffer cb, Surf* s, VkImageLayout layout, VkPipelineStageFlags2 stage,
                 VkAccessFlags2 access) {
    /* read after read in the same layout needs no barrier */
    int writes = (access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)) != 0;
    int wrote = (s->access & (VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)) != 0;
    if (s->layout == layout && !writes && !wrote) {
        s->stage |= stage;
        s->access |= access;
        return;
    }
    vk_render_end(); /* no barriers on images inside a rendering pass */
    vk_barrier(cb, s->img, vk_aspect(&s->f), s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->access,
               s->layout, stage, access, layout);
    s->layout = layout;
    s->stage = stage;
    s->access = access;
}

float g_vk_scale = 1.0f;
u32 vk_sc(u32 v) {
    u32 r = (u32)((float)v * g_vk_scale + 0.5f);
    return r ? r : 1;
}
float vk_scf(float v) { return v * g_vk_scale; }

static Surf* surf_create(u64 gva, u32 w, u32 h, VkFmt f, u32 fkey, u32 bl, u32 bh, u32 pitch, int is_rt) {
    Surf* s = calloc(1, sizeof *s);
    s->gva = gva;
    s->w = w;
    s->h = h;
    s->sw = is_rt ? vk_sc(w) : w;
    s->sh = is_rt ? vk_sc(h) : h;
    s->layers = 1;
    s->levels = 1;
    s->fkey = fkey;
    s->f = f;
    s->bl = bl;
    s->bh = bh;
    s->pitch = pitch;
    s->is_rt = is_rt;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (is_rt) usage |= f.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    s->type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = f.depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, /* sRGB and linear views */
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {s->sw, s->sh, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &s->img, &s->mem, NULL))) {
        free(s);
        return NULL;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = s->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f.vk,
                                .subresourceRange = {vk_aspect(&f), 0, 1, 0, 1}};
    VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->att));
    if (f.depth) vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; /* sampling reads depth */
    VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->view));
    u32 k = hkey(gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    return s;
}

Surf* vk_surf_first(u64 gva) { return g_surfh[hkey(gva)]; }
void vk_surf_add(Surf* s) {
    u32 k = hkey(s->gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
}

Surf* vk_rt_find(u64 gva) {
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) return s;
    return NULL;
}

/* an image of a scaled target's format at the guest's size, to move it to and from guest memory */
static VkImage unscaled_image(const Surf* s, VmaAllocation* mem) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = s->f.vk,
                            .extent = {s->w, s->h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img = VK_NULL_HANDLE;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &img, mem, NULL))) return VK_NULL_HANDLE;
    return img;
}
/* src (TRANSFER_SRC) scaled into dst (TRANSFER_DST), whole images */
static void scale_blit(VkCommandBuffer cb, VkImage src, u32 sw, u32 sh, VkImage dst, u32 dw, u32 dh, int is_int) {
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(i32)sw, (i32)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{0, 0, 0}, {(i32)dw, (i32)dh, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   is_int ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
}

/* the guest bytes of a render target: block linear or pitch linear, in guest memory */
static u64 rt_bytes(const Surf* s) { return s->bl ? vk_bl_size(s->w * s->f.bpp, s->h, s->bh) : (u64)s->pitch * s->h; }

void vk_rt_reload(u64 gva) {
    Surf* s = vk_rt_find(gva);
    if (!s) return;
    if (s->f.depth || !s->f.raw) { /* new memory over a target we can't load as is: drop it, memory is the truth */
        s->gva = gva | 1ull << 63;
        return;
    }
    u64 sz = rt_bytes(s), row = (u64)s->w * s->f.bpp;
    u8* tmp = NULL;
    const u8* src = gmem_ptr(gva, sz);
    if (!src) {
        tmp = malloc(sz);
        gmem_read(gva, tmp, sz);
        src = tmp;
    }
    vk_render_end();
    VkBuffer buf;
    u64 off;
    u8* p = vk_stage(row * s->h, s->f.bpp, &buf, &off);
    if (p) {
        if (s->bl)
            deswizzle(p, (u32)row, src, (u32)row, s->h, bl_height(s->h, s->bh));
        else
            for (u32 y = 0; y < s->h; y++) memcpy(p + y * row, src + (u64)y * s->pitch, row);
        VkCommandBuffer cb = vk_cmd();
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c = {.bufferOffset = off,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {s->w, s->h, 1}};
        VmaAllocation tm;
        VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : unscaled_image(s, &tm);
        if (!t)
            vkCmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        else { /* a scaled target: the guest's pixels into an image of their size, then scaled up into it */
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(cb, buf, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            scale_blit(cb, t, s->w, s->h, s->img, s->sw, s->sh, s->f.is_int);
            vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
    }
    free(tmp);
}

void vk_rt_flush(u64 gva) {
    Surf* s = vk_rt_find(gva);
    if (!s || s->f.depth || !s->f.raw || s->layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
    u64 row = (u64)s->w * s->f.bpp;
    vk_render_end();
    VkBuffer buf;
    u8* p = vk_readback_buffer(row * s->h, &buf);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {s->w, s->h, 1}};
    VmaAllocation tm;
    VkImage t = s->sw == s->w && s->sh == s->h ? VK_NULL_HANDLE : unscaled_image(s, &tm);
    if (!t)
        vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    else { /* a scaled target: scaled down to the guest's size first */
        vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        scale_blit(cb, s->img, s->sw, s->sh, t, s->w, s->h, s->f.is_int);
        vk_barrier(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        vk_defer_image(t, tm, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    vk_finish();
    vk_readback_ready();
    u64 sz = rt_bytes(s);
    u8* dst = gmem_ptr(gva, sz);
    if (dst) {
        if (s->bl)
            swizzle(dst, p, (u32)row, (u32)row, s->h, bl_height(s->h, s->bh));
        else
            for (u32 y = 0; y < s->h; y++) memcpy(dst + (u64)y * s->pitch, p + y * row, row);
    } else if (!s->bl) {
        for (u32 y = 0; y < s->h; y++) gmem_write(gva + (u64)y * s->pitch, p + y * row, row);
    }
}

Surf* vk_rt(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth) {
    u32 fkey = fmt | (depth ? 0x10000 : 0);
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt && s->w == w && s->h == h && s->fkey == fkey) {
            s->last_used = vk_frame_number();
            return s;
        }
    /* another size or format at this address: park the current one, reuse a parked one that fits */
    Surf* parked = NULL;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == (gva | 1ull << 63) && s->is_rt && s->w == w && s->h == h && s->fkey == fkey && !parked)
            parked = s;
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) s->gva = gva | 1ull << 63;
    if (parked) {
        parked->gva = gva;
        parked->last_used = vk_frame_number();
        return parked;
    }
    VkFmt f = depth ? vk_zt_format(fmt) : vk_rt_format(fmt);
    int linear = (tile >> 12) & 1;
    Surf* s = surf_create(gva, w, h, f, fkey, !linear, (tile >> 4) & 15, w * f.bpp, 1);
    if (!s) return NULL;
    s->last_used = vk_frame_number();
    /* the CPU may have written the memory already (a cleared or decoded frame): start from it */
    if (!depth) vk_rt_reload(gva);
    return s;
}

Surf* vk_rt_for_cpu(u64 cpu_addr, u32 w, u32 h) {
    Surf* best = NULL;
    for (int k = 0; k < SURFH; k++)
        for (Surf* s = g_surfh[k]; s; s = s->next)
            if (s->is_rt && !s->f.depth && !(s->gva >> 63) && gmmu_translate(s->gva) == cpu_addr)
                if (!best || (s->w == w && s->h == h) || s->last_used > best->last_used) best = s;
    return best;
}

void vk_surf_evict(u32 before) {
    for (int k = 0; k < SURFH; k++)
        for (Surf** pp = &g_surfh[k]; *pp;) {
            Surf* s = *pp;
            if (s->is_rt || (i32)(s->last_used - before) >= 0) {
                pp = &s->next;
                continue;
            }
            *pp = s->next;
            for (int i = 0; i < s->nsv; i++) vk_defer_image(VK_NULL_HANDLE, VK_NULL_HANDLE, s->sv[i].v, VK_NULL_HANDLE);
            vk_defer_image(s->img, s->mem, s->view, s->att);
            free(s);
        }
}

/* the 2D engine's blit between render targets, on the GPU. The surfaces are gpu/gpu.h's
   Surf2D. 0 when it can't be done there: the front end then copies through guest memory */
static int blit_ok(VkFormat f, VkFormatFeatureFlags need) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}
int vk_blit2d(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
              double sdy) {
    Surf* st = vk_rt_find(s->addr);
    if (!st || st->f.depth || dw <= 0 || dh <= 0) return 0;
    u32 tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Surf* dt = vk_rt(d->addr, d->width, d->height, d->format, tile, 0);
    if (!dt || dt == st || dt->f.depth || st->f.is_int != dt->f.is_int) return 0;
    if (!blit_ok(st->f.vk, VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !blit_ok(dt->f.vk, VK_FORMAT_FEATURE_BLIT_DST_BIT))
        return 0;
    /* in the images' pixels: targets are scaled */
    float S = g_vk_scale;
    i32 x0 = (i32)(sx * S), y0 = (i32)(sy * S), x1 = (i32)((sx + sdx * dw) * S + 0.5),
        y1 = (i32)((sy + sdy * dh) * S + 0.5);
    i32 X1 = (i32)((dx + dw) * S + 0.5f), Y1 = (i32)((dy + dh) * S + 0.5f);
    dx = (i32)(dx * S);
    dy = (i32)(dy * S);
    if (x1 > (i32)st->sw) x1 = (i32)st->sw;
    if (y1 > (i32)st->sh) y1 = (i32)st->sh;
    if (X1 > (i32)dt->sw) X1 = (i32)dt->sw;
    if (Y1 > (i32)dt->sh) Y1 = (i32)dt->sh;
    if (x1 <= x0 || y1 <= y0 || X1 <= dx || Y1 <= dy || dx < 0 || dy < 0) return 0;
    int scaled = x1 - x0 != X1 - dx || y1 - y0 != Y1 - dy;
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, st, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_surf_use(cb, dt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{x0, y0, 0}, {x1, y1, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{dx, dy, 0}, {X1, Y1, 1}}};
    int linear = scaled && !st->f.is_int && blit_ok(st->f.vk, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    vkCmdBlitImage(cb, st->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    dt->wgen++;
    dt->last_used = vk_frame_number();
    return 1;
}
