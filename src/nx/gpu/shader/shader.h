/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Maxwell shader metadata and direct SPIR-V translation. */
#ifndef SNX_SHADER_H
#define SNX_SHADER_H
#include "cpu/aarch64/cpu.h"
#ifdef __cplusplus
extern "C" {
#endif
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
    u8 flags;    /* 1: raw integer surface load, 2: multisample texel fetch, 4: indexed (SNX_TEX_INDEXED) */
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
    u32 clip_enable; /* key: the clip distances the 3D engine enables (SetUserClipEnable), others aren't written,
                        and SNX_SH_NOT_LAST */
    int uses_position_in, uses_clip_mask, writes_layer, writes_point_size;
    int fp32_flush, fp32_preserve; /* FP32 arithmetic whose FTZ/FMZ field flushes denormals, and whose doesn't */
    u32 gbuf_cbuf_off[16];
    int ngbuf;       /* global buffers found (their cbuf0 offset) */
    int unsupported; /* count of instructions we couldn't translate */
    char first_unsupported[64];
    u32 cs_local[3];
    int shared_size;
    u32 gbuf_wmask; /* global buffers written by compute or graphics shaders */
    struct {
        u32 cbuf_off;
        u8 type, bindless, bl_cbuf, kind; /* 0 float store, 1 unsigned atomic, 2 unsigned store */
    } img[8];
    int nimg; /* storage image stores and atomics */
    /* FS key: integer textures / outputs. rt_int_mask keys the primitives of the other stages instead: the
       tessellation parameters (register 0xC8) for TES, and the input primitive's vertex count for GS */
    u32 tex_int_mask, tex_sint_mask, rt_int_mask, rt_sint_mask;
} ShaderInfo;
/* the shader flushes FP32 denormals everywhere: every FADD, FMUL and FFMA it has asks for it, as the console's
   compiler emits them (Eden declares DenormFlushToZero on the same condition) */
/* One descriptor set per stage, with bindings grouped by resource kind. */
#define SNX_VKB_CB       0  /* 18 constant buffers */
#define SNX_VKB_SSBO     18 /* 16 global memory buffers */
#define SNX_VKB_TEX      34 /* 32 combined image samplers */
#define SNX_VKB_IMG      66 /* 8 storage images (compute) */
#define SNX_VKB_RS       74 /* fragment: 1/resolution scale (SNX_RS) */
#define SNX_VKB_FMT      75 /* compute: image format classes (SNX_FMT) */
#define SNX_VKB_SMAP     76 /* graphics: storage-buffer page indices for aliased bindings */
#define SNX_VKB_IMGBUF   77 /* 8 storage texel buffers: surface operations on buffer images (compute) */
#define SNX_VKB_TEXARRAY 85 /* SNX_TEX_ARRAY textures: the handles an indexed LDC loads (graphics) */
#define SNX_VKB_END      86 /* one past the last binding */
/* An indexed texture's handle is loaded by an LDC whose address takes a register: the shader picks one of
   SNX_TEX_ARRAY handles, each (1 << shift) bytes after the one before, from cbuf_off on. ShTex.flags holds the shift
   in bits 4-7. */
#define SNX_TEX_ARRAY    256
#define SNX_TEX_INDEXED  4
/* set by the renderer: the device indexes sampled image arrays per invocation (Vulkan's non-uniform indexing) */
extern int g_shader_texture_arrays;
/* set by the renderer: tessellation and geometry shaders may write the point size
   (shaderTessellationAndGeometryPointSize) */
extern int g_shader_stage_point_size;
/* set by the renderer: vertex and tessellation programs may write the layer (shaderOutputLayer, the ShaderLayer
   capability). Without it their layer writes are dropped (layer 0); 1 for the offline translator */
extern int g_shader_output_layer;
/* set by the renderer: the device has geometryShader, which the Geometry capability (a fragment program reading the
   layer) needs; 1 for the offline translator */
extern int g_shader_geometry;
/* set by the renderer: the device has shaderFloat16, which fp16 texture results need;
   1 for the offline translator */
extern int g_shader_float16;
/* in clip_enable: a later stage before rasterization follows this one (tessellation or geometry), and the last one
   applies the depth mode and the clip distances */
#define SNX_SH_NOT_LAST (1u << 31)
int shader_vk_set(int stage);
/* Direct SPIR-V translator. Returns NULL when the current opcode set cannot represent the program. */
u32* shader_to_spirv(const u8* code, u32 max_bytes, int stage, ShaderInfo* si, size_t* word_count, int can_flush,
                     int can_preserve, int can_subgroup);
u32* shader_compute_to_spirv(const u8* code, u32 max_bytes, ShaderInfo* si, size_t* word_count, int can_flush,
                             int can_preserve);
extern int (*g_shader_cb_read)(u32 idx, u32 off, u32* out);
int shader_brx_targets(const u64* words, u32 index, u32 count, u32* targets, int max_targets);
u64 shader_hash(const u8* code, u32 max_bytes, u32* out_len);
/* shader corpus (SNX_SHADER_CORPUS=dir): each translated shader's code and its ShaderInfo inputs, so
   tools/snx_shaderc.c can translate them again offline. The files only load into a build with the same ShaderInfo */
int shader_corpus_save(const char* dir, const u8* code, u32 len, u32 max_bytes, int stage, const ShaderInfo* si,
                       u64 key);
/* malloc'd code (capture's max_bytes long, zero past its recorded length); returns that recorded length */
u8* shader_corpus_load(const char* path, u32* max_bytes, int* stage, ShaderInfo* si);
/* VertexA+VertexB -> one program (A then B) */
int shader_merge_dual(const u8* a, const u8* b, u8* out, u32 max_bytes);
#ifdef __cplusplus
}
#endif
#endif
