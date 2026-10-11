/* The renderer interface, when the runtime has both renderers: these are the names the GPU front end calls, and
   each calls the active renderer, Vulkan (vkr_, gpu/gl/renderer.c's names stay for history) or OpenGL (glr_). The window
   backend picks one at startup (renderer_wants_vulkan), and falls back to OpenGL when Vulkan can't start. */
#include <string.h>
#include "gpu/gl/lsfg.h"
#include "gpu/gpu.h"
#include "platform/log.h"
#include "gpu/renderer.h"

int g_vkr;
static int g_pref; /* renderer=: 0 auto, 1 vulkan, 2 opengl */

int renderer_ini(const char* k, const char* v) {
    if (strcmp(k, "renderer")) return 0;
    g_pref = !strcmp(v, "vulkan") ? 1 : !strcmp(v, "opengl") ? 2 : 0;
    return 1;
}
int renderer_wants_vulkan(void) {
    if (g_pref) return g_pref == 1;
    /* LSFG frame generation runs on OpenGL only: with the user's Lossless.dll set up, auto keeps it */
    if (lsfg_available()) {
        LOG("renderer: OpenGL, for LSFG frame generation (renderer=vulkan uses Vulkan without it)\n");
        return 0;
    }
    return 1;
}

#define R(name, ...) (g_vkr ? vkr_##name(__VA_ARGS__) : glr_##name(__VA_ARGS__))

void gpu_gl_start(void) { glr_start(); } /* OpenGL only: Vulkan starts through vkr_start */
int gpu_async_submit(u64* e, u32 n, u32 syncpt, u32 incs) { return R(async_submit, e, n, syncpt, incs); }
void r3d_clear_gl(GpuChannel* ch, u32 v) { R(clear, ch, v); }
void r3d_draw_gl(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 bv, u32 bi, u32 instances) {
    R(draw, ch, indexed, topology, first, count, bv, bi, instances);
}
void r3d_cb_bind_gl(GpuChannel* ch, int stage, u32 v) { R(cb_bind, ch, stage, v); }
int r3d_blit_gl(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                double sdy) {
    return R(blit, s, d, dx, dy, dw, dh, sx, sy, sdx, sdy);
}
void gpu_gl_rt_flush(u64 gva) { R(rt_flush, gva); }
void gpu_gl_rt_reload(u64 gva) { R(rt_reload, gva); }
void gpu_gl_dispatch(GpuChannel* ch, const u32* q, u64 code) { R(dispatch, ch, q, code); }
void cs_mem_need(u64 a, u64 n) { R(cs_mem_need, a, n); }
void gpu_gl_before_syncwait(void) { R(before_syncwait); }
void gpu_gl_stream_incr(u32 id) { R(stream_incr, id); }
void gpu_gl_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    R(present_buffer, cpu_addr, w, h, pitch, layout, bhl, fmt);
}
u64 gpu_gl_screenshot(const char* path) { return R(screenshot, path); }

/* OpenGL only */
void gpu_gl_present_cpu(const u32* px, int w, int h, int stride) {
    if (!g_vkr) glr_present_cpu(px, w, h, stride);
}
void gpu_gl_note_compute(void) {
    if (!g_vkr) glr_note_compute();
}
void gpu_gl_dump_rts(void) {
    if (!g_vkr) glr_dump_rts();
}
void gpu_gl_rec_finish(void) {
    if (!g_vkr) glr_rec_finish();
}
void gpu_gl_trace(const char* s) {
    if (!g_vkr) glr_trace(s);
}
u64 gpu_busy_ns(void) { return g_vkr ? 0 : glr_busy_ns(); }
