/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* what the OpenGL renderer's files in gpu/gl share: their types, and the functions and state one file uses from another */
#ifndef SWR_GL_H
#define SWR_GL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "core/loader.h"
#include "core/main.h"
#include "core/options.h"
#include "gpu/gl/lsfg.h"
#include "gpu/renderer.h"
#include "gpu/texture/astc.h"
#include "gpu/texture/bcn.h"
#include "hle/nvdrv.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#ifdef _WIN32
#include "platform/recorder.h"
#endif
#include "gpu/gpu.h"
#include <time.h>
#include <math.h>
#include "gpu/gl/glfn.h"
#include "gpu/shader/shader.h"
#include "gpu/gl/glq.h"

/* GL calls in this file go through glq when it's on */
#define backend_gl_proc           glq_proc
#define backend_gl_swap           glq_swap
#define backend_gl_worker_current glq_worker_current
/* formats */
typedef struct {
    GLenum ifmt, fmt, type;
    u8 bpp, bw, bh, compressed, depth, is_int, astc, dec;
} GlFmt;
/* texture cache */
typedef struct Tex {
    u64 gva;
    u32 w, h, d, levels, target;
    u32 fkey;
    GlFmt f;
    u32 bl, bh, bd, pitch;
    u64 size;
    GLuint id;
    u64 hash;
    int is_rt, gpu_written;
    u32 frame_checked, last_used;
    GLint swz[4];
    u64 tr_last;
    u32 stable, next_check;
    GLuint view;
    GLenum view_target;
    GLint vswz[4];
    u32 wgen, crop_gen;
    struct Tex* crop;
    struct Tex* lay;
    struct Tex* vol;
    int skipclr, zero3d;
    u64 alias;
    u64 shash;
    u32 tfull;
    struct Tex* next;
} Tex;
#define TEXH 4096
/* GPU thread time per category, for performance.txt */
enum { TM_SH, TM_SHREADY, TM_STATE, TM_TEX, TM_BUF, TM_DRAW, TM_SWAP, TM_CB, TM_GB, TM_N };
#define TM(k_, code_)                                                                                                  \
    do {                                                                                                               \
        u64 t_ = plat_ns();                                                                                            \
        code_;                                                                                                         \
        g_tm[k_] += plat_ns() - t_;                                                                                    \
    } while (0)
typedef struct {
    u32 gen, frame;
    u32 tic[8];
    Tex* t;
} TdcEnt;
/* shaders and programs */
struct WJob;
typedef struct Sh {
    u64 hash;
    u32 key2;
    int stage;
    GLuint gl;
    ShaderInfo si;
    struct Sh* next;
    int failed;
    char* src;
    GLuint sp;
    GLint uloc;
    int sps;
    struct WJob* sj;
    u32 clen;
    u64 qh;
    u32 clen_a;
    u64 qh_a;
    float lu[4];
    int lu_ok;
    u64 skey;
} Sh; /* sp: this shader's own separable program */
typedef struct WJob {
    const char* vs;
    const char* fs;
    char* own_vs;
    char* own_fs;
    u64 key;
    volatile int state;
    GLuint prog;
    int keep;
    int sep;
    GLenum stype;
    char log[1536];
    int try_bin;
    char bin_path[160];
} WJob;
#define WQN 8192
typedef struct {
    WJob* volatile e[WQN];
    volatile u32 h, t;
} WRing;
typedef void(APIENTRY* PFN_UPS_)(GLuint, GLbitfield, GLuint);
typedef void(APIENTRY* PFN_BPP_)(GLuint);
typedef struct Prog {
    Sh* vs;
    Sh* fs;
    GLuint p;
    GLint vp_loc, at_loc;
    int pending;
    WJob* job;
    struct Prog* next;
} Prog;
typedef struct {
    u64 cb[5][18];
    u32 cbsize[5][18];
} CbState;
/* multi-bind (GL 4.4): a stage's textures + samplers in two calls, none if nothing changed.
   other glBindTexture users (uploads, creation) use g_scratch_unit */
typedef void(APIENTRY* PFN_BT_)(GLuint, GLsizei, const GLuint*);
/* buffer cache keyed by guest range, validated once per submit */
typedef struct VbC {
    u64 start, len, hash, cpu;
    u32 epoch, used;
    GLuint id;
    struct VbC* next;
    u32 stable, chk_epoch;
    int dirty;
    u32 csseen;
    u64 shash;
    u32 schk;
} VbC;
typedef struct {
    GLuint tex;
    u64 t;
} LfQ;

/* renderer.c */
extern u64 g_draws;
extern __thread int t_gpu_thread;
extern int g_diag_draws;
extern u32 g_submit_epoch;
extern u64 st_tex_ok, st_tex_null, st_tex_mismatch, st_tex_rt;
extern u64 st_compiling, st_cb_skip, st_cb_bytes, st_vb_hashed, st_vb_up, st_tex_up;
extern u64 st_noshader, st_shfail, st_noprog, st_nort, st_drawn, st_clears, st_pres_rt, st_pres_cpu, st_shaders;
extern float g_scale;
u32 SC(u32 v);
extern u32 g_last_cw, g_last_ch;
float fabsf_(float x);
GLuint compile(GLenum type, const char* src, char* err, int errn);
GLuint link2(GLuint a, GLuint b, GLuint c3);
extern GLuint g_blit_prog, g_cpu_tex;
extern int g_cpu_w, g_cpu_h;
extern GLuint g_last_present_tex;
extern int g_last_flip, g_last_w, g_last_h;
extern int g_last_is_rt;
extern volatile i32 g_vi_crop[4];
extern volatile u32 g_vi_transform;

/* formats.c */
extern int g_gles;
extern int g_has_astc, g_has_s3tc, g_has_rgtc, g_has_bptc;
GlFmt tic_format(u32 w0, int srgb);
extern int g_snorm_fb;
GlFmt rt_format(u32 f);

/* texture.c */
extern Tex* texh[TEXH];
extern u32 g_frame;
u32 hkey(u64 a);
extern u64 g_tm[TM_N], g_tmp[TM_N];
extern u64 st_sh_new;
u64 bl_level_size(u32 w, u32 h, u32 d, const GlFmt* f, u32 bh, u32 bd, u32 level, u32* out_bh);
extern u64 st_tex_chk;
u64 fast_hash(const u8* p, u64 size, int sampled);
u64 rt_sig(u64 gva);
extern u32 g_tmap_gen;
Tex* tex_create(u64 gva, u32 w, u32 h, u32 d, u32 levels, GLenum target, GlFmt f, u32 fkey, u32 bl, u32 bh, u32 bd,
                u32 pitch, int is_rt);
Tex* tex_find_rt(u64 gva);
extern PFNGLCOPYIMAGESUBDATAPROC p_cis;
void gl_selftest(void);
extern TdcEnt g_tdc[512];
extern u64 st_tdc_hit, st_tdc_miss;
Tex* tex_from_tic(u32* tic);
GLuint sampler_get(const u32* tsc);

/* programs.c */
extern u64 st_sh_fast;
extern int g_sh_bind;
extern u32* g_last_r3d;
extern u32 g_fs_rtk, g_fs_rtm, g_fs_rts;
Sh* shader_get(u64 gva_code, u64 gva_a, int stage, u32 attr_int, u32 attr_sint);
extern int g_par;
extern WRing wq_hi, wq_lo;
extern volatile u64 g_cmp_ns, g_cmp_n, g_cmp_max, g_bin_n, g_bin_ns;
void worker_start(void);
extern int g_sep;
extern GLuint g_pipe;
extern int g_pipe_on;
extern GLuint g_pipe_vs, g_pipe_fs;
extern PFN_UPS_ p_ups;
extern PFN_BPP_ p_bpp;
int sh_sep_ready(Sh* s);
void sep_init(void);
Prog* program_get(Sh* vs, Sh* fs);
int program_ready(Prog* p);

/* framebuffer.c */
extern GLuint g_rfbo, g_dfbo;
extern GLuint g_fbo, g_scratch_vao;
extern int g_fb_sig_ok;
int sh_cb_read(u32 idx, u32 off, u32* out);
void sh_dump_cbs(u64 h);
extern GLuint g_fbo, g_vao, g_vbuf[32], g_ibuf, g_ubuf[5][18], g_scratch_vao;
extern CbState g_cb;
extern Tex* g_rt[8];
extern Tex* g_zt;
extern int g_nrt;
Tex* rt_texture(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth);
extern u64 st_rt_vol, st_waited;
extern int g_draw_layered;
extern int g_fb_lay[8];
extern u64 st_fb_skip;
extern int g_clear_layer;
void bind_framebuffer(GpuChannel* ch);
void glr_rt_flush(u64 gva);
void glr_rt_reload(u64 gva);
int glr_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy);

/* draw.c */
void mb_reset(void);
u64 sample_hash(u64 gva, u64 len);
extern int g_nosample;
void state_dirty(void);
extern u64 st_state_skip;
extern PFNGLCLIPCONTROLPROC p_clip;
void apply_state(GpuChannel* ch);
u32 attr_count(u32 size);
u32 attr_bits(u32 size);
GLenum topo(u32 t);
void glr_cb_bind(GpuChannel* ch, int stage, u32 v);
void capture_draw(GpuChannel* ch, int indexed, u32 first, u32 count);
GLuint buf_cached(GLenum target, u64 start, u64 len);
void bind_gbufs(int stage, Sh* s);
extern u8* g_ur_ptr;
extern PFNGLFENCESYNCPROC p_fs;
extern PFNGLCLIENTWAITSYNCPROC p_cws;
extern PFNGLDELETESYNCPROC p_ds;
extern u64 st_ur_wait;
extern u32 g_ub_bound[64];
void ur_init(void);
void upload_cbufs(int stage, Sh* s);
extern PFN_BT_ p_bt, p_bs;
extern u64 st_tb_skip;
void mb_init(void);
extern PFNGLTEXTUREVIEWPROC p_tview;
GLuint tex_view(Tex* t, GLenum want);
u32 sh_tex_handle(int stage, u32 tcb, const ShTex* st);
Sh* fs_get_keyed(GpuChannel* ch, u64 addr);
GLuint null_tex(GLenum tg);
void bind_textures(GpuChannel* ch, int stage, Sh* s);
extern u8* g_stage_buf;
extern size_t g_stage_cap;
extern u32 g_csblk[65536], g_csgen;
extern VbC* vbc[2048];
extern int g_va_inval;
void ww_touch(u64 cpu);
void ww_poll(void);
extern u64 st_cs_gpucopy, st_cs_sync, st_cs_sync_ns;

/* trace.c */
void trace_note(const char* fmt, ...);
extern int g_tr_arm, g_tr_on, g_tr_n, g_tr_img;
void glr_note_compute(void);
void glr_trace(const char* s);
void trace_frame(void);
void glr_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances);
void glr_clear(GpuChannel* ch, u32 v);

/* capture.c */
void glr_rec_finish(void);

/* framegen.c */
void lsfg_pump(void);
extern int g_lfn;

/* compute.c */
void cs_complete(int wait);
void cs_kick_async(void);
int cs_inflight(void);
void cs_submit_done(u32 id, u32 n);
void glr_cs_mem_need(u64 a, u64 n);
int cs_gpu_src(u64 a, u64 n, GLuint* buf, u32* off);

#ifdef _WIN32
/* the F7 recorder, which the GPU thread stops at exit or on F7 */
extern int g_rec_on;
extern volatile int g_rec_exit; /* at exit: 1 = finalize, 2 = done */
void rec_stop(void);
#endif
void screen_quad(GLuint tex, const float* xf, int tw, int th);
void screen_swap(void);
extern LfQ g_lfq[6];
extern u64 st_lsfg_gen, st_lsfg_ns;
void present_cpu(u32* px, int w, int h);
void gpu_evict_step(void);
int present_addr(u64 cpu_addr, int w, int h);
extern u64 g_last_shot_hash;
void do_screenshot(const char* path);
void dump_rts(void);
void APIENTRY gl_dbg(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar* msg, const void* up);
void warm_load(void);
void warm_step(void);
extern FILE* g_rf;
extern u64 g_rf_t0;
extern u64 g_fr_t0, g_fr_max;
extern int g_fr_slow;
extern u64 g_fr_shnew;
extern GLuint g_rs_ubo;
void glr_stream_incr(u32 id);
void glr_before_syncwait(void);
void glr_dispatch(GpuChannel* ch, const u32* q, u64 code);

#endif
