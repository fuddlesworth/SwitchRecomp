/* GLSL (from the Maxwell translator in Vulkan mode) to SPIR-V, with glslang. Shared by the Vulkan renderer and
   tools/swr_shaderc.c, so it depends on nothing else of the runtime. */
#include "gpu/shader/shader.h"
#include "gpu/vk/spirv.h"
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_glslang_ready;

static glslang_stage_t glslang_stage(int stage) {
    switch (stage) {
        case ST_VS: return GLSLANG_STAGE_VERTEX;
        case ST_TCS: return GLSLANG_STAGE_TESSCONTROL;
        case ST_TES: return GLSLANG_STAGE_TESSEVALUATION;
        case ST_GS: return GLSLANG_STAGE_GEOMETRY;
        case ST_FS: return GLSLANG_STAGE_FRAGMENT;
        default: return GLSLANG_STAGE_COMPUTE;
    }
}

u32* vk_glsl_to_spirv(const char* glsl, int stage, size_t* nwords, char* err, size_t errn) {
    if (!g_glslang_ready) {
        glslang_initialize_process();
        g_glslang_ready = 1;
    }
    if (err && errn) err[0] = 0;
    glslang_input_t in = {.language = GLSLANG_SOURCE_GLSL,
                          .stage = glslang_stage(stage),
                          .client = GLSLANG_CLIENT_VULKAN,
                          .client_version = GLSLANG_TARGET_VULKAN_1_3,
                          .target_language = GLSLANG_TARGET_SPV,
                          .target_language_version = GLSLANG_TARGET_SPV_1_6,
                          .code = glsl,
                          .default_version = 450,
                          .default_profile = GLSLANG_NO_PROFILE,
                          .messages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT,
                          .resource = glslang_default_resource()};
    u32* words = NULL;
    glslang_shader_t* sh = glslang_shader_create(&in);
    glslang_program_t* pr = NULL;
    const char* what = NULL;
    if (!glslang_shader_preprocess(sh, &in))
        what = "preprocess";
    else if (!glslang_shader_parse(sh, &in))
        what = "parse";
    else {
        pr = glslang_program_create();
        glslang_program_add_shader(pr, sh);
        if (!glslang_program_link(pr, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT))
            what = "link";
        else {
            glslang_program_SPIRV_generate(pr, in.stage);
            size_t n = glslang_program_SPIRV_get_size(pr);
            words = malloc(n * 4);
            glslang_program_SPIRV_get(pr, words);
            *nwords = n;
        }
    }
    if (what && err && errn) {
        const char* log = pr ? glslang_program_get_info_log(pr) : glslang_shader_get_info_log(sh);
        snprintf(err, errn, "%s: %s", what, log ? log : "");
    }
    if (pr) glslang_program_delete(pr);
    glslang_shader_delete(sh);
    return words;
}

static u64 fnv64(u64 h, const void* p, size_t n) {
    const u8* b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
u32* vk_spirv_cached(const char* glsl, int stage, const char* cache_dir, size_t* nwords, char* err, size_t errn) {
    if (!cache_dir) return vk_glsl_to_spirv(glsl, stage, nwords, err, errn);
    glslang_version_t v;
    glslang_get_version(&v);
    u64 h = fnv64(1469598103934665603ull, glsl, strlen(glsl));
    h = fnv64(h, &stage, sizeof stage);
    int ver[3] = {v.major, v.minor, v.patch}; /* not the struct: its flavor is a pointer */
    h = fnv64(h, ver, sizeof ver);
    char path[1200];
    snprintf(path, sizeof path, "%s/%016llx.spv", cache_dir, (unsigned long long)h);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u32* w = n >= 20 && !(n & 3) ? malloc((size_t)n) : NULL;
        int ok = w && fread(w, 1, (size_t)n, f) == (size_t)n && w[0] == 0x07230203u; /* SPIR-V magic */
        fclose(f);
        if (ok) {
            *nwords = (size_t)n / 4;
            return w;
        }
        free(w);
    }
    u32* w = vk_glsl_to_spirv(glsl, stage, nwords, err, errn);
    if (w) {
        /* written under a temporary name and renamed, so a reader never sees half a file */
        char tmp[1300];
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(w, 4, *nwords, f) == *nwords;
            fclose(f);
            if (ok)
                rename(tmp, path);
            else
                remove(tmp);
        }
    }
    return w;
}

/* gpu/gl/renderer.c's sh_post for Vulkan: the alpha test discards before every return of a fragment shader, and a vertex
   shader for a guest in -1..1 depth mode moves z to Vulkan's 0..1 (the viewport handles y, see gpu/vk/render.c) */
char* vk_glsl_post(char* src, int stage) {
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
