/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the texture cache and samplers */
#include "gpu/gl/gl.h"

Tex* texh[TEXH];
u32 g_frame;
u32 hkey(u64 a) { return (u32)((a >> 8) ^ (a >> 20)) & (TEXH - 1); }
u64 g_tm[TM_N], g_tmp[TM_N];
u64 st_sh_new;

/* full size of a bl texture, all layers and mips */
u64 bl_level_size(u32 w, u32 h, u32 d, const GlFmt* f, u32 bh, u32 bd, u32 level, u32* out_bh) {
    u32 lw = w >> level, lh = h >> level, ld = d >> level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    if (!ld) ld = 1;
    u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
    u32 rowb = tw * f->bpp;
    u32 gx = (rowb + 63) / 64, gy = (th + 7) / 8;
    u32 b = bh;
    while (b > 0 && gy <= (1u << (b - 1))) b--;
    u32 bdd = bd;
    while (bdd > 0 && ld <= (1u << (bdd - 1))) bdd--;
    if (out_bh) *out_bh = b;
    u64 gys = ((gy + (1u << b) - 1) >> b) << b;
    u64 lds = ((ld + (1u << bdd) - 1) >> bdd) << bdd;
    return (u64)gx * gys * lds * 512;
}

/* hack for SMO's Cap Kingdom sea: the VS offsets the surface by (h-0.5)*k using an R16F heightmap that's
   still all zeros at this point (haven't found what writes it). zeros push the sea up into the sky, 0.5 is
   neutral. only applied while the memory really is all zero */
static int tex_zero_height_fix(Tex* t) {
    static int off = -1;
    if (off < 0) off = getenv("SWR_NO_SEA_FIX") != NULL;
    if (off) return 0;
    if ((t->fkey & 0x7FFFF) != 0x7ff9b || t->is_rt || t->target != GL_TEXTURE_2D || t->levels < 6 || t->w != t->h ||
        t->w < 256 || (t->w & (t->w - 1)) || !t->size)
        return 0;
    const u8* p = gmem_ptr(t->gva, t->size);
    if (!p) return 0;
    const u64* q = (const u64*)p;
    for (u64 k = 0; k < t->size / 8; k++)
        if (q[k]) return 0;
    u16* half = malloc((size_t)t->w * t->h * 2);
    if (!half) return 0;
    for (u64 k = 0; k < (u64)t->w * t->h; k++) half[k] = 0x3800;
    glBindTexture(GL_TEXTURE_2D, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    for (u32 l = 0; l < t->levels; l++) {
        u32 lw = t->w >> l, lh = t->h >> l;
        if (!lw) lw = 1;
        if (!lh) lh = 1;
        glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, lw, lh, GL_RED, GL_HALF_FLOAT, half);
    }
    free(half);
    mb_reset();
    {
        static int said;
        if (said < 8) {
            said++;
            LOG("gpu: R16F heightmap %ux%u at %llx is all zero: filled with neutral height 0.5 (sea workaround, SWR_NO_SEA_FIX=1 disables)\n",
                t->w, t->h, (unsigned long long)t->gva);
        }
    }
    return 1;
}
static void tex_upload(Tex* t) {
    st_tex_up += t->size;
    if (tex_zero_height_fix(t)) return;
    const GlFmt* f = &t->f;
    u32 layers = t->target == GL_TEXTURE_3D ? 1 : t->d;
    u32 depth3d = t->target == GL_TEXTURE_3D ? t->d : 1;
    u64 layer_size = 0;
    for (u32 l = 0; l < t->levels; l++) layer_size += bl_level_size(t->w, t->h, depth3d, f, t->bh, t->bd, l, NULL);
    if (t->bl && layers > 1) {
        u64 al = 512ull << t->bh;
        layer_size = (layer_size + al - 1) / al * al;
    }
    glBindTexture(t->target, t->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    int was_zero3d = t->target == GL_TEXTURE_3D ? t->zero3d : 0;
    (void)was_zero3d;
    if (t->target == GL_TEXTURE_3D) t->zero3d = 1;
    for (u32 layer = 0; layer < layers; layer++) {
        u64 off = layer * layer_size;
        for (u32 l = 0; l < t->levels; l++) {
            u32 lw = t->w >> l, lh = t->h >> l, ld = depth3d >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            if (!ld) ld = 1;
            u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
            u32 rowb = tw * f->bpp;
            u32 lbh;
            u64 lsz = t->bl ? bl_level_size(t->w, t->h, depth3d, f, t->bh, t->bd, l, &lbh) : (u64)t->pitch * th;
            u64 lin_size = (u64)rowb * th * ld;
            u8* lin = malloc(lin_size ? lin_size : 1);
            u8* src = malloc(lsz ? lsz : 1);
            if (!lin || !src) {
                free(lin);
                free(src);
                static int said;
                if (said < 4) {
                    said++;
                    LOG("gpu: out of memory uploading texture %ux%ux%u (%llu MB): skipped\n", t->w, t->h, t->d,
                        (unsigned long long)(lsz >> 20));
                }
                off += t->bl ? lsz : (u64)t->pitch * th;
                continue;
            }
            gmem_read(t->gva + off, src, lsz);
            if (t->target == GL_TEXTURE_3D && t->zero3d) {
                for (u64 k = 0; k < lsz; k++)
                    if (src[k]) {
                        t->zero3d = 0;
                        break;
                    }
                if (!t->zero3d && was_zero3d) {
                    static int said;
                    if (said < 16) {
                        said++;
                        LOG("gpu: 3D texture %ux%ux%u at %llx got its data in memory (frame %u)\n", t->w, t->h, t->d,
                            (unsigned long long)t->gva, g_frame);
                    }
                }
            }
            if (t->bl) {
                u32 lbd = t->bd;
                while (lbd > 0 && ld <= (1u << (lbd - 1))) lbd--; /* 3D: block depth (LUTs) */
                for (u32 z = 0; z < ld; z++)
                    for (u32 y = 0; y < th; y++) {
                        u8* dr = lin + ((u64)z * th + y) * rowb;
                        for (u32 x = 0; x < rowb; x += 16) {
                            u64 o = bl_offset(x, y, z, rowb, th, lbh, lbd);
                            u32 n = rowb - x < 16 ? rowb - x : 16;
                            if (o + n <= lsz) memcpy(dr + x, src + o, n);
                        }
                    }
            } else {
                for (u32 y = 0; y < th; y++) memcpy(lin + (u64)y * rowb, src + (u64)y * t->pitch, rowb);
            }
            const u8* up = lin;
            u8* dec = NULL;
            if ((f->astc || f->dec) && !(dec = malloc((size_t)lw * lh * 4 + 64))) {
                free(lin);
                free(src);
                off += t->bl ? lsz : (u64)t->pitch * th;
                continue;
            }
            if (f->astc) {
                astc_decode_image(lin, lw, lh, f->bw, f->bh, dec);
                up = dec;
            } else if (f->dec) {
                u32* o = (u32*)dec;
                if (f->dec < 10) {
                    swr_bc_decode(f->dec, lin, lw, lh, o);
                } else {
                    const u16* q = (const u16*)lin;
                    for (u32 y = 0; y < lh; y++)
                        for (u32 x = 0; x < lw; x++) {
                            u32 v = q[(size_t)y * (rowb / 2) + x], rr, gg, bb, aa;
                            if (f->dec == 10) {
                                rr = (v & 15) * 17;
                                gg = ((v >> 4) & 15) * 17;
                                bb = ((v >> 8) & 15) * 17;
                                aa = ((v >> 12) & 15) * 17;
                            } else if (f->dec == 11) {
                                rr = (v & 31) * 255 / 31;
                                gg = ((v >> 5) & 31) * 255 / 31;
                                bb = ((v >> 10) & 31) * 255 / 31;
                                aa = (v >> 15) ? 255 : 0;
                            } else {
                                rr = (v & 31) * 255 / 31;
                                gg = ((v >> 5) & 63) * 255 / 63;
                                bb = ((v >> 11) & 31) * 255 / 31;
                                aa = 255;
                            }
                            o[(size_t)y * lw + x] = rr | (gg << 8) | (bb << 16) | (aa << 24);
                        }
                }
                up = dec;
            }
            {
                static int dmp = -1, nd;
                if (dmp < 0) dmp = getenv("SWR_DUMP_TEX") != NULL;
                if (dmp && l == 0 && layer == 0 && nd < 20 && (f->astc || (!f->compressed && f->bpp == 4))) {
                    char pth[128];
                    snprintf(pth, sizeof pth, "tex_%02d_%llx_%ux%u.ppm", nd++, (unsigned long long)t->gva, lw, lh);
                    FILE* fo = fopen(pth, "wb");
                    if (fo) {
                        fprintf(fo, "P6\n%u %u\n255\n", lw, lh);
                        for (u64 k = 0; k < (u64)lw * lh; k++) fwrite(up + k * 4, 1, 3, fo);
                        fclose(fo);
                    }
                }
            }
            if (f->compressed && !f->astc && !f->dec) {
                GLsizei isz = (GLsizei)(rowb * th);
                if (t->target == GL_TEXTURE_2D)
                    glCompressedTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, lw, lh, f->ifmt, isz, up);
                else if (t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_CUBE_MAP_ARRAY)
                    glCompressedTexSubImage3D(t->target, l, 0, 0, layer, lw, lh, 1, f->ifmt, isz, up);
                else if (t->target == GL_TEXTURE_CUBE_MAP)
                    glCompressedTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, l, 0, 0, lw, lh, f->ifmt, isz,
                                              up);
                else if (t->target == GL_TEXTURE_3D)
                    glCompressedTexSubImage3D(GL_TEXTURE_3D, l, 0, 0, 0, lw, lh, ld, f->ifmt, isz * ld, up);
            } else {
                GLenum fm = (f->astc || f->dec) ? GL_RGBA : f->fmt,
                       ty = (f->astc || f->dec) ? GL_UNSIGNED_BYTE : f->type;
                if (t->target == GL_TEXTURE_2D)
                    glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, lw, lh, fm, ty, up);
                else if (t->target == GL_TEXTURE_2D_ARRAY || t->target == GL_TEXTURE_CUBE_MAP_ARRAY)
                    glTexSubImage3D(t->target, l, 0, 0, layer, lw, lh, 1, fm, ty, up);
                else if (t->target == GL_TEXTURE_CUBE_MAP)
                    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, l, 0, 0, lw, lh, fm, ty, up);
                else if (t->target == GL_TEXTURE_3D)
                    glTexSubImage3D(GL_TEXTURE_3D, l, 0, 0, 0, lw, lh, ld, fm, ty, up);
                else if (t->target == GL_TEXTURE_1D)
                    glTexSubImage2D(GL_TEXTURE_2D, l, 0, 0, lw, 1, fm, ty, up);
            }
            free(dec);
            free(lin);
            free(src);
            off += t->bl ? lsz : (u64)t->pitch * th;
        }
    }
}
static u32 g_tex_bud_frame;
static i64 g_tex_bud;
u64 st_tex_chk;
u64 fast_hash(const u8* p, u64 size, int sampled) {
    const u64* q = (const u64*)p;
    u64 n = size / 8, step = (sampled && n > 8192) ? 16 : 1; /* sample one word per 128 bytes */
    u64 a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    u64 i = 0;
    for (; i + 3 * step < n; i += 4 * step) {
        a = (a ^ q[i]) * 0x100000001B3ull;
        b = (b ^ q[i + step]) * 0x100000001B3ull;
        c = (c ^ q[i + 2 * step]) * 0x100000001B3ull;
        d = (d ^ q[i + 3 * step]) * 0x100000001B3ull;
    }
    for (; i < n; i += step) { a = (a ^ q[i]) * 0x100000001B3ull; }
    for (u64 k = n * 8; k < size; k++) { b = (b ^ p[k]) * 0x100000001B3ull; }
    return a ^ (b << 1) ^ (c << 2) ^ (d << 3) ^ size;
}
static u64 mem_hash(u64 gva, u64 size) {
    /* quick sample: one word per 64 bytes + the first 4K */
    u64 h = 1469598103934665603ull;
    u8* p = gmem_ptr(gva, size);
    if (p) return fast_hash(p, size, 1);
    for (u64 o = 0; o < size; o += 4096) {
        u64 v[8];
        gmem_read(gva + o, v, size - o < 64 ? size - o : 64);
        for (int i = 0; i < 8; i++) {
            h ^= v[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}
u64 rt_sig(u64 gva) {
    u8* p = gmem_ptr(gva, 4096);
    if (p) return fast_hash(p, 4096, 0);
    u8 b[4096];
    gmem_read(gva, b, 4096);
    return fast_hash(b, 4096, 0);
}
static GLenum tex_target(u32 type, u32 depth) {
    switch (type) {
        case 0: return GL_TEXTURE_2D;
        case 1:
        case 7: return depth > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        case 2: return GL_TEXTURE_3D;
        case 3: return GL_TEXTURE_CUBE_MAP;
        case 4:
        case 5: return GL_TEXTURE_2D_ARRAY;
        case 8: return GL_TEXTURE_CUBE_MAP_ARRAY;
        default: return GL_TEXTURE_2D;
    }
}
/* generation counter for the TIC -> texture mapping. Bump it on anything that can change what a TIC
   resolves to (tex created/freed, RT dropped or recreated, FBO attachments, clears, new frame).
   tex_from_tic memoizes its result while this stays the same */
u32 g_tmap_gen = 1;
Tex* tex_create(u64 gva, u32 w, u32 h, u32 d, u32 levels, GLenum target, GlFmt f, u32 fkey, u32 bl, u32 bh, u32 bd,
                u32 pitch, int is_rt) {
    g_tmap_gen++;
    Tex* t = calloc(1, sizeof *t);
    t->swz[0] = GL_RED;
    t->swz[1] = GL_GREEN;
    t->swz[2] = GL_BLUE;
    t->swz[3] = GL_ALPHA;
    t->gva = gva;
    t->w = w;
    t->h = h;
    t->d = d;
    t->levels = levels;
    t->target = target;
    t->f = f;
    t->fkey = fkey;
    t->bl = bl;
    t->bh = bh;
    t->bd = bd;
    t->pitch = pitch;
    t->is_rt = is_rt;
    glGenTextures(1, &t->id);
    glBindTexture(target, t->id);
    GLenum ifmt = f.ifmt;
    u32 sw = is_rt ? SC(w) : w, sh = is_rt ? SC(h) : h;
    if (target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP)
        glTexStorage2D(target, levels, ifmt, sw, sh);
    else if (target == GL_TEXTURE_CUBE_MAP_ARRAY)
        glTexStorage3D(target, levels, ifmt, sw, sh, d * 6 > 6 ? d : 6);
    else
        glTexStorage3D(target, levels, ifmt, sw, sh, d);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    t->last_used = g_frame;
    u32 k = hkey(gva);
    t->next = texh[k];
    texh[k] = t;
    return t;
}
Tex* tex_find_rt(u64 gva) {
    for (Tex* t = texh[hkey(gva)]; t; t = t->next)
        if (t->gva == gva && t->is_rt) return t;
    return NULL;
}

/* the game samples an RT through a smaller TIC (DRS, crops). On hw uv 1.0 is the TIC edge, not the RT
   edge, so hand out a copy of just that region */
PFNGLCOPYIMAGESUBDATAPROC p_cis;
static u64 st_rt_crop;
static Tex* rt_crop(Tex* rt, u32 w, u32 h) {
    if (!p_cis || getenv("SWR_NO_RTCROP")) return rt;
    Tex* c = rt->crop;
    if (c && (c->w != w || c->h != h)) {
        glDeleteTextures(1, &c->id);
        if (c->view) glDeleteTextures(1, &c->view);
        free(c);
        c = rt->crop = NULL;
    }
    if (!c) {
        g_tmap_gen++;
        c = calloc(1, sizeof *c);
        c->gva = rt->gva;
        c->w = w;
        c->h = h;
        c->d = 1;
        c->levels = 1;
        c->target = GL_TEXTURE_2D;
        c->f = rt->f;
        c->fkey = rt->fkey;
        c->is_rt = 1;
        c->bl = rt->bl;
        c->bh = rt->bh;
        c->swz[0] = GL_RED;
        c->swz[1] = GL_GREEN;
        c->swz[2] = GL_BLUE;
        c->swz[3] = GL_ALPHA;
        glGenTextures(1, &c->id);
        glBindTexture(GL_TEXTURE_2D, c->id);
        glTexStorage2D(GL_TEXTURE_2D, 1, rt->f.ifmt, SC(w), SC(h));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        c->crop_gen = ~0u;
        rt->crop = c;
        static int said;
        if (said < 10) {
            said++;
            LOG("gpu: %ux%u texture reads the %ux%u render target (%llx): only that region is used\n", w, h, rt->w,
                rt->h, (unsigned long long)rt->gva);
        }
    }
    if (c->crop_gen != rt->wgen) {
        p_cis(rt->id, GL_TEXTURE_2D, 0, 0, 0, 0, c->id, GL_TEXTURE_2D, 0, 0, 0, 0, (GLsizei)SC(w), (GLsizei)SC(h), 1);
        c->crop_gen = rt->wgen;
        st_rt_crop++;
    }
    c->last_used = g_frame;
    return c;
}
/* copies into mip levels of textures we assemble from RTs. Drivers disagree here (RX 7600: levels > 0
   come back black with CopyImageSubData, NVIDIA is fine), so gl_selftest() decides at startup:
   0 = glCopyImageSubData, 1 = blit. g_mipgen_manual: glGenerateMipmap from base level > 0 is broken,
   build the levels with linear blits */
static int g_lvl_mode, g_mipgen_manual;

static void lvl_attach(GLenum fbt, GLuint tex, GLenum target, int level, int layer) {
    if (target == GL_TEXTURE_2D || !tex)
        glFramebufferTexture2D(fbt, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
    else if (target == GL_TEXTURE_CUBE_MAP)
        glFramebufferTexture2D(fbt, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)layer, tex, level);
    else
        glFramebufferTextureLayer(fbt, GL_COLOR_ATTACHMENT0, tex, level, layer);
}
/* color blit src(target/level/layer) -> dst(level/layer); saves and restores scissor, srgb, color mask */
static void lvl_blit(GLuint src, GLenum st, int sl, int slay, GLsizei sw, GLsizei sh, GLuint dst, GLenum dtg, int dl,
                     int dlay, GLsizei dw, GLsizei dh, GLenum filt) {
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), sr = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLboolean cm[4] = {1, 1, 1, 1};
    glGetBooleanv(GL_COLOR_WRITEMASK, cm);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
    lvl_attach(GL_READ_FRAMEBUFFER, src, st, sl, slay);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
    lvl_attach(GL_DRAW_FRAMEBUFFER, dst, dtg, dl, dlay);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glBlitFramebuffer(0, 0, sw, sh, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, filt);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (sr) glEnable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, cm[0], cm[1], cm[2], cm[3]);
}
/* read one texel of a level via the read fbo (glReadPixels is queued, glGetTextureSubImage isn't) */
static void lvl_read1(GLuint tex, GLenum target, int level, int layer, GLint x, GLint y, float* out) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
    lvl_attach(GL_READ_FRAMEBUFFER, tex, target, level, layer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, out);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
}
static void lvl_copy(GLuint src, GLuint dst, GLenum target, int level, int layer, GLsizei w, GLsizei h, int color) {
    if (!g_lvl_mode || !color) {
        p_cis(src, GL_TEXTURE_2D, 0, 0, 0, 0, dst, target, level, 0, 0, target == GL_TEXTURE_2D ? 0 : layer, w, h, 1);
        return;
    }
    lvl_blit(src, GL_TEXTURE_2D, 0, 0, w, h, dst, target, level, layer, w, h, GL_NEAREST);
}
/* fill levels from+1..levels-1 from level 'from' */
static void lvl_gen(GLuint tex, GLenum target, int from, int levels, u32 layers, u32 W0, u32 H0) {
    if (!g_mipgen_manual) {
        glBindTexture(target, tex);
        glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, from);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
        glGenerateMipmap(target);
        glTexParameteri(target, GL_TEXTURE_BASE_LEVEL, 0);
        return;
    }
    u32 nl = target == GL_TEXTURE_CUBE_MAP ? 6 : target == GL_TEXTURE_2D ? 1 : layers;
    for (int l = from; l + 1 < levels; l++) {
        GLsizei sw = (GLsizei)(W0 >> l), sh = (GLsizei)(H0 >> l), dw = (GLsizei)(W0 >> (l + 1)),
                dh = (GLsizei)(H0 >> (l + 1));
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        for (u32 y = 0; y < nl; y++)
            lvl_blit(tex, target, l, (int)y, sw, sh, tex, target, l + 1, (int)y, dw, dh, GL_LINEAR);
    }
    glBindTexture(target, tex);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
}
/* cubemaps / arrays drawn face by face: each face+mip is its own 2D RT at
   gva + layer*layer_size + mip_offset. gather them into a real cube texture (GPU copies, only when a
   face changed) so cube samplers get something */
static u64 st_lay_copy;
/* 3D texture rendered slice by slice. In 3D block linear the slices of a block interleave per GOB:
   base + (z % block_depth) * (block_height * 512) + (z / block_depth) * block_layer_size.
   gather them into a 3D texture, copying only changed slices */
static u64 st_vol_copy;
static u64 slice3d_addr(u64 gva, u32 w, u32 h, u32 d, const GlFmt* f, u32 bh, u32 bd, u32 z) {
    u32 tw = (w + f->bw - 1) / f->bw, th = (h + f->bh - 1) / f->bh;
    u32 rowb = tw * f->bpp;
    u32 gx = (rowb + 63) / 64, gy = (th + 7) / 8;
    u32 b = bh;
    while (b > 0 && gy <= (1u << (b - 1))) b--;
    u32 bdd = bd;
    while (bdd > 0 && d <= (1u << (bdd - 1))) bdd--;
    u64 gys = ((u64)(gy + (1u << b) - 1) >> b) << b;
    return gva + (u64)(z >> bdd) * gx * gys * (1ull << bdd) * 512ull + ((u64)(z & ((1u << bdd) - 1)) << b) * 512ull;
}
static Tex* vol_from_slices(Tex* rt0, u64 gva, u32 w, u32 h, u32 d, GlFmt f, u32 bh, u32 bd) {
    if (!p_cis || d < 2 || d > 256 || !rt0->bl) return NULL;
    Tex* sl[256];
    u32 sig = 2166136261u;
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("SWR_LAYDBG") != 0;
    for (u32 z = 0; z < d; z++) {
        u64 sa = slice3d_addr(gva, w, h, d, &f, bh, bd, z);
        Tex* r = tex_find_rt(sa);
        if (!r || r->target != GL_TEXTURE_2D || r->w != w || r->h != h || r->f.ifmt != rt0->f.ifmt) {
            if (dbg) {
                static int n;
                if (n < 20) {
                    n++;
                    LOG("voldbg %llx %ux%ux%u bh=%u bd=%u z=%u at %llx: %s\n", (unsigned long long)gva, w, h, d, bh, bd,
                        z, (unsigned long long)sa, r ? "different rt" : "no rt");
                }
            }
            return NULL;
        }
        sl[z] = r;
        sig = (sig ^ (r->wgen + z * 31)) * 16777619u;
        sig = (sig ^ (u32)(uintptr_t)r) * 16777619u;
    }
    Tex* c = rt0->lay;
    if (c && (c->w != w || c->h != h || c->d != d || c->target != GL_TEXTURE_3D || c->fkey != rt0->fkey ||
              c->pitch != SC(w))) {
        glDeleteTextures(1, &c->id);
        if (c->view) glDeleteTextures(1, &c->view);
        free(c);
        c = rt0->lay = NULL;
        g_fb_sig_ok = 0;
        g_tmap_gen++;
    }
    if (!c) {
        g_tmap_gen++;
        c = calloc(1, sizeof *c);
        c->gva = gva;
        c->w = w;
        c->h = h;
        c->d = d;
        c->levels = 1;
        c->target = GL_TEXTURE_3D;
        c->f = rt0->f;
        c->fkey = rt0->fkey;
        c->is_rt = 1;
        c->bl = 1;
        c->bh = bh;
        c->pitch = SC(w);
        c->swz[0] = GL_RED;
        c->swz[1] = GL_GREEN;
        c->swz[2] = GL_BLUE;
        c->swz[3] = GL_ALPHA;
        glGenTextures(1, &c->id);
        glBindTexture(GL_TEXTURE_3D, c->id);
        glTexStorage3D(GL_TEXTURE_3D, 1, rt0->f.ifmt, SC(w), SC(h), d);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
        c->crop_gen = ~0u;
        rt0->lay = c;
        mb_reset();
        static int said;
        if (said < 12) {
            said++;
            LOG("gpu: 3D texture %ux%ux%u (%llx) assembled from its %u GPU-rendered slices\n", w, h, d,
                (unsigned long long)gva, d);
        }
    }
    if (c->crop_gen != sig) {
        for (u32 z = 0; z < d; z++)
            p_cis(sl[z]->id, GL_TEXTURE_2D, 0, 0, 0, 0, c->id, GL_TEXTURE_3D, 0, 0, 0, (GLint)z, (GLsizei)SC(w),
                  (GLsizei)SC(h), 1);
        c->crop_gen = sig;
        st_vol_copy++;
    }
    c->last_used = g_frame;
    return c;
}
static int getenv_cached_nomip(void) {
    static int v = -1;
    if (v < 0) v = getenv("SWR_NO_RT_MIPS") != NULL;
    return v;
}
static Tex* layered_from_rts(Tex* rt0, u64 gva, u32 w, u32 h, u32 d, u32 levels, GLenum target, GlFmt f, u32 fkey,
                             u32 bl, u32 bh, u32 bd) {
    if (!p_cis || levels > 15 || d < 1) return NULL;
    u64 loff[16], lsz = 0;
    for (u32 l = 0; l < levels; l++) {
        loff[l] = lsz;
        lsz += bl ? bl_level_size(w, h, 1, &f, bh, bd, l, NULL) : 0;
    }
    if (!bl) return NULL;
    if (d > 1) {
        u64 al = 512ull << bh;
        lsz = (lsz + al - 1) / al * al;
    }
    Tex* c = rt0->lay;
    if (c &&
        (c->w != w || c->h != h || c->d != d || c->levels != levels || c->target != target || c->fkey != rt0->fkey)) {
        glDeleteTextures(1, &c->id);
        if (c->view) glDeleteTextures(1, &c->view);
        free(c);
        c = rt0->lay = NULL;
        g_fb_sig_ok = 0;
        g_tmap_gen++;
    }
    u32 W0 = SC(w), H0 = SC(h);
    if (!c) {
        g_tmap_gen++;
        c = calloc(1, sizeof *c);
        c->gva = gva;
        c->w = w;
        c->h = h;
        c->d = d;
        c->levels = levels;
        c->target = target;
        c->f = rt0->f;
        c->fkey = rt0->fkey;
        c->is_rt = 1;
        c->bl = 1;
        c->bh = bh;
        c->swz[0] = GL_RED;
        c->swz[1] = GL_GREEN;
        c->swz[2] = GL_BLUE;
        c->swz[3] = GL_ALPHA;
        glGenTextures(1, &c->id);
        glBindTexture(target, c->id);
        if (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_2D)
            glTexStorage2D(target, levels, rt0->f.ifmt, W0, H0);
        else
            glTexStorage3D(target, levels, rt0->f.ifmt, W0, H0, d);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
        c->crop_gen = ~0u;
        c->stable = 0;
        rt0->lay = c;
        mb_reset();
        static int said;
        if (said < 6) {
            said++;
            LOG("gpu: %s %ux%u (%u layers, %u mips) assembled from its render targets (%llx)\n",
                target == GL_TEXTURE_CUBE_MAP ? "cubemap"
                : target == GL_TEXTURE_2D     ? "mip chain"
                                              : "array",
                w, h, d, levels, (unsigned long long)gva);
        }
    }
    /* signature: which RTs exist and their write counters */
    u32 sig = 2166136261u;
    int maxl = -1;
    for (u32 l = 0; l < levels; l++) {
        int all = 1;
        for (u32 y = 0; y < d; y++) {
            Tex* r = tex_find_rt(gva + (u64)y * lsz + loff[l]);
            u32 lw = w >> l, lh = h >> l;
            if (!lw) lw = 1;
            if (!lh) lh = 1;
            /* tiny mips get rendered into RTs padded to a GOB row (64 bytes), e.g. 8x4 / 8x2 for the 4x4 / 2x2 mips of an RGBA16F chain */
            u32 gobpx = rt0->f.bpp ? 64u / rt0->f.bpp : 16u;
            if (!gobpx) gobpx = 1;
            if (r && r->target == GL_TEXTURE_2D &&
                (r->w == lw || (l > 0 && r->w >= lw && (r->w < lw * 2 || r->w <= gobpx))) &&
                (r->h == lh || (l > 0 && r->h >= lh && r->h < lh * 2)) && r->f.ifmt == rt0->f.ifmt) {
                sig = (sig ^ (r->wgen + y * 31 + l * 977)) * 16777619u;
                sig = (sig ^ (u32)(uintptr_t)r) * 16777619u;
            } else
                all = 0;
        }
        if (!all) break;
        maxl = (int)l;
    }
    if (getenv("SWR_LAYDBG")) {
        static int n;
        if (n < 3) {
            n++;
            LOG("laydbg gva=%llx lsz=%llx bh=%u levels=%u maxl=%d\n", (unsigned long long)gva, (unsigned long long)lsz,
                bh, levels, maxl);
            for (u32 l = 0; l < levels; l++)
                for (u32 y = 0; y < d; y++) {
                    u64 a = gva + (u64)y * lsz + loff[l];
                    Tex* r = tex_find_rt(a);
                    LOG("  l%u c%u %llx -> %s %ux%u\n", l, y, (unsigned long long)a, r ? "rt" : "-", r ? r->w : 0,
                        r ? r->h : 0);
                }
            for (int k = 0; k < TEXH; k++)
                for (Tex* r = texh[k]; r; r = r->next)
                    if (r->is_rt && r->w <= 64 && r->h <= 64)
                        LOG("  RT %llx %ux%u\n", (unsigned long long)r->gva, r->w, r->h);
        }
    }
    if (maxl < 0) return NULL; /* missing faces -> normal path */
    if (c->crop_gen != sig) {
        int col = !rt0->f.depth && !rt0->f.is_int;
        for (int pass = 0; pass < 2; pass++) {
            for (int l = 0; l <= maxl; l++)
                for (u32 y = 0; y < d; y++) {
                    Tex* r = tex_find_rt(gva + (u64)y * lsz + loff[l]);
                    u32 cw = W0 >> l, ch = H0 >> l;
                    if (!cw) cw = 1;
                    if (!ch) ch = 1;
                    u32 rw = SC(r->w), rh = SC(r->h);
                    if (rw < cw) cw = rw;
                    if (rh < ch) ch = rh;
                    lvl_copy(r->id, c->id, target, l, (int)y, (GLsizei)cw, (GLsizei)ch, col);
                }
            /* sanity check the first few chains: center of level 1 must match its source RT. if the driver dropped the
         copy, switch to blits for good */
            static int nchk;
            if (pass || g_lvl_mode || !col || maxl < 1 || nchk >= 6) break;
            nchk++;
            Tex* r1 = tex_find_rt(gva + loff[1]);
            u32 cw = W0 >> 1, ch = H0 >> 1;
            u32 rw = SC(r1->w), rh = SC(r1->h);
            if (rw < cw) cw = rw;
            if (rh < ch) ch = rh;
            if (!cw || !ch) break;
            float a4[4] = {0}, b4[4] = {0};
            GLint px = (GLint)cw / 2, py = (GLint)ch / 2;
            lvl_read1(r1->id, GL_TEXTURE_2D, 0, 0, px, py, a4);
            lvl_read1(c->id, target, 1, 0, px, py, b4);
            int same = 1;
            for (int k = 0; k < 4; k++) {
                float e = a4[k] - b4[k];
                if (e < 0) e = -e;
                float m = a4[k] < 0 ? -a4[k] : a4[k];
                if (e > 0.02f + 0.02f * m) same = 0;
            }
            if (same) break;
            g_lvl_mode = 1;
            LOG("gpu: mip level copy check failed in game (%.3f %.3f %.3f -> %.3f %.3f %.3f): copies switched to blits\n",
                a4[0], a4[1], a4[2], b4[0], b4[1], b4[2]);
        }
        /* mips the game didn't render itself: generate them from the last real one. SMO samples its env cubemaps
       at LOD 5 (diffuse) and roughness*5 (spec, RGBM); with only level 0 those get the sharp image. a box chain
       isn't the game's prefilter but it's way closer than nothing */
        int topl = maxl;
        static int nogen = -1;
        if (nogen < 0) nogen = getenv("SWR_NO_CUBE_MIPGEN") != NULL;
        if (maxl < (int)levels - 1 && !nogen && !rt0->f.is_int && !rt0->f.depth &&
            (target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_2D)) {
            lvl_gen(c->id, target, maxl, (int)levels, d, W0, H0);
            topl = (int)levels - 1;
            mb_reset();
            static int said;
            if (said < 8) {
                said++;
                LOG("gpu: %s %ux%u at %llx: %d of %u mips rendered by the game, the rest generated\n",
                    target == GL_TEXTURE_CUBE_MAP ? "cubemap"
                    : target == GL_TEXTURE_2D     ? "mip chain"
                                                  : "array",
                    w, h, (unsigned long long)gva, maxl + 1, levels);
            }
        }
        if ((int)c->stable != topl) {
            glBindTexture(target, c->id);
            glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, topl);
            c->stable = (u32)topl;
            mb_reset();
        }
        c->crop_gen = sig;
        st_lay_copy++;
    }
    c->last_used = g_frame;
    rt0->last_used = g_frame;
    return c;
}
/* startup driver checks for the stuff the post-processing relies on. results go to the log as
   "gpu: driver check". each test writes a known color and reads it back */
static GLuint st_prog, st_samp, st_out, st_ofbo;
static int st_near(const float* a, const float* b, int n) {
    for (int i = 0; i < n; i++) {
        float d = a[i] - b[i];
        if (d < 0) d = -d;
        if (d > 0.03f || a[i] != a[i]) return 0;
    }
    return 1;
}
static void st_clear_level(GLuint tex, int level, const float* c) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_dfbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glClearBufferfv(GL_COLOR, 0, c);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
}
/* draw one pixel sampling tex at a fixed lod (min=max=lod, like the bloom passes) and return it */
static void st_sample(GLuint tex, float lod, float* out) {
    glSamplerParameterf(st_samp, GL_TEXTURE_MIN_LOD, lod);
    glSamplerParameterf(st_samp, GL_TEXTURE_MAX_LOD, lod);
    glBindFramebuffer(GL_FRAMEBUFFER, st_ofbo);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glViewport(0, 0, 1, 1);
    glUseProgram(st_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, st_samp);
    glBindVertexArray(g_scratch_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    float px[4] = {-9, -9, -9, -9};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, px);
    memcpy(out, px, 16);
    glBindSampler(0, 0);
    glUseProgram(0);
}
/* mip 1 filled from a separate RT, then sampled at lod 1 */
static int st_mip_copy(GLenum ifmt, int mode, float* got) {
    static const float c0[4] = {0.1f, 0.1f, 0.1f, 1}, c1[4] = {0.5f, 0.25f, 0.75f, 1};
    GLuint dst, src;
    glGenTextures(1, &dst);
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexStorage2D(GL_TEXTURE_2D, 3, ifmt, 8, 8);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glGenTextures(1, &src);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, 4, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    st_clear_level(dst, 0, c0);
    st_clear_level(dst, 1, c0);
    st_clear_level(src, 0, c1);
    int om = g_lvl_mode;
    g_lvl_mode = mode;
    lvl_copy(src, dst, GL_TEXTURE_2D, 1, 0, 4, 4, 1);
    g_lvl_mode = om;
    glBindTexture(GL_TEXTURE_2D, dst);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1);
    st_sample(dst, 1.0f, got);
    glDeleteTextures(1, &dst);
    glDeleteTextures(1, &src);
    return st_near(got, c1, 3);
}
/* draw level 1, generate 2.., sample level 2 */
static int st_mip_gen(GLenum ifmt, float* got) {
    static const float c0[4] = {0.1f, 0.1f, 0.1f, 1}, c1[4] = {0.5f, 0.25f, 0.75f, 1};
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 3, ifmt, 8, 8);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    st_clear_level(t, 0, c0);
    st_clear_level(t, 1, c1);
    st_clear_level(t, 2, c0);
    lvl_gen(t, GL_TEXTURE_2D, 1, 3, 1, 8, 8);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 2);
    st_sample(t, 2.0f, got);
    glDeleteTextures(1, &t);
    return st_near(got, c1, 3);
}
/* shader writes negative values to an SNORM target */
static int st_snorm(GLenum ifmt, int n, float* got) {
    static const char* fs = "#version 430 core\nout vec4 o; void main(){ o=vec4(-0.5,0.25,-1.0,0.75); }";
    static const char* vs =
        "#version 430 core\nvoid main(){ vec2 p=vec2(float((gl_VertexID&1)*4-1),float((gl_VertexID>>1)*4-1)); gl_Position=vec4(p,0.0,1.0); }";
    static GLuint pr;
    if (!pr) {
        GLuint a = compile(GL_VERTEX_SHADER, vs, NULL, 0), b = compile(GL_FRAGMENT_SHADER, fs, NULL, 0);
        if (a && b) pr = link2(a, b, 0);
        if (!pr) return 1;
    }
    GLuint t, fb;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexStorage2D(GL_TEXTURE_2D, 1, ifmt, 1, 1);
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    int ok = 0;
    got[0] = got[1] = got[2] = got[3] = -9;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        glViewport(0, 0, 1, 1);
        glUseProgram(pr);
        glBindVertexArray(g_scratch_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        /* read back with a shader, glReadPixels can clamp fixed point to 0..1 */
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        st_sample(t, 0.0f, got);
        static const float want[4] = {-0.5f, 0.25f, -1.0f, 0.75f};
        ok = st_near(got, want, n);
    }
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &t);
    return ok;
}
int g_texoff_emul;
void gl_selftest(void) {
    /* non-constant texture offsets: NVIDIA takes them, the spec / AMD / Mesa don't */
    if (g_gles)
        g_texoff_emul = 1;
    else {
        static const char* fs =
            "#version 430 core\nlayout(binding=0) uniform sampler2D t; uniform int k; out vec4 o; void main(){ o=textureOffset(t,vec2(0.5),ivec2(bitfieldExtract(k,0,4),bitfieldExtract(k,4,4))); }";
        GLuint sh = compile(GL_FRAGMENT_SHADER, fs, NULL, 0);
        g_texoff_emul = sh ? 0 : 1;
        if (sh) glDeleteShader(sh);
        if (getenv("SWR_TEXOFF_EMUL")) g_texoff_emul = 1;
        LOG("gpu: driver check: texture offsets from a register %s\n",
            g_texoff_emul ? "refused by the driver: added to the coordinates instead" : "accepted");
    }
    if (g_gles || !p_cis || getenv("SWR_NO_DRIVER_CHECK")) return;
    static const char* vs =
        "#version 430 core\nvoid main(){ vec2 p=vec2(float((gl_VertexID&1)*4-1),float((gl_VertexID>>1)*4-1)); gl_Position=vec4(p,0.0,1.0); }";
    static const char* fs =
        "#version 430 core\nlayout(binding=0) uniform sampler2D t; out vec4 o; void main(){ o=texture(t,vec2(0.5)); }";
    GLuint a = compile(GL_VERTEX_SHADER, vs, NULL, 0), b = compile(GL_FRAGMENT_SHADER, fs, NULL, 0);
    st_prog = a && b ? link2(a, b, 0) : 0;
    if (!st_prog) {
        LOG("gpu: driver check skipped (test shader did not compile)\n");
        return;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMaski(0, 1, 1, 1, 1);
    glGenSamplers(1, &st_samp);
    glSamplerParameteri(st_samp, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(st_samp, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(st_samp, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(st_samp, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &st_out);
    glBindTexture(GL_TEXTURE_2D, st_out);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, 1, 1);
    glGenFramebuffers(1, &st_ofbo);
    glBindFramebuffer(GL_FRAMEBUFFER, st_ofbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, st_out, 0);
    static const GLenum fmts[2] = {GL_R11F_G11F_B10F, GL_RGBA16F};
    static const char* fnm[2] = {"R11G11B10F", "RGBA16F"};
    float g[4];
    int force = -1;
    {
        const char* e = getenv("SWR_LEVEL_COPY");
        if (e) force = atoi(e);
    }
    /* copies into mip levels */
    int okm[2] = {1, 1};
    for (int m = 0; m < 2; m++)
        for (int k = 0; k < 2; k++) {
            int ok = st_mip_copy(fmts[k], m, g);
            okm[m] &= ok;
            LOG("gpu: driver check: mip level copy (%s, %s): %.2f %.2f %.2f -> %s\n", m ? "blit" : "CopyImageSubData",
                fnm[k], g[0], g[1], g[2], ok ? "ok" : "WRONG");
        }
    g_lvl_mode = force >= 0 ? (force ? 1 : 0) : okm[0] ? 0 : okm[1] ? 1 : 0;
    /* mips generated from a level above 0 */
    int okg = 1;
    for (int k = 0; k < 2; k++) {
        int ok = st_mip_gen(fmts[k], g);
        okg &= ok;
        LOG("gpu: driver check: mip generation from level 1 (%s): %.2f %.2f %.2f -> %s\n", fnm[k], g[0], g[1], g[2],
            ok ? "ok" : "WRONG");
    }
    if (!okg) {
        g_mipgen_manual = 1;
        int okg2 = 1;
        for (int k = 0; k < 2; k++) {
            int ok = st_mip_gen(fmts[k], g);
            okg2 &= ok;
            LOG("gpu: driver check: mip generation with blits (%s): %.2f %.2f %.2f -> %s\n", fnm[k], g[0], g[1], g[2],
                ok ? "ok" : "WRONG");
        }
        if (!okg2) g_mipgen_manual = 0;
    }
    if (getenv("SWR_MIPGEN_BLIT")) g_mipgen_manual = 1;
    /* SNORM render targets */
    int oks = st_snorm(GL_RGBA8_SNORM, 4, g);
    LOG("gpu: driver check: SNORM render target: %.2f %.2f %.2f %.2f -> %s\n", g[0], g[1], g[2], g[3],
        oks ? "ok" : "WRONG");
    if (!oks) {
        int okf = st_snorm(GL_RGBA16F, 4, g);
        g_snorm_fb = okf;
        LOG("gpu: driver check: half float instead: %.2f %.2f %.2f %.2f -> %s\n", g[0], g[1], g[2], g[3],
            okf ? "ok" : "WRONG");
    }
    if (getenv("SWR_SNORM_FLOAT")) g_snorm_fb = 1;
    LOG("gpu: driver check: mip level copies by %s, mip generation %s, SNORM targets %s\n",
        g_lvl_mode ? "blit" : "CopyImageSubData", g_mipgen_manual ? "with blits" : "by the driver",
        g_snorm_fb ? "stored as half float" : "native");
    glDeleteFramebuffers(1, &st_ofbo);
    glDeleteTextures(1, &st_out);
    glDeleteProgram(st_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    while (glGetError() != GL_NO_ERROR) {}
}
/* texture for a TIC */
static Tex* tex_from_tic_(u32* tic);
TdcEnt g_tdc[512];
u64 st_tdc_hit, st_tdc_miss;
Tex* tex_from_tic(u32* tic) {
    u32 k = (tic[1] ^ (tic[1] >> 9) ^ tic[0] ^ (tic[2] << 7) ^ tic[4] ^ (tic[5] << 3)) & 511;
    static int off = -1;
    if (off < 0) off = getenv("SWR_NO_TEX_MEMO") != NULL;
    if (!off && g_tdc[k].gen == g_tmap_gen && g_tdc[k].frame == (u32)g_frame && !memcmp(g_tdc[k].tic, tic, 32)) {
        st_tdc_hit++;
        return g_tdc[k].t;
    }
    Tex* t = tex_from_tic_(tic);
    st_tdc_miss++;
    /* tex_from_tic_ may have bumped the generation itself, cache under the new one */
    g_tdc[k].gen = g_tmap_gen;
    g_tdc[k].frame = (u32)g_frame;
    memcpy(g_tdc[k].tic, tic, 32);
    g_tdc[k].t = t;
    return t;
}
static Tex* tex_from_tic_(u32* tic) {
    u64 gva = ((u64)(tic[2] & 0xFFFF) << 32) | tic[1];
    u32 hv = (tic[2] >> 21) & 7;
    int bl = hv == 3 || hv == 4;
    u32 w = (tic[4] & 0xFFFF) + 1, h = (tic[5] & 0xFFFF) + 1, d = ((tic[5] >> 16) & 0x3FFF) + 1;
    u32 type = (tic[4] >> 23) & 0xF;
    int srgb = (tic[4] >> 22) & 1;
    u32 levels = ((tic[3] >> 28) & 0xF) + 1;
    u32 bh = (tic[3] >> 3) & 7, bd = (tic[3] >> 6) & 7, pitch = bl ? 0 : ((tic[3] & 0xFFFF) << 5);
    if (type == 0 || type == 4) h = 1;
    if (type == 3) {
        d = 6;
    } else if (type == 8) {
        d = d * 6;
    }
    GlFmt f = tic_format(tic[0], srgb);
    /* garbage TIC (bad format, null address, silly size): return null instead of trying to upload
     hundreds of MB of junk */
    if (!f.bpp || !gva || w > 16384 || h > 16384 || d > 2048 || levels > 15) { return NULL; }
    {
        u64 est = (u64)w * h * (type == 2 ? d : (d > 0 ? d : 1)) * (f.compressed ? 1 : f.bpp);
        if (est > (512ull << 20)) return NULL;
    }
    u32 fkey = (tic[0] & 0x7FFFF) | (srgb << 31);
    /* there's an RT at that address, use it */
    Tex* rt = tex_find_rt(gva);
    if (rt && rt->frame_checked != g_frame) {
        rt->frame_checked = g_frame;
        if (rt_sig(gva) != rt->hash) {
            rt->gva = ~0ull;
            rt = NULL;
            g_tmap_gen++;
        }
    }
    if (rt && rt->vol && rt->w == w && rt->h == h && d <= rt->vol->d &&
        ((type == 2 && rt->vol->target == GL_TEXTURE_3D) ||
         ((type == 5 || type == 1) && rt->vol->target == GL_TEXTURE_2D_ARRAY))) {
        rt->vol->last_used = g_frame;
        return rt->vol;
    }
    /* 3D TIC over a 2D RT: build from slices if they're all RTs, else the data is in memory */
    if (rt && type == 2 && d > 1 && !f.compressed && !f.astc) {
        Tex* v = vol_from_slices(rt, gva, w, h, d, f, bh, bd);
        if (v) return v;
        rt = NULL;
    } else if (rt && type == 2 && d > 1)
        rt = NULL;
    if (rt && !f.compressed && !f.astc && rt->target == GL_TEXTURE_2D && rt->w == w && rt->h == h &&
        (type == 3 || type == 8 || (type == 5 && d > 1))) {
        GLenum lt = type == 3 ? GL_TEXTURE_CUBE_MAP : type == 8 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_2D_ARRAY;
        Tex* lc = layered_from_rts(rt, gva, w, h, d, levels, lt, f, fkey, bl, bh, bd);
        if (lc) return lc;
    }
    /* mipped 2D TIC over an RT: the game renders each mip to its own RT and picks one via the sampler LOD clamp
     (SMO bloom: 160x180, 5 mips, read at lod 1..4). returning just level 0 makes every blur pass read the
     sharp image */
    if (rt && type == 1 && levels > 1 && d == 1 && !f.compressed && !f.astc && rt->target == GL_TEXTURE_2D &&
        rt->w == w && rt->h == h && !rt->f.depth && !getenv_cached_nomip()) {
        Tex* lc = layered_from_rts(rt, gva, w, h, 1, levels, GL_TEXTURE_2D, f, fkey, bl, bh, bd);
        if (lc) return lc;
    }
    if (rt && !f.compressed && !f.astc && rt->w >= w && rt->h >= h) {
        rt->last_used = g_frame;
        return (rt->w == w && rt->h == h) ? rt : rt_crop(rt, w, h);
    } /* smaller view -> just that region */
    GLenum target = tex_target(type, d);
    if (target == GL_TEXTURE_CUBE_MAP && d != 6) target = GL_TEXTURE_2D_ARRAY;
    if (f.astc && levels > 1) levels = 1;
    for (Tex* t = texh[hkey(gva)]; t; t = t->next) {
        if (t->gva == gva && t->w == w && t->h == h && t->d == d && t->fkey == fkey && t->target == target &&
            !t->is_rt) {
            t->last_used = g_frame;
            /* change check: frequent at first, backs off while the texture stays the same, capped in MB per frame */
            if (t->frame_checked != g_frame && (i32)(g_frame - t->next_check) >= 0) {
                if (g_tex_bud_frame != g_frame) {
                    g_tex_bud_frame = g_frame;
                    g_tex_bud = 24u << 20;
                }
                /* long-stable: cheap sampled check every 20 frames, sampled-hash check every 120 */
                if (g_nosample < 0) g_nosample = getenv("SWR_NO_SAMPLE_CHECK") != NULL;
                if (!g_nosample && t->stable >= 40 && t->size >= 16384 && (i32)(g_frame - t->tfull) < 120) {
                    u64 sh = sample_hash(gva, t->size);
                    t->frame_checked = g_frame;
                    if (sh == t->shash) {
                        t->next_check = g_frame + 20 + ((u32)(gva >> 9) & 7);
                        return t;
                    }
                    t->stable = 0;
                }
                if (g_tex_bud > 0 || t->stable < 4) {
                    t->frame_checked = g_frame;
                    g_tex_bud -= (i64)(t->size >> (t->size > 65536 ? 1 : 0));
                    st_tex_chk += t->size;
                    u64 hh = mem_hash(gva, t->size);
                    if (hh != t->hash) {
                        t->hash = hh;
                        tex_upload(t);
                        t->stable = 0;
                    } else if (t->stable < 100000)
                        t->stable++;
                    if (t->stable >= 39 && t->size >= 16384) {
                        t->shash = sample_hash(gva, t->size);
                        t->tfull = g_frame;
                    }
                    t->next_check = g_frame + (t->stable < 4 ? 4 : t->stable < 12 ? 8 : 20) + ((u32)(gva >> 9) & 3);
                }
            }
            return t;
        }
    }
    { /* debug: a texture that lives inside an RT is being read from memory */
        static int nd;
        if (nd < 24 && g_verbose >= 2) {
            for (int k = 0; k < TEXH && nd < 24; k++)
                for (Tex* q = texh[k]; q; q = q->next) {
                    if (!q->is_rt || q->gva == ~0ull) continue;
                    u64 rsz = (u64)q->w * q->h * (q->f.bpp ? q->f.bpp : 4);
                    if (gva >= q->gva && gva < q->gva + rsz) {
                        nd++;
                        LOG("gpu: texture %ux%u fmt %x at %llx falls inside render target %llx %ux%u (+%llx)%s: read from memory\n",
                            w, h, tic[0] & 0x7F, (unsigned long long)gva, (unsigned long long)q->gva, q->w, q->h,
                            (unsigned long long)(gva - q->gva), gva == q->gva ? " and is larger than it" : "");
                        break;
                    }
                }
        }
    }
    Tex* t = tex_create(gva, w, h, d, levels, target, f, fkey, bl, bh, bd, pitch, 0);
    u64 sz = 0;
    u32 dd = target == GL_TEXTURE_3D ? d : 1;
    for (u32 l = 0; l < levels; l++)
        sz += bl ? bl_level_size(w, h, dd, &f, bh, bd, l, NULL) : (u64)pitch * ((h + f.bh - 1) / f.bh);
    if (target != GL_TEXTURE_3D) sz *= d;
    t->size = sz;
    t->hash = mem_hash(gva, sz);
    t->frame_checked = g_frame;
    t->last_used = g_frame;
    tex_upload(t);
    return t;
}

/* ============================================================ samplers */
typedef struct {
    u32 tsc[8];
    GLuint id;
} Samp;
static Samp samps[1024];
static int nsamps;
static GLenum wrap(u32 m) {
    switch (m) {
        case 0: return GL_REPEAT;
        case 1: return GL_MIRRORED_REPEAT;
        case 2:
        case 4: return GL_CLAMP_TO_EDGE;
        case 3:
        case 6: return GL_CLAMP_TO_BORDER;
        case 5:
        case 7: return g_gles ? GL_MIRRORED_REPEAT : GL_MIRROR_CLAMP_TO_EDGE;
    }
    return GL_REPEAT;
}
static short samp_ht[4096];
GLuint sampler_get(const u32* tsc) {
    u32 hk = 2166136261u;
    for (int i = 0; i < 8; i++) {
        hk ^= tsc[i];
        hk *= 16777619u;
    }
    hk &= 4095;
    for (u32 j = hk;; j = (j + 1) & 4095) {
        int e = samp_ht[j];
        if (!e) break;
        if (!memcmp(samps[e - 1].tsc, tsc, 32)) return samps[e - 1].id;
    }
    GLuint s;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap(tsc[0] & 7));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap((tsc[0] >> 3) & 7));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, wrap((tsc[0] >> 6) & 7));
    u32 mag = tsc[1] & 3, min = (tsc[1] >> 4) & 3, mip = (tsc[1] >> 6) & 3;
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag == 2 ? GL_LINEAR : GL_NEAREST);
    GLenum mf = min == 2 ? (mip == 3   ? GL_LINEAR_MIPMAP_LINEAR
                            : mip == 2 ? GL_LINEAR_MIPMAP_NEAREST
                                       : GL_LINEAR)
                         : (mip == 3   ? GL_NEAREST_MIPMAP_LINEAR
                            : mip == 2 ? GL_NEAREST_MIPMAP_NEAREST
                                       : GL_NEAREST);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, mf);
    if ((tsc[0] >> 9) & 1) {
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, GL_NEVER + ((tsc[0] >> 10) & 7));
    }
    u32 aniso = (tsc[0] >> 20) & 7;
    if (aniso) glSamplerParameterf(s, 0x84FE, (float)(1 << aniso));
    float minl = (float)(tsc[2] & 0xFFF) / 256.f, maxl = (float)((tsc[2] >> 12) & 0xFFF) / 256.f;
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, minl);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, maxl);
    i32 bias = (i32)((tsc[1] >> 12) & 0x1FFF);
    if (bias & 0x1000) bias |= ~0x1FFF;
    if (!g_gles) glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, (float)bias / 256.f);
    float bc[4];
    {
        u32 b4[4];
        memcpy(b4, &tsc[4], 16);
        if (b4[3] & 0x80000000u) {
            b4[3] &= 0x7FFFFFFFu;
            glSamplerParameteri(s, 0x8A48, 0x8A4A);
        }
        memcpy(bc, b4, 16);
    }
    glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, bc); /* tsc[7] bit 31 = skip sRGB decode */
    if (nsamps < 1024) {
        memcpy(samps[nsamps].tsc, tsc, 32);
        samps[nsamps].id = s;
        nsamps++;
        u32 j = hk;
        while (samp_ht[j]) j = (j + 1) & 4095;
        samp_ht[j] = (short)nsamps;
    }
    return s;
}
