/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* no-GL build used by the headless tests */
#include "gpu.h"
int g_gl = 0;
u64 g_draws;
int gpu_async_submit(u64* e, u32 n, u32 syncpt, u32 incs) {
    (void)e;
    (void)n;
    (void)syncpt;
    (void)incs;
    return 0;
}
void gpu_gl_present_buffer(u64 a, u32 w, u32 h, u32 p, u32 l, u32 b, u32 f) {
    (void)a;
    (void)w;
    (void)h;
    (void)p;
    (void)l;
    (void)b;
    (void)f;
}
void r3d_clear_gl(GpuChannel* ch, u32 v) {
    (void)ch;
    (void)v;
}
void r3d_draw_gl(GpuChannel* ch, int i, u32 t, u32 f, u32 c, u32 bv, u32 bi, u32 in) {
    (void)ch;
    (void)i;
    (void)t;
    (void)f;
    (void)c;
    (void)bv;
    (void)bi;
    (void)in;
}
void r3d_cb_bind_gl(GpuChannel* ch, int s, u32 v) {
    (void)ch;
    (void)s;
    (void)v;
}

void gpu_gl_dump_rts(void) {}
void gpu_gl_rec_finish(void) {}
void gpu_gl_rt_flush(u64 a) { (void)a; }
void gpu_gl_rt_reload(u64 a) { (void)a; }
void gpu_gl_trace(const char* s) { (void)s; }
int r3d_blit_gl(void* s, void* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx, double sdy) {
    (void)s;
    (void)d;
    (void)dx;
    (void)dy;
    (void)dw;
    (void)dh;
    (void)sx;
    (void)sy;
    (void)sdx;
    (void)sdy;
    return 0;
}
void gpu_gl_note_compute(void) {}

void lsfg_ini(const char* k, const char* v) {
    (void)k;
    (void)v;
}
u64 gpu_busy_ns(void) { return 0; }
void gpu_gl_dispatch(void* ch, const void* q, unsigned long long code) {
    (void)ch;
    (void)q;
    (void)code;
}
volatile int g_cs_any;
void cs_mem_need(u64 a, u64 n) {
    (void)a;
    (void)n;
}
void gpu_gl_before_syncwait(void) {}
void gpu_gl_stream_incr(u32 id) {
    extern void syncpt_incr(u32);
    syncpt_incr(id);
}

/* set by vi QueueBuffer, only the GL presenter reads them */
volatile i32 g_vi_crop[4];
volatile u32 g_vi_transform;
int g_texoff_emul;
