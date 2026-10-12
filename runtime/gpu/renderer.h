/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The renderer interface: what the GPU front end (gpu.c, engines.c, maxwell3d.c) and the window backends ask of a
   renderer. The Vulkan (gpu/vk/renderer.c) and null (gpu/null/null.c) renderers each fill in every operation, with a
   no-op where they have nothing to do, so callers never check. save is the one exception (NULL when there is
   nothing to save). */
#ifndef SNX_RENDERER_H
#define SNX_RENDERER_H
#include "abi/cpu.h"
#include "gpu/gpu.h"

typedef struct Renderer {
    const char* name;
    int (*start)(void); /* 0 when it can't run here */
    /* called first for every GPFIFO submission, on the submitting thread before gpu_submit queues it. 1 would mean the
       renderer took it, skipping the GPU thread with its order and fence wait; both renderers return 0 (Vulkan only
       notes that guest memory may have changed) */
    int (*async_submit)(u64* e, u32 n, u32 syncpt, u32 incs);
    /* the 3D engine (maxwell3d.c) */
    void (*clear)(GpuChannel* ch, u32 v);
    void (*draw)(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
                 u32 instances);
    void (*cb_bind)(GpuChannel* ch, int stage, u32 v);
    /* the 2D engine: 1 when the renderer did the blit on the GPU, 0 to do it in guest memory */
    int (*blit)(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                double sdy);
    void (*rt_flush)(u64 gva);  /* a render target at gva is about to be read from guest memory: write it back */
    void (*rt_reload)(u64 gva); /* guest memory under a render target changed: load it again */
    void (*dispatch)(GpuChannel* ch, const u32* q, u64 code); /* a compute launch (q: its QMD) */
    /* gmem_read and gmem_ptr hit memory a compute dispatch wrote that hasn't landed yet: land it now */
    void (*cs_mem_need)(u64 a, u64 n);
    void (*before_syncwait)(void); /* the stream is about to wait on a syncpoint: flush everything in flight */
    void (*stream_incr)(u32 id);   /* a syncpoint increment in the stream, after everything before it is done */
    /* presenting a guest framebuffer */
    /* crop: the part shown (left, top, right, bottom), all 0 for the whole buffer */
    void (*present_buffer)(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4]);
    u64 (*screenshot)(const char* path); /* the hash of the frame it saved */
    /* writes what should outlive the process (the pipeline cache): every 5 s from the frontend loop, and before an exit
       that skips the normal shutdown. Any thread, while the others still run. NULL when the renderer keeps nothing */
    void (*save)(void);
} Renderer;

extern const Renderer vk_renderer, null_renderer;
/* the active renderer: Vulkan, or null in the headless build (defined next to each) */
extern const Renderer* g_renderer;

/* shared state (gpu/renderer.c) */
extern int g_renderer_up; /* the renderer started */
extern u64 g_draws;
extern volatile int g_cs_any; /* compute results are pending somewhere */

#endif
