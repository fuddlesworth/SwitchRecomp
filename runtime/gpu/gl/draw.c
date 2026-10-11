/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* fixed-function state and drawing */
#include "gpu/gl/gl.h"

/* ============================================================ fixed state */
static GLenum blend_factor(u32 v) {
    if (v & 0x4000) return v & ~0x4000u;
    switch (v) {
        case 1: return GL_ZERO;
        case 2: return GL_ONE;
        case 3: return GL_SRC_COLOR;
        case 4: return GL_ONE_MINUS_SRC_COLOR;
        case 5: return GL_SRC_ALPHA;
        case 6: return GL_ONE_MINUS_SRC_ALPHA;
        case 7: return GL_DST_ALPHA;
        case 8: return GL_ONE_MINUS_DST_ALPHA;
        case 9: return GL_DST_COLOR;
        case 10: return GL_ONE_MINUS_DST_COLOR;
        case 11: return GL_SRC_ALPHA_SATURATE;
        case 14: return GL_CONSTANT_COLOR;
        case 15: return GL_ONE_MINUS_CONSTANT_COLOR;
        case 16: return GL_SRC1_COLOR;
        case 17: return GL_ONE_MINUS_SRC1_COLOR;
        case 18: return GL_SRC1_ALPHA;
        case 19: return GL_ONE_MINUS_SRC1_ALPHA;
    }
    return GL_ONE;
}
static GLenum blend_eq(u32 v) {
    if (v >= 0x8000) return v;
    switch (v) {
        case 1: return GL_FUNC_ADD;
        case 2: return GL_FUNC_SUBTRACT;
        case 3: return GL_FUNC_REVERSE_SUBTRACT;
        case 4: return GL_MIN;
        case 5: return GL_MAX;
    }
    return GL_FUNC_ADD;
}
static GLenum cmp_func(u32 v) {
    if (v >= 0x200 && v <= 0x207) return v;
    if (v >= 1 && v <= 8) return GL_NEVER + (v - 1);
    return GL_ALWAYS;
}
static GLenum stencil_op(u32 v) {
    switch (v) {
        case 1: return GL_KEEP;
        case 2: return GL_ZERO;
        case 3: return GL_REPLACE;
        case 4: return GL_INCR;
        case 5: return GL_DECR;
        case 6: return GL_INVERT;
        case 7: return GL_INCR_WRAP;
        case 8: return GL_DECR_WRAP;
        case 0: return GL_ZERO;
    }
    return v;
}

/* fixed-state signature: unchanged regs since the last draw -> skip ~70 GL calls
   (each call costs on Intel's driver and we do ~1000 draws a frame) */
static int g_state_ok;
static u32 g_state_sig[256];
u64 st_state_skip;
/* native 0..1 depth (GL 4.5 clip control). remapping z*2-w in the shader loses precision far away */
PFNGLCLIPCONTROLPROC p_clip;
static int g_clip_mode = -1;
static int g_gs_ok;
void state_dirty(void) {
    g_state_ok = 0;
    g_gs_ok = 0;
}
void apply_state(GpuChannel* ch) {
    u32* r = ch->r3d;
    {
        u32 sig[256];
        int n = 0;
#define SR(a, b)                                                                                                       \
    for (u32 q_ = (a); q_ <= (b); q_++) sig[n++] = r[q_];
        SR(0x280, 0x285)
        SR(0x300, 0x303)
        SR(0x35F, 0x35F)
        SR(0x36B, 0x36B)
        SR(0x372, 0x372)
        SR(0x380, 0x382)
        SR(0x3D5, 0x3D7)
        SR(0x3E4, 0x3E4)
        SR(0x4B3, 0x4B3)
        SR(0x4B8, 0x4BA)
        SR(0x4C3, 0x4C3)
        SR(0x4C7, 0x4CA)
        SR(0x4D0, 0x4DF)
        SR(0x4E0, 0x4E7)
        SR(0x4ED, 0x4ED)
        SR(0x546, 0x546)
        SR(0x55B, 0x55B)
        SR(0x565, 0x569)
        SR(0x56F, 0x56F)
        SR(0x591, 0x592)
        SR(0x646, 0x648)
        SR(0x680, 0x687)
        SR(0x780, 0x7BF)
#undef SR
        sig[n++] = g_zt ? 1u : 0u;
        memcpy(&sig[n++], &g_scale, 4);
        if (g_state_ok && !memcmp(sig, g_state_sig, (size_t)n * 4)) {
            st_state_skip++;
            return;
        }
        memcpy(g_state_sig, sig, (size_t)n * 4);
        g_state_ok = 1;
    }
    if (p_clip) {
        int m = r[0x35F] & 1;
        if (m != g_clip_mode) {
            p_clip(0x8CA1 /* LOWER_LEFT */, m ? 0x935F /* ZERO_TO_ONE */ : 0x935E /* NEGATIVE_ONE_TO_ONE */);
            g_clip_mode = m;
        }
    }
    /* grouped so only what changed hits GL */
    static u32 gs[16][20];
    int ok = g_gs_ok;
    g_gs_ok = 1;
#define GRP(id_, ...)                                                                                                  \
    u32 k_##id_[] = {__VA_ARGS__};                                                                                     \
    if (!ok || memcmp(gs[id_], k_##id_, sizeof k_##id_))                                                               \
        if (memcpy(gs[id_], k_##id_, sizeof k_##id_))
    u32 gsc;
    memcpy(&gsc, &g_scale, 4);
    float vt[6];
    memcpy(vt, &r[0x280], 24);
    float sx = vt[0], sy = vt[1], tx = vt[3], ty = vt[4];
    {
        GRP(0, r[0x280], r[0x281], r[0x282], r[0x283], r[0x284], r[0x285], r[0x35F], r[0x300], r[0x301], r[0x302],
            r[0x303], r[0x380], r[0x381], r[0x382], gsc) {
            float x0 = tx - fabsf_(sx), y0 = ty - fabsf_(sy), w = 2 * fabsf_(sx), h = 2 * fabsf_(sy);
            if (w < 1 || h < 1) {
                u32 vx = r[0x300], vy = r[0x301];
                x0 = (float)(vx & 0xFFFF);
                w = (float)(vx >> 16);
                y0 = (float)(vy & 0xFFFF);
                h = (float)(vy >> 16);
            }
            glViewportIndexedf(0, x0 * g_scale, y0 * g_scale, w * g_scale, h * g_scale);
            float n, f;
            memcpy(&n, &r[0x302], 4);
            memcpy(&f, &r[0x303], 4);
            /* depth goes through the viewport transform (z*scale_z+translate_z) like on hw / yuzu's vk backend.
       SMO's fog pass draws at z=1 with NOTEQUAL to skip the sky, so this has to be exact */
            if ((vt[2] != 0.0f || vt[5] != 0.0f) && !getenv("SWR_DEPTH_REGS")) {
                float red = (r[0x35F] & 1) ? 0.0f : 1.0f;
                n = vt[5] - vt[2] * red;
                f = vt[5] + vt[2];
                if (fabsf_(n) < 1e-6f) n = 0.0f;
                if (fabsf_(n - 1.0f) < 1e-6f) n = 1.0f;
                if (fabsf_(f) < 1e-6f) f = 0.0f;
                if (fabsf_(f - 1.0f) < 1e-6f) f = 1.0f;
            }
            glDepthRangeIndexed(0, n, f);
            if (r[0x380] & 1) {
                u32 xs = r[0x381], ys = r[0x382];
                glEnable(GL_SCISSOR_TEST);
                glScissorIndexed(0, (GLint)((xs & 0xFFFF) * g_scale), (GLint)((ys & 0xFFFF) * g_scale),
                                 (GLsizei)(((xs >> 16) - (xs & 0xFFFF)) * g_scale + 0.5f),
                                 (GLsizei)(((ys >> 16) - (ys & 0xFFFF)) * g_scale + 0.5f));
            } else
                glDisable(GL_SCISSOR_TEST);
        }
    }
    /* raster state (winding flips with the Y flip) */
    {
        float w = 2 * fabsf_(sx), h = 2 * fabsf_(sy);
        int yflip = (w < 1 || h < 1) ? 0 : (sy < 0);
        GRP(1, r[0x646], r[0x647], r[0x648], r[0x36B], r[0x372], r[0x55B], r[0x56F], (u32)yflip) {
            if (r[0x646]) {
                glEnable(GL_CULL_FACE);
                glCullFace(r[0x648] == 0x404 ? GL_FRONT : r[0x648] == 0x408 ? GL_FRONT_AND_BACK : GL_BACK);
            } else
                glDisable(GL_CULL_FACE);
            {
                GLenum ff = r[0x647] == 0x900 ? GL_CW : GL_CCW;
                if (yflip) ff = ff == GL_CW ? GL_CCW : GL_CW;
                glFrontFace(ff);
            }
            glPolygonMode(GL_FRONT_AND_BACK, r[0x36B] == 0x1B01 ? GL_LINE : r[0x36B] == 0x1B00 ? GL_POINT : GL_FILL);
            if (r[0x372]) {
                float fac, un;
                memcpy(&fac, &r[0x55B], 4);
                memcpy(&un, &r[0x56F], 4);
                glEnable(GL_POLYGON_OFFSET_FILL);
                glPolygonOffset(fac, un);
            } else
                glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }
    /* depth / stencil */
    {
        u32 z = g_zt ? 1u : 0u;
        GRP(2, z, r[0x4B3], r[0x4C3], r[0x4BA], r[0x4E0], r[0x4E1], r[0x4E2], r[0x4E3], r[0x4E4], r[0x4E5], r[0x4E6],
            r[0x4E7], r[0x565], r[0x566], r[0x567], r[0x568], r[0x569], r[0x3D5], r[0x3D6], r[0x3D7]) {
            if (r[0x4B3] && g_zt) {
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(cmp_func(r[0x4C3]));
            } else
                glDisable(GL_DEPTH_TEST);
            glDepthMask(r[0x4BA] && g_zt ? GL_TRUE : GL_FALSE);
            if (r[0x4E0] && g_zt) {
                glEnable(GL_STENCIL_TEST);
                glStencilFuncSeparate(GL_FRONT, cmp_func(r[0x4E4]), r[0x4E5], r[0x4E6]);
                glStencilOpSeparate(GL_FRONT, stencil_op(r[0x4E1]), stencil_op(r[0x4E2]), stencil_op(r[0x4E3]));
                glStencilMaskSeparate(GL_FRONT, r[0x4E7]);
                if (r[0x565]) {
                    glStencilFuncSeparate(GL_BACK, cmp_func(r[0x569]), r[0x3D5], r[0x3D7]);
                    glStencilOpSeparate(GL_BACK, stencil_op(r[0x566]), stencil_op(r[0x567]), stencil_op(r[0x568]));
                    glStencilMaskSeparate(GL_BACK, r[0x3D6]);
                } else {
                    glStencilFuncSeparate(GL_BACK, cmp_func(r[0x4E4]), r[0x4E5], r[0x4E6]);
                    glStencilOpSeparate(GL_BACK, stencil_op(r[0x4E1]), stencil_op(r[0x4E2]), stencil_op(r[0x4E3]));
                    glStencilMaskSeparate(GL_BACK, r[0x4E7]);
                }
            } else
                glDisable(GL_STENCIL_TEST);
        }
    }
    /* blend + color masks */
    {
        GRP(3, r[0x4B8] & 1, r[0x4C7], r[0x4C8], r[0x4C9], r[0x4CA]) {
            if (r[0x4B8] & 1)
                glEnable(GL_FRAMEBUFFER_SRGB);
            else
                glDisable(GL_FRAMEBUFFER_SRGB); /* framebuffer_srgb */
            float bc[4];
            memcpy(bc, &r[0x4C7], 16);
            glBlendColor(bc[0], bc[1], bc[2], bc[3]);
        }
    }
    for (int i = 0; i < 8; i++) {
        u32 cm = r[0x3E4] ? r[0x680] : r[0x680 + i];
        int en = r[0x4D8 + i];
        u32* b = &r[0x780 + i * 8];
        u32 k[12] = {cm & 0xFFFF, (u32)en, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        if (en) {
            k[2] = r[0x4B9];
            if (r[0x4B9]) {
                k[3] = b[1];
                k[4] = b[2];
                k[5] = b[3];
                k[6] = b[4];
                k[7] = b[5];
                k[8] = b[6];
            } else {
                k[3] = r[0x4D0];
                k[4] = r[0x4D1];
                k[5] = r[0x4D2];
                k[6] = r[0x4D3];
                k[7] = r[0x4D4];
                k[8] = r[0x4D6];
            }
        }
        if (ok && !memcmp(gs[4 + i], k, sizeof k)) continue;
        memcpy(gs[4 + i], k, sizeof k);
        glColorMaski(i, (cm & 0xF) != 0, (cm & 0xF0) != 0, (cm & 0xF00) != 0, (cm & 0xF000) != 0);
        if (en) {
            glEnablei(GL_BLEND, i);
            /* hw always uses the alpha fields, separate_alpha doesn't disable them (yuzu agrees). menus' alpha masks need this */
            if (r[0x4B9]) {
                glBlendEquationSeparatei(i, blend_eq(b[1]), blend_eq(b[4] ? b[4] : b[1]));
                glBlendFuncSeparatei(i, blend_factor(b[2]), blend_factor(b[3]), blend_factor(b[5] ? b[5] : b[2]),
                                     blend_factor(b[6] ? b[6] : b[3]));
            } else {
                glBlendEquationSeparatei(i, blend_eq(r[0x4D0]), blend_eq(r[0x4D3] ? r[0x4D3] : r[0x4D0]));
                glBlendFuncSeparatei(i, blend_factor(r[0x4D1]), blend_factor(r[0x4D2]),
                                     blend_factor(r[0x4D4] ? r[0x4D4] : r[0x4D1]),
                                     blend_factor(r[0x4D6] ? r[0x4D6] : r[0x4D2]));
            }
        } else
            glDisablei(GL_BLEND, i);
    }
    {
        GRP(12, r[0x591], r[0x592], r[0x4ED], r[0x546]) {
            if (r[0x591]) {
                glEnable(GL_PRIMITIVE_RESTART);
                glPrimitiveRestartIndex(r[0x592]);
            } else
                glDisable(GL_PRIMITIVE_RESTART);
            {
                float lw;
                memcpy(&lw, &r[0x4ED], 4);
                if (lw > 0 && lw < 64) glLineWidth(lw);
            }
            {
                float ps;
                memcpy(&ps, &r[0x546], 4);
                if (ps > 0 && ps < 256) glPointSize(ps);
            }
        }
    }
#undef GRP
}

/* ============================================================ drawing */
u32 attr_count(u32 size) {
    switch (size) {
        case 0x01: return 4;
        case 0x02: return 3;
        case 0x03: return 4;
        case 0x04: return 2;
        case 0x05: return 3;
        case 0x0A: return 4;
        case 0x0F: return 2;
        case 0x12: return 1;
        case 0x13: return 3;
        case 0x18: return 2;
        case 0x1B: return 1;
        case 0x1D: return 1;
        case 0x30: return 4;
        case 0x31: return 3;
    }
    return 4;
}
u32 attr_bits(u32 size) {
    switch (size) {
        case 0x01:
        case 0x02:
        case 0x04:
        case 0x12: return 32;
        case 0x03:
        case 0x05:
        case 0x0F:
        case 0x1B: return 16;
        case 0x30:
        case 0x31: return 0;
    }
    return 8;
}
GLenum topo(u32 t) {
    switch (t) {
        case 0: return GL_POINTS;
        case 1: return GL_LINES;
        case 2: return GL_LINE_LOOP;
        case 3: return GL_LINE_STRIP;
        case 4: return GL_TRIANGLES;
        case 5: return GL_TRIANGLE_STRIP;
        case 6: return GL_TRIANGLE_FAN;
        case 10: return GL_LINES_ADJACENCY;
        case 11: return GL_LINE_STRIP_ADJACENCY;
        case 12: return GL_TRIANGLES_ADJACENCY;
        case 13: return GL_TRIANGLE_STRIP_ADJACENCY;
        case 14: return GL_PATCHES;
    }
    return GL_TRIANGLES;
}

void glr_cb_bind(GpuChannel* ch, int stage, u32 v) {
    u32* r = ch->r3d;
    u32 idx = (v >> 4) & 31;
    if (stage > 4 || idx >= 18) return;
    if (v & 1) {
        g_cb.cb[stage][idx] = ((u64)r[0x8E1] << 32) | r[0x8E2];
        g_cb.cbsize[stage][idx] = r[0x8E0];
    } else {
        g_cb.cb[stage][idx] = 0;
        g_cb.cbsize[stage][idx] = 0;
    }
}

/* full draw capture to screenshots/draw_N/ for replaying elsewhere */
static void cap_write(const char* dir, const char* name, const void* d, size_t n) {
    char p[300];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE* f = fopen(p, "wb");
    if (f) {
        fwrite(d, 1, n, f);
        fclose(f);
    }
}
static void cap_mem(const char* dir, const char* name, u64 gva, u64 n) {
    if (!gva || !n) return;
    if (n > (1u << 20)) n = 1u << 20;
    u8* b = malloc(n);
    gmem_read(gva, b, n);
    cap_write(dir, name, b, n);
    free(b);
}
void capture_draw(GpuChannel* ch, int indexed, u32 first, u32 count) {
    static int ncap;
    if (ncap >= 8) return;
    ncap++;
    u32* r = ch->r3d;
    char dir[128];
    plat_mkdir("captures");
    snprintf(dir, sizeof dir, "captures/draw_%d", ncap);
    plat_mkdir(dir);
    cap_write(dir, "regs.bin", r, 0x1000 * 4);
    {
        char info[256];
        int n = snprintf(info, sizeof info, "indexed=%d first=%u count=%u\n", indexed, first, count);
        cap_write(dir, "draw.txt", info, (size_t)n);
    }
    u64 code = ((u64)r[0x582] << 32) | r[0x583];
    for (int i = 0; i < 6; i++) {
        u32 cfg = r[0x800 + i * 0x10];
        if (i != 1 && !(cfg & 1)) continue;
        char nm[32];
        snprintf(nm, sizeof nm, "shader%d.bin", i);
        cap_mem(dir, nm, code + r[0x801 + i * 0x10], 0x8000);
        u8* cb = malloc(0x8000);
        gmem_read(code + r[0x801 + i * 0x10], cb, 0x8000);
        ShaderInfo si;
        memset(&si, 0, sizeof si);
        char* g = shader_to_glsl(cb, 0x8000, i == 1 ? ST_VS : i == 5 ? ST_FS : ST_GS, &si);
        if (g) {
            snprintf(nm, sizeof nm, "shader%d.glsl", i);
            cap_write(dir, nm, g, strlen(g));
            free(g);
        }
        free(cb);
    }
    for (int st = 0; st < 5; st++)
        for (int i = 0; i < 18; i++)
            if (g_cb.cb[st][i]) {
                char nm[32];
                snprintf(nm, sizeof nm, "cb_%d_%d.bin", st, i);
                u32 sz = g_cb.cbsize[st][i];
                cap_mem(dir, nm, g_cb.cb[st][i], sz ? sz : 16);
            }
    for (int b = 0; b < 16; b++) {
        u32* va = &r[0x700 + b * 4];
        if (!(va[0] & 0x1000)) continue;
        u64 start = ((u64)va[1] << 32) | va[2];
        u64 lim = ((u64)r[0x7C0 + b * 2] << 32) | r[0x7C1 + b * 2];
        char nm[32];
        snprintf(nm, sizeof nm, "vb_%d.bin", b);
        if (lim >= start) cap_mem(dir, nm, start, lim - start + 1);
    }
    if (indexed) {
        u32 fmt = r[0x5F6];
        u32 isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        u64 ia = ((u64)r[0x5F2] << 32) | r[0x5F3];
        cap_mem(dir, "ib.bin", ia, (u64)(first + count) * isz);
    }
    {
        u64 tb = ((u64)r[0x55D] << 32) | r[0x55E], sb = ((u64)r[0x557] << 32) | r[0x558];
        cap_mem(dir, "tic.bin", tb, 0x8000);
        cap_mem(dir, "tsc.bin", sb, 0x4000);
    }
    LOG("gpu: draw captured in %s\n", dir);
}
/* NVN ssbo table at c0[0x110+16k]: u64 addr + u32 size, for LDG */
#ifndef GL_SHADER_STORAGE_BUFFER
#define GL_SHADER_STORAGE_BUFFER 0x90D2
#endif
static GLuint g_sbuf[3][8];
void bind_gbufs(int stage, Sh* s) {
    if (!s->si.ngbuf) return;
    int slot = stage == 0 ? 0 : stage == 4 ? 1 : 2;
    u64 cb0 = g_cb.cb[stage][0];
    static u8* tmp;
    static size_t cap;
    for (int i = 0; i < s->si.ngbuf && i < 8; i++) {
        if (!g_sbuf[slot][i]) glGenBuffers(1, &g_sbuf[slot][i]);
        u64 addr = 0;
        u32 size = 16;
        if (cb0) {
            u32 v[3];
            gmem_read(cb0 + s->si.gbuf_cbuf_off[i], v, 12);
            addr = ((u64)v[1] << 32) | v[0];
            size = v[2];
        }
        if (!size) size = 16;
        if (size > (16u << 20)) size = 16u << 20;
        size = (size + 3) & ~3u;
        {
            static int nc = -1;
            if (nc < 0) nc = getenv("SWR_NO_SSBO_CACHE") != NULL;
            if (!nc && addr && !((s->si.gbuf_wmask >> i) & 1)) {
                GLuint id = buf_cached(GL_SHADER_STORAGE_BUFFER, addr, size);
                if (id) {
                    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot * 8 + i, id);
                    continue;
                }
            }
        }
        if (cap < size) {
            tmp = realloc(tmp, size);
            cap = size;
        }
        if (addr)
            gmem_read(addr, tmp, size);
        else
            memset(tmp, 0, size);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, g_sbuf[slot][i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, size, tmp, GL_STREAM_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot * 8 + i, g_sbuf[slot][i]);
    }
}
/* persistent ring for cbufs (GL 4.4 buffer_storage): memcpy + glBindBufferRange, no 64K realloc per update.
   fences per segment so we never overwrite something the GPU is still reading */
#define UR_SEGS 4
static GLuint g_ur;
u8* g_ur_ptr;
static u32 g_ur_size, g_ur_head, g_ur_align = 256;
static u64 g_ur_abs;
static GLsync g_ur_f[UR_SEGS];
PFNGLFENCESYNCPROC p_fs;
PFNGLCLIENTWAITSYNCPROC p_cws;
PFNGLDELETESYNCPROC p_ds;
u64 st_ur_wait;
u32 g_ub_bound[64];
void ur_init(void) {
    if (g_gles || getenv("SWR_NO_UBO_RING")) return;
    PFNGLBUFFERSTORAGEPROC bs = (PFNGLBUFFERSTORAGEPROC)glq_proc("glBufferStorage");
    PFNGLMAPBUFFERRANGEPROC mbr = (PFNGLMAPBUFFERRANGEPROC)glq_proc("glMapBufferRange");
    p_fs = (PFNGLFENCESYNCPROC)glq_proc("glFenceSync");
    p_cws = (PFNGLCLIENTWAITSYNCPROC)glq_proc("glClientWaitSync");
    p_ds = (PFNGLDELETESYNCPROC)glq_proc("glDeleteSync");
    if (!bs || !mbr || !p_fs || !p_cws || !p_ds) return;
    GLint al = 256;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &al);
    if (al < 16) al = 16;
    g_ur_align = (u32)al;
    g_ur_size = 32u << 20;
    GLuint b = 0;
    glGenBuffers(1, &b);
    glBindBuffer(GL_UNIFORM_BUFFER, b);
    GLbitfield fl = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    bs(GL_UNIFORM_BUFFER, (GLsizeiptr)g_ur_size + 65536, NULL, fl);
    u8* ptr = glGetError() == GL_NO_ERROR ? (u8*)mbr(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)g_ur_size + 65536, fl) : NULL;
    while (glGetError() != GL_NO_ERROR) {}
    if (!ptr) return;
    g_ur = b;
    g_ur_ptr = ptr;
}
static u32 ur_alloc(u32 sz) {
    u32 need = (sz + g_ur_align - 1) / g_ur_align * g_ur_align, seg = g_ur_size / UR_SEGS;
    u32 off = g_ur_head;
    if (off + need > g_ur_size) {
        g_ur_abs += g_ur_size - off;
        off = 0;
    }
    u32 cs = g_ur_head / seg;
    if (cs >= UR_SEGS) cs = UR_SEGS - 1;
    u32 es = (off + need - 1) / seg;
    if (es >= UR_SEGS) es = UR_SEGS - 1;
    while (cs != es) {
        if (g_ur_f[cs]) p_ds(g_ur_f[cs]);
        g_ur_f[cs] = p_fs(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        cs = (cs + 1) % UR_SEGS;
        if (g_ur_f[cs]) {
            GLenum rr = p_cws(g_ur_f[cs], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (rr == GL_TIMEOUT_EXPIRED) {
                st_ur_wait++;
                p_cws(g_ur_f[cs], GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
            }
            p_ds(g_ur_f[cs]);
            g_ur_f[cs] = 0;
        }
    }
    g_ur_head = off + need;
    g_ur_abs += need;
    return off;
}
typedef struct {
    u64 a, gen, abs;
    u32 sz, epoch, off;
} UrC;
static UrC urc[4096];
static void upload_cbufs_ring(int stage, Sh* s) {
    int bb = (stage == 0 ? 0 : stage == 4 ? 1 : 2) * 18;
    for (int i = 0; i < 18; i++) {
        if (!(s->si.cbuf_mask & (1u << i))) continue;
        u64 a = g_cb.cb[stage][i];
        u32 sz = g_cb.cbsize[stage][i];
        if (sz > 65536) sz = 65536;
        if (!sz) sz = 16;
        if (s->si.cbuf_max[i] && sz > s->si.cbuf_max[i]) sz = s->si.cbuf_max[i]; /* only the bytes the shader reads */
        UrC* e = &urc[(u32)((a >> 4) ^ (a >> 16) ^ (a >> 28)) & 4095];
        u32 off;
        /* same submit/address, not GPU-written, ring hasn't wrapped: reuse */
        if (a && e->a == a && e->epoch == g_submit_epoch && e->sz >= sz && g_ur_abs - e->abs < g_ur_size / 2 &&
            !gpu_mem_dirty_since(e->gen, a, sz)) {
            off = e->off;
            st_cb_skip++;
        } else {
            st_cb_bytes += sz;
            off = ur_alloc(sz);
            u8* dst = g_ur_ptr + off;
            u8* src = a ? gmem_ptr(a, sz) : NULL;
            if (src)
                memcpy(dst, src, sz);
            else if (a)
                gmem_read(a, dst, sz);
            else
                memset(dst, 0, sz);
            if (a) {
                e->a = a;
                e->sz = sz;
                e->epoch = g_submit_epoch;
                e->gen = g_gw_gen;
                e->off = off;
                e->abs = g_ur_abs;
            }
        }
        if (g_ub_bound[bb + i] != off + 1) {
            glBindBufferRange(GL_UNIFORM_BUFFER, bb + i, g_ur, off, 65536);
            g_ub_bound[bb + i] = off + 1;
        }
    }
}
void upload_cbufs(int stage, Sh* s) {
    if (g_ur_ptr) {
        upload_cbufs_ring(stage, s);
        return;
    }
    for (int i = 0; i < 18; i++) {
        if (!(s->si.cbuf_mask & (1u << i))) continue;
        u64 a = g_cb.cb[stage][i];
        u32 sz = g_cb.cbsize[stage][i];
        if (sz > 65536) sz = 65536;
        if (!sz) sz = 16;
        if (s->si.cbuf_max[i] && sz > s->si.cbuf_max[i]) sz = s->si.cbuf_max[i]; /* only the bytes the shader reads */
        static u8* last[5][18];
        static u32 last_sz[5][18];
        if (!last[stage][i]) last[stage][i] = calloc(1, 65536);
        { /* same submit/address, not GPU-written: unchanged (games don't write while the GPU reads) */
            static u64 la[5][18], lg[5][18], le[5][18];
            if (a && la[stage][i] == a && le[stage][i] == g_submit_epoch && last_sz[stage][i] >= sz &&
                !gpu_mem_dirty_since(lg[stage][i], a, sz)) {
                st_cb_skip++;
                glBindBufferBase(GL_UNIFORM_BUFFER, (stage == 0 ? 0 : stage == 4 ? 1 : 2) * 18 + i, g_ubuf[stage][i]);
                continue;
            }
            la[stage][i] = a;
            lg[stage][i] = g_gw_gen;
            le[stage][i] = g_submit_epoch;
        }
        st_cb_bytes += sz;
        u8* src = a ? gmem_ptr(a, sz) : NULL;
        u8 tmp_local[256];
        u8* tmp = NULL;
        if (!src) {
            static u8 big[65536];
            tmp = sz <= 256 ? tmp_local : big;
            if (a)
                gmem_read(a, tmp, sz);
            else
                memset(tmp, 0, sz);
            src = tmp;
        }
        if (last_sz[stage][i] < sz || memcmp(last[stage][i], src, sz)) { /* exact compare */
            memcpy(last[stage][i], src, sz);
            last_sz[stage][i] = sz;
            glBindBuffer(GL_UNIFORM_BUFFER, g_ubuf[stage][i]);
            glBufferData(GL_UNIFORM_BUFFER, 65536, NULL, GL_STREAM_DRAW);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sz, src);
        }
        glBindBufferBase(GL_UNIFORM_BUFFER, (stage == 0 ? 0 : stage == 4 ? 1 : 2) * 18 + i, g_ubuf[stage][i]);
    }
}
PFN_BT_ p_bt, p_bs;
static int g_scratch_unit = -1;
static GLuint g_tb_last[3][32], g_sb_last[3][32];
static int g_tb_n[3], g_tb_ok[3];
u64 st_tb_skip;
void mb_init(void) {
    if (g_gles || getenv("SWR_NO_MULTIBIND")) return;
    GLint mu = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &mu);
    if (mu < 65) return;
    p_bt = (PFN_BT_)glq_proc("glBindTextures");
    p_bs = (PFN_BT_)glq_proc("glBindSamplers");
    if (!p_bt || !p_bs) {
        p_bt = p_bs = NULL;
        return;
    }
    g_scratch_unit = mu - 1;
    glActiveTexture(GL_TEXTURE0 + g_scratch_unit);
}
/* sampler type mismatch (2D vs array, cube vs array): use a texture view */
PFNGLTEXTUREVIEWPROC p_tview;
static u64 st_tex_view;
GLuint tex_view(Tex* t, GLenum want) {
    if (!p_tview || !t) return 0;
    if (t->view && t->view_target == want) return t->view;
    GLenum a = t->target;
    u32 layers = a == GL_TEXTURE_2D ? 1 : a == GL_TEXTURE_CUBE_MAP ? 6 : t->d;
    u32 nl;
    int ok2d =
        a == GL_TEXTURE_2D || a == GL_TEXTURE_2D_ARRAY || a == GL_TEXTURE_CUBE_MAP || a == GL_TEXTURE_CUBE_MAP_ARRAY;
    if (!ok2d) return 0;
    if (want == GL_TEXTURE_2D)
        nl = 1;
    else if (want == GL_TEXTURE_2D_ARRAY)
        nl = layers;
    else if (want == GL_TEXTURE_CUBE_MAP) {
        if (layers < 6) return 0;
        nl = 6;
    } else if (want == GL_TEXTURE_CUBE_MAP_ARRAY) {
        if (layers < 6) return 0;
        nl = layers / 6 * 6;
    } else
        return 0;
    if (t->f.fmt == 0 && !t->f.compressed) return 0;
    if (t->view) {
        glDeleteTextures(1, &t->view);
        t->view = 0;
        g_fb_sig_ok = 0;
    }
    GLuint v = 0;
    glGenTextures(1, &v);
    p_tview(v, want, t->id, t->f.ifmt, 0, t->levels, 0, nl);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &v);
        return 0;
    }
    t->view = v;
    t->view_target = want;
    st_tex_view++;
    t->vswz[0] = GL_RED;
    t->vswz[1] = GL_GREEN;
    t->vswz[2] = GL_BLUE;
    t->vswz[3] = GL_ALPHA;
    return v;
}
void mb_reset(void) {
    g_tb_ok[0] = g_tb_ok[1] = g_tb_ok[2] = 0;
    if (g_scratch_unit >= 0) glActiveTexture(GL_TEXTURE0 + g_scratch_unit);
}
static int ifmt_signed(GLenum f) {
    return f == GL_RGBA8I || f == GL_RGBA16I || f == GL_RGBA32I || f == GL_RG8I || f == GL_RG16I || f == GL_RG32I ||
           f == GL_R8I || f == GL_R16I || f == GL_R32I;
}
/* FS key: integer textures (u/isampler) and integer RT outputs (u/ivec4) */
/* texture handle for a slot: from the texture cbuf, or for bindless from whatever bank the shader read.
   split handles (tex | sampler) are combined the same way the shader does it */
u32 sh_tex_handle(int stage, u32 tcb, const ShTex* st) {
    u64 cba = g_cb.cb[stage][(st->bindless ? st->bl_cbuf : tcb) & 31];
    u32 h = cba ? gmem_r32(cba + st->cbuf_off * 4) : 0;
    if (st->bindless && st->bl2) {
        u64 c2 = g_cb.cb[stage][st->bl2_cbuf & 31];
        if (c2) h |= gmem_r32(c2 + st->bl2_off * 4);
    }
    return h;
}
Sh* fs_get_keyed(GpuChannel* ch, u64 addr) {
    u32* r = ch->r3d;
    u32 ctl = r[0x487];
    u32 count = ctl & 15;
    if (count > 8) count = 8;
    u32 rm = 0, rs = 0;
    for (u32 i = 0; i < count; i++) {
        u32 idx = (ctl >> (4 + i * 3)) & 7;
        u32* rt = &r[0x200 + idx * 0x10];
        if (!rt[4]) continue;
        GlFmt f = rt_format(rt[4]);
        if (f.is_int) {
            rm |= 1u << i;
            if (ifmt_signed(f.ifmt)) rs |= 1u << i;
        }
    }
    g_fs_rtk = 0;
    g_fs_rtm = 0;
    g_fs_rts = 0;
    Sh* s0 = shader_get(addr, 0, ST_FS, 0, 0);
    if (!s0) return s0;
    u32 tm = 0, ts = 0;
    int nt = s0->si.ntex;
    if (nt > 32) nt = 32;
    if (nt) {
        u32 tcb = r[0x982];
        u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E];
        u64 cba = g_cb.cb[4][tcb & 31];
        for (int i = 0; i < nt; i++) {
            ShTex* st = &s0->si.tex[i];
            if (st->shadow) continue;
            u32 handle = sh_tex_handle(4, tcb, st);
            (void)cba;
            u32 tic[8];
            gmem_read(tic_base + (handle & 0xFFFFF) * 32ull, tic, 32);
            u32 ty = (tic[0] >> 7) & 7;
            Tex* t = tex_from_tic(tic);
            if (t ? t->f.is_int : (ty == 3 || ty == 4)) {
                tm |= 1u << i;
                if ((t && ifmt_signed(t->f.ifmt)) || (!t && ty == 3)) ts |= 1u << i;
            }
        }
    }
    rm &= s0->si.sph[18] ? 0xFF : 0;
    if (!tm && !rm) return s0;
    g_fs_rtm = rm;
    g_fs_rts = rs;
    g_fs_rtk = rm * 0x27D4EB2Fu ^ rs * 0x165667B1u ^ 0x5A5A0000u;
    Sh* s = shader_get(addr, 0, ST_FS, tm, ts);
    g_fs_rtk = 0;
    g_fs_rtm = 0;
    g_fs_rts = 0;
    return s;
}
/* transparent 0,0,0,0 for bad TICs; an empty GL unit returns opaque black */
GLuint null_tex(GLenum tg) {
    static GLuint n2, na, nc, nca;
    static const u8 z[4 * 6] = {0};
    GLuint* p = tg == GL_TEXTURE_2D               ? &n2
                : tg == GL_TEXTURE_2D_ARRAY       ? &na
                : tg == GL_TEXTURE_CUBE_MAP       ? &nc
                : tg == GL_TEXTURE_CUBE_MAP_ARRAY ? &nca
                                                  : NULL;
    if (!p) return 0;
    if (*p) return *p;
    glGenTextures(1, p);
    glBindTexture(tg, *p);
    if (tg == GL_TEXTURE_2D) {
        glTexStorage2D(tg, 1, GL_RGBA8, 1, 1);
        glTexSubImage2D(tg, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, z);
    } else if (tg == GL_TEXTURE_CUBE_MAP) {
        glTexStorage2D(tg, 1, GL_RGBA8, 1, 1);
        for (int f = 0; f < 6; f++)
            glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, z);
    } else {
        int d = tg == GL_TEXTURE_CUBE_MAP_ARRAY ? 6 : 1;
        glTexStorage3D(tg, 1, GL_RGBA8, 1, 1, d);
        glTexSubImage3D(tg, 0, 0, 0, 0, 1, 1, d, GL_RGBA, GL_UNSIGNED_BYTE, z);
    }
    mb_reset();
    return *p;
}
static int getenv_cached_rtswz(void) {
    static int v = -1;
    if (v < 0) v = getenv("SWR_OLD_RT_SWIZZLE") != NULL;
    return v;
} /* 1 = old behaviour, ignore reorders on RTs */
void bind_textures(GpuChannel* ch, int stage, Sh* s) {
    u32* r = ch->r3d;
    u32 tcb = r[0x982];
    u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E], tsc_base = ((u64)r[0x557] << 32) | r[0x558];
    int mb = p_bt != NULL, slot = (stage == 0 ? 0 : stage == 4 ? 1 : 2), nt = s->si.ntex;
    if (nt > 32) nt = 32;
    GLuint tids[32], sids[32];
    for (int i = 0; i < nt; i++) {
        ShTex* st = &s->si.tex[i];
        u32 handle = sh_tex_handle(stage, tcb, st);
        u32 tic_id = handle & 0xFFFFF, tsc_id = (handle >> 20) & 0xFFF;
        u32 tic[8], tsc[8];
        gmem_read(tic_base + tic_id * 32ull, tic, 32);
        gmem_read(tsc_base + tsc_id * 32ull, tsc, 32);
        Tex* t = tex_from_tic(tic);
        int unit = (stage == 0 ? 0 : stage == 4 ? 1 : 2) * (g_gles ? 16 : 32) + i;
        if (!mb) glActiveTexture(GL_TEXTURE0 + unit);
        /* the shader's sampler type wins */
        GLenum want = st->type == 2   ? GL_TEXTURE_3D
                      : st->type == 3 ? (st->array ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_CUBE_MAP)
                      : st->array     ? GL_TEXTURE_2D_ARRAY
                                      : GL_TEXTURE_2D;
        tids[i] = 0;
        if (t && t->target == want && !(want == GL_TEXTURE_3D && t->zero3d && !t->is_rt)) {
            if (mb)
                tids[i] = t->id;
            else
                glBindTexture(want, t->id);
            if (t->is_rt)
                st_tex_rt++;
            else
                st_tex_ok++;
            if (want == GL_TEXTURE_3D) {
                static u64 seen3[16];
                static int n3;
                int k = 0;
                for (; k < n3; k++)
                    if (seen3[k] == t->gva) break;
                if (k == n3 && n3 < 16) {
                    seen3[n3++] = t->gva;
                    LOG("gpu: 3D texture %ux%ux%u at %llx %s\n", t->w, t->h, t->d, (unsigned long long)t->gva,
                        t->is_rt ? "rendered by the GPU (3D render target)" : "read from memory (has data)");
                }
            }
        } else if (
            want ==
            GL_TEXTURE_3D) { /* 3D LUT with no data (its writer isn't emulated yet): hand out a neutral table instead of black */
            /* identity table at the LUT's size. an sRGB->linear curve is the theoretically right thing for float
         LUTs but compared to real hw it came out much too dark; identity is close. SWR_LUT_LINEAR=1 for the curve */
            int lin_out = 0;
            static int force_lin = -1;
            if (force_lin < 0) force_lin = getenv("SWR_LUT_LINEAR") != NULL;
            if (force_lin && t &&
                (t->f.ifmt == GL_R11F_G11F_B10F || t->f.ifmt == GL_RGBA16F || t->f.ifmt == GL_RGBA32F))
                lin_out = 1;
            /* must match the LUT's real size: the shader samples texel centers (c*(N-1)+0.5)/N, so a 32^3 read as 8^3
         lifts black to ~6% gray */
            int NN = t && t->w >= 2 && t->w <= 64 && t->w == t->h && t->d == t->w ? (int)t->w : 32;
            static GLuint lut_tex[65][2];
            GLuint* lt = &lut_tex[NN][lin_out];
            if (!*lt) {
                const int N = NN;
                float* px = malloc((size_t)N * N * N * 4 * 4);
                for (int z = 0; z < N; z++)
                    for (int y = 0; y < N; y++)
                        for (int x = 0; x < N; x++) {
                            float* q = px + ((size_t)(z * N + y) * N + x) * 4;
                            int c3[3] = {x, y, z};
                            for (int k = 0; k < 3; k++) {
                                float v = (float)c3[k] / (float)(N - 1);
                                if (lin_out) v = v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
                                q[k] = v;
                            }
                            q[3] = 1.f;
                        }
                glGenTextures(1, lt);
                glBindTexture(GL_TEXTURE_3D, *lt);
                glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA16F, N, N, N);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, N, N, N, GL_RGBA, GL_FLOAT, px);
                free(px);
            }
            if (mb)
                tids[i] = *lt;
            else
                glBindTexture(GL_TEXTURE_3D, *lt);
            st_tex_mismatch++;
            {
                static u64 seenz[8];
                static int nz;
                int k = 0;
                u64 g = t ? t->gva : 0;
                for (; k < nz; k++)
                    if (seenz[k] == g) break;
                if (k == nz && nz < 8) {
                    seenz[nz++] = g;
                    LOG("gpu: 3D texture (color LUT) %ux%ux%u at %llx (fmt %x) has no data: using a %s LUT (SWR_LUT_LINEAR=1: sRGB->linear)\n",
                        t ? t->w : 0, t ? t->h : 0, t ? t->d : 0, (unsigned long long)g, t ? t->fkey : 0,
                        lin_out ? "neutral sRGB->linear" : "identity");
                }
            }
        } else if (t && tex_view(t, want)) {
            GLuint v = t->view;
            if (mb)
                tids[i] = v;
            else
                glBindTexture(want, v);
        } else if (!t && null_tex(want)) {
            GLuint z = null_tex(want);
            if (mb)
                tids[i] = z;
            else
                glBindTexture(want, z);
            st_tex_null++;
        } else {
            if (!mb) glBindTexture(want, 0);
            if (!t)
                st_tex_null++;
            else {
                st_tex_mismatch++;
                static int said;
                if (said < 6) {
                    said++;
                    LOG("gpu: texture %ux%ux%u TIC type %u does not match the shader sampler (wants %x, has %x)\n",
                        t->w, t->h, t->d, (tic[4] >> 23) & 0xF, want, t->target);
                }
            }
        }
        /* sRGB storage read through a non-sRGB TIC returns raw values on hw */
        /* sRGB storage read through a non-sRGB TIC returns raw values on hw. in GL the sampler's SRGB_DECODE wins
       over the texture's, so it's a sampler flag (tsc[7] bit 31) */
        int srgb_raw = t && t->f.ifmt == GL_SRGB8_ALPHA8 && !((tic[4] >> 22) & 1);
        /* compare mode from the shader's sampler type; a non-shadow sampler with compare on reads 0 in GL */
        if (st->shadow) {
            if (!((tsc[0] >> 9) & 1)) {
                tsc[0] |= 1u << 9;
                tsc[0] = (tsc[0] & ~(7u << 10)) | (3u << 10);
            }
        } else
            tsc[0] &= ~(1u << 9);
        if (t && t->f.is_int)
            tsc[1] = (tsc[1] & ~0xF3u) | 0x51u; /* int textures must be NEAREST, LINEAR makes them incomplete */
        if (srgb_raw) tsc[7] |= 0x80000000u;
        if (mb)
            sids[i] = sampler_get(tsc);
        else
            glBindSampler(unit, sampler_get(tsc));
        /* TIC swizzle */
        if (t && (t->target == want || (t->view && t->view_target == want))) {
            static const GLint sw[8] = {GL_ZERO, GL_ZERO, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ONE, GL_ONE};
            GLint m[4] = {sw[(tic[0] >> 19) & 7], sw[(tic[0] >> 22) & 7], sw[(tic[0] >> 25) & 7],
                          sw[(tic[0] >> 28) & 7]};
            /* RTs: only apply swizzles with constant 0/1 (e.g. an R8 HUD mask read as 1,1,1,R). pure reorders are
         left alone since the RT format already has the host order */
            /* RTs: the TIC swizzle is in memory component order but the GL texture is in logical order, so only RTs
         stored BGRA (A8R8G8B8 / X8R8G8B8) need R/B swapped. SMO reads its light buffer as GBAR and R8 masks as RRRR */
            if (t->is_rt && !getenv_cached_rtswz()) {
                u32 rf = t->fkey & 0xFF;
                int bgra = rf == 0xCF || rf == 0xD0 || rf == 0xE6 || rf == 0xE7;
                if (bgra)
                    for (int k = 0; k < 4; k++) {
                        if (m[k] == GL_RED)
                            m[k] = GL_BLUE;
                        else if (m[k] == GL_BLUE)
                            m[k] = GL_RED;
                    }
            } else if (t->is_rt) {
                int hc = 0;
                for (int k = 0; k < 4; k++)
                    if (m[k] == GL_ZERO || m[k] == GL_ONE) hc = 1;
                if (!hc) {
                    m[0] = GL_RED;
                    m[1] = GL_GREEN;
                    m[2] = GL_BLUE;
                    m[3] = GL_ALPHA;
                }
            }
            if (t->f.depth) {
                m[0] = GL_RED;
                m[1] = GL_RED;
                m[2] = GL_RED;
                m[3] = GL_RED;
            }
            int isv = t->target != want;
            GLint* cur = isv ? t->vswz : t->swz;
            GLuint tid = isv ? t->view : t->id;
            if (memcmp(cur, m, sizeof m)) {
                memcpy(cur, m, sizeof m);
                if (mb)
                    glBindTexture(want, tid); /* use the scratch unit */
                else
                    glBindTexture(want, tid);
                glTexParameteriv(want, GL_TEXTURE_SWIZZLE_RGBA, m);
            }
        }
    }
    if (mb && nt > 0) {
        int base = slot * 32;
        if (g_tb_ok[slot] && g_tb_n[slot] == nt && !memcmp(g_tb_last[slot], tids, (size_t)nt * 4) &&
            !memcmp(g_sb_last[slot], sids, (size_t)nt * 4)) {
            st_tb_skip++;
            return;
        }
        if (!g_tb_ok[slot] || g_tb_n[slot] != nt || memcmp(g_tb_last[slot], tids, (size_t)nt * 4)) p_bt(base, nt, tids);
        if (!g_tb_ok[slot] || g_tb_n[slot] != nt || memcmp(g_sb_last[slot], sids, (size_t)nt * 4)) p_bs(base, nt, sids);
        memcpy(g_tb_last[slot], tids, (size_t)nt * 4);
        memcpy(g_sb_last[slot], sids, (size_t)nt * 4);
        g_tb_n[slot] = nt;
        g_tb_ok[slot] = 1;
    }
}

u8* g_stage_buf;
size_t g_stage_cap;
/* 64K blocks (by gpu va, hashed) that got compute output, with a generation. a cached buffer on one of them
   gets reread. avoids walking the whole cache on every compute result */
u32 g_csblk[65536], g_csgen;
static int cs_blk_dirty(u64 a, u64 n, u32 seen) {
    if (g_csgen == seen) return 0;
    for (u64 b = a >> 16, e = (a + n - 1) >> 16; b <= e; b++)
        if (g_csblk[b & 0xFFFF] > seen) return 1;
    return 0;
}
VbC* vbc[2048];
/* write-watch generation per 64K block; untouched ranges skip the reread/hash */
static u32* g_cgen;
static int g_ww = -1;
static u64 st_ww_skip;
int g_va_inval;
static void ww_mark(u64 off) {
    if (off < AS_SIZE) g_cgen[off >> 16] = g_submit_epoch;
}
void ww_poll(void) {
    if (!g_ww) return;
    if (g_ww < 0) {
        g_ww = g_ww_ok ? 1 : 0;
        if (g_ww) {
            g_cgen = calloc((size_t)(AS_SIZE >> 16), 4);
            if (!g_cgen)
                g_ww = 0;
            else { g_ww_touch = ww_touch; }
        }
        if (!g_ww) return;
        LOG("gpu: write tracking active (unchanged buffers are not re-read)\n");
    }
    if (plat_ww_collect(g_mem, AS_SIZE, ww_mark)) {
        g_ww = 0;
        LOG("gpu: write watch not available\n");
    }
}
void ww_touch(u64 cpu) {
    if (g_cgen && cpu < AS_SIZE) g_cgen[cpu >> 16] = g_submit_epoch + 1;
}
static int ww_dirty(u64 ga, u64 len, u32 since) {
    for (u64 c = ga >> 16, e = (ga + len - 1) >> 16; c <= e; c++)
        if (g_cgen[c] > since) return 1;
    return 0;
}
/* cheap check for big ranges: first/last 64 bytes + 62 spread out 32-byte blocks (~2K read). good at catching
   a new level loaded over old data, not small edits; the full check handles those */
u64 sample_hash(u64 gva, u64 len) {
    u8 tmp[2048 + 128];
    u32 n = 0;
    if (len <= sizeof tmp) {
        gmem_read(gva, tmp, len);
        return fast_hash(tmp, len, 7);
    }
    gmem_read(gva, tmp, 64);
    n = 64;
    gmem_read(gva + len - 64, tmp + n, 64);
    n += 64;
    u64 step = (len - 32) / 62;
    for (int i = 0; i < 62; i++) {
        gmem_read(gva + (u64)i * step, tmp + n, 32);
        n += 32;
    }
    return fast_hash(tmp, n, 7);
}
int g_nosample = -1;
/* range fully inside a compute result that hasn't landed in guest memory yet: copy GPU->GPU instead of
   waiting, reading back and reuploading (SMO particles hit this every frame) */
u64 st_cs_gpucopy, st_cs_sync, st_cs_sync_ns;
GLuint buf_cached(GLenum target, u64 start, u64 len) {
    if (g_cs_any) {
        GLuint sb;
        u32 so;
        if (cs_gpu_src(start, len, &sb, &so)) {
            u32 k = (u32)((start >> 6) ^ (start >> 20) ^ len) & 2047;
            VbC* e = vbc[k];
            for (; e; e = e->next)
                if (e->start == start && e->len == len) break;
            if (!e) {
                e = calloc(1, sizeof *e);
                e->start = start;
                e->len = len;
                glGenBuffers(1, &e->id);
                e->next = vbc[k];
                vbc[k] = e;
            }
            glBindBuffer(GL_COPY_WRITE_BUFFER, e->id);
            glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)len, NULL, GL_STATIC_DRAW);
            glBindBuffer(GL_COPY_READ_BUFFER, sb);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, so, 0, (GLsizeiptr)len);
            /* the CPU-side copy lands later (cs_apply marks the block); the reread then finds identical data */
            e->hash = ~0ull;
            e->stable = 0;
            e->epoch = g_submit_epoch;
            e->chk_epoch = g_submit_epoch;
            e->csseen = g_csgen;
            e->cpu = 0;
            st_cs_gpucopy++;
            {
                static int said;
                if (!said) {
                    said = 1;
                    LOG("gpu: compute results reused on the GPU (no wait for the readback)\n");
                }
            }
            glBindBuffer(target, e->id);
            return e->id;
        }
        glr_cs_mem_need(start, len);
    }
    u32 k = (u32)((start >> 6) ^ (start >> 20) ^ len) & 2047;
    VbC* e = vbc[k];
    for (; e; e = e->next)
        if (e->start == start && e->len == len) break;
    if (e && e->csseen != g_csgen) {
        if (cs_blk_dirty(start, len, e->csseen)) {
            e->epoch = 0;
            e->chk_epoch = g_submit_epoch - 100000u;
            e->stable = 0;
        }
        e->csseen = g_csgen;
    } /* written by a compute, reread */
    if (e && e->epoch == g_submit_epoch) {
        glBindBuffer(target, e->id);
        return e->id;
    }
    /* nobody touched it since last time (still rechecked every 240 submits) */
    if (e && g_ww > 0 && e->cpu && g_submit_epoch - e->chk_epoch < 240 && gmmu_translate(start) == e->cpu &&
        !ww_dirty(e->cpu, len, e->chk_epoch)) {
        e->epoch = g_submit_epoch;
        st_ww_skip++;
        glBindBuffer(target, e->id);
        return e->id;
    }
    /* big buffers that never changed (static geometry): check every 8 submits */
    if (e && g_ww <= 0 && e->stable >= 16 && len >= (64u << 10) && g_submit_epoch - e->chk_epoch < 8) {
        e->epoch = g_submit_epoch;
        glBindBuffer(target, e->id);
        return e->id;
    }
    if (e && g_ww <= 0 && e->stable >= 32 && len < (64u << 10) && g_submit_epoch - e->chk_epoch < 4) {
        e->epoch = g_submit_epoch;
        glBindBuffer(target, e->id);
        return e->id;
    }
    /* long-stable buffers: full check every 64/240 submits, sampled every 4 in between.
     no write tracking on Proton/Linux, without this the Deck hashed ~9 MB of vertices a frame */
    if (g_nosample < 0) g_nosample = getenv("SWR_NO_SAMPLE_CHECK") != NULL;
    if (e && g_ww <= 0 && !g_nosample && e->stable >= 64 && len >= 4096 &&
        g_submit_epoch - e->chk_epoch < (e->stable >= 256 ? 240u : 64u)) {
        if (g_submit_epoch - e->schk < 4) {
            e->epoch = g_submit_epoch;
            glBindBuffer(target, e->id);
            return e->id;
        }
        u64 sh = sample_hash(start, len);
        e->schk = g_submit_epoch;
        st_vb_hashed += 2048;
        if (sh == e->shash) {
            e->epoch = g_submit_epoch;
            glBindBuffer(target, e->id);
            return e->id;
        }
    }
    u8* src = gmem_ptr(start, len);
    u64 cpu = src ? (u64)(src - g_mem) : 0;
    if (!src) {
        if (g_stage_cap < len) {
            u8* nb = realloc(g_stage_buf, len);
            if (!nb) return 0;
            g_stage_buf = nb;
            g_stage_cap = len;
        }
        gmem_read(start, g_stage_buf, len);
        src = g_stage_buf;
    }
    u64 h = fast_hash(src, len, 0);
    st_vb_hashed += len;
    if (!e) {
        e = calloc(1, sizeof *e);
        e->start = start;
        e->len = len;
        e->csseen = g_csgen;
        glGenBuffers(1, &e->id);
        e->next = vbc[k];
        vbc[k] = e;
        e->hash = h ^ 1;
    }
    glBindBuffer(target, e->id);
    if (e->hash != h) {
        e->hash = h;
        glBufferData(target, (GLsizeiptr)len, src, GL_STATIC_DRAW);
        st_vb_up += len;
        e->stable = 0;
    } else if (e->stable < 100000)
        e->stable++;
    if (e->stable >= 63 && len >= 4096) {
        e->shash = sample_hash(start, len);
        e->schk = g_submit_epoch;
    }
    e->cpu = cpu;
    e->epoch = g_submit_epoch;
    e->chk_epoch = g_submit_epoch;
    return e->id;
}
