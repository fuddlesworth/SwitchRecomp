/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the renderer of the headless build (tests without a GPU): draws nothing, and finishes each submission at once */
#include "gpu/renderer.h"
#include "hle/nvdrv.h"
#include "gpu/gpu.h"

static int null_start(void) { return 1; }
static int null_async_submit(u64* e, u32 n, u32 syncpt, u32 incs) {
    (void)e;
    (void)n;
    (void)syncpt;
    (void)incs;
    return 0;
}
static void null_clear(GpuChannel* ch, u32 v) {
    (void)ch;
    (void)v;
}
static void null_draw(GpuChannel* ch, int i, u32 t, u32 f, u32 c, u32 bv, u32 bi, u32 in) {
    (void)ch;
    (void)i;
    (void)t;
    (void)f;
    (void)c;
    (void)bv;
    (void)bi;
    (void)in;
}
static void null_cb_bind(GpuChannel* ch, int s, u32 v) {
    (void)ch;
    (void)s;
    (void)v;
}
static int null_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
                     double sdy) {
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
static void null_gva(u64 a) { (void)a; }
static void null_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    (void)ch;
    (void)q;
    (void)code;
}
static void null_cs_mem_need(u64 a, u64 n) {
    (void)a;
    (void)n;
}
static void null_void(void) {}
static void null_present_buffer(u64 a, u32 w, u32 h, u32 p, u32 l, u32 b, u32 f) {
    (void)a;
    (void)w;
    (void)h;
    (void)p;
    (void)l;
    (void)b;
    (void)f;
}
static u64 null_screenshot(const char* path) {
    (void)path;
    return 0;
}

const Renderer null_renderer = {
    .name = "null",
    .start = null_start,
    .async_submit = null_async_submit,
    .clear = null_clear,
    .draw = null_draw,
    .cb_bind = null_cb_bind,
    .blit = null_blit,
    .rt_flush = null_gva,
    .rt_reload = null_gva,
    .dispatch = null_dispatch,
    .cs_mem_need = null_cs_mem_need,
    .before_syncwait = null_void,
    .stream_incr = syncpt_incr,
    .present_buffer = null_present_buffer,
    .screenshot = null_screenshot,
};
const Renderer* g_renderer = &null_renderer;
