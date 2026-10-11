/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* LSFG frame generation in the present path */
#include "gpu/gl/gl.h"
#include "core/clock.h"

/* ============================================================ LSFG frame gen (user's own Lossless.dll) */
static GLuint g_lf_fbo;
static u64 g_lf_last, g_lf_int = 33333333ull;
static int g_lf_on, g_lf_W, g_lf_H;
LfQ g_lfq[6];
int g_lfn;
u64 st_lsfg_gen, st_lsfg_ns;
static const float XF_ID[4] = {0, 0, 1, 1};
static void lf_flush(void) {
    for (int i = 0; i < g_lfn; i++) {
        screen_quad(g_lfq[i].tex, XF_ID, g_lf_W, g_lf_H);
        screen_swap();
    }
    g_lfn = 0;
}
void lsfg_pump(void) {
    if (!g_lfn) return;
    u64 now = plat_ns();
    int k = 0;
    while (k < g_lfn && now >= g_lfq[k].t) {
        screen_quad(g_lfq[k].tex, XF_ID, g_lf_W, g_lf_H);
        screen_swap();
        k++;
    }
    if (k) {
        memmove(g_lfq, g_lfq + k, (size_t)(g_lfn - k) * sizeof *g_lfq);
        g_lfn -= k;
    }
}
/* real frame: copy into the LSFG input, generate the in-between frames and present them spread over the
   interval, real one last. 0 = not active (already at target, or no dll) */
static int g_blit_rt; /* shown image is an RT, i.e. scaled by the res scale */
static int lsfg_frame(GLuint tex, const float* xf, int tw, int th) {
    if (!lsfg_ready() || tw <= 0 || th <= 0) return 0;
#ifdef _WIN32
    {
        if (g_vt_on) {
            lf_flush();
            g_lf_on = 0;
            return 0;
        }
    } /* smooth recording: only real frames */
#endif
    u64 now = plat_ns();
    if (g_lf_last) {
        u64 d = now - g_lf_last;
        if (d > 250000000ull) d = 250000000ull;
        g_lf_int = (g_lf_int * 7 + d) / 8;
    }
    g_lf_last = now;
    double base = 1e9 / (double)g_lf_int;
    int m = (int)(g_lsfg_target / base + 0.5);
    if (m > g_lsfg_mult) m = g_lsfg_mult;
    if (base > g_lsfg_target * (g_lf_on ? 0.92 : 0.85)) m = 1; /* already at target */
    {
        static int ls = -1;
        int st = m >= 2 ? m : 0;
        if (st != ls) {
            ls = st;
#ifdef _WIN32
            backend_lsfg_state(st != 0, st);
#endif
            LOG("lsfg: %s\n", st ? "ON" : "standby (the game already reaches the target)");
        }
    }
    if (m < 2) {
        lf_flush();
        g_lf_on = 0;
        return 0;
    }
    /* at a low res scale the real image is smaller than the window; generating bigger just burns GPU on
     stretched pixels (x0.25 at 720p cost ~29ms on an iGPU). min 360 */
    int content = g_blit_rt ? (int)(th * g_scale + 0.5f) : th;
    if (content < 360) content = 360;
    if (content > th) content = th;
    u32 H = (u32)(content < g_lsfg_height ? content : g_lsfg_height), W = (u32)((u64)H * (u32)tw / (u32)th) & ~1u;
    if (W < 16 || H < 16) return 0;
    if (!lsfg_context(W, H)) { return 0; }
    if (!g_lf_on) {
        g_lf_on = 1;
        lsfg_reset();
    }
    lf_flush();
    g_lf_W = (int)W;
    g_lf_H = (int)H;
    GLuint in = lsfg_in_tex();
    if (!g_lf_fbo) glGenFramebuffers(1, &g_lf_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_lf_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, in, 0);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    glViewport(0, 0, (GLsizei)W, (GLsizei)H);
    glUseProgram(g_blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glTexParameteri(GL_TEXTURE_2D, 0x8A48, 0x8A4A);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    {
        static const GLint idm[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, idm);
    }
    {
        static GLint xl = -2;
        static GLuint xp;
        if (xl == -2 || xp != g_blit_prog) {
            xl = glGetUniformLocation(g_blit_prog, "xf");
            xp = g_blit_prog;
        }
        glUniform4fv(xl, 1, xf);
    }
    glBindVertexArray(g_scratch_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glTexParameteri(GL_TEXTURE_2D, 0x8A48, 0x8A49);
    GLuint outs[4];
    u64 t0 = plat_ns();
    int n = lsfg_process(m - 1, outs);
    st_lsfg_ns += plat_ns() - t0;
    {
        static int nl;
        if (nl < 4 && n > 0) {
            nl++;
            glFinish();
            LOG("lsfg: frame generated in %.1f ms (GPU included, x%d, base %.1f FPS)\n", (plat_ns() - t0) / 1e6, m,
                base);
        }
    }
    {
        static int shot = -1, cnt;
        if (shot < 0) {
            const char* e = getenv("SWR_LSFG_SHOT");
            shot = e ? atoi(e) : 0;
        }
        if (shot > 0 && n > 0 && ++cnt == shot) {
            GLuint a, b;
            u32 w, h;
            lsfg_debug_tex(&a, &b, &w, &h);
            GLuint tx[3] = {a, outs[0], b};
            u8* px = malloc((size_t)w * h * 4 * 3);
            u8* row = malloc((size_t)w * 3 * 3);
            for (int k = 0; k < 3; k++) {
                glBindTexture(GL_TEXTURE_2D, tx[k]);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px + (size_t)k * w * h * 4);
            }
            FILE* f = fopen("lsfg_shot.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%u %u\n255\n", w * 3, h);
                for (int y = (int)h - 1; y >= 0; y--) {
                    for (int k = 0; k < 3; k++)
                        for (u32 x = 0; x < w; x++) {
                            u8* q = px + (size_t)k * w * h * 4 + ((size_t)y * w + x) * 4;
                            memcpy(row + ((size_t)k * w + x) * 3, q, 3);
                        }
                    fwrite(row, 1, (size_t)w * 9, f);
                }
                fclose(f);
                LOG("lsfg: previous | generated | current saved to lsfg_shot.ppm\n");
            }
            free(px);
            free(row);
            mb_reset();
        }
    }
    /* the LSFG dispatches clobbered programs, textures, samplers and UBO 0 */
    g_pipe_on = 0;
    state_dirty();
    mb_reset();
    memset(g_ub_bound, 0, sizeof g_ub_bound);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    g_last_present_tex = in;
    g_last_flip = 0;
    g_last_w = (int)W;
    g_last_h = (int)H;
    g_last_is_rt = 0;
    if (n <= 0) {
        screen_quad(in, XF_ID, (int)W, (int)H);
        screen_swap();
        return 1;
    }
    u64 step = g_lf_int / (u64)(n + 1);
    screen_quad(outs[0], XF_ID, (int)W, (int)H);
    screen_swap();
    st_lsfg_gen += (u64)n;
    for (int k = 1; k < n; k++) {
        g_lfq[g_lfn].tex = outs[k];
        g_lfq[g_lfn].t = now + step * (u64)k;
        g_lfn++;
    }
    g_lfq[g_lfn].tex = in;
    g_lfq[g_lfn].t = now + step * (u64)n;
    g_lfn++;
    return 1;
}

static void blit_to_screen(GLuint tex, int flip, int tw, int th) {
    state_dirty();
    /* game-requested crop (e.g. 1280x720 inside a 1920x1080 buffer) */
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    {
        i32 l = g_vi_crop[0], t = g_vi_crop[1], r = g_vi_crop[2], b = g_vi_crop[3];
        if (r > l && b > t && tw > 0 && th > 0 && r <= tw * 4 && b <= th * 4) {
            u0 = (float)l / tw;
            u1 = (float)r / tw;
            v0 = (float)t / th;
            v1 = (float)b / th;
            tw = r - l;
            th = b - t;
        }
    }
    if (g_vi_transform & 2) {
        float x = v0;
        v0 = v1;
        v1 = x;
    } /* FLIP_V */
    if (g_vi_transform & 1) {
        float x = u0;
        u0 = u1;
        u1 = x;
    } /* FLIP_H */
    float xf[4];
    if (flip) {
        xf[0] = u0;
        xf[1] = v1;
        xf[2] = u1 - u0;
        xf[3] = v0 - v1;
    } else {
        xf[0] = u0;
        xf[1] = 1 - v1;
        xf[2] = u1 - u0;
        xf[3] = v1 - v0;
    }
    if (lsfg_frame(tex, xf, tw, th)) return;
    screen_quad(tex, xf, tw, th);
    screen_swap();
    g_last_present_tex = tex;
    g_last_flip = flip;
    g_last_w = tw;
    g_last_h = th;
}
void present_cpu(u32* px, int w, int h) {
    if (!g_cpu_tex || g_cpu_w != w || g_cpu_h != h) {
        if (g_cpu_tex) {
            glDeleteTextures(1, &g_cpu_tex);
            g_fb_sig_ok = 0;
        }
        glGenTextures(1, &g_cpu_tex);
        glBindTexture(GL_TEXTURE_2D, g_cpu_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        g_cpu_w = w;
        g_cpu_h = h;
    }
    glBindTexture(GL_TEXTURE_2D, g_cpu_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    blit_to_screen(g_cpu_tex, 1, w, h);
    g_last_is_rt = 0;
}
/* eviction: free memory-backed textures unused for ~40s and orphaned RTs, otherwise switching worlds slowly
   eats the iGPU's shared RAM. walks part of the table each frame; textures get recreated from memory if needed */
static u64 st_tex_evict, st_vb_evict;
static void tex_free(Tex* t) {
    glDeleteTextures(1, &t->id);
    if (t->view) glDeleteTextures(1, &t->view);
    if (t->crop) {
        glDeleteTextures(1, &t->crop->id);
        if (t->crop->view) glDeleteTextures(1, &t->crop->view);
        free(t->crop);
    }
    if (t->vol) {
        glDeleteTextures(1, &t->vol->id);
        if (t->vol->view) glDeleteTextures(1, &t->vol->view);
        free(t->vol);
    }
    free(t);
}
void gpu_evict_step(void) {
    static u32 cur, vcur;
    if (g_tr_on) return;
    int any = 0;
    for (int n = 0; n < TEXH / 64; n++, cur = (cur + 1) & (TEXH - 1)) {
        for (Tex** pp = &texh[cur]; *pp;) {
            Tex* t = *pp;
            u32 age = g_frame - t->last_used;
            int busy = (t == g_zt);
            for (int i = 0; i < 8; i++)
                if (g_rt[i] == t) busy = 1;
            int old = t->is_rt ? (t->gva == ~0ull && age > 300) : (age > 2400);
            if (old && !busy) {
                *pp = t->next;
                tex_free(t);
                st_tex_evict++;
                any = 1;
            } else
                pp = &t->next;
        }
    }
    if (any) {
        mb_reset();
        g_fb_sig_ok = 0;
        memset(g_tdc, 0, sizeof g_tdc);
        g_tmap_gen++;
    }
    int vany = 0;
    for (int n = 0; n < 2048 / 64; n++, vcur = (vcur + 1) & 2047) {
        for (VbC** pp = &vbc[vcur]; *pp;) {
            VbC* e = *pp;
            if (g_submit_epoch - e->epoch > 4800) {
                *pp = e->next;
                glDeleteBuffers(1, &e->id);
                free(e);
                st_vb_evict++;
                vany = 1;
            } else
                pp = &e->next;
        }
    }
    if (vany) g_va_inval = 1;
}
int present_addr(u64 cpu_addr, int w, int h) {
    /* find the RT whose gva maps this memory */
    Tex* best = NULL;
    for (int k = 0; k < TEXH; k++)
        for (Tex* t = texh[k]; t; t = t->next)
            if (t->is_rt && gmmu_translate(t->gva) == cpu_addr) {
                if (!best || (t->w == (u32)w && t->h == (u32)h) || t->last_used > best->last_used) best = t;
            }
    trace_note("PRESENT %llx %dx%d -> %s %llx\n", (unsigned long long)cpu_addr, w, h, best ? "rt" : "(none, CPU)",
               (unsigned long long)(best ? best->gva : 0));
    if (best) {
        best->last_used = g_frame;
        g_blit_rt = 1;
        blit_to_screen(best->id, 1, (int)best->w, (int)best->h);
        g_blit_rt = 0;
        g_last_is_rt = 1;
        st_pres_rt++;
        return 1;
    }
    st_pres_cpu++;
    static int said;
    if (said < 3) {
        said++;
        LOG("gpu: the image to present (%llx %dx%d) is not a known render target. Render targets:\n",
            (unsigned long long)cpu_addr, w, h);
        int n = 0;
        for (int k = 0; k < TEXH && n < 12; k++)
            for (Tex* t = texh[k]; t && n < 12; t = t->next)
                if (t->is_rt) {
                    n++;
                    LOG("   rt gva=%llx cpu=%llx %ux%u fmt=%x used=%u\n", (unsigned long long)t->gva,
                        (unsigned long long)gmmu_translate(t->gva), t->w, t->h, t->fkey, t->last_used);
                }
        if (!n) LOG("   (none: the game has not drawn with the GPU yet)\n");
    }
    return 0;
}
