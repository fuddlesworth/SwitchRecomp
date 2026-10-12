/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Compute: the compute class's launches (QMD), on a compute queue of their own when the device has one (a queue of a
   compute-only family, or a second queue of the graphics family), else on the graphics queue. Each dispatch gets
   copies of its constant and storage buffers in a host-visible arena and is recorded into a batch. The batch is
   submitted and its results land in guest memory before anything could observe them: a read of that memory
   (gmem_read and gmem_ptr call cs_mem_need), anything after it in the stream the game can observe (semaphore releases
   and reports: gpu_signal_point), a GPU-side write of that memory, a syncpoint increment or a wait. A byte is written
   only where the shader changed it and guest memory still holds what the shader read, since on the console the GPU
   stores while it runs and a later CPU write wins.

   A queue of its own only waits for compute, so holding signals until results land costs a dispatch's run time,
   not the rendering backlog's (which made that ordering too slow under the old OpenGL renderer, ROADMAP.md Phase 2).
   Dispatches that read textures run on the graphics queue, after everything recorded before them. */
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu.h"
#include "gpu/shader/shader.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "gpu/vk/vk.h"
#include "gpu/vk/spirv.h"
#include "gpu/renderer.h"

typedef struct CsPipe {
    u64 key;
    VkPipeline p;
    ShaderInfo si;
    VkCbReads cbr; /* the constant buffer words translation read */
    int failed;
    struct CsPipe* next;
} CsPipe;
static CsPipe* g_csp[256];

/* the arena: inputs and outputs of the batch's dispatches, and what the outputs held before (org, host only) */
static VkBuffer g_ar;
static VmaAllocation g_ar_mem;
static u8* g_ar_ptr;
static u8* g_org;
static u64 g_ar_cap, g_ar_used;
typedef struct {
    u64 addr;
    u64 off;
    u32 size;
} CsOut;
static CsOut g_out[256];
static int g_nout;
static u64 g_out_lo = ~0ull, g_out_hi;

static VkCommandPool g_cpool;
static VkCommandBuffer g_ccb;
static VkFence g_cfence;
static VkDescriptorPool g_dpool;
static int g_recording, g_landing;
static int g_inflight; /* submitted, results not landed yet */
/* draws can read results on the GPU before they land (vk_compute_gpu_src): the batch signals g_vk_csem, the next
   graphics submit waits for g_vk_cs_wait_value, and the arena they read stays alive until that frame is done */
VkSemaphore g_vk_csem;
u64 g_vk_cs_wait_value;
static u64 g_cval;
static int g_ar_gfx;
static u64 g_org_cap;
u64 g_vk_dispatches, g_vk_cs_batches, g_vk_cs_skipped, g_vk_cs_wait_ns;

static u8 g_cb[8][0x10000];
static u32 g_cbn[8];
static int cs_cb_read(u32 idx, u32 off, u32* out) {
    if (idx < 8 && off + 4 <= g_cbn[idx]) {
        memcpy(out, g_cb[idx] + off, 4);
        return 1;
    }
    return 0;
}

static void cs_init(void) {
    if (g_cpool) return;
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g_vk.compute_family};
    VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = g_cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_ccb));
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_OK(vkCreateFence(g_vk.dev, &fci, NULL, &g_cfence));
    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_vk_csem));
    /* the dispatches of a batch: a batch that runs out lands, which resets the pool */
    VkDescriptorPoolSize sz[4];
    vk_pool_sizes(sz, 1024);
    VkDescriptorPoolCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1024, .poolSizeCount = 4, .pPoolSizes = sz};
    VK_OK(vkCreateDescriptorPool(g_vk.dev, &dci, NULL, &g_dpool));
}

/* a bigger arena; only between batches, nothing points into it then */
static u64 g_ar_want = 8ull << 20; /* doubles when a batch fills the arena, up to 64 MB */
/* arenas draws read from: reused once the frames that read them are done */
static struct {
    VkBuffer b;
    VmaAllocation m;
    u8* p;
    u64 cap, frame;
} g_retired[8];
static int g_nretired;
static void arena_retire(void) {
    if (!g_ar) return;
    if (g_nretired == 8) { /* full: the oldest goes for good */
        vk_defer_buffer(g_retired[0].b, g_retired[0].m);
        memmove(g_retired, g_retired + 1, sizeof g_retired[0] * 7);
        g_nretired--;
    }
    g_retired[g_nretired++] = (typeof(g_retired[0])){g_ar, g_ar_mem, g_ar_ptr, g_ar_cap, vk_frame_number()};
    g_ar = VK_NULL_HANDLE;
}
static int arena_fit(u64 need) {
    if (g_ar && need <= g_ar_cap && g_ar_cap >= g_ar_want) return 1;
    vk_cmd(); /* frames older than VK_FRAMES are done once the current one has started */
    for (int i = 0; i < g_nretired; i++)
        if (g_retired[i].frame + VK_FRAMES <= vk_frame_number() && g_retired[i].cap >= need &&
            g_retired[i].cap >= g_ar_want) {
            if (g_ar) vk_defer_buffer(g_ar, g_ar_mem);
            g_ar = g_retired[i].b;
            g_ar_mem = g_retired[i].m;
            g_ar_ptr = g_retired[i].p;
            g_ar_cap = g_retired[i].cap;
            g_retired[i] = g_retired[--g_nretired];
            return 1;
        }
    if (g_ar) vk_defer_buffer(g_ar, g_ar_mem); /* draws may still read it */
    u64 cap = g_ar_want;
    while (cap < need) cap *= 2;
    u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = cap,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                              /* both queues use it, one at a time, without ownership transfers */
                              .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
                              .pQueueFamilyIndices = fam};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaAllocationInfo info;
    g_ar = VK_NULL_HANDLE;
    g_ar_cap = 0;
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &g_ar, &g_ar_mem, &info))) return 0;
    g_ar_ptr = info.pMappedData;
    g_ar_cap = cap;
    if (g_org_cap < cap) {
        free(g_org);
        g_org = malloc(cap);
        g_org_cap = g_org ? cap : 0;
    }
    return g_org != NULL;
}
static u64 arena_alloc(u64 size) {
    u64 al = vk_stage_align(), at = (g_ar_used + al - 1) & ~(al - 1);
    g_ar_used = at + size;
    return at;
}

/* a byte of the result lands when the shader changed it and guest memory still holds what the shader read */
static void write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    u8* dst = gmem_ptr(addr, size);
    u8* tmp = NULL;
    const u8* cur = dst;
    if (!cur) {
        tmp = malloc(size);
        gmem_read(addr, tmp, size);
        cur = tmp;
    }
    for (u32 i = 0; i < size;) {
        if (i + 8 <= size && !memcmp(res + i, org + i, 8)) {
            i += 8;
            continue;
        }
        if (res[i] == org[i] || cur[i] != org[i]) {
            i++;
            continue;
        }
        u32 s = i;
        while (i < size && res[i] != org[i] && cur[i] == org[i]) i++;
        if (dst)
            memcpy(dst + s, res + s, i - s);
        else
            gmem_write(addr + s, res + s, i - s);
    }
    free(tmp);
    gpu_mem_written(addr, size); /* caches built from this memory check it again */
}

/* submits the batch without waiting; it signals g_vk_csem with the next value */
static void cs_submit(void) {
    if (!g_recording) return;
    vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
    VK_OK(vkEndCommandBuffer(g_ccb));
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = g_ccb};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                 .semaphore = g_vk_csem,
                                 .value = ++g_cval,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &c,
                        .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    VkResult r = vkQueueSubmit2(g_vk.compute, 1, &si, g_cfence);
    if (r == VK_ERROR_DEVICE_LOST) fatal("vulkan: the GPU device was lost (driver reset or crash)");
    g_recording = 0;
    g_inflight = VK_OK(r);
    if (!g_inflight) g_cval--; /* never signaled: draws don't wait for it, landing doesn't wait for the fence */
}
/* submits the batch if it isn't yet, waits for it, and lands its results */
static void cs_land(void) {
    if ((!g_recording && !g_inflight) || g_landing) return;
    g_landing = 1;
    u64 t0 = plat_ns();
    cs_submit();
    if (g_inflight) {
        VK_OK(vkWaitForFences(g_vk.dev, 1, &g_cfence, VK_TRUE, UINT64_MAX));
        VK_OK(vkResetFences(g_vk.dev, 1, &g_cfence));
    }
    g_vk_cs_wait_ns += plat_ns() - t0;
    vmaInvalidateAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE);
    g_inflight = 0;
    for (int i = 0; i < g_nout; i++)
        write_changed(g_out[i].addr, g_ar_ptr + g_out[i].off, g_org + g_out[i].off, g_out[i].size);
    /* only now: until the results are in memory, readers on other threads wait for this landing in
       vk_compute_mem_need */
    g_cs_any = 0;
    g_nout = 0;
    g_out_lo = ~0ull;
    g_out_hi = 0;
    g_ar_used = 0;
    vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
    if (g_ar_gfx) { /* draws read this arena: it waits until their frame is done, the next batch gets another */
        arena_retire();
        g_ar_gfx = 0;
    }
    g_vk_cs_batches++;
    g_landing = 0;
}
/* a draw reads [gva, gva + len), which one pending result covers: it reads it from the arena, after the batch. Any
   other overlap lands the results now, so the caller's caches see them in memory */
int vk_compute_gpu_src(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    if (!g_cs_any || g_landing || gva >= g_out_hi || gva + len <= g_out_lo) return 0;
    for (int i = 0; i < g_nout; i++) {
        CsOut* o = &g_out[i];
        if (gva >= o->addr + o->size || o->addr >= gva + len) continue;
        /* partly covered, or at an offset that suits no use of it (index, storage and uniform buffers): it lands */
        if (gva < o->addr || gva + len > o->addr + o->size || ((o->off + (gva - o->addr)) & (vk_stage_align() - 1))) {
            cs_land();
            return 0;
        }
        cs_submit();
        g_ar_gfx = 1;
        g_vk_cs_wait_value = g_cval;
        *buf = g_ar;
        *off = o->off + (gva - o->addr);
        return 1;
    }
    return 0;
}
void vk_compute_land(void) {
    if (!g_recording && !g_inflight) return;
    vk_lock();
    cs_land();
    vk_unlock();
}
/* the front end writes guest memory (size > 0) or reaches a point the game can observe (size 0) */
void vk_compute_before_write(u64 gva, u64 size) {
    if (!size)
        vk_compute_land();
    else if (g_cs_any)
        vk_compute_mem_need(gva, size);
}
/* a read of guest memory: results for it have to be there first. Called from any thread that reads guest memory while
   g_cs_any is set: the pending results are looked at under vk_lock, which recording and landing them hold, so a
   reader waits for a landing in progress on another thread. The landing thread itself passes (g_landing) */
void vk_compute_mem_need(u64 a, u64 n) {
    vk_lock();
    if (!g_landing && a < g_out_hi && a + n > g_out_lo)
        for (int i = 0; i < g_nout; i++)
            if (a < g_out[i].addr + g_out[i].size && g_out[i].addr < a + n) {
                cs_land();
                break;
            }
    vk_unlock();
}

/* the pipeline of a program (80 zero bytes, then the code), by its code and launch parameters, and by the constant
   buffer words its translation read */
static CsPipe* cs_pipe(const u8* code, u32 lx, u32 ly, u32 lz, u32 shm) {
    u32 blen;
    u64 key = vk_shader_hash(code, 0x10000 + 80, &blen, cs_cb_read) ^ ((u64)lx << 40) ^ ((u64)ly << 48) ^
              ((u64)lz << 56) ^ ((u64)shm << 20);
    CsPipe* p = g_csp[key & 255];
    while (p && (p->key != key || !vk_cb_reads_hold(&p->cbr, cs_cb_read))) p = p->next;
    if (p) return p;
    p = calloc(1, sizeof *p);
    p->key = key;
    p->next = g_csp[key & 255];
    g_csp[key & 255] = p;
    p->si.cs_local[0] = lx;
    p->si.cs_local[1] = ly;
    p->si.cs_local[2] = lz;
    p->si.shared_size = (int)shm;
    const char* corpus = getenv("SWR_SHADER_CORPUS");
    if (corpus) shader_corpus_save(corpus, code, blen, 0x10000 + 80, ST_CS, &p->si, key);
    char* src = vk_translate(code, 0x10000 + 80, ST_CS, &p->si, cs_cb_read, &p->cbr);
    if (!src || p->si.unsupported) {
        p->failed = 1;
        LOG("vulkan: compute %016llx not translated (%d instructions, first %s)\n", (unsigned long long)key,
            p->si.unsupported, p->si.first_unsupported);
        free(src);
        return p;
    }
    size_t nw = 0;
    char err[2048] = "";
    u32* spv = vk_spirv_cached(src, ST_CS, g_vk_spirv_dir, &nw, err, sizeof err);
    free(src);
    if (!spv) {
        p->failed = 1;
        WARN("vulkan: compute %016llx didn't compile: %s\n", (unsigned long long)key, err);
        return p;
    }
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m;
    int ok = VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    free(spv);
    if (!ok) {
        p->failed = 1;
        return p;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m,
                                                .pName = "main"},
                                      .layout = g_vk_playout};
    if (!VK_OK(vkCreateComputePipelines(g_vk.dev, vk_pipeline_cache(), 1, &ci, NULL, &p->p))) p->failed = 1;
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (!p->failed)
        LOG_LIMIT(12, "vulkan: compute %016llx ready: block %ux%ux%u, %d global buffers, %d textures, %d images\n",
                  (unsigned long long)key, lx, ly, lz, p->si.ngbuf, p->si.ntex, p->si.nimg);
    return p;
}

void vk_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    u32* rc = ch->rcomp;
    u32 gx = q[12] & 0x7FFFFFFF, gy = q[13] & 0xFFFF, gz = q[13] >> 16, lx = q[18] >> 16, ly = q[19] & 0xFFFF,
        lz = q[19] >> 16, shm = q[17] & 0x3FFFF;
    if (!gx || !gy || !gz || !lx || !ly || !lz) return;
    const VkPhysicalDeviceLimits* lim = &g_vk.props.limits;
    if (gx > lim->maxComputeWorkGroupCount[0] || gy > lim->maxComputeWorkGroupCount[1] ||
        gz > lim->maxComputeWorkGroupCount[2] || lx > lim->maxComputeWorkGroupSize[0] ||
        ly > lim->maxComputeWorkGroupSize[1] || lz > lim->maxComputeWorkGroupSize[2] ||
        (u64)lx * ly * lz > lim->maxComputeWorkGroupInvocations || shm > lim->maxComputeSharedMemorySize) {
        LOG_LIMIT(1,
                  "vulkan: a dispatch of %ux%ux%u blocks of %ux%ux%u with %u bytes shared is past this GPU's limits "
                  "(skipped)\n",
                  gx, gy, gz, lx, ly, lz, shm);
        g_vk_cs_skipped++;
        return;
    }
    cs_init();
    /* constant buffers from the QMD (reading them lands results they overlap) */
    u64 cba[8] = {0};
    for (int i = 0; i < 8; i++) {
        g_cbn[i] = 0;
        if (!((q[20] >> i) & 1)) continue;
        cba[i] = ((u64)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
        u32 sz = (q[30 + 2 * i] >> 15) & 0x1FFFF;
        if (!sz || sz > 0x10000) sz = 0x10000;
        g_cbn[i] = sz;
        gmem_read(cba[i], g_cb[i], sz);
    }
    /* the program at an address is taken as the same until the next submit, as vk_shader_get does: the code isn't
       read and hashed again for every dispatch of it */
    static struct {
        u64 addr;
        u32 epoch, lx, ly, lz, shm;
        CsPipe* p;
    } fc[64];
    u64 pa = code + q[8];
    typeof(fc[0])* fe = &fc[((pa >> 8) ^ (pa >> 16)) & 63];
    CsPipe* p = fe->p;
    if (!p || fe->addr != pa || fe->epoch != g_vk_epoch || fe->lx != lx || fe->ly != ly || fe->lz != lz ||
        fe->shm != shm || !vk_cb_reads_hold(&p->cbr, cs_cb_read)) {
        static u8 code_buf[0x10000 + 80]; /* compute programs have no header: 80 zero bytes stand in for one */
        memset(code_buf, 0, 80);
        gmem_read(pa, code_buf + 80, 0x10000);
        p = cs_pipe(code_buf, lx, ly, lz, shm);
        *fe = (typeof(fc[0])){pa, g_vk_epoch, lx, ly, lz, shm, p};
    }
    if (p->failed) {
        g_vk_cs_skipped++;
        return;
    }
    if (p->si.nimg) {
        LOG_LIMIT(1, "vulkan: compute that writes images isn't done yet (skipped)\n");
        g_vk_cs_skipped++;
        return;
    }
    /* storage buffers: {address, size} in constant buffer 0 */
    u64 sa[16];
    u32 ss[16];
    int ng = p->si.ngbuf > 16 ? 16 : p->si.ngbuf;
    u64 need = 0;
    for (int i = 0; i < ng; i++) {
        u32 v[3] = {0, 0, 0};
        cs_cb_read(0, p->si.gbuf_cbuf_off[i], &v[0]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 4, &v[1]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 8, &v[2]);
        sa[i] = ((u64)v[1] << 32) | v[0];
        ss[i] = v[2] ? v[2] : 16;
        if (ss[i] > (16u << 20)) ss[i] = 16u << 20;
        ss[i] = (ss[i] + 3) & ~3u;
        need += ss[i] + 256;
    }
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) need += g_cbn[i] + 256;
    need += 2048;
    /* dispatches that read textures run on the graphics queue, after what was recorded before them. Inputs that
       pending results overlap land those first, before anything is put in the arena */
    int gfx = p->si.ntex > 0, overlap = 0;
    /* a read-only input inside a pending result of this batch is read from the arena on the GPU, after a barrier
       (multi-pass skinning reads what the pass before wrote); any other overlap lands the results first */
    i64 chain[16];
    for (int i = 0; i < ng; i++) {
        chain[i] = -1;
        for (int k = 0; k < g_nout && !overlap; k++) {
            CsOut* o = &g_out[k];
            if (sa[i] >= o->addr + o->size || o->addr >= sa[i] + ss[i]) continue;
            if (!((p->si.gbuf_wmask >> i) & 1) && g_recording && sa[i] >= o->addr && sa[i] + ss[i] <= o->addr + o->size)
                chain[i] = (i64)(o->off + (sa[i] - o->addr));
            else
                overlap = 1;
        }
    }
    if (g_recording && g_ar_used + need > g_ar_cap && g_ar_want < (64ull << 20))
        g_ar_want *= 2; /* fewer, bigger batches */
    if (gfx || overlap || g_inflight || g_nout + ng > 256 || g_ar_used + need > g_ar_cap) {
        cs_land();
        for (int i = 0; i < ng; i++) chain[i] = -1; /* the results are in memory now */
    }
    if (!g_recording && !arena_fit(need)) return;

    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = g_dpool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &g_vk_dslayout[shader_vk_set(ST_CS)]};
    if (vkAllocateDescriptorSets(g_vk.dev, &dai, &set) != VK_SUCCESS) {
        cs_land();
        if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &dai, &set))) return;
    }
    VkWriteDescriptorSet w[64];
    VkDescriptorBufferInfo bi[64];
    VkDescriptorImageInfo ii[32];
    u32 nw = 0;
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) {
            /* only the bytes the shader reads (robust buffer access keeps reads past them in bounds, of undefined
               value) */
            u32 range = g_cbn[i];
            if (p->si.cbuf_max[i] && range > p->si.cbuf_max[i]) range = p->si.cbuf_max[i];
            range = (range + 15) & ~15u;
            if (!range) range = 16;
            if (range > g_vk.props.limits.maxUniformBufferRange) range = g_vk.props.limits.maxUniformBufferRange;
            u64 o = arena_alloc(range);
            u32 n = g_cbn[i] < range ? g_cbn[i] : range;
            memcpy(g_ar_ptr + o, g_cb[i], n);
            memset(g_ar_ptr + o + n, 0, range - n);
            bi[nw] = (VkDescriptorBufferInfo){g_ar, o, range};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SWR_VKB_CB + i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    u64 so[16];
    int chained = 0;
    for (int i = 0; i < ng; i++) {
        if (chain[i] >= 0) chained = 1;
        u64 o = so[i] = chain[i] >= 0 ? (u64)chain[i] : arena_alloc(ss[i]);
        if (chain[i] >= 0)
            ; /* written by an earlier dispatch of the batch, read in place */
        else if (sa[i])
            gmem_read(sa[i], g_ar_ptr + o, ss[i]);
        else
            memset(g_ar_ptr + o, 0, ss[i]);
        bi[nw] = (VkDescriptorBufferInfo){g_ar, o, ss[i]};
        w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = SWR_VKB_SSBO + i,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &bi[nw]};
        nw++;
    }
    VkCommandBuffer cb;
    if (gfx) {
        /* textures from the compute engine's headers; texture handles from its texture constant buffer bank. Compute
           shaders declare every texture as float (they aren't keyed on integer textures), and depth textures aren't
           sampled here */
        u64 tic_base = ((u64)rc[0x55D] << 32) | rc[0x55E], tsc_base = ((u64)rc[0x557] << 32) | rc[0x558];
        u32 tcb = rc[0x982] & 7;
        int fmt_used = 0;
        i32 fmt[320] = {0}; /* SWR_FMT, std140: swr_ic[8] swr_tc[32] swr_ib[8] swr_tb[32], 16 bytes each */
        for (int i = 0; i < p->si.ntex && i < SH_MAXTEX; i++) {
            ShTex* st = &p->si.tex[i];
            u32 h = 0, h2 = 0;
            cs_cb_read(st->bindless ? (st->bl_cbuf & 7) : tcb, st->cbuf_off * 4, &h);
            if (st->bindless && st->bl2 && cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2)) h |= h2;
            u32 tic[8], tsc[8];
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, 32);
            gmem_read(tsc_base + ((h >> 20) & 0xFFF) * 32ull, tsc, 32);
            VkImageViewType want = vk_tex_view_type(st);
            Surf* t = vk_tex_from_tic(tic);
            VkImageView v = t && !t->f.depth ? vk_tex_bind_view(t, want, tic, 0) : VK_NULL_HANDLE;
            if (v)
                vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_SAMPLED_STAGES,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            else
                v = vk_null_view(want, 0);
            VkSampler smp = vk_tex_bind_sampler(tsc, st, 0);
            if (!v || !smp) { /* before any output is recorded: nothing of this dispatch lands */
                g_vk_cs_skipped++;
                return;
            }
            ii[i] = (VkDescriptorImageInfo){smp, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = SWR_VKB_TEX + i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             .pImageInfo = &ii[i]};
            fmt[(8 + i) * 4] = t ? (t->f.vk == VK_FORMAT_R16G16B16A16_SFLOAT || t->f.vk == VK_FORMAT_R16G16_SFLOAT ||
                                            t->f.vk == VK_FORMAT_R16_SFLOAT
                                        ? 1
                                    : t->f.bpp == 4 && !t->f.is_int ? 2
                                                                    : 0)
                                 : 0;
            fmt[(48 + i) * 4] = t && t->f.bpp ? t->f.bpp : 4;
            fmt_used = 1;
        }
        if (fmt_used) {
            u64 o = arena_alloc(sizeof fmt);
            memcpy(g_ar_ptr + o, fmt, sizeof fmt);
            bi[nw] = (VkDescriptorBufferInfo){g_ar, o, sizeof fmt};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = SWR_VKB_FMT,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           .pBufferInfo = &bi[nw]};
            nw++;
        }
    }
    /* the outputs, once every input is read (an input can alias another storage buffer's output) */
    for (int i = 0; i < ng; i++)
        if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
            memcpy(g_org + so[i], g_ar_ptr + so[i], ss[i]);
            g_out[g_nout++] = (CsOut){sa[i], so[i], ss[i]};
            if (sa[i] < g_out_lo) g_out_lo = sa[i];
            if (sa[i] + ss[i] > g_out_hi) g_out_hi = sa[i] + ss[i];
        }
    if (gfx) {
        vk_render_end();
        cb = vk_cmd();
    } else {
        if (!g_recording) {
            VkCommandBufferBeginInfo b = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VK_OK(vkBeginCommandBuffer(g_ccb, &b));
            g_recording = 1;
        }
        cb = g_ccb;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, w, 0, NULL);
    if (chained) {
        VkMemoryBarrier2 cmb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
        VkDependencyInfo cdi = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &cmb};
        vkCmdPipelineBarrier2(cb, &cdi);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_vk_playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    /* results visible to the host */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
    g_vk_dispatches++;
    if (gfx) {
        /* everything before it on the graphics queue runs first; then its results land at once */
        vmaFlushAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE); /* the arena may not be host coherent */
        vk_finish();
        vmaInvalidateAllocation(g_vk.vma, g_ar_mem, 0, VK_WHOLE_SIZE);
        g_landing = 1;
        for (int i = 0; i < g_nout; i++)
            write_changed(g_out[i].addr, g_ar_ptr + g_out[i].off, g_org + g_out[i].off, g_out[i].size);
        g_nout = 0;
        g_out_lo = ~0ull;
        g_out_hi = 0;
        g_ar_used = 0;
        vkResetDescriptorPool(g_vk.dev, g_dpool, 0);
        g_landing = 0;
        return;
    }
    g_cs_any = g_nout > 0;
}
