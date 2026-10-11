/* Frames in flight. Each frame has a command buffer that collects everything recorded until the next submit (a
   present or a readback), a staging ring for uploads, and the resources to destroy once its work is done. Starting
   to record a frame waits for its previous use to finish. */
#include "vk.h"

typedef struct {
    VkImage img;
    VmaAllocation mem;
    VkImageView v1, v2;
    VkBuffer buf;
} Dead;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence done;
    int recording, pending; /* pending: submitted, fence not waited yet */
    /* staging ring */
    VkBuffer stage;
    VmaAllocation stage_mem;
    u8* stage_ptr;
    u64 stage_cap, stage_used;
    /* destroyed once this frame's work is done */
    Dead* dead;
    int ndead, capdead;
} Frame;

static Frame g_fr[VK_FRAMES];
static u32 g_cur;
static u64 g_frame_no;

static void dead_push(Frame* f, Dead d) {
    if (f->ndead == f->capdead) {
        f->capdead = f->capdead ? f->capdead * 2 : 64;
        f->dead = realloc(f->dead, sizeof *f->dead * f->capdead);
    }
    f->dead[f->ndead++] = d;
}
static void dead_free(Frame* f) {
    for (int i = 0; i < f->ndead; i++) {
        Dead* d = &f->dead[i];
        if (d->v1) vkDestroyImageView(g_vk.dev, d->v1, NULL);
        if (d->v2) vkDestroyImageView(g_vk.dev, d->v2, NULL);
        if (d->img) vmaDestroyImage(g_vk.vma, d->img, d->mem);
        if (d->buf) vmaDestroyBuffer(g_vk.vma, d->buf, d->mem);
    }
    f->ndead = 0;
}

void vk_cmd_init(void) {
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (int i = 0; i < VK_FRAMES; i++) {
        Frame* f = &g_fr[i];
        VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &f->pool));
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .commandPool = f->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cb));
        VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &f->done));
    }
}

/* waits for a frame's previous submit, then frees what it was holding */
static void frame_wait(Frame* f) {
    if (!f->pending) return;
    VK_OK(vkWaitForFences(g_vk.dev, 1, &f->done, VK_TRUE, UINT64_MAX));
    VK_OK(vkResetFences(g_vk.dev, 1, &f->done));
    f->pending = 0;
    dead_free(f);
}

VkCommandBuffer vk_cmd(void) {
    Frame* f = &g_fr[g_cur];
    if (!f->recording) {
        frame_wait(f);
        vkResetCommandPool(g_vk.dev, f->pool, 0);
        f->stage_used = 0;
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK_OK(vkBeginCommandBuffer(f->cb, &bi));
        f->recording = 1;
    }
    return f->cb;
}

u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off) {
    vk_cmd(); /* the ring is only reused once this frame's previous work is done */
    Frame* f = &g_fr[g_cur];
    if (align < 16) align = 16;
    u64 at = (f->stage_used + align - 1) & ~(align - 1);
    if (!f->stage || at + size > f->stage_cap) {
        /* a bigger buffer; the old one stays alive until this frame's work is done */
        if (f->stage) dead_push(f, (Dead){.buf = f->stage, .mem = f->stage_mem});
        u64 cap = f->stage_cap ? f->stage_cap * 2 : (16ull << 20);
        while (cap < size) cap *= 2;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO};
        VmaAllocationInfo info;
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &f->stage, &f->stage_mem, &info))) {
            f->stage = VK_NULL_HANDLE;
            f->stage_cap = 0;
            return NULL;
        }
        f->stage_ptr = info.pMappedData;
        f->stage_cap = cap;
        at = 0;
    }
    f->stage_used = at + size;
    *buf = f->stage;
    *off = at;
    return f->stage_ptr + at;
}

void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) {
    vk_render_end();
    Frame* f = &g_fr[g_cur];
    vk_cmd(); /* a submit with nothing recorded still has to wait and signal */
    /* staging memory is host coherent, but the host writes must be visible to the transfers */
    if (f->stage_mem) vmaFlushAllocation(g_vk.vma, f->stage_mem, 0, VK_WHOLE_SIZE);
    VK_OK(vkEndCommandBuffer(f->cb));
    VkSemaphoreSubmitInfo w = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait, .stageMask = wait_stage};
    VkSemaphoreSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                               .semaphore = signal,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = f->cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = wait ? 1u : 0u,
                        .pWaitSemaphoreInfos = &w,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = signal ? 1u : 0u,
                        .pSignalSemaphoreInfos = &s};
    VK_OK(vkQueueSubmit2(g_vk.gfx, 1, &si, f->done));
    f->recording = 0;
    f->pending = 1;
    g_cur = (g_cur + 1) % VK_FRAMES;
    g_frame_no++;
}

void vk_finish(void) {
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    for (int i = 0; i < VK_FRAMES; i++) frame_wait(&g_fr[i]);
}

u64 vk_frame_number(void) { return g_frame_no; }

void vk_defer_image(VkImage img, VmaAllocation a, VkImageView v1, VkImageView v2) {
    dead_push(&g_fr[g_cur], (Dead){.img = img, .mem = a, .v1 = v1, .v2 = v2});
}
void vk_defer_buffer(VkBuffer b, VmaAllocation a) { dead_push(&g_fr[g_cur], (Dead){.buf = b, .mem = a}); }

void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
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
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

/* one buffer the GPU copies into for the host to read: vk_readback_buffer before recording the copy, vk_finish,
   then vk_readback_ready before reading the pointer */
static VkBuffer g_rb;
static VmaAllocation g_rb_mem;
static u8* g_rb_ptr;
static u64 g_rb_cap;
u8* vk_readback_buffer(u64 size, VkBuffer* buf) {
    if (size > g_rb_cap) {
        if (g_rb) vk_defer_buffer(g_rb, g_rb_mem);
        u64 cap = g_rb_cap ? g_rb_cap : (16ull << 20);
        while (cap < size) cap *= 2;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
        VmaAllocationInfo info;
        if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_rb, &g_rb_mem, &info))) {
            g_rb = VK_NULL_HANDLE;
            g_rb_cap = 0;
            return NULL;
        }
        g_rb_ptr = info.pMappedData;
        g_rb_cap = cap;
    }
    *buf = g_rb;
    return g_rb_ptr;
}
void vk_readback_ready(void) { vmaInvalidateAllocation(g_vk.vma, g_rb_mem, 0, VK_WHOLE_SIZE); }
