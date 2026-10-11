/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* LSFG frame generation (lsfg.c, or lsfg_stub.c where there is none). Textures are GL names (GLuint) */
#ifndef SWR_LSFG_H
#define SWR_LSFG_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern int g_lsfg_mult, g_lsfg_height;
extern float g_lsfg_target;
void lsfg_ini(const char* k, const char* v);
int lsfg_available(void); /* Lossless.dll was found */
int lsfg_init(void);
int lsfg_ready(void);
int lsfg_context(uint32_t W, uint32_t H);
unsigned lsfg_in_tex(void);
void lsfg_reset(void);
int lsfg_process(int ngen, unsigned* outs);
void lsfg_debug_tex(unsigned* prev, unsigned* cur, uint32_t* W, uint32_t* H);

/* SPIR-V to GLSL through SPIRV-Cross (lsfg_spvc.cpp) */
typedef struct {
    int img;
    int smp;
} LsfgUnit; /* original bindings: image 32+, sampler 16+ (-1 = none) */
/* malloc'd GLSL or NULL. units[]: one entry per combined sampler, texture unit = index */
char* lsfg_spv_to_glsl(const unsigned* words, size_t nwords, LsfgUnit* units, int* nunits, int maxunits, char* err,
                       int errn);
#ifdef __cplusplus
}
#endif

#endif
