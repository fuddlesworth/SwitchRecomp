/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GPU state shared between the engines and the backends */
#ifndef SWR_GPU_H
#define SWR_GPU_H
#include "abi/cpu.h"
typedef struct GpuChannel {
    u32 id;
    u32 sub_class[8];
    u32 puller[0x40];
    u32 r3d[0x1000]; /* 3D engine regs, 0xE00+ are MME macro calls */
    u32 rcomp[0x1000];
    u32 r2d[0x400];
    u32 rdma[0x200];
    u32 ri2m[0x80];
    /* inline to memory */
    u8* i2m_buf;
    u32 i2m_buf_cap, i2m_len, i2m_pos, i2m_w, i2m_lines, i2m_pitch, i2m_bh;
    u64 i2m_addr;
    int i2m_linear;
    u32 i2m_bd, i2m_dw, i2m_dh, i2m_x, i2m_y, i2m_z;
    /* macros (MME) */
    u32 pb_mode, pb_sub, pb_m, pb_cnt, pb_k; /* a method batch can continue into the next GPFIFO entry */
    u32 mme_code[0x2000];
    u32 mme_start[0x100];
    u32 mme_upload_ptr, mme_start_ptr;
    u32 mme_params[0x10000];
    u32 mme_nparams;
    int mme_active;
    u32 mme_index;
    u32 mme_shadow[0x1000];
} GpuChannel;
typedef struct {
    GpuChannel* chan[64];
    int nchan;
} GpuState;
extern GpuState g_gpu;
/* a 2D engine surface, the source or destination of a blit */
typedef struct {
    u32 format, linear, bh, depth, layer, pitch, width, height;
    u64 addr;
} Surf2D;
int gmem_read(u64 gva, void* dst, u64 size);
int gmem_write(u64 gva, const void* src, u64 size);
u8* gmem_ptr(u64 gva, u64 size);
u32 gmem_r32(u64 gva);
void gmem_w32(u64 gva, u32 v);
void gmem_w64(u64 gva, u64 v);
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);
void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2);
void swizzle(u8* dst, const u8* src, u32 src_pitch, u32 w_bytes, u32 h, u32 bh_log2);
void i2m_launch(GpuChannel* ch, u32* regs, int base);
void i2m_data(GpuChannel* ch, u32 v);
void maxwell3d_method(GpuChannel* ch, u32 m, u32 v, int last);
void compute_method(GpuChannel* ch, u32 m, u32 v, int last);
void kepler_i2m_method(GpuChannel* ch, u32 m, u32 v);
void fermi2d_method(GpuChannel* ch, u32 m, u32 v);
void dma_method(GpuChannel* ch, u32 m, u32 v);
void gpu_flush(GpuChannel* ch);
void gpu_mem_written(u64 gva, u64 size);
void gpu_signal_point(void); /* the GPU is about to do something the game can observe */
void gpu3d_init(void);

/* gpu.c: GPFIFO submission */
void gpu_init(void);
void gpu_submit(u64* entries, u32 n, u32 channel, u32 incs);
void gpu_submit_now(u64* e, u32 n, u32 channel);
extern volatile u64 g_gpu_submits;
extern void (*g_ww_touch)(u64 cpu); /* GPU wrote guest memory, record it for write tracking */
/* the renderer lands work it is holding (compute results) before the stream goes on past it: before a write of
   memory those results overlap (size > 0), and before anything the game can observe (gpu_signal_point, size 0) */
extern void (*g_gpu_before_write)(u64 gva, u64 size);

/* engines.c */
u32 fmt_bpp_2d(u32 f); /* bytes per pixel of a 2D engine format */
u64 gpu_ticks(void);
void r3d_pending_flush(void); /* instanced draws are merged into one while they come in, this sends it now */
int r3d_pending_on(void);
extern u64 g_draws_merged;
extern volatile u64 g_compute_launches, g_dma_copies, g_gpu_sub_ns;

/* maxwell3d.c: the 3D engine's side of the renderer */
void r3d_clear(GpuChannel* ch, u32 v);
void r3d_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances);
void r3d_cb_bind(GpuChannel* ch, int stage, u32 v);
void r3d_const_upload(GpuChannel* ch, u32 v);
int r3d_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy);
void r3d_flush(GpuChannel* ch);
extern u64 g_gw_gen; /* the GPU-side write log: entries so far, a generation to compare against */
int gpu_mem_dirty_since(u64 since, u64 a, u64 sz);
#endif
