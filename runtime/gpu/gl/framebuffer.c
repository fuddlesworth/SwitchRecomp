/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* framebuffers, render targets to and from guest memory, and 2D blits on the GPU */
#include "gpu/gl/gl.h"

/* ============================================================ framebuffers */
GLuint g_fbo, g_vao, g_vbuf[32], g_ibuf, g_ubuf[5][18], g_scratch_vao;
CbState g_cb;
void sh_dump_cbs(u64 h) {
    char fp[200];
    FILE* f;
    for (int ci = 0; ci < 18; ci++) {
        u64 ca = g_cb.cb[g_sh_bind][ci];
        if (!ca) continue;
        u32 csz = g_cb.cbsize[g_sh_bind][ci];
        if (!csz || csz > 0x10000) csz = 0x10000;
        u8* cbb = malloc(csz);
        gmem_read(ca, cbb, csz);
        snprintf(fp, sizeof fp, "captures/failed/%016llx_cb%d.bin", (unsigned long long)h, ci);
        f = fopen(fp, "wb");
        if (f) {
            fwrite(cbb, 1, csz, f);
            fclose(f);
        }
        free(cbb);
    }
}
int sh_cb_read(u32 idx, u32 off, u32* out) {
    if (idx >= 18) return 0;
    u64 a = g_cb.cb[g_sh_bind][idx];
    u32 sz = g_cb.cbsize[g_sh_bind][idx];
    if (!a || off + 4 > (sz ? sz : 65536)) return 0;
    gmem_read(a + off, out, 4);
    return 1;
}
Tex* g_rt[8];
Tex* g_zt;
int g_nrt;

/* RT memory signature; if the CPU or DMA overwrote it something else lives there now */
Tex* rt_texture(u64 gva, u32 w, u32 h, u32 fmt, u32 tile, int depth) {
    GlFmt f = rt_format(fmt);
    u32 fkey = fmt | (depth ? 0x10000 : 0);
    for (Tex* t = texh[hkey(gva)]; t; t = t->next)
        if (t->gva == gva && t->is_rt && t->w == w && t->h == h && t->fkey == fkey) {
            t->last_used = g_frame;
            return t;
        }
    /* games reuse the same memory for RTs of different size/format within a frame (SMO does every frame).
     instead of destroying the old one, park it as an alias and reuse it when that size comes back.
     recreating meant a GL sync plus a deswizzle each time */
    Tex* rv = NULL;
    for (Tex* t = texh[hkey(gva)]; t; t = t->next)
        if (t->gva == ~0ull && t->alias == gva && t->is_rt && t->w == w && t->h == h && t->fkey == fkey && !rv) rv = t;
    for (Tex* t = texh[hkey(gva)]; t; t = t->next)
        if (t->gva == gva && t->is_rt) {
            t->gva = ~0ull;
            t->alias = gva;
            g_tmap_gen++;
        }
    if (rv && !getenv("SWR_NO_RT_ALIAS")) {
        rv->gva = gva;
        rv->alias = 0;
        rv->last_used = g_frame;
        rv->hash = rt_sig(gva);
        rv->frame_checked = g_frame;
        return rv;
    }
    Tex* t = tex_create(gva, w, h, 1, 1, GL_TEXTURE_2D, f, fkey, !((tile >> 12) & 1), (tile >> 4) & 15,
                        (tile >> 8) & 15, 0, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* seed from memory in case the CPU wrote it */
    if (!depth && !f.compressed && f.fmt == GL_RGBA && f.bpp == 4) {
        u64 sz = (u64)w * h * 4;
        u8* lin = malloc(sz);
        u8* src = gmem_ptr(gva, sz * 2);
        if (src && t->bl) {
            deswizzle(lin, w * 4, src, w * 4, h, t->bh);
            glBindTexture(GL_TEXTURE_2D, t->id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, lin);
        }
        free(lin);
    }
    t->hash = rt_sig(gva);
    return t;
}

/* 3D RT (tile mode bit 16) or array (layers @0x206). drawn either in one pass with the VS picking the layer
   or slice by slice (base layer @0x208). backed by a 3D/array texture that 3D/array TICs at the same
   address then use directly (32^3 LUTs) */
u64 st_rt_vol, st_waited;
int g_draw_layered;
static Tex* rt_vol(Tex* t, u32 dep, int is3d) {
    GLenum tg = is3d ? GL_TEXTURE_3D : GL_TEXTURE_2D_ARRAY;
    Tex* v = t->vol;
    u32 sw = SC(t->w), sh = SC(t->h);
    if (v && (v->d != dep || v->target != tg || v->pitch != sw || v->size != sh)) {
        glDeleteTextures(1, &v->id);
        if (v->view) glDeleteTextures(1, &v->view);
        free(v);
        v = t->vol = NULL;
        g_fb_sig_ok = 0;
        g_tmap_gen++;
        mb_reset();
    }
    if (!v) {
        v = calloc(1, sizeof *v);
        v->gva = t->gva;
        v->w = t->w;
        v->h = t->h;
        v->d = dep;
        v->levels = 1;
        v->target = tg;
        v->f = t->f;
        v->fkey = t->fkey;
        v->is_rt = 1;
        v->gpu_written = 1;
        v->bl = t->bl;
        v->bh = t->bh;
        v->pitch = sw;
        v->size = sh;
        v->swz[0] = GL_RED;
        v->swz[1] = GL_GREEN;
        v->swz[2] = GL_BLUE;
        v->swz[3] = GL_ALPHA;
        glGenTextures(1, &v->id);
        glBindTexture(tg, v->id);
        glTexStorage3D(tg, 1, t->f.ifmt, sw, sh, dep);
        glTexParameteri(tg, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        t->vol = v;
        st_rt_vol++;
        static int said;
        if (said < 8) {
            said++;
            LOG("gpu: %s render target %ux%ux%u at %llx (fmt %x)\n", is3d ? "3D" : "array", t->w, t->h, dep,
                (unsigned long long)t->gva, t->fkey & 0xFFFF);
        }
    }
    return v;
}
/* current g_fbo attachments; skip reattaching if unchanged, every change makes the driver revalidate */
static GLuint g_fb_sig[10];
int g_fb_lay[8];
int g_fb_sig_ok;
u64 st_fb_skip;
int g_clear_layer = -1;
void bind_framebuffer(GpuChannel* ch) {
    u32* r = ch->r3d;
    u32 ctl = r[0x487];
    u32 count = ctl & 15;
    if (count > 8) count = 8;
    int n = 0;
    g_nrt = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    GLuint sig[10];
    int lay[8];
    int anylay = 0;
    for (u32 i = 0; i < 8; i++) {
        Tex* t = NULL;
        lay[i] = -2;
        if (i < count) {
            u32 idx = (ctl >> (4 + i * 3)) & 7;
            u32* rt = &r[0x200 + idx * 0x10];
            u64 a = ((u64)rt[0] << 32) | rt[1];
            {
                static int dbg = -1;
                if (dbg < 0) dbg = getenv("SWR_RTDBG") != 0;
                if (dbg)
                    LOG("rtdbg %d: %llx %ux%u fmt=%x tile=%x arr=%x stride=%x base=%x\n", i, (unsigned long long)a,
                        rt[2], rt[3], rt[4], rt[5], rt[6], rt[7], rt[8]);
            }
            if (a && rt[4] && rt[2] && rt[3]) t = rt_texture(a, rt[2], rt[3], rt[4], rt[5], 0);
            u32 dep = rt[6] & 0xFFFF;
            int is3d = (rt[5] >> 16) & 1;
            if (t && !t->f.depth && dep > 1 && dep <= 2048 && !getenv("SWR_NO_RTVOL")) {
                Tex* v = rt_vol(t, dep, is3d);
                if (v) {
                    v->wgen++;
                    v->last_used = g_frame;
                    if (g_clear_layer >= 0)
                        lay[i] = (int)(rt[8] & 0xFFFF) + g_clear_layer;
                    else
                        lay[i] = g_draw_layered ? -1 : (int)(rt[8] & 0xFFFF);
                    if (lay[i] >= (int)dep) lay[i] = (int)dep - 1;
                    if (lay[i] == -1) anylay = 1;
                    g_rt[i] = t;
                    sig[i] = v->id;
                    n = i + 1;
                    t->gpu_written = 1;
                    t->wgen++;
                    continue;
                }
            }
        }
        g_rt[i] = t;
        sig[i] = t ? t->id : 0;
        if (t) {
            n = i + 1;
            t->gpu_written = 1;
            t->wgen++;
        }
    }
    g_nrt = n;
    g_zt = NULL;
    if (r[0x54E]) {
        u64 za = ((u64)r[0x3F8] << 32) | r[0x3F9];
        u32 zw = r[0x48A], zh = r[0x48B];
        if (za && zw && zh) g_zt = rt_texture(za, zw, zh, r[0x3FA], r[0x3FB], 1);
    }
    if (anylay && g_zt) {
        g_zt = NULL;
        static int said;
        if (!said) {
            said = 1;
            LOG("gpu: layered draw with depth: drawn without depth\n");
        }
    }
    if (g_zt) g_zt->wgen++;
    GLenum att = g_zt ? (g_zt->f.fmt == GL_DEPTH_STENCIL ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT)
                      : GL_DEPTH_STENCIL_ATTACHMENT;
    sig[8] = g_zt ? g_zt->id : 0;
    sig[9] = att;
    if (g_fb_sig_ok && !memcmp(sig, g_fb_sig, sizeof sig) && !memcmp(lay, g_fb_lay, sizeof lay)) {
        st_fb_skip++;
        return;
    }
    g_tmap_gen++;
    GLenum bufs[8];
    for (u32 i = 0; i < 8; i++) {
        if (!g_fb_sig_ok || g_fb_sig[i] != sig[i] || g_fb_lay[i] != lay[i]) {
            if (lay[i] == -2)
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, sig[i], 0);
            else if (lay[i] == -1)
                glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, sig[i], 0);
            else
                glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, sig[i], 0, lay[i]);
        }
        bufs[i] = sig[i] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
    }
    memcpy(g_fb_lay, lay, sizeof lay);
    {
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("SWR_RTDBG") != 0;
        if (dbg) {
            GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            LOG("rtdbg fbo %x lay %d %d layered=%d err=%x\n", st, lay[0], lay[1], g_draw_layered, glGetError());
        }
    }
    glDrawBuffers(8, bufs);
    if (!g_fb_sig_ok || g_fb_sig[8] != sig[8] || g_fb_sig[9] != sig[9]) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, sig[8], 0);
    }
    memcpy(g_fb_sig, sig, sizeof sig);
    g_fb_sig_ok = 1;
}

/* ============================================================ RT <-> memory, GPU 2D blits */
GLuint g_rfbo, g_dfbo;
static int rt_plain(Tex* t) {
    return t && !t->f.depth && !t->f.is_int && t->f.fmt == GL_RGBA && t->f.type == GL_UNSIGNED_BYTE && t->f.bpp == 4;
}
/* write RT contents back to guest memory for CPU-side DMA/2D */
/* res scale != 1: GPU copy between the scaled RT and native size */
static GLuint rt_unscaled_copy(Tex* t, int to_real) {
    GLuint tmp = 0;
    glGenTextures(1, &tmp);
    glBindTexture(GL_TEXTURE_2D, tmp);
    glTexStorage2D(GL_TEXTURE_2D, 1, t->f.ifmt, t->w, t->h);
    if (to_real) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp, 0);
        {
            GLenum b = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &b);
        }
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMaski(0, 1, 1, 1, 1);
        state_dirty();
        glBlitFramebuffer(0, 0, (GLint)SC(t->w), (GLint)SC(t->h), 0, 0, (GLint)t->w, (GLint)t->h, GL_COLOR_BUFFER_BIT,
                          GL_LINEAR);
    }
    return tmp;
}
static void rt_scaled_put(Tex* t, GLuint tmp) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    state_dirty();
    glBlitFramebuffer(0, 0, (GLint)t->w, (GLint)t->h, 0, 0, (GLint)SC(t->w), (GLint)SC(t->h), GL_COLOR_BUFFER_BIT,
                      GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}
/* any uncompressed color RT (R16F heightmaps, RG16F flow maps...), games render these once and DMA them
   into the final texture */
static int rt_flushable(Tex* t) {
    return t && !t->f.depth && !t->f.compressed && t->f.bpp > 0 && t->f.bpp <= 16 && t->target == GL_TEXTURE_2D &&
           !t->vol;
}
void glr_rt_flush(u64 gva) {
    Tex* t = tex_find_rt(gva);
    if (!rt_flushable(t) || (g_gles && !rt_plain(t))) return;
    {
        static int nlog;
        if (nlog < 40 && !rt_plain(t)) {
            nlog++;
            LOG("rt %llx %ux%u (fkey %x) copied to memory for a DMA/2D copy\n", (unsigned long long)gva, t->w, t->h,
                t->fkey);
        }
    }
    u32 w = t->w, h = t->h, bp = t->f.bpp;
    u8* px = malloc((size_t)w * h * bp);
    if (!px) return;
    GLuint tmp = g_scale != 1.0f ? rt_unscaled_copy(t, 1) : 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp ? tmp : t->id, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (rt_plain(t))
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    else {
        glBindTexture(GL_TEXTURE_2D, tmp ? tmp : t->id);
        glGetTexImage(GL_TEXTURE_2D, 0, t->f.fmt, t->f.type, px);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (tmp) glDeleteTextures(1, &tmp);
    state_dirty();
    u64 sz = t->bl ? bl_level_size(w, h, 1, &t->f, t->bh, 0, 0, NULL) : (u64)w * h * bp;
    u8* dst = gmem_ptr(gva, sz);
    if (dst) {
        if (t->bl)
            swizzle(dst, px, w * bp, w * bp, h, t->bh);
        else
            memcpy(dst, px, (size_t)w * h * bp);
    }
    free(px);
    t->hash = rt_sig(gva);
    trace_note("  (rt %llx -> memory, %u bytes/px)\n", (unsigned long long)gva, bp);
}
/* CPU copy changed the memory, reload the RT */
void glr_rt_reload(u64 gva) {
    Tex* t = tex_find_rt(gva);
    /* DMA over an RT we can't reload (R8 video luma etc): drop the RT and read memory instead */
    if (t && !rt_plain(t)) {
        t->gva = ~0ull;
        g_tmap_gen++;
        trace_note("  (new memory over rt %llx: rt discarded)\n", (unsigned long long)gva);
        return;
    }
    if (!rt_plain(t)) return;
    u32 w = t->w, h = t->h;
    u64 sz = t->bl ? bl_level_size(w, h, 1, &t->f, t->bh, 0, 0, NULL) : (u64)w * h * 4;
    u8* src = gmem_ptr(gva, sz);
    if (!src) return;
    u8* lin = malloc((size_t)w * h * 4);
    if (t->bl)
        deswizzle(lin, w * 4, src, w * 4, h, t->bh);
    else
        memcpy(lin, src, (size_t)w * h * 4);
    GLuint tmp = g_scale != 1.0f ? rt_unscaled_copy(t, 0) : 0;
    glBindTexture(GL_TEXTURE_2D, tmp ? tmp : t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, lin);
    if (tmp) {
        rt_scaled_put(t, tmp);
        glDeleteTextures(1, &tmp);
    }
    free(lin);
    t->hash = rt_sig(gva);
    t->wgen++;
    trace_note("  (memory -> rt %llx)\n", (unsigned long long)gva);
}
/* 2D blit from an RT, done on the GPU */
int glr_blit(const Surf2D* s, const Surf2D* d, int dx, int dy, int dw, int dh, double sx, double sy, double sdx,
             double sdy) {
    state_dirty();
    Tex* st = tex_find_rt(s->addr);
    trace_note("BLIT2D %llx(%ux%u f%x%s) -> %llx(%ux%u f%x) dst(%d,%d %dx%d) src(%.1f,%.1f step %.3f,%.3f)\n",
               (unsigned long long)s->addr, s->width, s->height, s->format, st ? " RT" : "",
               (unsigned long long)d->addr, d->width, d->height, d->format, dx, dy, dw, dh, sx, sy, sdx, sdy);
    if (!st || st->f.depth) return 0;
    u32 tile = (d->linear ? 0x1000u : 0) | (d->bh << 4);
    Tex* dt = rt_texture(d->addr, d->width, d->height, d->format, tile, 0);
    if (!dt || dt == st || dt->f.depth) return 0;
    if (st->f.is_int != dt->f.is_int) return 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st->id, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dt->id, 0);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    float S = g_scale;
    int scaled = sdx != 1.0 || sdy != 1.0 || S != 1.0f;
    glBlitFramebuffer((GLint)(sx * S), (GLint)(sy * S), (GLint)((sx + sdx * dw) * S), (GLint)((sy + sdy * dh) * S),
                      (GLint)(dx * S), (GLint)(dy * S), (GLint)((dx + dw) * S), (GLint)((dy + dh) * S),
                      GL_COLOR_BUFFER_BIT, (scaled && !st->f.is_int) ? GL_LINEAR : GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    dt->last_used = g_frame;
    dt->wgen++;
    return 1;
}
