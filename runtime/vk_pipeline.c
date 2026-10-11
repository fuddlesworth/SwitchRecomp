/* Shaders and pipelines. A guest shader is translated to GLSL in the translator's Vulkan mode (shader.c), patched
   like gpu_gl.c's sh_post (alpha test, depth range) with push constants instead of uniforms, compiled to SPIR-V
   through the on-disk cache and kept as a module. Every stage uses the same descriptor set layout, one set per
   stage (shader_vk_set). Pipelines are made per PipeKey: the shaders, vertex layout, attachment formats and blend
   state; everything else is dynamic state. */
#include "vk.h"
#include "vk_shader.h"

VkPipelineLayout g_vk_playout;
VkDescriptorSetLayout g_vk_dslayout;
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

void vk_pipeline_init(void) {
    /* one layout for every stage's set: the binding ranges in shader.h */
    VkDescriptorSetLayoutBinding b[SWR_VKB_FMT + 1];
    for (u32 i = 0; i <= SWR_VKB_FMT; i++) {
        VkDescriptorType t = i < SWR_VKB_SSBO  ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                             : i < SWR_VKB_TEX ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                             : i < SWR_VKB_IMG ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                             : i < SWR_VKB_RS  ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                               : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i,
                                              .descriptorType = t,
                                              .descriptorCount = 1,
                                              .stageFlags = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT};
    }
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = SWR_VKB_FMT + 1, .pBindings = b};
    VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &dl, NULL, &g_vk_dslayout));
    VkDescriptorSetLayout sets[3] = {g_vk_dslayout, g_vk_dslayout, g_vk_dslayout};
    /* push constants: swr_vp (vertex) at 0, swr_at (fragment) at 16 */
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 3,
                                     .pSetLayouts = sets,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &pc};
    VK_OK(vkCreatePipelineLayout(g_vk.dev, &pl, NULL, &g_vk_playout));
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VK_OK(vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pcache));
    plat_mkdir("shader_cache");
    plat_mkdir("shader_cache/spirv");
    g_shader_vulkan = 1;
}

/* ------------------------------------------------------------ shaders */
/* gpu_gl.c's sh_post for Vulkan: the alpha test discards before every return of a fragment shader, and a vertex
   shader for a guest in -1..1 depth mode moves z to Vulkan's 0..1 (the viewport handles y, see vk_render.c) */
static char* sh_post(char* src, int stage) {
    if (!src) return src;
    const char* fn = NULL;
    const char* ret = NULL;
    if (stage == ST_FS && strstr(src, "out vec4 frag_color0")) {
        fn =
            "layout(push_constant) uniform SWR_PC { layout(offset=16) vec4 swr_at; };\n"
            "bool swr_atest(float a){ int f=int(swr_at.y); float r=swr_at.z;\n"
            "  if(f==0) return false; if(f==1) return a<r; if(f==2) return a==r; if(f==3) return a<=r; if(f==4) return a>r; if(f==5) return a!=r; if(f==6) return a>=r; return true; }\n";
        ret = "{ if(swr_at.x!=0.0 && !swr_atest(frag_color0.w)) discard; return; }";
    } else if (stage == ST_VS) {
        fn = "layout(push_constant) uniform SWR_PC { vec4 swr_vp; };\n";
        ret = "{ if(swr_vp.y!=0.0) gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; return; }";
    }
    char* p = fn ? strstr(src, "void main(){") : NULL;
    if (!p) return src;
    size_t pre = (size_t)(p - src), nret = 0;
    for (const char* q = p; (q = strstr(q, "return;")); q += 7) nret++;
    char* out = malloc(strlen(src) + strlen(fn) + nret * strlen(ret) + 1);
    memcpy(out, src, pre);
    char* o = out + pre;
    o += sprintf(o, "%s", fn);
    for (const char* q = p; *q;) {
        if (!strncmp(q, "return;", 7)) {
            o += sprintf(o, "%s", ret);
            q += 7;
        } else
            *o++ = *q++;
    }
    *o = 0;
    free(src);
    return out;
}

static VkSh* g_shh[1024];
typedef struct {
    u64 gva, gva_a;
    u32 key2, epoch;
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

static int stage_of(int st) {
    return st == ST_VS ? VK_SHADER_STAGE_VERTEX_BIT : st == ST_FS ? VK_SHADER_STAGE_FRAGMENT_BIT : 0;
}

VkSh* vk_shader_get(u64 gva, u64 gva_a, int stage, u32 int_mask, u32 sint_mask, u32 rt_int, u32 rt_sint) {
    g_sh_bind = stage == ST_VS ? 0 : 4;
    g_shader_cb_read = sh_cb_read;
    u32 key2 = stage == ST_VS ? (int_mask ^ (sint_mask * 31))
                              : (int_mask * 0x9E3779B1u ^ sint_mask * 0x85EBCA6Bu ^
                                 (rt_int || rt_sint ? rt_int * 0x27D4EB2Fu ^ rt_sint * 0x165667B1u ^ 0x5A5A0000u : 0));
    ShA* ca =
        &g_sha[(u32)((gva >> 6) ^ (gva >> 18) ^ (gva_a >> 4) ^ ((u64)key2 * 0x9E3779B1u) ^ ((u32)stage << 11)) & 4095];
    if (ca->s && ca->gva == gva && ca->gva_a == gva_a && ca->stage == stage && ca->key2 == key2) {
        if (ca->epoch == g_vk_epoch) return ca->s;
        VkSh* s = ca->s;
        if (code_hash(gva, s->clen) == s->qh && (!gva_a || code_hash(gva_a, s->clen_a) == s->qh_a)) {
            ca->epoch = g_vk_epoch;
            return s;
        }
    }
    u8* code = malloc(0x10000);
    gmem_read(gva, code, 0x10000);
    u32 len, len_a = 0;
    u64 qh, qh_a = 0;
    shader_hash(code, 0x10000, &len);
    qh = vk_hash(code, len);
    if (gva_a) { /* VertexA + VertexB: one merged program */
        u8* a = malloc(0x10000);
        gmem_read(gva_a, a, 0x10000);
        shader_hash(a, 0x10000, &len_a);
        qh_a = vk_hash(a, len_a);
        u8* m = malloc(0x10000);
        if (shader_merge_dual(a, code, m, 0x10000)) {
            free(code);
            code = m;
        } else
            free(m);
        free(a);
    }
    u32 mlen;
    u64 h = shader_hash(code, 0x10000, &mlen);
    VkSh* s = NULL;
    for (VkSh* q = g_shh[h & 1023]; q; q = q->next)
        if (q->hash == h && q->stage == stage && q->key2 == key2) s = q;
    if (!s) {
        s = calloc(1, sizeof *s);
        s->hash = h;
        s->stage = stage;
        s->key2 = key2;
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
        }
        {
            static const char* corpus = (const char*)-1;
            if (corpus == (const char*)-1) corpus = getenv("SWR_SHADER_CORPUS");
            if (corpus) shader_corpus_save(corpus, code, mlen, 0x10000, stage, &s->si, h ^ key2);
        }
        char* src = sh_post(shader_to_glsl(code, 0x10000, stage, &s->si), stage);
        if (stage != ST_FS) s->si.attr_sint_mask = sint_mask;
        if (s->si.unsupported)
            WARN("shader %016llx: %d untranslated instructions (first: %s)\n", (unsigned long long)h, s->si.unsupported,
                 s->si.first_unsupported);
        size_t nw = 0;
        char err[2048] = "";
        u32* spv = src ? vk_spirv_cached(src, stage, "shader_cache/spirv", &nw, err, sizeof err) : NULL;
        if (spv) {
            VkShaderModuleCreateInfo mi = {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
            if (!VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &s->mod))) s->failed = 1;
            free(spv);
        } else {
            s->failed = 1;
            WARN("vulkan: shader %016llx (stage %d) didn't compile: %s\n", (unsigned long long)h, stage, err);
            if (src && getenv("SWR_DUMP_SHADERS")) {
                char pth[64];
                snprintf(pth, sizeof pth, "vkshader_%016llx_%d.glsl", (unsigned long long)h, stage);
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
    ca->key2 = key2;
    ca->stage = stage;
    ca->epoch = g_vk_epoch;
    ca->s = s;
    return s;
}

/* an empty fragment shader for depth-only passes */
static VkShaderModule null_fs(void) {
    static VkShaderModule m;
    if (m) return m;
    size_t nw = 0;
    char err[512];
    u32* spv = vk_spirv_cached("#version 450\nvoid main(){}\n", ST_FS, "shader_cache/spirv", &nw, err, sizeof err);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = spv};
    VK_OK(vkCreateShaderModule(g_vk.dev, &mi, NULL, &m));
    free(spv);
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
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT dv[17];
    u32 ndv = 0;
    for (u32 i = 0; i < k->nbind; i++) {
        vb[i] = (VkVertexInputBindingDescription){.binding = k->bind[i].binding,
                                                  .stride = 0, /* dynamic */
                                                  .inputRate = k->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                  : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->bind[i].divisor > 1 && g_vk.has_divisor)
            dv[ndv++] = (VkVertexInputBindingDivisorDescriptionEXT){k->bind[i].binding, k->bind[i].divisor};
    }
    VkVertexInputAttributeDescription va[16];
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
    for (u32 i = 0; i < k->ncolor; i++)
        cba[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = k->blend[i].en,
                                                       .srcColorBlendFactor = (VkBlendFactor)k->blend[i].sc,
                                                       .dstColorBlendFactor = (VkBlendFactor)k->blend[i].dc,
                                                       .colorBlendOp = (VkBlendOp)k->blend[i].cop,
                                                       .srcAlphaBlendFactor = (VkBlendFactor)k->blend[i].sa,
                                                       .dstAlphaBlendFactor = (VkBlendFactor)k->blend[i].da,
                                                       .alphaBlendOp = (VkBlendOp)k->blend[i].aop,
                                                       .colorWriteMask = k->blend[i].mask};
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
    p->next = g_pipeh[b];
    g_pipeh[b] = p;
    return p->p;
}
