/* Vulkan renderer, step 1: presents the frames the CPU path converts from the game's framebuffer (vi_show_buffer
   with g_gl = 0). Each frame is copied into a staging buffer, uploaded to an image and scaled onto the swapchain
   image, letterboxed to keep its aspect. GPU commands still go through gpu_nogl.c: nothing is drawn yet. */
#include "vk.h"

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence done;
    VkSemaphore acquired;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    u8* staging_ptr;
    u64 staging_size;
} VkFrame;

static VkFrame g_fr[VK_FRAMES];
static u32 g_fi;
static VkImage g_upload;
static VkDeviceMemory g_upload_mem;
static int g_uw, g_uh;
static int g_ok;
static PMutex g_present_lock;
/* the last frame, for screenshots */
static u32* g_last;
static int g_lw, g_lh;

void gpu_vk_start(void) {
    pm_init(&g_present_lock);
    if (!vk_init()) fatal("Vulkan could not start (see the log). Build with -DSWR_GPU=gl to use OpenGL.");
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < VK_FRAMES; i++) {
        VkFrame* f = &g_fr[i];
        VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &f->pool));
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = f->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cmd));
        VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &f->done));
        VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &f->acquired));
    }
    g_ok = 1;
}

/* a host-visible staging buffer of at least size bytes, kept mapped */
static int staging_fit(VkFrame* f, u64 size) {
    if (f->staging_size >= size) return 1;
    if (f->staging) {
        vkDestroyBuffer(g_vk.dev, f->staging, NULL);
        vkFreeMemory(g_vk.dev, f->staging_mem, NULL);
        f->staging = VK_NULL_HANDLE;
        f->staging_size = 0;
    }
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bi, NULL, &f->staging))) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, f->staging, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = mr.size,
                               .memoryTypeIndex =
                                   vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == ~0u || !VK_OK(vkAllocateMemory(g_vk.dev, &ai, NULL, &f->staging_mem))) return 0;
    vkBindBufferMemory(g_vk.dev, f->staging, f->staging_mem, 0);
    if (!VK_OK(vkMapMemory(g_vk.dev, f->staging_mem, 0, size, 0, (void**)&f->staging_ptr))) return 0;
    f->staging_size = size;
    return 1;
}

/* the device-local image a frame is uploaded to before it is scaled onto the swapchain */
static int upload_fit(int w, int h) {
    if (g_upload && g_uw == w && g_uh == h) return 1;
    if (g_upload) {
        vkDeviceWaitIdle(g_vk.dev);
        vkDestroyImage(g_vk.dev, g_upload, NULL);
        vkFreeMemory(g_vk.dev, g_upload_mem, NULL);
        g_upload = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {(u32)w, (u32)h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!VK_OK(vkCreateImage(g_vk.dev, &ii, NULL, &g_upload))) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, g_upload, &mr);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = mr.size,
                               .memoryTypeIndex =
                                   vk_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == ~0u || !VK_OK(vkAllocateMemory(g_vk.dev, &ai, NULL, &g_upload_mem))) return 0;
    vkBindImageMemory(g_vk.dev, g_upload, g_upload_mem, 0);
    g_uw = w;
    g_uh = h;
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkImageLayout from, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src_access,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst_access,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

/* called from the game's present (vi QueueBuffer), with the frame in RGBA8 */
void vk_present_rgba(const u32* px, int w, int h, int stride) {
    if (!g_ok || w <= 0 || h <= 0) return;
    pm_lock(&g_present_lock);
    /* keep a copy for screenshots */
    if (g_lw != w || g_lh != h) {
        free(g_last);
        g_last = malloc((size_t)w * h * 4);
        g_lw = w;
        g_lh = h;
    }
    for (int y = 0; y < h; y++) memcpy(g_last + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 4);

    int ww, wh;
    backend_window_size(&ww, &wh);
    if (ww > 0 && wh > 0 && ((u32)ww != g_vk.sc_extent.width || (u32)wh != g_vk.sc_extent.height))
        vk_swapchain_create();
    if (!g_vk.swapchain && !vk_swapchain_create()) goto out; /* minimized */

    VkFrame* f = &g_fr[g_fi];
    vkWaitForFences(g_vk.dev, 1, &f->done, VK_TRUE, UINT64_MAX);
    u32 idx;
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, 1000000000ull, f->acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vk_swapchain_create();
        goto out;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_OK(r);
        goto out;
    }
    vkResetFences(g_vk.dev, 1, &f->done);
    if (!staging_fit(f, (u64)w * h * 4) || !upload_fit(w, h)) goto out;
    memcpy(f->staging_ptr, g_last, (size_t)w * h * 4);

    VkCommandBuffer cb = f->cmd;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImage sc = g_vk.sc_images[idx];
    barrier(cb, g_upload, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {(u32)w, (u32)h, 1}};
    vkCmdCopyBufferToImage(cb, f->staging, g_upload, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, g_upload, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, sc, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(cb, sc, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    /* letterbox: the largest rectangle of the frame's aspect that fits the window, centered */
    u32 sw = g_vk.sc_extent.width, sh = g_vk.sc_extent.height, dw = sw, dh = (u32)((u64)sw * h / w);
    if (dh > sh) {
        dh = sh;
        dw = (u32)((u64)sh * w / h);
    }
    i32 dx = (i32)(sw - dw) / 2, dy = (i32)(sh - dh) / 2;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{dx, dy, 0}, {dx + (i32)dw, dy + (i32)dh, 1}}};
    vkCmdBlitImage(cb, g_upload, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    barrier(cb, sc, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cb);

    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = f->acquired,
                                  .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSemaphoreSubmitInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                    .semaphore = g_vk.sc_done[idx],
                                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = 1,
                        .pWaitSemaphoreInfos = &wait,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &cbi,
                        .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &signal};
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_vk.sc_done[idx],
                           .swapchainCount = 1,
                           .pSwapchains = &g_vk.swapchain,
                           .pImageIndices = &idx};
    pm_lock(&g_vk.gfx_lock);
    VK_OK(vkQueueSubmit2(g_vk.gfx, 1, &si, f->done));
    r = vkQueuePresentKHR(g_vk.gfx, &pi);
    pm_unlock(&g_vk.gfx_lock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        vk_swapchain_create();
    else
        VK_OK(r);
    g_fi = (g_fi + 1) % VK_FRAMES;
out:
    pm_unlock(&g_present_lock);
}

/* screenshots of the last presented frame, as PPM (the backends' unattended runs call this) */
u64 gpu_gl_screenshot(const char* path) {
    pm_lock(&g_present_lock);
    u64 h = 1469598103934665603ull;
    if (g_last) {
        for (size_t i = 0; i < (size_t)g_lw * g_lh; i++) h = (h ^ g_last[i]) * 1099511628211ull;
        if (path) {
            char p[1100];
            snprintf(p, sizeof p, "%s.ppm", path);
            FILE* f = fopen(p, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", g_lw, g_lh);
                for (size_t i = 0; i < (size_t)g_lw * g_lh; i++) {
                    u8 rgb[3] = {(u8)g_last[i], (u8)(g_last[i] >> 8), (u8)(g_last[i] >> 16)};
                    fwrite(rgb, 1, 3, f);
                }
                fclose(f);
            }
        }
    }
    pm_unlock(&g_present_lock);
    return h;
}
