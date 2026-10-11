/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* compute (the Kepler compute class): dispatches, and landing their results in guest memory */
#include "gpu/gl/gl.h"

/* ============================================================ compute (Kepler compute class, QMD)
   SMO fires ~25 dispatches a frame (particles, sims) touching global memory and a 3D texture.
   QMD -> compute GLSL, bind cbufs, textures and ssbos ({addr,size} in c0), and copy whatever got written
   back to guest memory afterwards. */
/* compute output goes GPU-side into a staging buffer and is read back in one go: before the next draw,
   at the end of the submit, or earlier if another dispatch reads that memory.
   g_cs_orig mirrors it with each SSBO's contents from before the dispatch, at the same offsets, so a result lands
   the way it would have on the console, where the GPU stores while it runs, long before the result gets here:
   - a byte the shader didn't change isn't written (the GPU writes only what it stores)
   - a byte that changed in guest memory since the dispatch keeps its new value: on the console that write came
     after the GPU's. The game reuses memory as soon as a semaphore says the GPU is done, which here can be before
     the result lands (Catherine allocates job nodes in its compute output ranges, and the late write-back put
     vertex data over them) */
static GLuint g_cs_stage, g_cs_orig;
static u32 g_cs_stage_cap, g_cs_stage_used;
static int g_cs_npend;
static struct {
    u64 addr;
    u32 off, size;
} g_cs_pend[256];
/* readback is async: at the end of each submit the staging copy gets a fence and we pick it up when it's
   signaled. only an actual read of that memory (draw, texture, DMA, another compute) has to wait */
typedef struct {
    u64 addr;
    u32 off, size;
} CsPend;
static struct {
    GLuint buf, orig;
    u32 cap, used;
    GLsync fence;
    int n;
    CsPend p[256];
} g_cs_fl;                           /* in flight */
static u64 g_cs_lo = ~0ull, g_cs_hi; /* bounds of everything pending/in flight, for a quick reject */
static int g_cs_busy;
volatile int g_cs_any;
static struct {
    u32 id, n;
} g_cs_incs[64];
static int g_cs_nincs; /* g_cs_any: anything pending/in flight (gmem_ptr/gmem_read check it) */
static inline u64 cs_ld64(const u8* p) {
    u64 v;
    memcpy(&v, p, 8);
    return v;
}
/* a byte of the result lands when the shader changed it (res != org) and guest memory still holds what the shader
   started from (cur == org) */
static inline int cs_lands(const u8* res, const u8* org, const u8* cur, u32 i) {
    return res[i] != org[i] && cur[i] == org[i];
}
/* all 8 bytes at p land: none of res equals org, and cur equals org */
static inline int cs_lands8(const u8* res, const u8* org, const u8* cur) {
    u64 x = cs_ld64(res) ^ cs_ld64(org);
    u64 zero_byte = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
    return !zero_byte && cs_ld64(cur) == cs_ld64(org);
}
/* writes the runs of the result that land, found 8 bytes at a time where they can be. dst is guest memory when the
   range is contiguous, else NULL */
static void cs_write_changed(u64 addr, u8* dst, const u8* res, const u8* org, u32 size) {
    u8* tmp = NULL;
    const u8* cur = dst;
    if (!cur) {
        tmp = malloc(size);
        gmem_read(addr, tmp, size);
        cur = tmp;
    }
    u32 i = 0;
    while (i < size) {
        if (i + 8 <= size && cs_ld64(res + i) == cs_ld64(org + i)) {
            i += 8;
            continue;
        }
        if (!cs_lands(res, org, cur, i)) {
            i++;
            continue;
        }
        u32 s = i;
        while (i + 8 <= size && cs_lands8(res + i, org + i, cur + i)) i += 8;
        while (i < size && cs_lands(res, org, cur, i)) i++;
        if (dst) {
            memcpy(dst + s, res + s, i - s);
            for (u64 o = (u64)(dst - g_mem) + s, e = o + (i - s); o < e; o = (o | 0xFFFF) + 1) ww_touch(o);
        } else
            gmem_write(addr + s, res + s, i - s);
    }
    free(tmp);
}
static void cs_apply(const u8* res, const u8* org, const CsPend* p, int n) {
    g_cs_busy = 1;
    /* cached vertex/storage buffers on this range are stale */
    g_csgen++;
    for (int i = 0; i < n; i++)
        for (u64 b = p[i].addr >> 16, e = (p[i].addr + p[i].size - 1) >> 16; b <= e; b++) g_csblk[b & 0xFFFF] = g_csgen;
    for (int i = 0; i < n; i++)
        cs_write_changed(p[i].addr, gmem_ptr(p[i].addr, p[i].size), res + p[i].off, org + p[i].off, p[i].size);
    g_cs_busy = 0;
}
/* the results and the pre-dispatch contents of a staging pair, read back */
static void cs_readback(GLuint stage, GLuint orig, u32 used, u8** res, u8** org) {
    static u8 *r, *o;
    static u32 cap;
    if (cap < used) {
        r = realloc(r, used);
        o = realloc(o, used);
        cap = used;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, stage);
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, used, r);
    glBindBuffer(GL_COPY_READ_BUFFER, orig);
    glGetBufferSubData(GL_COPY_READ_BUFFER, 0, used, o);
    *res = r;
    *org = o;
}
/* wait=0: only if the GPU is already done */
void cs_complete(int wait) {
    if (!g_cs_fl.fence) return;
    GLenum r = p_cws ? p_cws(g_cs_fl.fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? 4000000000ull : 0) : GL_ALREADY_SIGNALED;
    if (r == GL_TIMEOUT_EXPIRED) return;
    if (p_ds) p_ds(g_cs_fl.fence);
    g_cs_fl.fence = 0;
    u8 *res, *org;
    cs_readback(g_cs_fl.buf, g_cs_fl.orig, g_cs_fl.used, &res, &org);
    cs_apply(res, org, g_cs_fl.p, g_cs_fl.n);
    g_cs_fl.n = 0;
    g_cs_fl.used = 0;
    g_cs_any = g_cs_npend > 0;
    if (!g_cs_any) {
        g_cs_lo = ~0ull;
        g_cs_hi = 0;
    }
    /* submits that completed while this was in flight only count as done now; the game waits on them before reading */
    for (int i = 0; i < g_cs_nincs; i++)
        for (u32 k = 0; k < g_cs_incs[i].n; k++) syncpt_incr(g_cs_incs[i].id);
    g_cs_nincs = 0;
}
/* syncpoint incrs of a submit get held back (in order) while compute results are in flight */
void cs_submit_done(u32 id, u32 n) {
    if (g_cs_fl.fence && g_cs_nincs >= 64) cs_complete(1);
    if (g_cs_fl.fence) {
        g_cs_incs[g_cs_nincs].id = id;
        g_cs_incs[g_cs_nincs].n = n;
        g_cs_nincs++;
        return;
    }
    for (u32 k = 0; k < n; k++) syncpt_incr(id);
}
/* an incr inside the stream means everything before it is done, so it has to go after any pending compute results */
void glr_stream_incr(u32 id) {
    if (!t_gpu_thread) {
        syncpt_incr(id);
        return;
    }
    if (g_cs_npend) cs_kick_async();
    cs_submit_done(id, 1);
}
/* the stream is about to wait on a syncpoint: flush everything in flight */
void glr_before_syncwait(void) {
    if (t_gpu_thread && g_cs_fl.fence) cs_complete(1);
}
int cs_inflight(void) { return g_cs_fl.fence != 0; }
/* sync path: land everything pending/in flight in guest memory now */
static void cs_flush_pending(void) {
    cs_complete(1);
    if (!g_cs_npend) {
        g_cs_any = 0;
        g_cs_lo = ~0ull;
        g_cs_hi = 0;
        return;
    }
    glMemoryBarrier(0x00000200 /* GL_BUFFER_UPDATE_BARRIER_BIT */);
    u8 *res, *org;
    cs_readback(g_cs_stage, g_cs_orig, g_cs_stage_used, &res, &org);
    cs_apply(res, org, (const CsPend*)g_cs_pend, g_cs_npend);
    g_cs_npend = 0;
    g_cs_stage_used = 0;
    g_cs_any = 0;
    g_cs_lo = ~0ull;
    g_cs_hi = 0;
}
/* end of submit: pending copy goes in flight with a fence */
void cs_kick_async(void) {
    if (!g_cs_npend) return;
    static int sync = -1;
    if (sync < 0) sync = getenv("SWR_CS_SYNC") != NULL || !p_fs || !p_cws || !p_ds;
    if (sync) {
        cs_flush_pending();
        return;
    }
    cs_complete(1); /* one in flight at a time */
    glMemoryBarrier(0x00000200);
    GLuint b = g_cs_fl.buf, bo = g_cs_fl.orig;
    u32 c = g_cs_fl.cap;
    g_cs_fl.buf = g_cs_stage;
    g_cs_fl.orig = g_cs_orig;
    g_cs_fl.cap = g_cs_stage_cap;
    g_cs_fl.used = g_cs_stage_used;
    g_cs_fl.n = g_cs_npend;
    memcpy(g_cs_fl.p, g_cs_pend, sizeof(CsPend) * g_cs_npend);
    g_cs_fl.fence = p_fs(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    g_cs_stage = b;
    g_cs_orig = bo;
    g_cs_stage_cap = c; /* the other staging pair (0 = create on next use) */
    g_cs_npend = 0;
    g_cs_stage_used = 0;
    g_cs_any = 1;
}
/* gmem_ptr/gmem_read on the GPU thread hit memory a compute wrote that hasn't landed yet -> pull it now */
void glr_cs_mem_need(u64 a, u64 n) {
    if (a >= g_cs_hi || a + n <= g_cs_lo) return;
    if (g_cs_busy || !t_gpu_thread) return; /* GL from the GPU thread only */
    int hit = 0;
    for (int i = 0; i < g_cs_fl.n && !hit; i++)
        if (a < g_cs_fl.p[i].addr + g_cs_fl.p[i].size && g_cs_fl.p[i].addr < a + n) hit = 1;
    for (int i = 0; i < g_cs_npend && !hit; i++)
        if (a < g_cs_pend[i].addr + g_cs_pend[i].size && g_cs_pend[i].addr < a + n) hit = 1;
    if (hit) {
        u64 t0 = plat_ns();
        cs_flush_pending();
        st_cs_sync++;
        st_cs_sync_ns += plat_ns() - t0;
    }
}
static int cs_pending_overlaps(u64 a, u32 n) {
    for (int i = 0; i < g_cs_npend; i++)
        if (a < g_cs_pend[i].addr + g_cs_pend[i].size && g_cs_pend[i].addr < a + n) return 1;
    return 0;
}
/* newest overlapping result must cover all of [a,a+n) (pending is newer than in flight) */
int cs_gpu_src(u64 a, u64 n, GLuint* buf, u32* off) {
    static int off_ = -1;
    if (off_ < 0) off_ = getenv("SWR_CS_NO_GPUCOPY") != NULL;
    if (off_) return 0;
    if (!t_gpu_thread || g_cs_busy || a >= g_cs_hi || a + n <= g_cs_lo || n == 0) return 0;
    for (int i = g_cs_npend - 1; i >= 0; i--) {
        u64 pa = g_cs_pend[i].addr, pe = pa + g_cs_pend[i].size;
        if (a < pe && pa < a + n) {
            if (a >= pa && a + n <= pe && g_cs_stage) {
                *buf = g_cs_stage;
                *off = g_cs_pend[i].off + (u32)(a - pa);
                return 1;
            }
            return 0;
        }
    }
    if (g_cs_fl.fence)
        for (int i = g_cs_fl.n - 1; i >= 0; i--) {
            u64 pa = g_cs_fl.p[i].addr, pe = pa + g_cs_fl.p[i].size;
            if (a < pe && pa < a + n) {
                if (a >= pa && a + n <= pe && g_cs_fl.buf) {
                    *buf = g_cs_fl.buf;
                    *off = g_cs_fl.p[i].off + (u32)(a - pa);
                    return 1;
                }
                return 0;
            }
        }
    return 0;
}
/* format class for raw SUST.D / SULD.D: 0 f32, 1 f16, 2 unorm8, 3 unorm16 */
static int fmt_cls(GLenum f) {
    switch (f) {
        case GL_R16F:
        case GL_RG16F:
        case GL_RGBA16F: return 1;
        case GL_R8:
        case GL_RG8:
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8: return 2;
        case GL_R16:
        case GL_RG16:
        case GL_RGBA16: return 3;
        default: return 0;
    }
}
static GLenum img_fmt(GLenum f) {
    switch (f) {
        case GL_SRGB8_ALPHA8: return GL_RGBA8;
        case GL_RGBA32F:
        case GL_RGBA16F:
        case GL_RG32F:
        case GL_RG16F:
        case GL_R11F_G11F_B10F:
        case GL_R32F:
        case GL_R16F:
        case GL_RGBA32UI:
        case GL_RGBA16UI:
        case GL_RGB10_A2UI:
        case GL_RGBA8UI:
        case GL_RG32UI:
        case GL_RG16UI:
        case GL_RG8UI:
        case GL_R32UI:
        case GL_R16UI:
        case GL_R8UI:
        case GL_RGBA32I:
        case GL_RGBA16I:
        case GL_RGBA8I:
        case GL_RG32I:
        case GL_RG16I:
        case GL_RG8I:
        case GL_R32I:
        case GL_R16I:
        case GL_R8I:
        case GL_RGBA16:
        case GL_RGB10_A2:
        case GL_RGBA8:
        case GL_RG16:
        case GL_RG8:
        case GL_R16:
        case GL_R8:
        case GL_RGBA16_SNORM:
        case GL_RGBA8_SNORM:
        case GL_RG16_SNORM:
        case GL_RG8_SNORM:
        case GL_R16_SNORM:
        case GL_R8_SNORM: return f;
        default: return 0;
    }
}
typedef struct CsProg {
    u64 key;
    GLuint prog;
    int failed;
    ShaderInfo si;
    struct CsProg* next;
    GLint loc[4];
    int loc_ok;
} CsProg;
static CsProg* g_csp[256];
static u64 st_cs_run, st_cs_fail;
static u8 g_cs_cb_[8][0x10000];
static u32 g_cs_cbn[8];
static int cs_cb_read(u32 idx, u32 off, u32* out) {
    if (idx < 8 && off + 4 <= g_cs_cbn[idx]) {
        memcpy(out, g_cs_cb_[idx] + off, 4);
        return 1;
    }
    return 0;
}
void glr_dispatch(GpuChannel* ch, const u32* q, u64 code) {
    if (getenv("SWR_NO_COMPUTE")) return;
    r3d_pending_flush();
    u32* rc = ch->rcomp;
    u64 prog = code + q[8];
    u32 gx = q[12] & 0x7FFFFFFF, gy = q[13] & 0xFFFF, gz = q[13] >> 16, lx = q[18] >> 16, ly = q[19] & 0xFFFF,
        lz = q[19] >> 16, shm = q[17] & 0x3FFFF;
    if (!gx || !gy || !gz || !lx || !ly || !lz) return;
    /* cbufs from the QMD */
    u64 cba[8] = {0};
    u32 cbs[8] = {0};
    for (int i = 0; i < 8; i++)
        if ((q[20] >> i) & 1) {
            cba[i] = ((u64)(q[30 + 2 * i] & 0xFF) << 32) | q[29 + 2 * i];
            u32 sz = (q[30 + 2 * i] >> 15) & 0x1FFFF;
            if (!sz || sz > 0x10000) sz = 0x10000;
            cbs[i] = sz;
            g_cs_cbn[i] = sz;
            gmem_read(cba[i], g_cs_cb_[i], sz);
        } else
            g_cs_cbn[i] = 0;
    static u8 buf[0x10000 + 80];
    memset(buf, 0, 80);
    gmem_read(prog, buf + 80, 0x10000);
    u64 key = fast_hash(buf + 80, 0x4000, 0) ^ ((u64)lx << 40) ^ ((u64)ly << 48) ^ ((u64)lz << 56) ^ ((u64)shm << 20);
    CsProg* p = g_csp[key & 255];
    while (p && p->key != key) p = p->next;
    if (!p) {
        p = calloc(1, sizeof *p);
        p->key = key;
        p->next = g_csp[key & 255];
        g_csp[key & 255] = p;
        memset(&p->si, 0, sizeof p->si);
        p->si.cs_local[0] = lx;
        p->si.cs_local[1] = ly;
        p->si.cs_local[2] = lz;
        p->si.shared_size = (int)shm;
        int (*old)(u32, u32, u32*) = g_shader_cb_read;
        g_shader_cb_read = cs_cb_read;
        if (getenv("SWR_SHADER_CORPUS")) {
            u32 blen;
            shader_hash(buf, sizeof buf, &blen);
            shader_corpus_save(getenv("SWR_SHADER_CORPUS"), buf, blen, sizeof buf, ST_CS, &p->si, key);
        }
        char* src = shader_to_glsl(buf, sizeof buf, ST_CS, &p->si);
        g_shader_cb_read = old;
        if (!src || p->si.unsupported) {
            p->failed = 1;
            LOG("compute %016llx: not translated (%d instructions, first %s)\n", (unsigned long long)key,
                p->si.unsupported, p->si.first_unsupported);
        } else {
            char err[4096];
            GLuint sh = compile(GL_COMPUTE_SHADER, src, err, sizeof err);
            if (!sh) {
                p->failed = 1;
                LOG("compute %016llx failed to compile: %.600s\n", (unsigned long long)key, err);
            } else {
                GLuint pr = glCreateProgram();
                glAttachShader(pr, sh);
                glLinkProgram(pr);
                GLint ok = 0;
                glGetProgramiv(pr, GL_LINK_STATUS, &ok);
                glDeleteShader(sh);
                if (!ok) {
                    glGetProgramInfoLog(pr, sizeof err, NULL, err);
                    LOG("compute %016llx failed to link: %.600s\n", (unsigned long long)key, err);
                    glDeleteProgram(pr);
                    p->failed = 1;
                } else {
                    p->prog = pr;
                    static int said;
                    if (said < 12) {
                        said++;
                        LOG("compute %016llx ready: block %ux%ux%u, %d global buffers, %d textures\n",
                            (unsigned long long)key, lx, ly, lz, p->si.ngbuf, p->si.ntex);
                    }
                }
            }
        }
        if (src && getenv("SWR_DUMP_SHADERS")) {
            char pth[128];
            snprintf(pth, sizeof pth, "shader_%016llx_cs.glsl", (unsigned long long)key);
            FILE* f = fopen(pth, "w");
            if (f) {
                fputs(src, f);
                fclose(f);
            }
        }
        free(src);
    }
    if (p->failed || !p->prog) {
        st_cs_fail++;
        trace_note("COMPUTE %016llx (not translated)\n", (unsigned long long)key);
        return;
    }
    glUseProgram(p->prog);
    g_pipe_on = 0;
    /* cbufs at binding 36+i (translator's 'other stages' range) */
    static GLuint ub[8];
    for (int i = 0; i < 8; i++)
        if (p->si.cbuf_mask & (1u << i)) {
            if (!ub[i]) glGenBuffers(1, &ub[i]);
            glBindBuffer(GL_UNIFORM_BUFFER, ub[i]);
            static u8 z[0x10000];
            u32 n = g_cs_cbn[i];
            if (n) {
                if (n < 0x10000) {
                    memcpy(z, g_cs_cb_[i], n);
                    memset(z + n, 0, 0x10000 - n);
                    glBufferData(GL_UNIFORM_BUFFER, 0x10000, z, GL_STREAM_DRAW);
                } else
                    glBufferData(GL_UNIFORM_BUFFER, 0x10000, g_cs_cb_[i], GL_STREAM_DRAW);
            } else {
                memset(z, 0, sizeof z);
                glBufferData(GL_UNIFORM_BUFFER, 0x10000, z, GL_STREAM_DRAW);
            }
            glBindBufferRange(GL_UNIFORM_BUFFER, 36 + i, ub[i], 0, 0x10000);
            g_ub_bound[36 + i] = 0;
        }
    /* texture handles come from bank tex_cb_index (0x982) of the compute engine */
    GLint tcls[32] = {0}, tbpp[32] = {0}, icls[8] = {0}, ibpp[8] = {0};
    int nimg_ok = 0;
    {
        u64 tic_base = ((u64)rc[0x55D] << 32) | rc[0x55E], tsc_base = ((u64)rc[0x557] << 32) | rc[0x558];
        u32 tcb = rc[0x982] & 7;
        for (int i = 0; i < p->si.ntex && i < 32; i++) {
            ShTex* st = &p->si.tex[i];
            u32 cb = st->bindless ? (st->bl_cbuf & 7) : tcb;
            u32 h = 0;
            cs_cb_read(cb, st->cbuf_off * 4, &h);
            if (st->bindless && st->bl2) {
                u32 h2 = 0;
                cs_cb_read(st->bl2_cbuf & 7, st->bl2_off * 4, &h2);
                h |= h2;
            }
            u32 tic[8], tsc[8];
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, 32);
            gmem_read(tsc_base + ((h >> 20) & 0xFFF) * 32ull, tsc, 32);
            GLenum want = st->type == 2   ? GL_TEXTURE_3D
                          : st->type == 3 ? (st->array ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP)
                          : st->array     ? GL_TEXTURE_2D_ARRAY
                                          : GL_TEXTURE_2D;
            Tex* t = tex_from_tic(tic);
            GLuint id = t && t->target == want ? t->id : (t && tex_view(t, want) ? t->view : null_tex(want));
            glActiveTexture(GL_TEXTURE0 + 64 + i);
            glBindTexture(want, id);
            glBindSampler(64 + i, sampler_get(tsc));
            tcls[i] = t ? fmt_cls(t->f.ifmt) : 0;
            tbpp[i] = t && t->f.bpp ? (int)t->f.bpp : 4;
        }
        /* SUST targets */
        for (int i = 0; i < p->si.nimg && i < 8; i++) {
            u32 cb = p->si.img[i].bindless ? (p->si.img[i].bl_cbuf & 7) : tcb;
            u32 h = 0;
            cs_cb_read(cb, p->si.img[i].cbuf_off * 4, &h);
            u32 tic[8];
            gmem_read(tic_base + (h & 0xFFFFF) * 32ull, tic, 32);
            Tex* t = tex_from_tic(tic);
            GLenum f = t ? img_fmt(t->f.ifmt) : 0;
            icls[i] = t ? fmt_cls(t->f.ifmt) : 0;
            ibpp[i] = t && t->f.bpp ? (int)t->f.bpp : 4;
            if (t && f) {
                int ty = p->si.img[i].type;
                glBindImageTexture(i, t->id, 0, (ty == 2 || ty == 4 || ty == 5) ? GL_TRUE : GL_FALSE, 0, GL_WRITE_ONLY,
                                   f);
                t->gpu_written = 1;
                t->wgen++;
                nimg_ok++;
                static int said;
                if (said < 8) {
                    said++;
                    LOG("compute: writes image %ux%ux%u at %llx (format %x)\n", t->w, t->h, t->d,
                        (unsigned long long)t->gva, f);
                }
            } else {
                glBindImageTexture(i, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
                static int said;
                if (said < 4) {
                    said++;
                    LOG("compute: image %d without a usable texture (tic %x)\n", i, h);
                }
            }
        }
        { /* cache uniform locations per program; a glGet per dispatch stalls mesa_glthread */
            if (!p->loc_ok) {
                p->loc[0] = glGetUniformLocation(p->prog, "swr_ic");
                p->loc[1] = glGetUniformLocation(p->prog, "swr_ib");
                p->loc[2] = glGetUniformLocation(p->prog, "swr_tc");
                p->loc[3] = glGetUniformLocation(p->prog, "swr_tb");
                p->loc_ok = 1;
            }
            if (p->loc[0] >= 0) glUniform1iv(p->loc[0], 8, icls);
            if (p->loc[1] >= 0) glUniform1iv(p->loc[1], 8, ibpp);
            if (p->loc[2] >= 0) glUniform1iv(p->loc[2], 32, tcls);
            if (p->loc[3] >= 0) glUniform1iv(p->loc[3], 32, tbpp);
        }
        mb_reset();
    }
    /* ssbos */
    static GLuint sb[16];
    u64 sa[16];
    u32 ss[16];
    for (int i = 0; i < p->si.ngbuf && i < 16; i++) {
        if (!sb[i]) glGenBuffers(1, &sb[i]);
        u32 v[3] = {0, 0, 0};
        cs_cb_read(0, p->si.gbuf_cbuf_off[i], &v[0]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 4, &v[1]);
        cs_cb_read(0, p->si.gbuf_cbuf_off[i] + 8, &v[2]);
        sa[i] = ((u64)v[1] << 32) | v[0];
        ss[i] = v[2];
        if (!ss[i]) ss[i] = 16;
        if (ss[i] > (16u << 20)) ss[i] = 16u << 20;
        ss[i] = (ss[i] + 3) & ~3u;
        {
            GLuint gsb;
            u32 gso;
            if (sa[i] && g_cs_any && cs_gpu_src(sa[i], ss[i], &gsb, &gso)) {
                glBindBuffer(GL_COPY_WRITE_BUFFER, sb[i]);
                glBufferData(GL_COPY_WRITE_BUFFER, ss[i], NULL, GL_DYNAMIC_COPY);
                glBindBuffer(GL_COPY_READ_BUFFER, gsb);
                glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, gso, 0, ss[i]);
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, sb[i]);
                st_cs_gpucopy++;
                continue;
            }
        }
        if (sa[i] && cs_pending_overlaps(sa[i], ss[i])) {
            u64 t0 = plat_ns();
            cs_flush_pending();
            st_cs_sync++;
            st_cs_sync_ns += plat_ns() - t0;
        }
        static u8* tmp;
        static u32 cap;
        if (cap < ss[i]) {
            tmp = realloc(tmp, ss[i]);
            cap = ss[i];
        }
        if (sa[i])
            gmem_read(sa[i], tmp, ss[i]);
        else
            memset(tmp, 0, ss[i]);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sb[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, ss[i], tmp, GL_DYNAMIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, sb[i]);
    }
    /* staging space for what this dispatch writes, reserved now so the SSBOs' contents from before the dispatch can
       go into g_cs_orig at the offsets their results will take in g_cs_stage */
    u32 cs_off[16];
    if (p->si.gbuf_wmask) {
        u32 need = 0;
        int nw = 0;
        for (int i = 0; i < p->si.ngbuf && i < 16; i++)
            if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
                need += (ss[i] + 255) & ~255u;
                nw++;
            }
        if (g_cs_npend + nw > 256 || g_cs_stage_used + need > (32u << 20)) cs_flush_pending();
        if (!g_cs_stage || g_cs_stage_used + need > g_cs_stage_cap) { /* growing: read back what's pending first */
            cs_flush_pending();
            u32 nc = g_cs_stage_cap ? g_cs_stage_cap * 2 : (1u << 20);
            while (nc < need) nc *= 2;
            if (!g_cs_stage) glGenBuffers(1, &g_cs_stage);
            if (!g_cs_orig) glGenBuffers(1, &g_cs_orig);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g_cs_stage);
            glBufferData(GL_COPY_WRITE_BUFFER, nc, NULL, GL_DYNAMIC_READ);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g_cs_orig);
            glBufferData(GL_COPY_WRITE_BUFFER, nc, NULL, GL_DYNAMIC_READ);
            g_cs_stage_cap = nc;
        }
        for (int i = 0; i < p->si.ngbuf && i < 16; i++)
            if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
                cs_off[i] = g_cs_stage_used;
                g_cs_stage_used = (g_cs_stage_used + ss[i] + 255) & ~255u;
                glBindBuffer(GL_COPY_READ_BUFFER, sb[i]);
                glBindBuffer(GL_COPY_WRITE_BUFFER, g_cs_orig);
                glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, cs_off[i], ss[i]);
            }
    }
    glDispatchCompute(gx, gy, gz);
    st_cs_run++;
    if (nimg_ok) glMemoryBarrier(0x20 | 0x08 | 0x100 | 0x400 | 0x80); /* images -> textures, copies, fbos */
    trace_note("COMPUTE %016llx grid %ux%ux%u block %ux%ux%u buffers %d (writes %x)\n", (unsigned long long)key, gx, gy,
               gz, lx, ly, lz, p->si.ngbuf, p->si.gbuf_wmask);
    if (p->si.gbuf_wmask) {
        glMemoryBarrier(0x00000200 | 0x00002000 /* BUFFER_UPDATE | SHADER_STORAGE */);
        for (int i = 0; i < p->si.ngbuf && i < 16; i++)
            if (((p->si.gbuf_wmask >> i) & 1) && sa[i]) {
                glBindBuffer(GL_COPY_READ_BUFFER, sb[i]);
                glBindBuffer(GL_COPY_WRITE_BUFFER, g_cs_stage);
                glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, cs_off[i], ss[i]);
                g_cs_pend[g_cs_npend].addr = sa[i];
                g_cs_pend[g_cs_npend].off = cs_off[i];
                g_cs_pend[g_cs_npend].size = ss[i];
                g_cs_npend++;
                g_cs_any = 1;
                if (sa[i] < g_cs_lo) g_cs_lo = sa[i];
                if (sa[i] + ss[i] > g_cs_hi) g_cs_hi = sa[i] + ss[i];
            }
        /* fresh SSBOs for the next dispatch so it doesn't wait on these */
        for (int i = 0; i < p->si.ngbuf && i < 16; i++)
            if ((p->si.gbuf_wmask >> i) & 1) {
                glDeleteBuffers(1, &sb[i]);
                sb[i] = 0;
            }
    }
    glUseProgram(0);
    state_dirty();
}
