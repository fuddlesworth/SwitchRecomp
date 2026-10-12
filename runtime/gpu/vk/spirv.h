/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the SwitchRecomp fork contributors */
/* GLSL to SPIR-V for the Vulkan renderer (glslang) */
#ifndef SWR_VK_SPIRV_H
#define SWR_VK_SPIRV_H
#include <stddef.h>
#include "gpu/shader/shader.h"

/* the translator's GLSL patched for the renderer: the alpha test and the depth range through push constants. Takes
   and returns malloc'd text (NULL stays NULL) */
char* vk_glsl_post(char* src, int stage);
/* malloc'd SPIR-V words, or NULL with the compiler's log in err, through an on-disk cache: cache_dir/<hash>.spv, the
   hash of the GLSL text, the stage, glslang's version and the compile target, so a new compiler compiles again.
   cache_dir NULL compiles without one */
u32* vk_spirv_cached(const char* glsl, int stage, const char* cache_dir, size_t* nwords, char* err, size_t errn);

#endif
