/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* SPIR-V -> GLSL for the LSFG shaders via SPIRV-Cross (Apache 2.0, https://github.com/KhronosGroup/SPIRV-Cross).
   The shaders themselves aren't shipped, they come out of the user's own Lossless.dll at runtime. */
#include "spirv_glsl.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {
typedef struct {
    int img;
    int smp;
} LsfgUnit; /* original bindings: image 32+, sampler 16+ (-1 = none) */

/* malloc'd GLSL or NULL. units[]: one entry per combined sampler, texture unit = index */
char* lsfg_spv_to_glsl(const unsigned* words, size_t nwords, LsfgUnit* units, int* nunits, int maxunits, char* err,
                       int errn) {
    try {
        spirv_cross::CompilerGLSL glsl(words, nwords);
        spirv_cross::CompilerGLSL::Options o;
        o.version = 430;
        o.es = false;
        o.vulkan_semantics = false;
        glsl.set_common_options(o);
        uint32_t dummy = glsl.build_dummy_sampler_for_combined_images();
        glsl.build_combined_image_samplers();
        int k = 0;
        for (auto& c : glsl.get_combined_image_samplers()) {
            if (k >= maxunits) {
                snprintf(err, errn, "too many textures");
                return NULL;
            }
            units[k].img = (int)glsl.get_decoration(c.image_id, spv::DecorationBinding);
            units[k].smp =
                (dummy && c.sampler_id == dummy) || !glsl.has_decoration(c.sampler_id, spv::DecorationBinding)
                    ? -1
                    : (int)glsl.get_decoration(c.sampler_id, spv::DecorationBinding);
            glsl.set_decoration(c.combined_id, spv::DecorationBinding, (uint32_t)k);
            k++;
        }
        *nunits = k;
        spirv_cross::ShaderResources res = glsl.get_shader_resources();
        for (auto& r : res.storage_images) {
            uint32_t b = glsl.get_decoration(r.id, spv::DecorationBinding);
            glsl.set_decoration(r.id, spv::DecorationBinding, b >= 48 ? b - 48 : b);
        }
        for (auto& r : res.uniform_buffers) glsl.set_decoration(r.id, spv::DecorationBinding, 0);
        std::string s = glsl.compile();
        char* out = (char*)malloc(s.size() + 1);
        memcpy(out, s.c_str(), s.size() + 1);
        return out;
    } catch (const std::exception& e) {
        snprintf(err, errn, "%s", e.what());
        return NULL;
    } catch (...) {
        snprintf(err, errn, "unknown error");
        return NULL;
    }
}
}
