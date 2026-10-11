/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GL 4.3 renderer for the Maxwell command stream. Runs on its own thread like the real GPU: the game
   submits pushbuffers, we execute them and signal syncpoints. Textures, buffers and shaders are
   translated once and cached.

   This file has the work queue, the GPU thread, presenting, resolution scaling and the renderer interface
   (gl_renderer). The rest is split by job: formats.c, texture.c, programs.c, framebuffer.c, draw.c, compute.c,
   trace.c, capture.c and framegen.c, sharing gl.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "core/loader.h"
#include "core/main.h"
#include "core/options.h"
#include "gpu/gl/lsfg.h"
#include "gpu/renderer.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "gpu/gpu.h"
#include <time.h>
#include <math.h>
#include "gpu/gl/glfn.h"
#include "gpu/shader/shader.h"

int g_gl = 0; /* active renderer */
static int g_gl_ready;
#include "gpu/gl/glq.h"
#include "gpu/gl/gl.h"
#include "core/clock.h"
u64 g_draws;
__thread int t_gpu_thread; /* GL context owner */
int g_diag_draws;
u32 g_submit_epoch; /* the buffer cache's entries are validated once per submit */
static u64 g_gpu_busy;
u64 st_tex_ok, st_tex_null, st_tex_mismatch, st_tex_rt;
static u64 glr_busy_ns(void) { return __atomic_load_n(&g_gpu_busy, __ATOMIC_RELAXED); }
u64 st_compiling, st_cb_skip, st_cb_bytes, st_vb_hashed, st_vb_up, st_tex_up;
u64 st_noshader, st_shfail, st_noprog, st_nort, st_drawn, st_clears, st_pres_rt, st_pres_cpu, st_shaders;
float g_scale = 1.0f; /* internal res scale, 0.25..4 */
u32 SC(u32 v) {
    u32 r = (u32)(v * g_scale + 0.5f);
    return r ? r : 1;
}
u32 g_last_cw, g_last_ch; /* last color target size, used to spot shadow passes */
float fabsf_(float x) { return x < 0 ? -x : x; }

/* ============================================================ work queue */
enum { WK_SUBMIT, WK_PRESENT_CPU, WK_PRESENT_ADDR, WK_SHOT, WK_SYNC, WK_DUMP };
typedef struct {
    int kind;
    u64* ent;
    u32 n, syncpt, incs;
    u32* px;
    int w, h;
    u64 addr;
    u32 fmt;
    PCond* done_cv;
    volatile int* done;
    char path[256];
    u64 result;
    u64 sub_ns;
} Work;
#define QN 1024
static Work q[QN];
static int qh, qt;
static PMutex g_ql;
static PCond g_qcv, g_qspace;
static void q_push(Work* w) {
    pm_lock(&g_ql);
    while (((qh + 1) % QN) == qt) pc_wait(&g_qspace, &g_ql, 1000000);
    q[qh] = *w;
    qh = (qh + 1) % QN;
    pc_signal(&g_qcv);
    pm_unlock(&g_ql);
}
static int q_pop(Work* w, i64 timeout) {
    pm_lock(&g_ql);
    if (qh == qt) pc_wait(&g_qcv, &g_ql, timeout);
    if (qh == qt) {
        pm_unlock(&g_ql);
        return 0;
    }
    *w = q[qt];
    qt = (qt + 1) % QN;
    pc_signal(&g_qspace);
    pm_unlock(&g_ql);
    return 1;
}

/* ============================================================ GL helpers */
GLuint compile(GLenum type, const char* src, char* err, int errn) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (err) glGetShaderInfoLog(s, errn, NULL, err);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
GLuint link2(GLuint a, GLuint b, GLuint c3) {
    GLuint p = glCreateProgram();
    if (a) glAttachShader(p, a);
    if (b) glAttachShader(p, b);
    if (c3) glAttachShader(p, c3);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, 2048, NULL, log);
        WARN("GL link: %s\n", log);
        return 0;
    }
    return p;
}

/* ============================================================ present */
GLuint g_blit_prog, g_cpu_tex;
int g_cpu_w, g_cpu_h;
GLuint g_last_present_tex;
int g_last_flip, g_last_w, g_last_h;
int g_last_is_rt;
static const char* BLIT_VS_ES =
    "#version 310 es\nprecision highp float;\nout vec2 uv; uniform vec4 xf; void main(){ vec2 p=vec2(float(gl_VertexID&1)*2.0-1.0,float(gl_VertexID>>1)*2.0-1.0); uv=(p*0.5+0.5)*xf.zw+xf.xy; gl_Position=vec4(p,0.0,1.0); }";
static const char* BLIT_FS_ES =
    "#version 310 es\nprecision highp float;\nin vec2 uv; out vec4 o; layout(binding=0) uniform highp sampler2D t; void main(){ o=vec4(texture(t,uv).rgb,1.0); }";
static const char* BLIT_VS =
    "#version 430 core\nout vec2 uv; uniform vec4 xf; void main(){ vec2 p=vec2((gl_VertexID&1)*2.0-1.0,(gl_VertexID>>1)*2.0-1.0); uv=(p*0.5+0.5)*xf.zw+xf.xy; gl_Position=vec4(p,0.0,1.0); }";
static const char* BLIT_FS =
    "#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; void main(){ o=vec4(texture(t,uv).rgb,1.0); }";
volatile i32 g_vi_crop[4];
volatile u32 g_vi_transform; /* crop/transform from QueueBuffer */
static void rs_update(void) {
    if (!g_rs_ubo) {
        GLint mx = 84;
        glGetIntegerv(GL_MAX_UNIFORM_BUFFER_BINDINGS, &mx);
        if (mx < 2) return;
        g_rs_binding = mx - 1;
        glGenBuffers(1, &g_rs_ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, g_rs_ubo);
        glBufferData(GL_UNIFORM_BUFFER, 16, NULL, GL_DYNAMIC_DRAW);
    }
    float v[4] = {1.0f / g_scale, 1.0f / g_scale, g_scale, 0};
    glBindBuffer(GL_UNIFORM_BUFFER, g_rs_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, 16, v);
    glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)g_rs_binding, g_rs_ubo);
}
static void rt_rescale(float ns) {
    float old = g_scale;
    if (ns == old) return;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    glDepthMask(GL_TRUE);
    glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
    int n = 0;
    for (int k = 0; k < TEXH; k++)
        for (Tex* t = texh[k]; t; t = t->next) {
            if (!t->is_rt || t->gva == ~0ull) continue;
            u32 ow = (u32)(t->w * old + 0.5f), oh = (u32)(t->h * old + 0.5f), nw = (u32)(t->w * ns + 0.5f),
                nh = (u32)(t->h * ns + 0.5f);
            if (!ow) ow = 1;
            if (!oh) oh = 1;
            if (!nw) nw = 1;
            if (!nh) nh = 1;
            GLuint nt = 0;
            glGenTextures(1, &nt);
            glBindTexture(GL_TEXTURE_2D, nt);
            glTexStorage2D(GL_TEXTURE_2D, 1, t->f.ifmt, nw, nh);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            int ds = t->f.depth && t->f.fmt == GL_DEPTH_STENCIL;
            GLenum att = t->f.depth ? (ds ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT) : GL_COLOR_ATTACHMENT0;
            GLbitfield mask =
                t->f.depth ? (GL_DEPTH_BUFFER_BIT | (ds ? GL_STENCIL_BUFFER_BIT : 0)) : GL_COLOR_BUFFER_BIT;
            GLenum none = GL_NONE, c0 = GL_COLOR_ATTACHMENT0;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, t->id, 0);
            glReadBuffer(t->f.depth ? GL_NONE : GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, nt, 0);
            glDrawBuffers(1, t->f.depth ? &none : &c0);
            glBlitFramebuffer(0, 0, (GLint)ow, (GLint)oh, 0, 0, (GLint)nw, (GLint)nh, mask,
                              (t->f.depth || t->f.is_int) ? GL_NEAREST : GL_LINEAR);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, GL_TEXTURE_2D, 0, 0);
            if (g_last_present_tex == t->id) g_last_present_tex = nt;
            glDeleteTextures(1, &t->id);
            t->id = nt;
            t->wgen++;
            if (t->crop) {
                glDeleteTextures(1, &t->crop->id);
                if (t->crop->view) glDeleteTextures(1, &t->crop->view);
                free(t->crop);
                t->crop = NULL;
            }
            if (t->view) {
                glDeleteTextures(1, &t->view);
                t->view = 0;
            }
            n++;
        }
    {
        GLenum c0 = GL_COLOR_ATTACHMENT0;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
        glDrawBuffers(1, &c0);
    }
    while (glGetError() != GL_NO_ERROR) {}
    g_scale = ns;
    rs_update();
    g_fb_sig_ok = 0;
    g_tmap_gen++;
    mb_reset();
    state_dirty();
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    LOG("gpu: internal resolution %.0f%% -> %.0f%% (%d render targets)\n", old * 100, ns * 100, n);
    if (g_rf) {
        fprintf(g_rf, "   internal resolution %.0f%% -> %.0f%%\n", old * 100, ns * 100);
        fflush(g_rf);
    }
}
static u64 rd_t0, rd_f0, rd_busy0, rd_cpu0, rd_swap;
static int rd_hold, rd_up;
static u64 rd_lastdown;
static void res_dyn_step(void) {
    if (!g_opt_resdyn || g_gles || !p_cis) return;
    u64 now = plat_ns();
    if (!rd_t0) {
        rd_t0 = now;
        rd_f0 = g_frame;
        rd_busy0 = g_gpu_busy;
        rd_cpu0 = plat_thread_cpu_ns();
        rd_swap = 0;
        return;
    }
    u64 dt = now - rd_t0;
    if (dt < 1000000000ull) return;
    double fps = (double)(g_frame - rd_f0) * 1e9 / dt, swapw = (double)rd_swap / dt,
           busy = (double)(g_gpu_busy - rd_busy0) / dt;
    u64 c = plat_thread_cpu_ns();
    double cpu = (double)(c - rd_cpu0) / dt;
    rd_t0 = now;
    rd_f0 = g_frame;
    rd_busy0 = g_gpu_busy;
    rd_cpu0 = c;
    rd_swap = 0;
    {
        static int tst = -1;
        if (tst < 0) tst = getenv("SWR_RES_TEST") != NULL;
        if (tst) {
            rt_rescale(g_scale > 0.95f ? 0.7f : 1.0f);
            return;
        }
    } /* test: flip the scale back and forth */
    { /* compiling shaders / loading: those hitches aren't GPU load, don't drop res */
        static u64 last_new;
        u32 pend = __atomic_load_n(&wq_hi.t, __ATOMIC_RELAXED) - __atomic_load_n(&wq_hi.h, __ATOMIC_RELAXED);
        if (pend > 0 || st_sh_new != last_new) {
            last_new = st_sh_new;
            rd_hold = 2;
            return;
        }
    }
    if (rd_hold > 0) {
        rd_hold--;
        return;
    }
    float mx = g_opt_resf, mn = g_opt_resmin;
    if (mn > mx) mn = mx;
    /* GPU-bound if the GPU thread spends its time in swap or busy without using CPU */
    int gpu_bound = swapw > 0.20 || (busy > 0.90 && cpu < 0.55 * busy);
    int headroom = swapw < 0.04 && busy < 0.65;
    if (fps < 28.5 && gpu_bound && g_scale > mn + 0.01f) {
        float ns = g_scale - 0.1f;
        if (ns < mn) ns = mn;
        rt_rescale(ns);
        rd_hold = 2;
        rd_up = 0;
        rd_lastdown = now;
    } else if (headroom && g_scale < mx - 0.01f && now - rd_lastdown > 20000000000ull) {
        if (++rd_up >= 4) {
            float ns = g_scale + 0.1f;
            if (ns > mx) ns = mx;
            rt_rescale(ns);
            rd_hold = 3;
            rd_up = 0;
        }
    } else
        rd_up = 0;
}
/* end of frame: was it slow, and where did the time go */
static void frame_stats(void) {
    u64 now = plat_ns();
    if (!g_fr_t0) {
        g_fr_t0 = now;
        memset(g_tm, 0, sizeof g_tm);
        g_fr_shnew = st_sh_new;
        return;
    }
    u64 dt = now - g_fr_t0;
    g_fr_t0 = now;
    if (dt > g_fr_max) g_fr_max = dt;
    if (dt > 120000000ull && g_rf && g_fr_slow < 300) {
        g_fr_slow++;
        u32 pend = __atomic_load_n(&wq_hi.t, __ATOMIC_RELAXED) - __atomic_load_n(&wq_hi.h, __ATOMIC_RELAXED);
        fprintf(
            g_rf,
            "   slow frame %.0f ms (GPU thread: shaders %.0f+%.0f, state %.0f, textures %.0f, buffers %.0f, driver %.0f, present %.0f ms) new shaders %llu, queued %u\n",
            dt / 1e6, g_tm[TM_SH] / 1e6, g_tm[TM_SHREADY] / 1e6, g_tm[TM_STATE] / 1e6, g_tm[TM_TEX] / 1e6,
            (g_tm[TM_BUF] + g_tm[TM_CB] + g_tm[TM_GB]) / 1e6, g_tm[TM_DRAW] / 1e6, g_tm[TM_SWAP] / 1e6,
            (unsigned long long)(st_sh_new - g_fr_shnew), pend);
    }
    rd_swap += g_tm[TM_SWAP];
    for (int k = 0; k < TM_N; k++) {
        g_tmp[k] += g_tm[k];
        g_tm[k] = 0;
    }
    g_fr_shnew = st_sh_new;
    { /* resolution edited in the ini while running: apply now */
        if (!g_opt_resdyn && !g_gles && p_cis && (g_scale > g_opt_resf + 0.01f || g_scale < g_opt_resf - 0.01f))
            rt_rescale(g_opt_resf);
    }
    res_dyn_step();
}
static void gpu_thread(void* arg) {
    t_gpu_thread = 1;
    (void)arg;
    { plat_thread_high_priority(); }
    {
        int qs = glq_start(); /* 1: GL lives on its own thread (context + fn pointers made there), 0: this thread */
        if (qs < 0 || (qs == 0 && (!backend_gl_create() || !swr_gl_load(backend_gl_proc)))) {
            WARN("No OpenGL 4.3: running without a graphics card\n");
            g_gl = 0;
            g_gl_ready = 1;
            return;
        }
    }
    {
        GLint mj = 4, mn = 3;
        glGetIntegerv(GL_MAJOR_VERSION, &mj);
        glGetIntegerv(GL_MINOR_VERSION, &mn);
        g_glsl_version = (mj > 4 || (mj == 4 && mn >= 4)) ? 440 : 430;
        const char* vs = (const char*)glGetString(GL_VERSION);
        if (vs && strstr(vs, "OpenGL ES")) {
            g_gles = 1;
            g_glsl_es = 1;
        }
        if (g_gles) {
            GLint n = 0;
            glGetIntegerv(GL_NUM_EXTENSIONS, &n);
            typedef const GLubyte*(APIENTRY * GSI)(GLenum, GLuint);
            GSI gsi = (GSI)backend_gl_proc("glGetStringi");
            for (GLint k = 0; k < n && gsi; k++) {
                const char* e = (const char*)gsi(GL_EXTENSIONS, (GLuint)k);
                if (!e) continue;
                if (!strcmp(e, "GL_KHR_texture_compression_astc_ldr")) g_has_astc = 1;
                if (!strcmp(e, "GL_EXT_texture_compression_s3tc")) g_has_s3tc = 1;
                if (!strcmp(e, "GL_EXT_texture_compression_rgtc")) g_has_rgtc = 1;
                if (!strcmp(e, "GL_EXT_texture_compression_bptc")) g_has_bptc = 1;
            }
            if (getenv("SWR_FORCE_DECODE")) { g_has_s3tc = g_has_rgtc = g_has_bptc = 0; }
            LOG("GLES: native ASTC=%d, BC1-3=%d, BC4-5=%d, BC6-7=%d (the rest are decoded on the CPU)\n", g_has_astc,
                g_has_s3tc, g_has_rgtc, g_has_bptc);
        }
    }
    LOG("OpenGL: %s / %s / %s\n", (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_RENDERER),
        (const char*)glGetString(GL_VERSION));
    { /* compile on the driver's threads so we don't freeze */
        g_par = 0;
        int vl = 0;
        GLint n = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &n);
        typedef const GLubyte*(APIENTRY * GSI2)(GLenum, GLuint);
        GSI2 gsi = (GSI2)backend_gl_proc("glGetStringi");
        for (GLint k = 0; k < n && gsi; k++) {
            const char* e = (const char*)gsi(GL_EXTENSIONS, (GLuint)k);
            if (!e) continue;
            if (!strcmp(e, "GL_KHR_parallel_shader_compile") || !strcmp(e, "GL_ARB_parallel_shader_compile")) g_par = 1;
            if (!g_gles && !strcmp(e, "GL_KHR_texture_compression_astc_ldr")) g_has_astc = 1;
            if (!strcmp(e, "GL_ARB_shader_viewport_layer_array")) vl |= 1;
            if (!strcmp(e, "GL_AMD_vertex_shader_layer")) vl |= 2;
            if (!strcmp(e, "GL_NV_viewport_array2")) vl |= 4;
        }
        {
            g_vs_layer_ext = g_gles ? 0 : (vl & 1) ? 1 : (vl & 2) ? 2 : (vl & 4) ? 3 : 0;
            LOG("gpu: render target layer from the vertex shader: %s\n",
                g_vs_layer_ext == 1   ? "ARB_shader_viewport_layer_array"
                : g_vs_layer_ext == 2 ? "AMD_vertex_shader_layer"
                : g_vs_layer_ext == 3 ? "NV_viewport_array2"
                                      : "not available (drawn to layer 0)");
        }
        if (!g_gles)
            LOG("ASTC textures: %s\n", g_has_astc && !getenv("SWR_ASTC_CPU") ? "decoded by the GPU (fast, with mipmaps)"
                                                                             : "decoded on the CPU");
        if (getenv("SWR_SYNC_SHADERS")) g_par = 0;
        if (g_par) {
            typedef void(APIENTRY * MSCT)(GLuint);
            MSCT f = (MSCT)backend_gl_proc("glMaxShaderCompilerThreadsKHR");
            if (!f) f = (MSCT)backend_gl_proc("glMaxShaderCompilerThreadsARB");
            if (f) f(0xFFFFFFFFu);
        }
        LOG("shaders: parallel compilation %s\n",
            g_par ? "enabled (no stutter: anything still compiling is drawn as soon as it is ready)"
                  : "not available in this driver");
    }
    {
        GLint v[6] = {0};
        glGetIntegerv(0x8B4D, &v[0]);
        glGetIntegerv(0x8872, &v[1]);
        glGetIntegerv(0x8A2F, &v[2]);
        glGetIntegerv(0x8A2B, &v[3]);
        glGetIntegerv(0x8A2D, &v[4]);
        glGetIntegerv(0x826E, &v[5]);
        LOG("OpenGL limits: textures %d (per stage %d), UBO %d (VS %d, FS %d), uniform locations %d\n", v[0], v[1],
            v[2], v[3], v[4], v[5]);
    }
    if (g_verbose >= 2) {
        typedef void(APIENTRY * DBGCB)(GLDEBUGPROC, const void*);
        DBGCB f = (DBGCB)backend_gl_proc("glDebugMessageCallback");
        if (f) {
            glEnable(GL_DEBUG_OUTPUT);
            f(gl_dbg, NULL);
        }
    }
    glGenFramebuffers(1, &g_fbo);
    glGenFramebuffers(1, &g_rfbo);
    glGenFramebuffers(1, &g_dfbo);
    glGenVertexArrays(1, &g_vao);
    glGenVertexArrays(1, &g_scratch_vao);
    glGenBuffers(32, g_vbuf);
    glGenBuffers(1, &g_ibuf);
    for (int s = 0; s < 5; s++) glGenBuffers(18, g_ubuf[s]);
    {
        GLuint a = compile(GL_VERTEX_SHADER, g_gles ? BLIT_VS_ES : BLIT_VS, NULL, 0),
               b = compile(GL_FRAGMENT_SHADER, g_gles ? BLIT_FS_ES : BLIT_FS, NULL, 0);
        g_blit_prog = link2(a, b, 0);
    }
    if (!g_gles) {
        glEnable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glEnable(0x8642);
    } /* GL_PROGRAM_POINT_SIZE, gl_PointSize comes from the shader */
    {
        g_scale = g_opt_resf;
        if (g_scale != 1.0f) LOG("internal resolution x%.2f\n", g_scale);
    }
    rs_update();
    worker_start();
    sep_init();
    mb_init();
    ur_init();
    if (!g_gles && !getenv("SWR_NO_CLIPCONTROL")) p_clip = (PFNGLCLIPCONTROLPROC)backend_gl_proc("glClipControl");
    if (!g_gles && !getenv("SWR_NO_TEXVIEW")) p_tview = (PFNGLTEXTUREVIEWPROC)backend_gl_proc("glTextureView");
    if (!g_gles) lsfg_init();
    p_cis = (PFNGLCOPYIMAGESUBDATAPROC)backend_gl_proc("glCopyImageSubData");
    gl_selftest();
    state_dirty();
    mb_reset();
    LOG("gpu: native 0..1 depth %s, texture views %s\n", p_clip ? "yes" : "no", p_tview ? "yes" : "no");
    LOG("gpu: texture multi-bind %s, persistent constant ring %s\n", p_bt ? "yes" : "no", g_ur_ptr ? "yes" : "no");
    warm_load();
    g_gl = 1;
    g_gl_ready = 1;
    for (;;) {
        warm_step();
        { /* loading screens etc: log how long we went without a new image */
            static u64 lp_t, lp_f;
            u64 tn = plat_ns();
            if (g_frame != lp_f) {
                if (lp_t && tn - lp_t > 1500000000ull && g_rf) {
                    fprintf(g_rf, "   -> %.1f s without a new image (loading or waiting)\n", (double)(tn - lp_t) / 1e9);
                    fflush(g_rf);
                }
                lp_f = g_frame;
                lp_t = tn;
            }
        }
        {
            static u64 next_st;
            u64 now = plat_ns();
            if (!next_st) next_st = now + 5000000000ull;
            if (now >= next_st) {
                static int nst;
                nst++;
                next_st = now + (nst < 24 ? 5000000000ull : 30000000000ull);
                LOG("gpu: frames=%d submits=%llu draws=%llu (drawn %llu, no shader %llu, failed shader %llu, no program %llu, no target %llu) clears=%llu shaders=%llu present: rt=%llu cpu=%llu | textures ok=%llu rt=%llu null=%llu type-mismatch=%llu, dma copies=%llu, compute=%llu, waiting for shader=%llu\n",
                    g_frames, (unsigned long long)g_gpu_submits, (unsigned long long)g_draws,
                    (unsigned long long)st_drawn, (unsigned long long)st_noshader, (unsigned long long)st_shfail,
                    (unsigned long long)st_noprog, (unsigned long long)st_nort, (unsigned long long)st_clears,
                    (unsigned long long)st_shaders, (unsigned long long)st_pres_rt, (unsigned long long)st_pres_cpu,
                    (unsigned long long)st_tex_ok, (unsigned long long)st_tex_rt, (unsigned long long)st_tex_null,
                    (unsigned long long)st_tex_mismatch, (unsigned long long)g_dma_copies,
                    (unsigned long long)g_compute_launches, (unsigned long long)st_compiling);
                static u64 last_frames_pf;
                {
                    static u64 last_t, last_busy, last_frames;
                    u64 dt = now - last_t;
                    if (last_t && dt) {
                        LOG("performance: %.1f FPS, GPU thread busy %d%% (near 100%% = the bottleneck is the graphics card; low = it is the CPU running the game)\n",
                            (double)(g_frame - last_frames) * 1e9 / (double)dt,
                            (int)((g_gpu_busy - last_busy) * 100 / dt));
                    }
                    last_t = now;
                    last_busy = g_gpu_busy;
                    last_frames = g_frame;
                }
                { /* performance.txt: compact summary for diagnosing on someone else's PC */
                    static u64 l_now, l_pcpu, l_gcpu, l_cn, l_cns, l_fr2, l_skip, l_dr2;
                    FILE* rf = g_rf;
                    u64 t_start = g_rf_t0;
                    if (!rf) {
                        rf = g_rf = fopen("performance.txt", "w");
                        t_start = g_rf_t0 = now;
                        if (rf) {
                            fprintf(
                                rf,
                                "SwitchRecomp - performance (each line = last few seconds). CPU: %d cores. GPU: %s\n",
                                plat_ncpu(), (const char*)glGetString(GL_RENDERER));
                            {
                                if (g_code_tcc)
                                    fprintf(
                                        rf,
                                        "WARNING: the game code was compiled with TCC and runs several times slower than with GCC. Recompile with GCC.\n");
                            }
                            fprintf(
                                rf,
                                " sec |  FPS | process CPU | GPU thread (CPU) | draws/frame | shaders compiled (avg ms, max) | draws waiting for shader\n");
                        }
                    }
                    u64 pc = plat_process_cpu_ns(), gc = plat_thread_cpu_ns();
                    u64 dtn = now - l_now;
                    if (rf && l_now && dtn) {
                        u64 cn = g_cmp_n - l_cn, cns = g_cmp_ns - l_cns;
                        u64 frn = g_frame - l_fr2;
                        fprintf(rf, "%4llu | %4.1f | %3d%% of %d cores | %3d%% | %6llu | %4llu (%5.1f, %5.1f) | %llu\n",
                                (unsigned long long)((now - t_start) / 1000000000ull), (double)frn * 1e9 / (double)dtn,
                                (int)((pc - l_pcpu) * 100 / dtn), plat_ncpu(), (int)((gc - l_gcpu) * 100 / dtn),
                                (unsigned long long)(frn ? (st_drawn - l_dr2) / frn : 0), (unsigned long long)cn,
                                cn ? (double)cns / cn / 1e6 : 0.0, (double)g_cmp_max / 1e6,
                                (unsigned long long)(st_compiling - l_skip));
                        {
                            double fr = frn ? (double)frn : 1.0;
                            static u64 l_bn, l_bns, l_csw;
                            fprintf(
                                rf,
                                "      GPU thread breakdown (ms per frame): shaders %.1f + load %.1f | state %.1f | textures %.1f | buffers %.1f (constants %.1f, storage %.1f, vertices %.1f, compute waits %.1f) | driver draw %.1f | present/wait GPU %.1f | slowest frame %.0f ms | cache binaries %llu (%.1f ms)\n",
                                g_tmp[TM_SH] / 1e6 / fr, g_tmp[TM_SHREADY] / 1e6 / fr, g_tmp[TM_STATE] / 1e6 / fr,
                                g_tmp[TM_TEX] / 1e6 / fr, (g_tmp[TM_BUF] + g_tmp[TM_CB] + g_tmp[TM_GB]) / 1e6 / fr,
                                g_tmp[TM_CB] / 1e6 / fr, g_tmp[TM_GB] / 1e6 / fr, g_tmp[TM_BUF] / 1e6 / fr,
                                (double)(st_cs_sync_ns - l_csw) / 1e6 / fr, g_tmp[TM_DRAW] / 1e6 / fr,
                                g_tmp[TM_SWAP] / 1e6 / fr, g_fr_max / 1e6, (unsigned long long)(g_bin_n - l_bn),
                                (g_bin_n - l_bn) ? (double)(g_bin_ns - l_bns) / (g_bin_n - l_bn) / 1e6 : 0.0);
                            l_bn = g_bin_n;
                            l_bns = g_bin_ns;
                            l_csw = st_cs_sync_ns;
                            memset(g_tmp, 0, sizeof g_tmp);
                            g_fr_max = 0;
                            {
                                static u64 l_lg, l_lns;
                                u64 ng = st_lsfg_gen - l_lg;
                                if (ng) {
                                    fprintf(
                                        rf,
                                        "      frame generation (LSFG): +%llu frames -> %.1f FPS on screen, %.1f ms of GPU thread per generated frame\n",
                                        (unsigned long long)ng, (double)(frn + ng) * 1e9 / (double)dtn,
                                        (double)(st_lsfg_ns - l_lns) / (double)ng / 1e6);
                                }
                                l_lg = st_lsfg_gen;
                                l_lns = st_lsfg_ns;
                            }
                        }
                        fflush(rf);
                        g_cmp_max = 0;
                    }
                    l_now = now;
                    l_pcpu = pc;
                    l_gcpu = gc;
                    l_cn = g_cmp_n;
                    l_cns = g_cmp_ns;
                    l_fr2 = g_frame;
                    l_skip = st_compiling;
                    l_dr2 = st_drawn;
                }
                {
                    static u64 l_cbs, l_cbb, l_vbh, l_vbu, l_tex, l_dr;
                    u64 fr = g_frame - last_frames_pf;
                    if (!fr) fr = 1;
                    static u64 l_shf, l_sh, l_fbs, l_tbs, l_urw, l_tck, l_ss, l_dm, l_csg, l_css, l_csn, l_th, l_tm;
                    LOG("per-frame profile: %llu draws | constants: %.1f KB copied, %llu reused | vertices/indices: %.1f KB checked, %.1f KB uploaded | textures: %.1f KB checked, %.1f KB uploaded | shaders via fast cache %llu/%llu | same fbo %llu, same textures %llu, same state %llu, ring waits %llu, merged instances %llu | compute: %llu GPU copies, %llu waits (%.1f ms) | texture lookups reused %.0f%%\n",
                        (unsigned long long)((st_drawn - l_dr) / fr), (double)(st_cb_bytes - l_cbb) / 1024.0 / fr,
                        (unsigned long long)((st_cb_skip - l_cbs) / fr), (double)(st_vb_hashed - l_vbh) / 1024.0 / fr,
                        (double)(st_vb_up - l_vbu) / 1024.0 / fr, (double)(st_tex_chk - l_tck) / 1024.0 / fr,
                        (double)(st_tex_up - l_tex) / 1024.0 / fr, (unsigned long long)((st_sh_fast - l_shf) / fr),
                        (unsigned long long)((st_shaders - l_sh) / fr), (unsigned long long)((st_fb_skip - l_fbs) / fr),
                        (unsigned long long)((st_tb_skip - l_tbs) / fr),
                        (unsigned long long)((st_state_skip - l_ss) / fr), (unsigned long long)(st_ur_wait - l_urw),
                        (unsigned long long)((g_draws_merged - l_dm) / fr),
                        (unsigned long long)((st_cs_gpucopy - l_csg) / fr),
                        (unsigned long long)((st_cs_sync - l_css) / fr), (double)(st_cs_sync_ns - l_csn) / 1e6 / fr,
                        (st_tdc_hit - l_th) + (st_tdc_miss - l_tm)
                            ? 100.0 * (st_tdc_hit - l_th) / (double)((st_tdc_hit - l_th) + (st_tdc_miss - l_tm))
                            : 0.0);
                    l_th = st_tdc_hit;
                    l_tm = st_tdc_miss;
                    l_csg = st_cs_gpucopy;
                    l_css = st_cs_sync;
                    l_csn = st_cs_sync_ns;
                    l_dm = g_draws_merged;
                    l_shf = st_sh_fast;
                    l_sh = st_shaders;
                    l_fbs = st_fb_skip;
                    l_tbs = st_tb_skip;
                    l_urw = st_ur_wait;
                    l_tck = st_tex_chk;
                    l_ss = st_state_skip;
                    l_cbs = st_cb_skip;
                    l_cbb = st_cb_bytes;
                    l_vbh = st_vb_hashed;
                    l_vbu = st_vb_up;
                    l_tex = st_tex_up;
                    l_dr = st_drawn;
                    last_frames_pf = g_frame;
                }
            }
        }
        lsfg_pump();
        i64 to_ = 100000000;
        if (g_lfn) {
            u64 nw = plat_ns();
            to_ = g_lfq[0].t > nw ? (i64)(g_lfq[0].t - nw) : 0;
            if (to_ < 200000) to_ = 200000;
        }
#ifdef _WIN32
        if (g_rec_exit == 1) {
            rec_stop();
            g_rec_exit = 2;
        }
        if (g_rec_toggle && g_rec_on) {
            g_rec_toggle = 0;
            rec_stop();
        } /* let F7 stop even when nothing is being presented */
#endif
        if (cs_inflight()) {
            cs_complete(0);
            if (cs_inflight() && to_ > 1000000) to_ = 1000000;
        }
        Work w;
        if (!q_pop(&w, to_)) {
            lsfg_pump();
            continue;
        }
        u64 t_busy0 = plat_ns();
        switch (w.kind) {
            case WK_SUBMIT:
                g_submit_epoch++;
                ww_poll();
                {
                    static u32 fr = ~0u;
                    if (fr != g_frame || !g_gpu_sub_ns) {
                        fr = g_frame;
                        g_gpu_sub_ns = w.sub_ns;
                    }
                } /* stamp the whole frame with the time of its first submit */
                gpu_submit_now(w.ent, w.n, w.syncpt);
                cs_kick_async();
                free(w.ent);
                cs_submit_done(w.syncpt, w.incs);
                break;
            case WK_PRESENT_CPU:
                trace_note("PRESENT CPU image %dx%d\n", w.w, w.h);
                present_cpu(w.px, w.w, w.h);
                free(w.px);
                g_frame++;
                gpu_evict_step();
                frame_stats();
                trace_frame();
                break;
            case WK_PRESENT_ADDR:
                if (!present_addr(w.addr, w.w, w.h)) {
                    u32* px = malloc((size_t)w.w * w.h * 4);
                    vi_convert_cpu(w.addr, w.w, w.h, w.n, (w.fmt >> 8) & 15, (w.fmt >> 12) & 15, w.fmt & 0xFF, px);
                    present_cpu(px, w.w, w.h);
                    free(px);
                }
                g_frame++;
                gpu_evict_step();
                frame_stats();
                trace_frame();
                break;
            case WK_DUMP:
                dump_rts();
                g_tr_arm = 1;
                break;
            case WK_SHOT:
                do_screenshot(w.path[0] ? w.path : NULL);
                if (w.done) { *w.done = 1; }
                break;
        }
        g_gpu_busy += plat_ns() - t_busy0;
    }
}
static int glr_start(void) {
    pm_init(&g_ql);
    pc_init(&g_qcv);
    pc_init(&g_qspace);
    if (plat_thread(gpu_thread, NULL, 64u << 20)) fatal("could not create the GPU thread");
    while (!g_gl_ready) plat_sleep_ns(1000000);
    return 1;
}
/* requests from other threads */
static int glr_async_submit(u64* e, u32 n, u32 syncpt, u32 incs) {
    if (!g_gl) return 0;
    Work w;
    memset(&w, 0, sizeof w);
    w.kind = WK_SUBMIT;
    { w.sub_ns = guest_ns(); }
    w.ent = malloc((size_t)n * 8 + 8);
    memcpy(w.ent, e, (size_t)n * 8);
    w.n = n;
    w.syncpt = syncpt;
    w.incs = incs;
    q_push(&w);
    return 1;
}
static void glr_present_cpu(const u32* px, int w, int h, int stride) {
    if (!g_gl) return;
    Work k;
    memset(&k, 0, sizeof k);
    k.kind = WK_PRESENT_CPU;
    k.px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) memcpy(k.px + (size_t)y * w, px + (size_t)y * stride, (size_t)w * 4);
    k.w = w;
    k.h = h;
    q_push(&k);
}
static void glr_present_buffer(u64 cpu_addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt) {
    Work k;
    memset(&k, 0, sizeof k);
    k.kind = WK_PRESENT_ADDR;
    k.addr = cpu_addr;
    k.w = (int)w;
    k.h = (int)h;
    k.fmt = fmt | (layout << 8) | (bhl << 12);
    k.n = pitch;
    q_push(&k);
}
static u64 glr_screenshot(const char* path) {
    if (!g_gl) return 0;
    volatile int done = 0;
    Work k;
    memset(&k, 0, sizeof k);
    k.kind = WK_SHOT;
    k.done = &done;
    if (path) strncpy(k.path, path, 255);
    q_push(&k);
    u64 t0 = plat_ns();
    while (!done && plat_ns() - t0 < 30000000000ull) plat_sleep_ns(2000000);
    return g_last_shot_hash;
}
static void glr_dump_rts(void) {
    if (!g_gl) return;
    Work k;
    memset(&k, 0, sizeof k);
    k.kind = WK_DUMP;
    q_push(&k);
}

const Renderer gl_renderer = {
    .name = "OpenGL",
    .start = glr_start,
    .async_submit = glr_async_submit,
    .clear = glr_clear,
    .draw = glr_draw,
    .cb_bind = glr_cb_bind,
    .blit = glr_blit,
    .rt_flush = glr_rt_flush,
    .rt_reload = glr_rt_reload,
    .dispatch = glr_dispatch,
    .cs_mem_need = glr_cs_mem_need,
    .before_syncwait = glr_before_syncwait,
    .stream_incr = glr_stream_incr,
    .present_buffer = glr_present_buffer,
    .present_cpu = glr_present_cpu,
    .screenshot = glr_screenshot,
    .note_compute = glr_note_compute,
    .dump_rts = glr_dump_rts,
    .rec_finish = glr_rec_finish,
    .trace = glr_trace,
    .busy_ns = glr_busy_ns,
};
/* the renderer until a backend picks one (gpu/renderer.h) */
const Renderer* g_renderer = &gl_renderer;
