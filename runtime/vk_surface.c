/* Render targets (and later textures) by guest GPU address. A render target lives on the GPU; vk_rt_flush writes it
   back to guest memory when the CPU side needs it (DMA and 2D copies), vk_rt_reload reads it again after the CPU
   side wrote that memory. Games reuse one address for render targets of different sizes and formats within a frame:
   a target whose address is taken by another is parked and reused when its size and format come back. */
#include "vk.h"

#define SURFH 4096
static Surf* g_surfh[SURFH];
static u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (SURFH - 1); }

/* bytes of a block-linear surface of w_bytes x h, block height 2^bh GOBs (shrunk for small surfaces, as the
   hardware does) */
static u64 bl_size(u32 w_bytes, u32 h, u32 bh) {
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
static VkImageAspectFlags aspect_of(const VkFmt* f) {
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
    vk_barrier(cb, s->img, aspect_of(&s->f), s->stage ? s->stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, s->access,
               s->layout, stage, access, layout);
    s->layout = layout;
    s->stage = stage;
    s->access = access;
}

static Surf* surf_create(u64 gva, u32 w, u32 h, VkFmt f, u32 fkey, u32 bl, u32 bh, u32 pitch, int is_rt) {
    Surf* s = calloc(1, sizeof *s);
    s->gva = gva;
    s->w = w;
    s->h = h;
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
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = f.vk,
                            .extent = {w, h, 1},
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
                                .subresourceRange = {aspect_of(&f), 0, 1, 0, 1}};
    VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->att));
    if (f.depth) vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; /* sampling reads depth */
    VK_OK(vkCreateImageView(g_vk.dev, &vi, NULL, &s->view));
    u32 k = hkey(gva);
    s->next = g_surfh[k];
    g_surfh[k] = s;
    return s;
}

Surf* vk_rt_find(u64 gva) {
    for (Surf* s = g_surfh[hkey(gva)]; s; s = s->next)
        if (s->gva == gva && s->is_rt) return s;
    return NULL;
}

/* the guest bytes of a render target: block linear or pitch linear, in guest memory */
static u64 rt_bytes(const Surf* s) { return s->bl ? bl_size(s->w * s->f.bpp, s->h, s->bh) : (u64)s->pitch * s->h; }

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
        vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy c = {.bufferOffset = off,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .imageExtent = {s->w, s->h, 1}};
        vkCmdCopyBufferToImage(cb, buf, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
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
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {s->w, s->h, 1}};
    vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
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
