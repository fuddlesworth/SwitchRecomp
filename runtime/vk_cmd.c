/* Frames in flight. Each frame has a command buffer that collects everything recorded until the next submit (a
   present or a readback), a staging ring for uploads and for data draws read directly (vertices, indices, constant
   buffers), descriptor pools, and the resources to destroy once its work is done. Starting to record a frame waits
   for its previous use to finish. */
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
    /* descriptor pools, reset when the frame is reused; pool[npool - 1] is the one allocating */
    VkDescriptorPool pool_d[64];
    int npool;
} Frame;

static Frame g_fr[VK_FRAMES];
static u32 g_cur;
static u64 g_frame_no;
u32 g_vk_epoch, g_vk_present;

u64 vk_hash(const void* p, u64 size) {
    const u64* q = p;
    u64 n = size / 8, i = 0;
    u64 a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    for (; i + 3 < n; i += 4) {
        a = (a ^ q[i]) * 0x100000001B3ull;
        b = (b ^ q[i + 1]) * 0x100000001B3ull;
        c = (c ^ q[i + 2]) * 0x100000001B3ull;
        d = (d ^ q[i + 3]) * 0x100000001B3ull;
    }
    for (; i < n; i++) a = (a ^ q[i]) * 0x100000001B3ull;
    for (u64 k = n * 8; k < size; k++) b = (b ^ ((const u8*)p)[k]) * 0x100000001B3ull;
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ size;
}

u64 vk_stage_align(void) {
    static u64 al;
    if (!al) {
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        al = 16;
        if (l->minUniformBufferOffsetAlignment > al) al = l->minUniformBufferOffsetAlignment;
        if (l->minStorageBufferOffsetAlignment > al) al = l->minStorageBufferOffsetAlignment;
    }
    return al;
}

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
        for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_vk.dev, f->pool_d[i], 0);
        if (f->npool > 1) { /* keep the first pool, which is rarely too small */
            for (int i = 1; i < f->npool; i++) vkDestroyDescriptorPool(g_vk.dev, f->pool_d[i], NULL);
            f->npool = 1;
        }
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
                                  .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        /* written once from the CPU, read by the GPU: device-local when the CPU can map it (resizable BAR) */
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
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

static VkDescriptorPool desc_pool_new(void) {
    /* sized for the per-stage set layout (vk_pipeline.c): a few thousand draws a frame */
    VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 * 6},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                 {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * 6},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 512}};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 4096,
                                     .poolSizeCount = sizeof sz / sizeof sz[0],
                                     .pPoolSizes = sz};
    VkDescriptorPool p = VK_NULL_HANDLE;
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p));
    return p;
}
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) {
    vk_cmd(); /* the pools are reset when the frame starts */
    Frame* f = &g_fr[g_cur];
    for (int tries = 0; tries < 2; tries++) {
        if (!f->npool || tries) {
            if (f->npool == (int)(sizeof f->pool_d / sizeof f->pool_d[0])) return VK_NULL_HANDLE;
            f->pool_d[f->npool++] = desc_pool_new();
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = f->pool_d[f->npool - 1],
                                          .descriptorSetCount = 1,
                                          .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            VK_OK(r);
            return VK_NULL_HANDLE;
        }
    }
    return VK_NULL_HANDLE;
}

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
