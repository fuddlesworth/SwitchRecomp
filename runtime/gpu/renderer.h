/* The renderer interface the GPU front end calls (gpu_gl_*, r3d_*_gl, gpu_async_submit, cs_mem_need) and the state the
   renderers share. With both renderers built in (SWR_VULKAN), gpu/renderer.c defines the interface and calls the active
   renderer's version: glr_ for OpenGL (gpu/gl/renderer.c, its names changed by gpu/gl/names.h) or vkr_ for Vulkan
   (gpu/vk/renderer.c, gpu/vk/compute.c). */
#ifndef SWR_RENDERER_H
#define SWR_RENDERER_H
#include "abi/cpu.h"
#include "gpu/gpu.h"

/* the interface: gpu/renderer.c, or without Vulkan the OpenGL renderer itself (or gpu/null/null.c) */
void gpu_gl_start(void);
int gpu_async_submit(u64* e, u32 n, u32 syncpt, u32 incs);
void r3d_clear_gl(GpuChannel* ch, u32 v);
void r3d_draw_gl(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances);
void r3d_cb_bind_gl(GpuChannel* ch, int stage, u32 v);
int r3d_blit_gl(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                double sdy);
void gpu_gl_rt_flush(u64 gva);
void gpu_gl_rt_reload(u64 gva);
void gpu_gl_dispatch(GpuChannel* ch, const u32* q, u64 code);
void cs_mem_need(u64 a, u64 n);
void gpu_gl_before_syncwait(void);
void gpu_gl_stream_incr(u32 id);
void gpu_gl_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);
u64 gpu_gl_screenshot(const char* path);
void gpu_gl_present_cpu(const u32* px, int w, int h, int stride);
void gpu_gl_note_compute(void);
void gpu_gl_dump_rts(void);
void gpu_gl_rec_finish(void);
void gpu_gl_trace(const char* s);
u64 gpu_busy_ns(void);

/* shared state, defined by the OpenGL renderer (or the null one) */
extern int g_gl;   /* a renderer is up */
extern int g_gles; /* the OpenGL renderer runs on GLES */
extern u64 g_draws;
extern volatile int g_cs_any;
extern volatile i32 g_vi_crop[4]; /* crop and transform from vi's QueueBuffer */
extern volatile u32 g_vi_transform;
extern volatile int g_shot8_req;

#ifdef SWR_VULKAN
extern int g_vkr; /* the Vulkan renderer is the active one */
/* what the settings ask for: renderer=auto (Vulkan unless LSFG frame generation is set up), vulkan or opengl */
int renderer_wants_vulkan(void);
int renderer_ini(const char* k, const char* v); /* 1 if it was a renderer key */

#define RENDERER_FUNCS(P)                                                                                              \
    int P##async_submit(u64* e, u32 n, u32 syncpt, u32 incs);                                                          \
    void P##clear(GpuChannel* ch, u32 v);                                                                              \
    void P##draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances);      \
    void P##cb_bind(GpuChannel* ch, int stage, u32 v);                                                                 \
    int P##blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,    \
                double sdy);                                                                                           \
    void P##rt_flush(u64 gva);                                                                                         \
    void P##rt_reload(u64 gva);                                                                                        \
    void P##dispatch(GpuChannel* ch, const u32* q, u64 code);                                                          \
    void P##cs_mem_need(u64 a, u64 n);                                                                                 \
    void P##before_syncwait(void);                                                                                     \
    void P##stream_incr(u32 id);                                                                                       \
    void P##present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);                       \
    u64 P##screenshot(const char* path);

/* OpenGL: everything; Vulkan returns 0 from start when it can't run, and has no CPU-frame present, frame trace,
   recorder or GPU busy time */
RENDERER_FUNCS(glr_)
void glr_start(void);
void glr_present_cpu(const u32* px, int w, int h, int stride);
void glr_note_compute(void);
void glr_dump_rts(void);
void glr_rec_finish(void);
void glr_trace(const char* s);
u64 glr_busy_ns(void);
RENDERER_FUNCS(vkr_)
int vkr_start(void);
#endif

#endif
