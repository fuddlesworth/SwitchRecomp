/* The renderer interface: what the GPU front end (gpu.c, engines.c, maxwell3d.c) and the window backends ask of a
   renderer. The OpenGL (gpu/gl/renderer.c), Vulkan (gpu/vk/renderer.c) and null (gpu/null/null.c) renderers each fill
   in every operation, with a no-op where they have nothing to do, so callers never check. */
#ifndef SWR_RENDERER_H
#define SWR_RENDERER_H
#include "abi/cpu.h"
#include "gpu/gpu.h"

typedef struct Renderer {
    const char* name;
    int (*start)(void); /* 0 when it can't run here */
    /* a GPFIFO submission from a channel's thread: 1 when the renderer took it to run on its own thread, 0 when the
       caller runs it now (gpu_submit) */
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
    /* presenting a guest framebuffer, or a finished RGBA image from the CPU */
    void (*present_buffer)(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt);
    void (*present_cpu)(const u32* px, int w, int h, int stride);
    u64 (*screenshot)(const char* path); /* the hash of the frame it saved */
    /* diagnostics, OpenGL only so far */
    void (*note_compute)(void);
    void (*dump_rts)(void);
    void (*rec_finish)(void); /* the F7 recorder finishes its file at exit */
    void (*trace)(const char* s);
    u64 (*busy_ns)(void); /* time the renderer's thread has spent working */
} Renderer;

extern const Renderer gl_renderer, vk_renderer, null_renderer;
/* the active renderer: OpenGL (or null in a headless build) until the window backend starts one */
extern const Renderer* g_renderer;

/* shared state, defined by the OpenGL renderer (or the null one) */
extern int g_gl;   /* a renderer is up */
extern int g_gles; /* the OpenGL renderer runs on GLES */
extern u64 g_draws;
extern volatile int g_cs_any;
extern volatile i32 g_vi_crop[4]; /* crop and transform from vi's QueueBuffer */
extern volatile u32 g_vi_transform;
extern volatile int g_shot8_req;

#ifdef SWR_VULKAN
/* what the settings ask for: renderer=auto (Vulkan unless LSFG frame generation is set up), vulkan or opengl */
int renderer_wants_vulkan(void);
int renderer_ini(const char* k, const char* v); /* 1 if it was a renderer key */
#endif

#endif
