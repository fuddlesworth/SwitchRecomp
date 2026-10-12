/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* Shaders and pipelines. A guest shader is translated to GLSL (shader.c), patched for what Vulkan does differently
   (vk_glsl_post: the alpha test and the depth range, with push constants), compiled to SPIR-V through the on-disk
   cache and kept as a module. Every stage has a descriptor set of the same bindings, one set per stage
   (shader_vk_set). Pipelines are made per PipeKey: the shaders, vertex layout, attachment formats and blend state;
   everything else is dynamic state. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/main.h"
#include "gpu/gpu.h"
#include "gpu/shader/shader.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "gpu/vk/vk.h"
#include "gpu/vk/spirv.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

/* The caches live in shader_cache/ next to the games, shared by all of them, wherever the runtime was started from */
static char g_cache_dir[1100], g_pcache_path[1200], g_pcache_tmp[1230];
char g_vk_spirv_dir[1100];
static int g_pcache_dirty; /* pipelines made since the cache was last written (atomic: it is saved outside vk_lock) */

VkPipelineLayout g_vk_playout;
VkDescriptorSetLayout g_vk_dslayout[3];
static VkPipelineCache g_pcache;
u64 g_vk_cb[5][18];
u32 g_vk_cbsize[5][18];
static int g_sh_bind; /* the stage whose constant buffers the translator reads */

static int sh_cb_read(u32 idx, u32 off, u32* out) {
    if (idx >= 18) return 0;
    u64 a = g_vk_cb[g_sh_bind][idx];
    u32 sz = g_vk_cbsize[g_sh_bind][idx];
    if (!a || off + 4 > (sz ? sz : 65536)) return 0;
    gmem_read(a + off, out, 4);
    return 1;
}

/* translation records what it reads through rec_read */
static VkCbReader g_rec_read;
static VkCbReads* g_rec;
static int rec_read(u32 idx, u32 off, u32* out) {
    int ok = g_rec_read(idx, off, out);
    VkCbReads* r = g_rec;
    if (r->n % 16 == 0) {
        VkCbRead* g = realloc(r->r, sizeof *g * (r->n + 16));
        if (!g) return ok;
        r->r = g;
    }
    r->r[r->n++] = (VkCbRead){idx, off, (u32)ok, ok ? *out : 0};
    return ok;
}
char* vk_translate(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, VkCbReader read, VkCbReads* reads) {
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_rec_read = read;
    g_rec = reads;
    g_shader_cb_read = rec_read;
    char* src = shader_to_glsl(code, max_bytes, stage, si);
    g_shader_cb_read = old;
    return src;
}
/* shader_hash with the jump tables of BRX read through read, as the translation reads them: without a reader the
   length stops at the first EXIT and leaves out code only a jump table reaches */
u64 vk_shader_hash(const u8* code, u32 max_bytes, u32* out_len, VkCbReader read) {
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = read;
    u64 h = shader_hash(code, max_bytes, out_len);
    g_shader_cb_read = old;
    return h;
}
int vk_cb_reads_hold(const VkCbReads* reads, VkCbReader read) {
    for (u32 i = 0; i < reads->n; i++) {
        const VkCbRead* c = &reads->r[i];
        u32 v = 0;
        int ok = read(c->idx, c->off, &v);
        if ((u32)ok != c->ok || (ok && v != c->v)) return 0;
    }
    return 1;
}

/* descriptors of each type in one set: the binding ranges in shader.h */
static VkDescriptorType binding_type(u32 i) {
    return i < SWR_VKB_SSBO  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
           : i < SWR_VKB_TEX ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
           : i < SWR_VKB_IMG ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
           : i < SWR_VKB_RS  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
void vk_pool_sizes(VkDescriptorPoolSize* sz, u32 sets) {
    static const VkDescriptorType ty[4] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    for (int k = 0; k < 4; k++) {
        u32 n = 0;
        for (u32 i = 0; i <= SWR_VKB_FMT; i++) n += binding_type(i) == ty[k];
        sz[k] = (VkDescriptorPoolSize){ty[k], n * sets};
    }
}

void vk_pipeline_init(void) {
    /* a layout per set, the same bindings, each visible to the stages that use the set (shader_vk_set): within the
       per-stage limits a device has, which all stages seeing every set would multiply by three */
    static const VkShaderStageFlags vis[3] = {
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT |
            VK_SHADER_STAGE_GEOMETRY_BIT};
    for (int s = 0; s < 3; s++) {
        VkDescriptorSetLayoutBinding b[SWR_VKB_FMT + 1];
        for (u32 i = 0; i <= SWR_VKB_FMT; i++)
            b[i] = (VkDescriptorSetLayoutBinding){
                .binding = i, .descriptorType = binding_type(i), .descriptorCount = 1, .stageFlags = vis[s]};
        VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = SWR_VKB_FMT + 1,
                                              .pBindings = b};
        VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_vk_dslayout[s]));
    }
    {
        VkDescriptorPoolSize n[4];
        vk_pool_sizes(n, 1);
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        if (n[0].descriptorCount > l->maxPerStageDescriptorUniformBuffers ||
            n[1].descriptorCount > l->maxPerStageDescriptorStorageBuffers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSamplers ||
            n[2].descriptorCount > l->maxPerStageDescriptorSampledImages ||
            n[3].descriptorCount > l->maxPerStageDescriptorStorageImages ||
            n[0].descriptorCount + n[1].descriptorCount + n[2].descriptorCount + n[3].descriptorCount + 8 >
                l->maxPerStageResources)
            WARN("vulkan: a stage's descriptor set (%u uniform and %u storage buffers, %u textures, %u images) is "
                 "past this GPU's per-stage limits (%u, %u, %u, %u): pipelines may fail\n",
                 n[0].descriptorCount, n[1].descriptorCount, n[2].descriptorCount, n[3].descriptorCount,
                 l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers,
                 l->maxPerStageDescriptorSamplers, l->maxPerStageDescriptorStorageImages);
    }
    VkDescriptorSetLayout sets[3] = {g_vk_dslayout[0], g_vk_dslayout[1], g_vk_dslayout[2]};
    /* push constants: swr_vp (vertex) at 0, swr_at (fragment) at 16 */
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 3,
                                     .pSetLayouts = sets,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc};
    VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_vk_playout));
    snprintf(g_cache_dir, sizeof g_cache_dir, "%sshader_cache", g_exedir);
    plat_mkdir(g_cache_dir);
    snprintf(g_vk_spirv_dir, sizeof g_vk_spirv_dir, "%s/spirv", g_cache_dir);
    plat_mkdir(g_vk_spirv_dir);
    snprintf(g_pcache_path, sizeof g_pcache_path, "%s/vk_pipelines.bin", g_cache_dir);
    /* the temporary name is the process's own: two games running at once would otherwise write the same file */
#if defined(_WIN32)
    unsigned pid = (unsigned)GetCurrentProcessId();
#else
    unsigned pid = (unsigned)getpid();
#endif
    snprintf(g_pcache_tmp, sizeof g_pcache_tmp, "%s.%u.tmp", g_pcache_path, pid);
    /* the driver's compiled pipelines from earlier runs; it ignores data from another device or driver version */
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    PFileMap data;
    if (!plat_file_map(&data, g_pcache_path) && data.data) {
        ci.initialDataSize = (size_t)data.size;
        ci.pInitialData = data.data;
    }
    if (!VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache)) && ci.initialDataSize) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache));
    }
    if (ci.initialDataSize) LOG("vulkan: pipeline cache loaded (%zu KB)\n", ci.initialDataSize >> 10);
    plat_file_unmap(&data); /* the cache copied it, and a mapped file can't be replaced on Windows */
}

/* ------------------------------------------------------------ shaders */

static VkSh* g_shh[1024];
/* the shader last found for an address and key, two per set. It is checked against memory once per submit: unchanged
   by write tracking since then, or else by the hash of its code */
typedef struct {
    u64 gva, gva_a, gw;
    u32 mask[5], epoch, wseen;
    int stage;
    VkSh* s;
} ShA;
static ShA g_sha[4096];

static u64 code_hash(u64 gva, u32 len) {
    u8* p = gmem_ptr(gva, len);
    if (p) return vk_hash(p, len);
    u8* t = malloc(len ? len : 1);
    gmem_read(gva, t, len);
    u64 q = vk_hash(t, len);
    free(t);
    return q;
}
static int code_same(const ShA* ca, u64 gva, u32 len, u64 qh) {
    if (!gpu_mem_dirty_since(ca->gw, gva, len) && vk_written_since(gmem_ptr(gva, len), len, ca->wseen) == 0) return 1;
    return code_hash(gva, len) == qh;
}

VkSh* vk_shader_get(u64 gva, u64 gva_a, int stage, u32 int_mask, u32 sint_mask, u32 rt_int, u32 rt_sint, u32 clip) {
    g_sh_bind = stage == ST_VS ? 0 : 4;
    u32 mask[5] = {int_mask, sint_mask, rt_int, rt_sint, clip};
    u32 mix = int_mask * 0x9E3779B1u ^ sint_mask * 0x85EBCA6Bu ^ rt_int * 0x27D4EB2Fu ^ rt_sint * 0x165667B1u ^
              clip * 0x61C88647u;
    ShA* set = &g_sha[(u32)((gva >> 6) ^ (gva >> 18) ^ (gva_a >> 4) ^ mix ^ ((u32)stage << 11)) & 4094];
    ShA* ca = NULL;
    for (int w = 0; w < 2 && !ca; w++)
        if (set[w].s && set[w].gva == gva && set[w].gva_a == gva_a && set[w].stage == stage &&
            !memcmp(set[w].mask, mask, sizeof mask))
            ca = &set[w];
    if (ca) {
        VkSh* s = ca->s;
        int same = ca->epoch == g_vk_epoch ||
                   (code_same(ca, gva, s->clen, s->qh) && (!gva_a || code_same(ca, gva_a, s->clen_a, s->qh_a)));
        if (same && ca->epoch != g_vk_epoch) {
            ca->epoch = g_vk_epoch;
            ca->wseen = vk_ww_now();
            ca->gw = g_gw_gen;
        }
        if (same && vk_cb_reads_hold(&s->cbr, sh_cb_read)) return s;
    } else /* the way used longest ago */
        ca = !set[0].s || (set[1].s && (i32)(set[0].epoch - set[1].epoch) <= 0) ? &set[0] : &set[1];
    u32 wseen = vk_ww_now();
    u64 gw = g_gw_gen;
    u8* code = malloc(0x10000);
    gmem_read(gva, code, 0x10000);
    u32 len, len_a = 0;
    u64 qh, qh_a = 0;
    vk_shader_hash(code, 0x10000, &len, sh_cb_read);
    qh = vk_hash(code, len);
    if (gva_a) { /* VertexA + VertexB: one merged program */
        u8* a = malloc(0x10000);
        gmem_read(gva_a, a, 0x10000);
        vk_shader_hash(a, 0x10000, &len_a, sh_cb_read);
        qh_a = vk_hash(a, len_a);
        u8* m = malloc(0x10000);
        /* the merge sizes A and B with shader_hash too */
        int (*old)(u32, u32, u32*) = g_shader_cb_read;
        g_shader_cb_read = sh_cb_read;
        int merged = shader_merge_dual(a, code, m, 0x10000);
        g_shader_cb_read = old;
        if (merged) {
            free(code);
            code = m;
        } else {
            free(m);
            WARN_LIMIT(8, "vulkan: VertexA %llx and VertexB %llx couldn't be merged: VertexB runs alone\n",
                       (unsigned long long)gva_a, (unsigned long long)gva);
        }
        free(a);
    }
    u32 mlen;
    u64 h = vk_shader_hash(code, 0x10000, &mlen, sh_cb_read);
    VkSh* s = NULL;
    for (VkSh* q = g_shh[h & 1023]; q && !s; q = q->next)
        if (q->hash == h && q->stage == stage && !memcmp(q->mask, mask, sizeof mask) &&
            vk_cb_reads_hold(&q->cbr, sh_cb_read))
            s = q;
    if (!s) {
        s = calloc(1, sizeof *s);
        s->hash = h;
        s->stage = stage;
        memcpy(s->mask, mask, sizeof mask);
        s->clen = len;
        s->qh = qh;
        s->clen_a = len_a;
        s->qh_a = qh_a;
        if (stage == ST_FS) {
            s->si.tex_int_mask = int_mask;
            s->si.tex_sint_mask = sint_mask;
            s->si.rt_int_mask = rt_int;
            s->si.rt_sint_mask = rt_sint;
        } else {
            s->si.attr_int_mask = int_mask;
            s->si.attr_sint_mask = sint_mask;
            s->si.clip_enable = clip;
        }
        {
            static const char* corpus = (const char*)-1;
            if (corpus == (const char*)-1) corpus = getenv("SWR_SHADER_CORPUS");
            if (corpus) shader_corpus_save(corpus, code, mlen, 0x10000, stage, &s->si, h ^ mix);
        }
        char* src = vk_glsl_post(vk_translate(code, 0x10000, stage, &s->si, sh_cb_read, &s->cbr), stage);
        if (s->si.unsupported)
            WARN("shader %016llx: %d untranslated instructions (first: %s)\n", (unsigned long long)h, s->si.unsupported,
                 s->si.first_unsupported);
        size_t nw = 0;
        char err[2048] = "";
        u32* spv = src ? vk_spirv_cached(src, stage, g_vk_spirv_dir, &nw, err, sizeof err) : NULL;
        if (spv) {
            VkShaderModuleCreateInfo mi = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
            if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod))) s->failed = 1;
            free(spv);
        } else {
            s->failed = 1;
            WARN("vulkan: shader %016llx (stage %d) didn't compile: %s\n", (unsigned long long)h, stage, err);
            if (src && getenv("SWR_DUMP_SHADERS")) { /* next to the caches, not in the current directory */
                char pth[1200];
                snprintf(pth, sizeof pth, "%s/vkshader_%016llx_%d.glsl", g_cache_dir, (unsigned long long)h, stage);
                FILE* f = fopen(pth, "w");
                if (f) {
                    fputs(src, f);
                    fclose(f);
                }
            }
        }
        free(src);
        s->next = g_shh[h & 1023];
        g_shh[h & 1023] = s;
    }
    free(code);
    ca->gva = gva;
    ca->gva_a = gva_a;
    memcpy(ca->mask, mask, sizeof mask);
    ca->stage = stage;
    ca->epoch = g_vk_epoch;
    ca->wseen = wseen;
    ca->gw = gw;
    ca->s = s;
    return s;
}

VkShaderModule vk_module(const char* glsl, int stage) {
    size_t nw = 0;
    char err[1024] = "";
    u32* spv = vk_spirv_cached(glsl, stage, g_vk_spirv_dir, &nw, err, sizeof err);
    if (!spv) {
        WARN("vulkan: an internal shader didn't compile: %s\n", err);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    free(spv);
    return m;
}
/* an empty fragment shader for depth-only passes */
static VkShaderModule null_fs(void) {
    static VkShaderModule m;
    if (!m) m = vk_module("#version 450\nvoid main(){}\n", ST_FS);
    return m;
}

/* ------------------------------------------------------------ pipelines */
typedef struct Pipe {
    PipeKey k;
    u64 h;
    VkPipeline p;
    struct Pipe* next;
} Pipe;
#define PIPEH 4096
static Pipe* g_pipeh[PIPEH];

static VkPipeline pipe_create(const PipeKey* k) {
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                              .module = k->vs,
                                              .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                              .module = k->fs ? k->fs : null_fs(),
                                              .pName = "main"}};
    VkVertexInputBindingDescription vb[33];
    VkVertexInputBindingDivisorDescriptionEXT dv[33];
    u32 ndv = 0;
    for (u32 i = 0; i < k->nbind; i++) {
        vb[i] = (VkVertexInputBindingDescription){.binding = i,
                                                  .stride = 0, /* dynamic */
                                                  .inputRate = k->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                  : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->bind[i].divisor > 1 && g_vk.has_divisor)
            dv[ndv++] = (VkVertexInputBindingDivisorDescriptionEXT){i, k->bind[i].divisor};
    }
    VkVertexInputAttributeDescription va[32];
    for (u32 i = 0; i < k->nattr; i++)
        va[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].format,
                                                    k->attr[i].offset};
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = ndv,
        .pVertexBindingDivisors = dv};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .pNext = ndv ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = k->nbind,
                                               .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = k->nattr,
                                               .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)k->topo_class};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = (VkPolygonMode)k->polygon,
                                                 .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    int changed = 0;
    for (u32 i = 0; i < k->ncolor; i++) {
        cba[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = k->blend[i].en,
                                                       .srcColorBlendFactor = (VkBlendFactor)k->blend[i].sc,
                                                       .dstColorBlendFactor = (VkBlendFactor)k->blend[i].dc,
                                                       .colorBlendOp = (VkBlendOp)k->blend[i].cop,
                                                       .srcAlphaBlendFactor = (VkBlendFactor)k->blend[i].sa,
                                                       .dstAlphaBlendFactor = (VkBlendFactor)k->blend[i].da,
                                                       .alphaBlendOp = (VkBlendOp)k->blend[i].aop,
                                                       /* no fragment shader: its outputs would be undefined */
                                                       .colorWriteMask = k->fs ? k->blend[i].mask : 0};
        if (!g_vk.feat.dualSrcBlend) { /* the second source's factors become the first's */
            VkBlendFactor* f[4] = {&cba[i].srcColorBlendFactor, &cba[i].dstColorBlendFactor,
                                   &cba[i].srcAlphaBlendFactor, &cba[i].dstAlphaBlendFactor};
            for (int j = 0; j < 4; j++) {
                VkBlendFactor o = *f[j];
                *f[j] = o == VK_BLEND_FACTOR_SRC1_COLOR             ? VK_BLEND_FACTOR_SRC_COLOR
                        : o == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR ? VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR
                        : o == VK_BLEND_FACTOR_SRC1_ALPHA           ? VK_BLEND_FACTOR_SRC_ALPHA
                        : o == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA
                                                                    : o;
                changed |= *f[j] != o;
            }
        }
    }
    if (!g_vk.feat.independentBlend) /* every attachment blends as the first */
        for (u32 i = 1; i < k->ncolor; i++) {
            changed |= memcmp(&cba[i], &cba[0], sizeof cba[0]) != 0;
            cba[i] = cba[0];
        }
    if (changed) {
        LOG_LIMIT(
            1, "vulkan: blending that needs dual-source or independent blend, which this GPU lacks, is simplified\n");
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor,
                                              .pAttachments = cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
                            VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
                            VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                            VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                            VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
                            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
                            VK_DYNAMIC_STATE_LINE_WIDTH,
                            VK_DYNAMIC_STATE_DEPTH_BIAS,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_DEPTH_BOUNDS};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof dyn / sizeof dyn[0],
                                           .pDynamicStates = dyn};
    VkFormat cf[8];
    for (u32 i = 0; i < k->ncolor; i++) cf[i] = (VkFormat)k->color[i];
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = cf,
                                        .depthAttachmentFormat = (VkFormat)k->depth,
                                        .stencilAttachmentFormat = (VkFormat)k->stencil};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &ri,
                                       .stageCount = 2,
                                       .pStages = st,
                                       .pVertexInputState = &vi,
                                       .pInputAssemblyState = &ia,
                                       .pViewportState = &vp,
                                       .pRasterizationState = &rs,
                                       .pMultisampleState = &ms,
                                       .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb,
                                       .pDynamicState = &dy,
                                       .layout = g_vk_playout};
    VkPipeline p = VK_NULL_HANDLE;
    VK_OK(vkCreateGraphicsPipelines(g_vk.dev, g_pcache, 1, &gi, NULL, &p));
    return p;
}

VkPipeline vk_pipeline_get(const PipeKey* k) {
    u64 h = vk_hash(k, sizeof *k);
    u32 b = (u32)(h ^ (h >> 32)) & (PIPEH - 1);
    for (Pipe* p = g_pipeh[b]; p; p = p->next)
        if (p->h == h && !memcmp(&p->k, k, sizeof *k)) return p->p;
    Pipe* p = calloc(1, sizeof *p);
    p->k = *k;
    p->h = h;
    p->p = pipe_create(k);
    __atomic_store_n(&g_pcache_dirty, 1, __ATOMIC_RELAXED);
    p->next = g_pipeh[b];
    g_pipeh[b] = p;
    return p->p;
}

VkPipelineCache vk_pipeline_cache(void) {
    __atomic_store_n(&g_pcache_dirty, 1, __ATOMIC_RELAXED);
    return g_pcache;
}
/* writes the pipeline cache when pipelines were made since the last write. Called every few seconds and at exit
   (the renderer's save), outside vk_lock: the driver synchronizes the cache itself. Saves one at a time, so the one
   at exit waits for a periodic one in progress to finish its file before the process ends */
static PMutex g_psave = PMUTEX_INIT;
void vk_pipeline_save(void) {
    pm_lock(&g_psave);
    if (!g_pcache || !__atomic_exchange_n(&g_pcache_dirty, 0, __ATOMIC_RELAXED)) {
        pm_unlock(&g_psave);
        return;
    }
    size_t n = 0;
    void* d = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, NULL) == VK_SUCCESS && n) d = malloc(n);
    if (d && vkGetPipelineCacheData(g_vk.dev, g_pcache, &n, d) == VK_SUCCESS) {
        FILE* f = fopen(g_pcache_tmp, "wb");
        if (f) {
            int ok = fwrite(d, 1, n, f) == n;
            if (fclose(f)) ok = 0;
            if (ok)
                plat_replace_file(g_pcache_tmp, g_pcache_path); /* the old cache stays whole until this */
            else
                remove(g_pcache_tmp);
        }
    }
    free(d);
    pm_unlock(&g_psave);
}
