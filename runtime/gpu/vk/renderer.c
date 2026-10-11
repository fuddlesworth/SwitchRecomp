/* Vulkan renderer: its side of the renderer interface (renderer.c calls these vkr_ functions when the Vulkan
   renderer is the active one, the GL names otherwise), and presenting.

   Render targets, clears, draws (vk_render.c), compute (vk_compute.c), write-backs to and reloads from guest memory,
   and presenting the game's framebuffer from its render target (or from memory, converted on the CPU, when no
   render target holds it). Submits are processed on the thread that makes them, under vk_lock(). */
#include "gpu/vk/vk.h"
#include "gpu/renderer.h"

static VkSemaphore g_acquired[VK_FRAMES];
/* the CPU fallback's image, and the last presented frame for screenshots */
static Surf* g_cpu_img;
static Surf* g_last;
static u32 g_last_w, g_last_h;

/* 0 when Vulkan can't run here: the caller falls back to OpenGL */
int vkr_start(void) {
    if (!vk_init()) return 0;
    extern float g_opt_resf;
    g_vk_scale = g_opt_resf >= 0.25f && g_opt_resf <= 4 ? g_opt_resf : 1.0f;
    if (g_vk_scale != 1.0f) LOG("vulkan: render targets at %.2fx\n", g_vk_scale);
    vk_cmd_init();
    vk_pipeline_init();
    extern void (*g_gpu_before_write)(u64, u64);
    g_gpu_before_write = vk_compute_before_write;
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < VK_FRAMES; i++) VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_acquired[i]));
    g_gl = 1; /* a renderer is active */
    return 1;
}

/* ------------------------------------------------------------ the front end's calls */
int vkr_async_submit(u64* e, u32 n, u32 syncpt, u32 incs) {
    (void)e;
    (void)n;
    (void)syncpt;
    (void)incs;
    __atomic_add_fetch(&g_vk_epoch, 1, __ATOMIC_RELAXED); /* memory may have changed since the last submit */
    return 0;                                             /* processed now, on the submitting thread */
}
void vkr_clear(GpuChannel* ch, u32 v) {
    vk_lock();
    vk_render_clear(ch, v);
    vk_unlock();
}
void vkr_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances) {
    g_draws++;
    vk_lock();
    vk_render_draw(ch, indexed, topology, first, count, bv, bi, instances);
    vk_unlock();
}
/* a constant buffer bound to a stage: v bit 0 valid, bits 4-8 the slot; address and size from 0x8E0-0x8E2 */
void vkr_cb_bind(GpuChannel* ch, int stage, u32 v) {
    u32* r = ch->r3d;
    u32 idx = (v >> 4) & 31;
    if (stage > 4 || idx >= 18) return;
    g_vk_cb[stage][idx] = (v & 1) ? ((u64)r[0x8E1] << 32) | r[0x8E2] : 0;
    g_vk_cbsize[stage][idx] = (v & 1) ? r[0x8E0] : 0;
}
int vkr_blit(void* s, void* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx, double sdy) {
    vk_lock();
    int r = vk_blit2d(s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
    vk_unlock();
    return r; /* 0: the front end copies through memory, with gpu_gl_rt_flush/reload around it */
}
void vkr_rt_flush(u64 gva) {
    vk_lock();
    vk_rt_flush(gva);
    vk_unlock();
}
void vkr_rt_reload(u64 gva) {
    vk_lock();
    vk_rt_reload(gva);
    vk_unlock();
}
void vkr_dispatch(void* ch, const void* q, unsigned long long code) {
    extern void r3d_pending_flush(void);
    if (getenv("SWR_NO_COMPUTE")) return;
    r3d_pending_flush(); /* a merged draw the front end is holding comes first */
    vk_lock();
    vk_dispatch(ch, q, code);
    vk_unlock();
}
/* compute results land before the stream signals or waits (vk_compute.c) */
void vkr_before_syncwait(void) { vk_compute_land(); }
void vkr_stream_incr(u32 id) {
    extern void syncpt_incr(u32);
    vk_compute_land();
    syncpt_incr(id);
}

/* ------------------------------------------------------------ presenting */
/* the frame from memory when no render target holds it (the game drew it on the CPU, or a video frame) */
static Surf* cpu_frame(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    extern void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32 * out);
    if (g_cpu_img && (g_cpu_img->w != w || g_cpu_img->h != h)) {
        vk_defer_image(g_cpu_img->img, g_cpu_img->mem, g_cpu_img->view, g_cpu_img->att);
        free(g_cpu_img);
        g_cpu_img = NULL;
    }
    if (!g_cpu_img) {
        g_cpu_img = calloc(1, sizeof *g_cpu_img);
        g_cpu_img->w = g_cpu_img->sw = w;
        g_cpu_img->h = g_cpu_img->sh = h;
        g_cpu_img->f = vk_rt_format(0xD5); /* RGBA8 */
        g_cpu_img->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {w, h, 1},
                                .mipLevels = 1,
                                .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &g_cpu_img->img, &g_cpu_img->mem, NULL))) {
            free(g_cpu_img);
            g_cpu_img = NULL;
            return NULL;
        }
    }
    VkBuffer buf;
    u64 off;
    u32* px = (u32*)vk_stage((u64)w * h * 4, 4, &buf, &off);
    if (!px) return NULL;
    vi_convert_cpu(cpu_addr, w, h, pitch, layout, bhl, fmt, px);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, g_cpu_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy c = {
        .bufferOffset = off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyBufferToImage(cb, buf, g_cpu_img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return g_cpu_img;
}

/* the game queued a buffer for display: show it, letterboxed to the window */
void vkr_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    vk_lock();
    vk_render_end();
    Surf* src = vk_rt_for_cpu(cpu_addr, w, h);
    if (!src || src->f.is_int) src = cpu_frame(cpu_addr, w, h, pitch, layout, bhl, fmt);
    int ww, wh;
    backend_window_size(&ww, &wh);
    if (ww > 0 && wh > 0 && ((u32)ww != g_vk.sc_extent.width || (u32)wh != g_vk.sc_extent.height)) {
        vk_finish();
        vk_swapchain_create();
    }
    if (!src || (!g_vk.swapchain && !vk_swapchain_create())) goto out; /* minimized */
    VkCommandBuffer cb = vk_cmd(); /* waits for this frame slot, so its acquire semaphore is free */
    u32 slot = (u32)(vk_frame_number() % VK_FRAMES), idx;
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 1000000000ull, g_acquired[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_finish();
        vk_swapchain_create();
        goto out;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_OK(r);
        goto out;
    }
    VkImage sc = g_vk.sc_images[idx];
    vk_surf_use(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    /* letterbox: the largest rectangle of the frame's aspect that fits the window, centered */
    u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height, dw = sw, dh = (u32)((u64)sw * h / w);
    if (dh > sh) {
        dh = sh;
        dw = (u32)((u64)sh * w / h);
    }
    i32 dx = (i32)(sw - dw) / 2, dy = (i32)(sh - dh) / 2;
    /* the frame's part of the source, in its pixels (render targets are scaled) */
    u32 bw = (u32)((u64)src->sw * (w < src->w ? w : src->w) / src->w),
        bh = (u32)((u64)src->sh * (h < src->h ? h : src->h) / src->h);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(i32)bw, (i32)bh, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{dx, dy, 0}, {dx + (i32)dw, dy + (i32)dh, 1}}};
    vkCmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    vk_barrier(cb, sc, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    g_last = src;
    g_last_w = bw;
    g_last_h = bh;
    g_vk_present++;
    vk_tex_frame();
    vk_buf_frame();
    if (g_vk_present % 300 == 0) vk_pipeline_save();
    if (g_vk_present % 600 == 0) {
        extern u64 g_vk_drawn, g_vk_skipped, g_vk_dispatches, g_vk_cs_batches, g_vk_cs_skipped, g_vk_cs_wait_ns;
        LOG("vulkan: %llu draws, %llu skipped; %llu dispatches in %llu batches (%llu ms waiting), %llu skipped\n",
            (unsigned long long)g_vk_drawn, (unsigned long long)g_vk_skipped, (unsigned long long)g_vk_dispatches,
            (unsigned long long)g_vk_cs_batches, (unsigned long long)(g_vk_cs_wait_ns / 1000000),
            (unsigned long long)g_vk_cs_skipped);
    }
    vk_submit(g_acquired[slot], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_vk.sc_done[idx]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_vk.sc_done[idx],
                           .swapchainCount = 1,
                           .pSwapchains = &g_vk.swapchain,
                           .pImageIndices = &idx};
    r = vkQueuePresentKHR(g_vk.gfx, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vk_finish();
        vk_swapchain_create();
    } else
        VK_OK(r);
out:
    vk_unlock();
}

/* the last presented frame as PPM (path.ppm), and a hash of its pixels */
u64 vkr_screenshot(const char* path) {
    vk_lock();
    u64 hash = 1469598103934665603ull;
    Surf* s = g_last;
    if (!s) goto out;
    u32 w = g_last_w, h = g_last_h;
    /* blit to an RGBA8 image, whatever the target's format, then read that back */
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage tmp;
    VmaAllocation tmem;
    if (!VK_OK(vmaCreateImage(g_vk.vma, &ii, &aci, &tmp, &tmem, NULL))) goto out;
    VkBuffer buf;
    u8* p = vk_readback_buffer((u64)w * h * 4, &buf);
    if (!p) {
        vmaDestroyImage(g_vk.vma, tmp, tmem);
        goto out;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{0, 0, 0}, {(i32)w, (i32)h, 1}}};
    vkCmdBlitImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_NEAREST);
    vk_barrier(cb, tmp, VK_IMAGE_ASPECT_COLOR_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    vk_finish();
    vk_readback_ready();
    vmaDestroyImage(g_vk.vma, tmp, tmem);
    const u32* px = (const u32*)p;
    for (u64 i = 0; i < (u64)w * h; i++) hash = (hash ^ px[i]) * 1099511628211ull;
    if (path) {
        char fn[1100];
        snprintf(fn, sizeof fn, "%s.ppm", path);
        FILE* f = fopen(fn, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", w, h);
            for (u64 i = 0; i < (u64)w * h; i++) {
                u8 rgb[3] = {(u8)px[i], (u8)(px[i] >> 8), (u8)(px[i] >> 16)};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
    }
out:
    vk_unlock();
    return hash;
}
