/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Maxwell (GM20B) shader -> GLSL 4.50 translator, for Vulkan (gpu/vk/spirv.c compiles the result to SPIR-V) */
#ifndef SWR_SHADER_H
#define SWR_SHADER_H
#include "abi/cpu.h"
enum { ST_VS = 0, ST_TCS = 1, ST_TES = 2, ST_GS = 3, ST_FS = 4, ST_CS = 5 };
#define SH_MAXTEX 32
typedef struct {
    u32 cbuf_off; /* handle offset in the texture cbuf, in words */
    u8 type;      /* 0 1D 1 2D 2 3D 3 CUBE */
    u8 array, shadow;
    u8 bindless;
    u8 bl_cbuf;
    u8 bl2;
    u8 bl2_cbuf; /* bl2: handle = read1 | read2 (separate tex/sampler handles) */
    u32 bl2_off; /* word offset of read2 */
} ShTex;
typedef struct {
    int stage;
    u32 sph[20];
    u32 cbuf_mask;    /* cbufs used */
    u32 cbuf_max[18]; /* bytes used per cbuf, so we upload less */
    ShTex tex[SH_MAXTEX];
    int ntex;
    u32 in_mask;       /* generic attribs 0..31 read */
    u32 attr_int_mask; /* key: integer VS attribs */
    u32 attr_sint_mask;
    u32 clip_enable; /* key: the clip distances the 3D engine enables (SetUserClipEnable), others aren't written */
    int uses_position_in, uses_clip_mask, writes_layer, writes_point_size;
    u32 gbuf_cbuf_off[16];
    int ngbuf;       /* global buffers found (their cbuf0 offset) */
    int unsupported; /* count of instructions we couldn't translate */
    char first_unsupported[64];
    u32 cs_local[3];
    int shared_size;
    u32 gbuf_wmask; /* gbufs written (compute) */
    struct {
        u32 cbuf_off;
        u8 type, bindless, bl_cbuf, pad;
    } img[8];
    int nimg;                                                   /* SUST targets (compute) */
    u32 tex_int_mask, tex_sint_mask, rt_int_mask, rt_sint_mask; /* FS key: integer textures / outputs */
} ShaderInfo;
/* the GLSL is for Vulkan: one descriptor set per stage (shader_vk_set), bindings in it by kind */
#define SWR_VKB_CB   0  /* 18 constant buffers */
#define SWR_VKB_SSBO 18 /* 16 global memory buffers */
#define SWR_VKB_TEX  34 /* 32 combined image samplers */
#define SWR_VKB_IMG  66 /* 8 storage images (compute) */
#define SWR_VKB_RS   74 /* fragment: 1/resolution scale (SWR_RS) */
#define SWR_VKB_FMT  75 /* compute: image format classes (SWR_FMT) */
int shader_vk_set(int stage);
/* malloc'd GLSL, or NULL */
char* shader_to_glsl(const u8* code, u32 max_bytes, int stage, ShaderInfo* si);
extern int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len);
/* shader corpus (SWR_SHADER_CORPUS=dir): each translated shader's code and its ShaderInfo inputs, so
   tools/swr_shaderc.c can translate them again offline. The files only load into a build with the same ShaderInfo */
int shader_corpus_save(const char* dir, const u8* code, u32 len, u32 max_bytes, int stage, const ShaderInfo* si,
                       u64 key);
/* malloc'd code (max_bytes long, zero past len), or NULL */
u8* shader_corpus_load(const char* path, u32* max_bytes, int* stage, ShaderInfo* si);
/* VertexA+VertexB -> one program (A then B) */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes);
#endif
