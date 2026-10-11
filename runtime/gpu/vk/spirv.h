/* GLSL to SPIR-V for the Vulkan renderer (glslang) */
#ifndef SWR_VK_SHADER_H
#define SWR_VK_SHADER_H
#include <stddef.h>
#include "gpu/shader/shader.h"

/* malloc'd SPIR-V words, or NULL with the compiler's log in err */
u32* vk_glsl_to_spirv(const char* glsl, int stage, size_t* nwords, char* err, size_t errn);
/* the same through an on-disk cache: cache_dir/<hash>.spv, the hash of the GLSL text, the stage and glslang's
   version, so a new compiler compiles again. cache_dir NULL compiles without one */
/* the translator's GLSL patched for the renderer: the alpha test and the depth range through push constants. Takes
   and returns malloc'd text (NULL stays NULL) */
char* vk_glsl_post(char* src, int stage);
u32* vk_spirv_cached(const char* glsl, int stage, const char* cache_dir, size_t* nwords, char* err, size_t errn);

#endif
