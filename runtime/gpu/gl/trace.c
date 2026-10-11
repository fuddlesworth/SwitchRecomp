/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the F5 frame trace */
#include "gpu/gl/gl.h"

/* ============================================================ F5 frame trace */
int g_tr_arm, g_tr_on, g_tr_n, g_tr_img;
static FILE* g_tr;
static Tex* g_tr_tex[2048];
static int g_tr_ntex;
static u64 g_tr_sh[1024];
static int g_tr_nsh;
static GLuint g_tr_fb, g_tr_tx;
#define TRW 256
#define TRH 144
void trace_note(const char* fmt, ...) {
    if (!g_tr_on || !g_tr) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_tr, fmt, ap);
    va_end(ap);
}
void glr_note_compute(void) {}
void glr_trace(const char* s) { trace_note("%s", s); }
/* color | alpha side by side, row 0 at the top */
static void tr_save(const char* path, const u8* rgba, u32 w, u32 h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w * 2, h);
    u8* row = malloc((size_t)w * 6);
    for (u32 y = 0; y < h; y++) {
        const u8* q = rgba + (size_t)y * w * 4;
        for (u32 x = 0; x < w; x++) {
            row[x * 3] = q[x * 4];
            row[x * 3 + 1] = q[x * 4 + 1];
            row[x * 3 + 2] = q[x * 4 + 2];
            u8 a = q[x * 4 + 3];
            row[(w + x) * 3] = a;
            row[(w + x) * 3 + 1] = a;
            row[(w + x) * 3 + 2] = a;
        }
        fwrite(row, 1, (size_t)w * 6, f);
    }
    free(row);
    fclose(f);
}
static void tr_thumb(Tex* t, char* out) {
    state_dirty();
    strcpy(out, "-");
    if (!t || t->f.depth || t->f.is_int || g_tr_img >= 2500) return;
    if (!g_tr_fb) {
        glGenFramebuffers(1, &g_tr_fb);
        glGenTextures(1, &g_tr_tx);
        glBindTexture(GL_TEXTURE_2D, g_tr_tx);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TRW, TRH);
    }
    GLboolean sc = glIsEnabled(GL_SCISSOR_TEST), sr = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_tr_fb);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_tr_tx, 0);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glBlitFramebuffer(0, 0, (GLint)SC(t->w), (GLint)SC(t->h), 0, 0, TRW, TRH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    static u8 px[TRW * TRH * 4];
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_tr_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, TRW, TRH, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    if (sc) glEnable(GL_SCISSOR_TEST);
    if (sr) glEnable(GL_FRAMEBUFFER_SRGB);
    u64 h = fast_hash(px, sizeof px, 0);
    if (h == t->tr_last) {
        strcpy(out, "(no change)");
        return;
    }
    t->tr_last = h;
    char p[128];
    snprintf(p, sizeof p, "captures/traces/d%04d.ppm", g_tr_n);
    tr_save(p, px, TRW, TRH);
    g_tr_img++;
    snprintf(out, 32, "d%04d.ppm", g_tr_n);
}
static int tr_tex_id(Tex* t) {
    if (!t) return -1;
    for (int i = 0; i < g_tr_ntex; i++)
        if (g_tr_tex[i] == t) return i;
    if (g_tr_ntex < 2048) {
        g_tr_tex[g_tr_ntex] = t;
        return g_tr_ntex++;
    }
    return -1;
}
static void tr_shader(u64 addr, Sh* sh, int stage) {
    if (!sh) return;
    for (int i = 0; i < g_tr_nsh; i++)
        if (g_tr_sh[i] == sh->hash) return;
    if (g_tr_nsh >= 1024) return;
    g_tr_sh[g_tr_nsh++] = sh->hash;
    u8* b = malloc(0x8000);
    gmem_read(addr, b, 0x8000);
    char p[128];
    snprintf(p, sizeof p, "captures/traces/sh_%016llx_%s.bin", (unsigned long long)sh->hash,
             stage == ST_VS ? "vs" : "fs");
    FILE* f = fopen(p, "wb");
    if (f) {
        fwrite(b, 1, 0x8000, f);
        fclose(f);
    }
    ShaderInfo si;
    memset(&si, 0, sizeof si);
    si.attr_int_mask = sh->si.attr_int_mask;
    si.attr_sint_mask = sh->si.attr_sint_mask;
    char* g = shader_to_glsl(b, 0x8000, stage, &si);
    if (g) {
        snprintf(p, sizeof p, "captures/traces/sh_%016llx_%s.glsl", (unsigned long long)sh->hash,
                 stage == ST_VS ? "vs" : "fs");
        f = fopen(p, "wb");
        if (f) {
            fputs(g, f);
            fclose(f);
        }
        free(g);
    }
    free(b);
    trace_note("  shader %016llx %s: %d untranslated%s%s, failed=%d\n", (unsigned long long)sh->hash,
               stage == ST_VS ? "vs" : "fs", sh->si.unsupported, sh->si.unsupported ? " first: " : "",
               sh->si.first_unsupported, sh->failed);
}
static void tr_textures(GpuChannel* ch, int stage, Sh* sh) {
    u32* r = ch->r3d;
    u32 tcb = r[0x982];
    u64 tic_base = ((u64)r[0x55D] << 32) | r[0x55E], tsc_base = ((u64)r[0x557] << 32) | r[0x558];
    for (int i = 0; i < sh->si.ntex; i++) {
        ShTex* st = &sh->si.tex[i];
        u32 handle = sh_tex_handle(stage, tcb, st);
        u32 tic[8], tsc[8];
        gmem_read(tic_base + (handle & 0xFFFFF) * 32ull, tic, 32);
        gmem_read(tsc_base + ((handle >> 20) & 0xFFF) * 32ull, tsc, 32);
        Tex* t = tex_from_tic(tic);
        trace_note(
            "  %s tex%d: sampler type%u%s%s  tic fmt=%05x %ux%ux%u type=%u levels=%u srgb=%u addr=%llx swz=%03x -> %s #%d %ux%u  tsc %08x %08x %08x\n",
            stage == 0 ? "vs" : "fs", i, st->type, st->array ? " array" : "", st->shadow ? " shadow" : "",
            tic[0] & 0x7FFFF, (tic[4] & 0xFFFF) + 1, (tic[5] & 0xFFFF) + 1, ((tic[5] >> 16) & 0x3FFF) + 1,
            (tic[4] >> 23) & 0xF, ((tic[3] >> 28) & 0xF) + 1, (tic[4] >> 22) & 1,
            (unsigned long long)(((u64)(tic[2] & 0xFFFF) << 32) | tic[1]), (tic[0] >> 19) & 0xFFF,
            t ? (t->is_rt ? "RT" : "tex") : "NULL", tr_tex_id(t), t ? t->w : 0, t ? t->h : 0, tsc[0], tsc[1], tsc[2]);
    }
}
static void tr_draw(GpuChannel* ch, Sh* vs, Sh* fs, u64 vsa, u64 fsa, int indexed, u32 topology, u32 count,
                    u32 instances) {
    if (!g_tr_on || g_tr_n >= 6000) return;
    u32* r = ch->r3d;
    g_tr_n++;
    float vt[6];
    memcpy(vt, &r[0x280], 24);
    float ref;
    memcpy(&ref, &r[0x4C4], 4);
    trace_note("#%d DRAW vs=%016llx fs=%016llx count=%u inst=%u topo=%u idx=%d\n", g_tr_n, (unsigned long long)vs->hash,
               (unsigned long long)fs->hash, count, instances, topology, indexed);
    for (int i = 0; i < g_nrt; i++)
        if (g_rt[i])
            trace_note("  rt%d %llx %ux%u fmt=%x%s\n", i, (unsigned long long)g_rt[i]->gva, g_rt[i]->w, g_rt[i]->h,
                       g_rt[i]->fkey,
                       g_rt[i]->vol ? (g_fb_lay[i] == -1 ? " (volume: all layers)" : " (volume: one layer)") : "");
    if (g_zt) {
        float dn, df;
        memcpy(&dn, &r[0x302], 4);
        memcpy(&df, &r[0x303], 4);
        trace_note(
            "  z %llx %ux%u fmt=%x test=%u func=%x write=%u stencil=%u zscale=%.9g ztrans=%.9g range=%.9g..%.9g mode=%u\n",
            (unsigned long long)g_zt->gva, g_zt->w, g_zt->h, g_zt->fkey, r[0x4B3], r[0x4C3], r[0x4BA], r[0x4E0], vt[2],
            vt[5], dn, df, r[0x35F] & 1);
    }
    trace_note(
        "  blend en=%u,%u indep=%u common[sep=%u eq=%x src=%x dst=%x eqa=%x srca=%x dsta=%x] ind0[%x %x %x %x %x %x %x] mask=%x/%x common=%u srgb=%u alphatest=%u f=%x ref=%.3f cull=%u/%x vp(%.0f,%.0f,%.0f,%.0f) origin=%x yctl=%x vpctl=%x\n",
        r[0x4D8], r[0x4D9], r[0x4B9], r[0x4CF], r[0x4D0], r[0x4D1], r[0x4D2], r[0x4D3], r[0x4D4], r[0x4D6], r[0x780],
        r[0x781], r[0x782], r[0x783], r[0x784], r[0x785], r[0x786], r[0x680], r[0x681], r[0x3E4], r[0x4B8], r[0x4BB],
        r[0x4C5], ref, r[0x646], r[0x648], vt[0], vt[1], vt[3], vt[4], r[0x4EB], r[0x5E4], r[0x35F]);
    tr_shader(vsa, vs, ST_VS);
    if (fsa) tr_shader(fsa, fs, ST_FS);
    tr_textures(ch, 0, vs);
    if (fsa) tr_textures(ch, 4, fs);
    if (count <= 6) { /* fullscreen passes (tonemap, fog, lighting): dump their constants */
        for (int st = 0; st < 2; st++) {
            Sh* sh = st ? fs : vs;
            int stg = st ? 4 : 0;
            if (!sh || (st && !fsa)) continue;
            for (int ci = 0; ci < 18; ci++)
                if (sh->si.cbuf_mask & (1u << ci)) {
                    u64 ca = g_cb.cb[stg][ci];
                    if (!ca) continue;
                    float v[64];
                    gmem_read(ca, v, sizeof v);
                    trace_note("  %s cb%d:", st ? "fs" : "vs", ci);
                    for (int k = 0; k < 16; k++)
                        trace_note(" d[%d]=(%.4g,%.4g,%.4g,%.4g)", k, v[k * 4], v[k * 4 + 1], v[k * 4 + 2],
                                   v[k * 4 + 3]);
                    trace_note("\n");
                }
        }
    }
    char nm[40];
    tr_thumb(g_rt[0], nm);
    trace_note("  result rt0: %s\n", nm);
}
void trace_frame(void) {
    if (g_tr_on) {
        trace_note("==== end of frame. textures used: %d ====\n", g_tr_ntex);
        for (int i = 0; i < g_tr_ntex; i++) {
            Tex* t = g_tr_tex[i];
            trace_note("tex #%d %llx %ux%ux%u fmt=%x target=%x levels=%u rt=%d\n", i, (unsigned long long)t->gva, t->w,
                       t->h, t->d, t->fkey, t->target, t->levels, t->is_rt);
            if (!t->f.depth && !t->f.is_int && !t->f.compressed && (u64)t->w * t->h * t->d <= 4096 && !g_gles &&
                (t->target == GL_TEXTURE_2D || t->target == GL_TEXTURE_3D)) {
                /* small textures (exposure, LUTs): first texel + average */
                u32 W = t->is_rt ? SC(t->w) : t->w, H = t->is_rt ? SC(t->h) : t->h,
                    D = t->target == GL_TEXTURE_3D ? t->d : 1;
                float* px = malloc((size_t)W * H * D * 16);
                if (px) {
                    glBindTexture(t->target, t->id);
                    glPixelStorei(GL_PACK_ALIGNMENT, 4);
                    glGetTexImage(t->target, 0, GL_RGBA, GL_FLOAT, px);
                    double m[4] = {0};
                    for (u32 k = 0; k < W * H * D; k++)
                        for (int c = 0; c < 4; c++) m[c] += px[k * 4 + c];
                    trace_note("   texel0=(%.4g,%.4g,%.4g,%.4g) mean=(%.4g,%.4g,%.4g,%.4g)\n", px[0], px[1], px[2],
                               px[3], m[0] / (W * H * D), m[1] / (W * H * D), m[2] / (W * H * D), m[3] / (W * H * D));
                    free(px);
                    mb_reset();
                }
            }
            if (t->f.depth || t->f.is_int || (t->target != GL_TEXTURE_2D && t->target != GL_TEXTURE_2D_ARRAY)) continue;
            /* GPU downsizes to max 512 and we read only that; layer 0 for arrays */
            u32 w = t->is_rt ? SC(t->w) : t->w, h = t->is_rt ? SC(t->h) : t->h;
            u32 st = 1;
            while (w / st > 512 || h / st > 512) st *= 2;
            u32 ow = w / st ? w / st : 1, oh = h / st ? h / st : 1;
            u8* o = malloc((size_t)ow * oh * 4);
            if (!o) continue;
            int ok = 0;
            if (!t->f.compressed) {
                GLuint tt = 0, fb = 0;
                glGenTextures(1, &tt);
                glBindTexture(GL_TEXTURE_2D, tt);
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, ow, oh);
                glGenFramebuffers(1, &fb);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rfbo);
                if (t->target == GL_TEXTURE_2D_ARRAY)
                    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t->id, 0, 0);
                else
                    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tt, 0);
                if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
                    glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                    glDisable(GL_SCISSOR_TEST);
                    glDisable(GL_FRAMEBUFFER_SRGB);
                    glColorMaski(0, 1, 1, 1, 1);
                    {
                        GLenum bb = GL_COLOR_ATTACHMENT0;
                        glDrawBuffers(1, &bb);
                    }
                    glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, 0, 0, (GLint)ow, (GLint)oh, GL_COLOR_BUFFER_BIT,
                                      GL_LINEAR);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, o);
                    ok = 1;
                }
                glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
                glDeleteFramebuffers(1, &fb);
                glDeleteTextures(1, &tt);
                state_dirty();
                mb_reset();
            }
            if (!ok && t->target == GL_TEXTURE_2D && (u64)w * h <= (1u << 20)) {
                u8* px = malloc((size_t)w * h * 4);
                if (px) {
                    glBindTexture(GL_TEXTURE_2D, t->id);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    for (u32 y = 0; y < oh; y++)
                        for (u32 x = 0; x < ow; x++)
                            memcpy(o + ((size_t)y * ow + x) * 4, px + ((size_t)y * st * w + x * st) * 4, 4);
                    free(px);
                    ok = 1;
                }
            }
            if (ok) {
                char p[160];
                snprintf(p, sizeof p, "captures/traces/tex%03d_%llx_%ux%u_f%x%s.ppm", i, (unsigned long long)t->gva,
                         t->w, t->h, t->fkey, t->is_rt ? "_rt" : "");
                tr_save(p, o, ow, oh);
            }
            free(o);
        }
        fclose(g_tr);
        g_tr = NULL;
        g_tr_on = 0;
        LOG("trace: frame saved in captures/traces (%d draws, %d images)\n", g_tr_n, g_tr_img);
        fflush(NULL);
    }
    {
        static int envf = -2;
        if (envf == -2) {
            const char* e = getenv("SWR_TRACE");
            envf = e ? atoi(e) : -1;
        }
        if (envf >= 0 && (int)g_frame == envf) g_tr_arm = 1;
    }
    if (g_tr_arm) {
        g_tr_arm = 0;
        plat_mkdir("captures");
        plat_mkdir("captures/traces");
        g_tr = fopen("captures/traces/trace.txt", "w");
        g_tr_on = g_tr != NULL;
        g_tr_n = 0;
        g_tr_img = 0;
        g_tr_ntex = 0;
        g_tr_nsh = 0;
        for (int k = 0; k < TEXH; k++)
            for (Tex* t = texh[k]; t; t = t->next) t->tr_last = 0;
        LOG("trace: recording the next frame...\n");
    }
}
static void cs_flush_pending(void);
void glr_draw(GpuChannel* ch, int indexed, u32 topology, u32 first, u32 count, u32 base_vertex, u32 base_instance,
              u32 instances) {
    u32* r = ch->r3d;
    g_draws++;
    if (g_lfn && !(g_draws & 15)) lsfg_pump(); /* push pending generated frames out on time even mid-frame */
    if (!count) return;
    /* shaders */
    u64 code = ((u64)r[0x582] << 32) | r[0x583];
    Sh* vs = NULL;
    Sh* fs = NULL;
    u32 attr_int = 0, attr_sint = 0;
    for (int i = 0; i < 32; i++) {
        u32 a = r[0x458 + i];
        u32 ty = (a >> 27) & 7;
        if (ty == 3 || ty == 4) {
            attr_int |= 1u << i;
            if (ty == 3) attr_sint |= 1u << i;
        }
    }
    /* stage slots: 0 VertexA, 1 VertexB, 2 TessCtrl, 3 TessEval, 4 Geometry, 5 Fragment.
     VertexB is always on even with the enable bit clear (NVN doesn't set it). An enabled VertexA runs first
     (it usually writes the position, B the varyings), so both are translated as one program */
    g_last_r3d = r;
    for (int i = 1; i < 6; i += 4) {
        u32 cfg = r[0x800 + i * 0x10];
        if (i != 1 && !(cfg & 1)) continue;
        u64 addr = code + r[0x801 + i * 0x10];
        if (i == 1)
            TM(TM_SH, vs = shader_get(addr, (r[0x800] & 1) ? code + r[0x801] : 0, ST_VS, attr_int, attr_sint));
        else
            TM(TM_SH, fs = fs_get_keyed(ch, addr));
    }
    if ((r[0x820] | r[0x830] | r[0x840]) & 1) { /* tess / geometry: not translated yet, draw with just VS+FS */
        static u64 gseen[16];
        static int ng;
        u64 ga = code + r[0x841];
        int k = 0;
        for (; k < ng; k++)
            if (gseen[k] == ga) break;
        if (k == ng && ng < 16) {
            gseen[ng++] = ga;
            LOG("gpu: draw uses unsupported stages:%s%s%s (gs at %llx, rt0 %ux%u layers %u 3d %u) - drawn without them\n",
                (r[0x820] & 1) ? " TessCtrl" : "", (r[0x830] & 1) ? " TessEval" : "", (r[0x840] & 1) ? " Geometry" : "",
                (unsigned long long)ga, r[0x202], r[0x203], r[0x206] & 0xFFFF, (r[0x205] >> 16) & 1);
            if (r[0x840] & 1) {
                plat_mkdir("captures");
                char fp[160];
                snprintf(fp, sizeof fp, "captures/gs_%02d.bin", ng);
                u8* b = malloc(0x8000);
                gmem_read(ga, b, 0x8000);
                FILE* f = fopen(fp, "wb");
                if (f) {
                    fwrite(b, 1, 0x8000, f);
                    fclose(f);
                }
                free(b);
            }
        }
        trace_note("WARNING: unsupported stages enabled (tess/geometry)\n");
    }
    if (vs && !fs) { /* depth-only passes: use an empty FS */
        static Sh nullfs;
        static char nsrc[64];
        if (!nullfs.gl && !nullfs.failed) {
            char e[512];
            if (g_gles)
                snprintf(nsrc, sizeof nsrc, "#version 320 es\nvoid main(){}\n");
            else
                snprintf(nsrc, sizeof nsrc, "#version %d core\nvoid main(){}\n", g_glsl_version);
            nullfs.stage = ST_FS;
            nullfs.hash = 0x4E554C4C46530001ull;
            nullfs.src = nsrc;
            nullfs.gl = compile(GL_FRAGMENT_SHADER, nsrc, e, sizeof e);
            if (!nullfs.gl) nullfs.failed = 1;
        }
        fs = &nullfs;
    }
    if (!vs || !fs) {
        st_noshader++;
        trace_note("DRAW SKIPPED: no shader\n");
        return;
    }
    if (vs->failed || fs->failed) {
        st_shfail++;
        trace_note("DRAW SKIPPED: failed shader vs=%016llx(%d) fs=%016llx(%d)\n", (unsigned long long)vs->hash,
                   vs->failed, (unsigned long long)fs->hash, fs->failed);
        return;
    }
    Prog* p = NULL;
    GLuint vpp = 0, atp = 0;
    GLint vpl = -1, atl = -1;
    /* draws into tables (3D/array RTs, or <= 64x64) usually happen once at level load. if we skip them while the
     shader compiles the table stays empty forever, so wait for the compile here (one hitch) */
    int must_wait = 0;
    {
        u32 ctl = r[0x487];
        if ((ctl & 15) >= 1) {
            u32* q = &r[0x200 + ((ctl >> 4) & 7) * 0x10];
            if (q[0] | q[1]) {
                if (((q[5] >> 16) & 1) || (q[6] & 0xFFFF) > 1 || (q[2] && q[3] && q[2] <= 64 && q[3] <= 64))
                    must_wait = 1;
            }
        }
    }
    if (must_wait && g_sep > 0) {
        u64 t0 = plat_ns();
        while ((!sh_sep_ready(vs) || !sh_sep_ready(fs)) && plat_ns() - t0 < 3000000000ull) plat_sleep_ns(500000);
        st_waited++;
    }
    if (g_sep > 0) {
        int rv, rf;
        TM(TM_SHREADY, (rv = sh_sep_ready(vs), rf = sh_sep_ready(fs)));
        if (!rv || !rf) {
            st_compiling++;
            trace_note("DRAW SKIPPED: compiling\n");
            return;
        }
        if (!vs->sp || !fs->sp) {
            st_noprog++;
            trace_note("DRAW SKIPPED: shader without program vs=%016llx fs=%016llx\n", (unsigned long long)vs->hash,
                       (unsigned long long)fs->hash);
            return;
        }
        vpp = vs->sp;
        vpl = vs->uloc;
        atp = fs->sp;
        atl = fs->uloc;
    } else {
        p = program_get(vs, fs);
        if (must_wait) {
            u64 t0 = plat_ns();
            while (!program_ready(p) && plat_ns() - t0 < 3000000000ull) plat_sleep_ns(500000);
            st_waited++;
        }
        if (!program_ready(p)) {
            st_compiling++;
            trace_note("DRAW SKIPPED: compiling\n");
            return;
        }
        vpp = atp = p->p;
        vpl = p->vp_loc;
        atl = p->at_loc;
        if (!p->p) {
            st_noprog++;
            {
                static int said;
                if (said < 5) {
                    said++;
                    LOG("gpu: program not linked vs=%016llx fs=%016llx active stages:%s%s%s%s%s%s\n",
                        (unsigned long long)vs->hash, (unsigned long long)fs->hash, (r[0x800] & 1) ? " VertexA" : "",
                        (r[0x810] & 1) ? " VertexB" : "", (r[0x820] & 1) ? " TessCtrl" : "",
                        (r[0x830] & 1) ? " TessEval" : "", (r[0x840] & 1) ? " Geometry" : "",
                        (r[0x850] & 1) ? " Fragment" : "");
                }
            }
            trace_note("DRAW SKIPPED: link failed vs=%016llx fs=%016llx\n", (unsigned long long)vs->hash,
                       (unsigned long long)fs->hash);
            return;
        }
    }
    g_draw_layered = vs->si.writes_layer;
    TM(TM_STATE, bind_framebuffer(ch));
    if (g_nrt && g_rt[0]) {
        g_last_cw = g_rt[0]->w;
        g_last_ch = g_rt[0]->h;
    }
    {
        if (!g_opt_shadows && !g_nrt && g_zt && (g_zt->w != g_last_cw || g_zt->h != g_last_ch)) {
            trace_note("DRAW SKIPPED: shadow\n");
            /* ...and clear the map to far (1.0) once a frame so it reads as fully lit instead of black */
            {
                if (g_zt->skipclr != g_frames + 1) {
                    g_zt->skipclr = g_frames + 1;
                    static PFNGLCLEARTEXIMAGEPROC p_cz;
                    if (!p_cz) p_cz = (PFNGLCLEARTEXIMAGEPROC)backend_gl_proc("glClearTexImage");
                    if (p_cz) {
                        if (g_zt->f.fmt == GL_DEPTH_STENCIL) {
                            struct {
                                float d;
                                u32 s;
                            } ds = {1.0f, 0};
                            p_cz(g_zt->id, 0, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, &ds);
                        } else {
                            float one = 1.0f;
                            p_cz(g_zt->id, 0, GL_DEPTH_COMPONENT, GL_FLOAT, &one);
                        }
                        g_zt->wgen++;
                    }
                }
            }
            return;
        }
    }
    if (!g_nrt && !g_zt) {
        st_nort++;
        trace_note("DRAW SKIPPED: no target\n");
        return;
    }
    st_drawn++;
    if (g_sep > 0) {
        if (!g_pipe_on) {
            glUseProgram(0);
            p_bpp(g_pipe);
            g_pipe_on = 1;
        }
        if (g_pipe_vs != vpp) {
            p_ups(g_pipe, 0x00000001 /* VERTEX_SHADER_BIT */, vpp);
            g_pipe_vs = vpp;
        }
        if (g_pipe_fs != atp) {
            p_ups(g_pipe, 0x00000002 /* FRAGMENT_SHADER_BIT */, atp);
            g_pipe_fs = atp;
        }
    } else
        glUseProgram(p->p);
    TM(TM_STATE, apply_state(ch));
    {
        float vt[2];
        memcpy(vt, &r[0x280], 8);
        float fl[4] = {vt[1] < 0 ? -1.f : 1.f, p_clip ? 0.f : (float)(r[0x35F] & 1), 0, 0};
        if (vpl >= 0) {
            if (g_sep > 0) {
                if (!vs->lu_ok || memcmp(vs->lu, fl, 16)) {
                    memcpy(vs->lu, fl, 16);
                    vs->lu_ok = 1;
                    glProgramUniform4fv(vpp, vpl, 1, fl);
                }
            } else
                glProgramUniform4fv(vpp, vpl, 1, fl);
        }
    }
    if (atl >= 0) { /* alpha test: 0x4BB enable, 0x4C4 ref, 0x4C5 func (GL 0x200.. or D3D 1..8) */
        u32 fn = r[0x4C5];
        int f = fn >= 0x200 && fn <= 0x207 ? (int)(fn - 0x200) : fn >= 1 && fn <= 8 ? (int)fn - 1 : 7;
        float ref;
        memcpy(&ref, &r[0x4C4], 4);
        float at[4] = {(r[0x4BB] & 1) ? 1.f : 0.f, (float)f, ref, 0};
        if (g_sep > 0) {
            if (!fs->lu_ok || memcmp(fs->lu, at, 16)) {
                memcpy(fs->lu, at, 16);
                fs->lu_ok = 1;
                glProgramUniform4fv(atp, atl, 1, at);
            }
        } else
            glProgramUniform4fv(atp, atl, 1, at);
    }
    TM(TM_CB, (upload_cbufs(0, vs), upload_cbufs(4, fs)));
    TM(TM_GB, (bind_gbufs(0, vs), bind_gbufs(4, fs)));
    TM(TM_TEX, (bind_textures(ch, 0, vs), bind_textures(ch, 4, fs)));
    if (g_diag_draws > 0 && g_rt[0] && g_rt[0]->w >= 640) {
        g_diag_draws--;
        capture_draw(ch, indexed, first, count);
        float vt[6];
        memcpy(vt, &r[0x280], 24);
        LOG("draw %llu: vs %016llx fs %016llx rt0 %ux%u fmt=%x zt=%s count=%u inst=%u topo=%u\n",
            (unsigned long long)g_draws, (unsigned long long)vs->hash, (unsigned long long)fs->hash, g_rt[0]->w,
            g_rt[0]->h, g_rt[0]->fkey, g_zt ? "yes" : "no", count, instances, topology);
        LOG("   viewport scale(%.1f,%.1f,%.2f) center(%.1f,%.1f,%.2f) zmode=%u scissor=%u mask0=%x common=%u blend0=%u eq=%x src=%x dst=%x ztest=%u zfunc=%x zwrite=%u cull=%u\n",
            vt[0], vt[1], vt[2], vt[3], vt[4], vt[5], r[0x35F] & 1, r[0x380] & 1, r[0x680], r[0x3E4], r[0x4D8],
            r[0x4B9] ? r[0x781] : r[0x4D0], r[0x4B9] ? r[0x782] : r[0x4D1], r[0x4B9] ? r[0x783] : r[0x4D2], r[0x4B3],
            r[0x4C3], r[0x4BA], r[0x646]);
        LOG("   vs: %d textures, cbufs %x   fs: %d textures, cbufs %x, sph outputs %x\n", vs->si.ntex, vs->si.cbuf_mask,
            fs->si.ntex, fs->si.cbuf_mask, fs->si.sph[18]);
        for (int k = 0; k < fs->si.ntex && k < 6; k++) {
            ShTex* st = &fs->si.tex[k];
            u32 tcb = r[0x982];
            u32 hd = sh_tex_handle(4, tcb, st);
            u64 tb = ((u64)r[0x55D] << 32) | r[0x55E];
            u32 tic[8];
            gmem_read(tb + (hd & 0xFFFFF) * 32ull, tic, 32);
            LOG("   fs tex%d: handle=%x tic fmt=%x %ux%u type=%u addr=%llx\n", k, hd, tic[0] & 0x7FFFF,
                (tic[4] & 0xFFFF) + 1, (tic[5] & 0xFFFF) + 1, (tic[4] >> 23) & 0xF,
                (unsigned long long)(((u64)(tic[2] & 0xFFFF) << 32) | tic[1]));
        }
    }
    /* vertex attribs */
    u64 tb0_ = plat_ns();
    glBindVertexArray(g_vao);
    u32 used = vs->si.in_mask;
    /* attrib state cache for g_vao */
    static u32 va_en;
    static u32 va_key[16][8];
    static int va_const[16];
    if (g_va_inval) {
        g_va_inval = 0;
        memset(va_key, 0, sizeof va_key);
    }
#define VA_OFF(i_)                                                                                                     \
    do {                                                                                                               \
        if (va_en & (1u << (i_))) {                                                                                    \
            glDisableVertexAttribArray(i_);                                                                            \
            va_en &= ~(1u << (i_));                                                                                    \
        }                                                                                                              \
    } while (0)
    for (int i = 0; i < 16; i++) {
        if (!(used & (1u << i))) {
            VA_OFF(i);
            continue;
        }
        u32 a = r[0x458 + i];
        u32 buf = a & 31;
        int constant = (a >> 6) & 1;
        u32 off = (a >> 7) & 0x3FFF;
        u32 size = (a >> 21) & 0x3F, ty = (a >> 27) & 7;
        int bgra = (a >> 31) & 1;
        u32* va = &r[0x700 + buf * 4];
        if (constant || !(va[0] & 0x1000)) {
            VA_OFF(i);
            if (!va_const[i]) {
                glVertexAttrib4f(i, 0, 0, 0, 1);
                va_const[i] = 1;
            }
            continue;
        }
        u64 start = ((u64)va[1] << 32) | va[2];
        u64 lim = ((u64)r[0x7C0 + buf * 2] << 32) | r[0x7C1 + buf * 2];
        u32 stride = va[0] & 0xFFF;
        u64 len = lim >= start ? lim - start + 1 : 0;
        if (len > 64u << 20) len = 64u << 20;
        if (!len) {
            VA_OFF(i);
            continue;
        }
        GLuint vbid = buf_cached(GL_ARRAY_BUFFER, start, len);
        u32 cnt = attr_count(size), bits = attr_bits(size);
        GLenum gt;
        GLboolean norm = ty == 1 || ty == 2;
        if (size == 0x30)
            gt = ty == 1 ? GL_INT_2_10_10_10_REV : GL_UNSIGNED_INT_2_10_10_10_REV;
        else if (size == 0x31)
            gt = GL_UNSIGNED_INT_10F_11F_11F_REV;
        else if (ty == 7)
            gt = bits == 32 ? GL_FLOAT : GL_HALF_FLOAT;
        else if (ty == 1 || ty == 3 || ty == 6)
            gt = bits == 32 ? GL_INT : bits == 16 ? GL_SHORT : GL_BYTE;
        else
            gt = bits == 32 ? GL_UNSIGNED_INT : bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        GLint gcnt = (bgra && cnt == 4) ? GL_BGRA : (GLint)cnt;
        if (gcnt == GL_BGRA) norm = GL_TRUE;
        u32 div = r[0x620 + buf] ? (va[3] ? va[3] : 1) : 0;
        u32 key[8] = {vbid, (u32)gcnt, gt, (u32)norm | ((ty == 3 || ty == 4) ? 2u : 0u), stride, off, div, cnt};
        if (!(va_en & (1u << i))) {
            glEnableVertexAttribArray(i);
            va_en |= 1u << i;
        }
        va_const[i] = 0;
        if (memcmp(va_key[i], key, sizeof key)) {
            memcpy(va_key[i], key, sizeof key);
            if (ty == 3 || ty == 4)
                glVertexAttribIPointer(i, cnt, gt, stride, (void*)(size_t)off);
            else
                glVertexAttribPointer(i, gcnt, gt, norm, stride, (void*)(size_t)off);
            glVertexAttribDivisor(i, div);
        }
    }
    g_tm[TM_BUF] += plat_ns() - tb0_;
    tb0_ = plat_ns();
    GLenum mode = topo(topology);
    if (topology == 7 || topology == 8 || topology == 9) {
        mode = GL_TRIANGLE_FAN;
        if (topology == 7) mode = GL_TRIANGLES;
        if (topology == 8) mode = GL_TRIANGLE_STRIP;
    } /* quad strip = tri strip */
    if (indexed) {
        u32 fmt = r[0x5F6];
        u32 isz = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
        u64 ia = ((u64)r[0x5F2] << 32) | r[0x5F3];
        u64 ilen = (u64)(first + count) * isz;
        u8* src = NULL;
        if (topology == 7) {
            src = gmem_ptr(ia, ilen);
            if (!src) {
                if (g_stage_cap < ilen) {
                    u8* nb = realloc(g_stage_buf, ilen);
                    if (!nb) return;
                    g_stage_buf = nb;
                    g_stage_cap = ilen;
                }
                gmem_read(ia, g_stage_buf, ilen);
                src = g_stage_buf;
            }
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibuf);
        GLenum it = isz == 1 ? GL_UNSIGNED_BYTE : isz == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        if (topology == 7) { /* quads -> tris */
            u32 nq = count / 4;
            u32* idx = malloc((size_t)nq * 6 * 4);
            for (u32 k = 0; k < nq; k++) {
                u32 v[4];
                for (int j = 0; j < 4; j++) {
                    u64 o = (u64)(first + k * 4 + j) * isz;
                    v[j] = isz == 1 ? src[o] : isz == 2 ? *(u16*)(src + o) : *(u32*)(src + o);
                }
                u32 t6[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
                memcpy(idx + k * 6, t6, 24);
            }
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)nq * 24, idx, GL_STREAM_DRAW);
            free(idx);
            glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, nq * 6, GL_UNSIGNED_INT, 0, instances,
                                                          (GLint)base_vertex, base_instance);
        } else {
            buf_cached(GL_ELEMENT_ARRAY_BUFFER, ia + (u64)first * isz, (u64)count * isz); /* just this draw's range */
            glDrawElementsInstancedBaseVertexBaseInstance(mode, count, it, (void*)0, instances, (GLint)base_vertex,
                                                          base_instance);
        }
    } else {
        if (topology == 7) {
            u32 nq = count / 4;
            u32* idx = malloc((size_t)nq * 24 + 4);
            for (u32 k = 0; k < nq; k++) {
                u32 b = first + k * 4;
                u32 t6[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
                memcpy(idx + k * 6, t6, 24);
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_ibuf);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)nq * 24, idx, GL_STREAM_DRAW);
            free(idx);
            glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, nq * 6, GL_UNSIGNED_INT, 0, instances, 0,
                                                          base_instance);
        } else
            glDrawArraysInstancedBaseInstance(mode, first, count, instances, base_instance);
    }
    g_tm[TM_DRAW] += plat_ns() - tb0_;
    tr_draw(ch, vs, fs, code + r[0x801 + 1 * 0x10], (r[0x800 + 5 * 0x10] & 1) ? code + r[0x801 + 5 * 0x10] : 0, indexed,
            topology, count, instances);
}

void glr_clear(GpuChannel* ch, u32 v) {
    state_dirty();
    g_tmap_gen++;
    u32* r = ch->r3d;
    st_clears++;
    u32 rt = (v >> 6) & 15;
    g_clear_layer = (int)((v >> 10) & 0x7FF);
    bind_framebuffer(ch);
    g_clear_layer = -1;
    if (r[0x43E] & 0x10 && (r[0x380] & 1)) {
        u32 xs = r[0x381], ys = r[0x382];
        glEnable(GL_SCISSOR_TEST);
        glScissorIndexed(0, (GLint)((xs & 0xFFFF) * g_scale), (GLint)((ys & 0xFFFF) * g_scale),
                         (GLsizei)(((xs >> 16) - (xs & 0xFFFF)) * g_scale + 0.5f),
                         (GLsizei)(((ys >> 16) - (ys & 0xFFFF)) * g_scale + 0.5f));
    } else
        glDisable(GL_SCISSOR_TEST);
    if (v & 0x3C) {
        Tex* t = rt < 8 ? g_rt[rt] : NULL;
        if (!t && rt < 8) { /* clear targets an RT that isn't bound (Kirby does this): clear the texture directly */
            u32* q = &r[0x200 + rt * 0x10];
            u64 a = ((u64)q[0] << 32) | q[1];
            if (a && q[2] && q[3] && q[4]) {
                Tex* ct = rt_texture(a, q[2], q[3], q[4], q[5], 0);
                static PFNGLCLEARTEXIMAGEPROC p_cti;
                if (!p_cti) p_cti = (PFNGLCLEARTEXIMAGEPROC)backend_gl_proc("glClearTexImage");
                if (ct && p_cti && !ct->f.depth) {
                    float cc[4];
                    memcpy(cc, &r[0x360], 16);
                    if (ct->f.is_int) {
                        GLuint ui[4] = {(GLuint)cc[0], (GLuint)cc[1], (GLuint)cc[2], (GLuint)cc[3]};
                        p_cti(ct->id, 0, ct->f.fmt, GL_UNSIGNED_INT, ui);
                    } else
                        p_cti(ct->id, 0, GL_RGBA, GL_FLOAT, cc);
                    ct->wgen++;
                    static int said;
                    if (!said) {
                        said = 1;
                        LOG("gpu: clear of a RT outside the active list: cleared directly\n");
                    }
                }
            }
        }
        /* single float RT with color + depth (+stencil): one glClear so the driver can fast clear it all at once */
        if (t && rt == 0 && g_nrt == 1 && !t->f.is_int && (v & 0x3C) == 0x3C && (v & 1) && g_zt &&
            !getenv("SWR_NO_CLEARMERGE")) {
            float cc[4], dz;
            memcpy(cc, &r[0x360], 16);
            memcpy(&dz, &r[0x364], 4);
            glColorMaski(0, 1, 1, 1, 1);
            glClearColor(cc[0], cc[1], cc[2], cc[3]);
            glDepthMask(GL_TRUE);
            glClearDepth((double)dz);
            GLbitfield m = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
            if ((v & 2) && g_zt->f.fmt == GL_DEPTH_STENCIL) {
                glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
                glClearStencil((GLint)r[0x368]);
                m |= GL_STENCIL_BUFFER_BIT;
            }
            glClear(m);
            v &= ~3u;
            t = NULL;
        }
        if (t) {
            glColorMaski(rt, (v >> 2) & 1, (v >> 3) & 1, (v >> 4) & 1, (v >> 5) & 1);
            float cc[4];
            memcpy(cc, &r[0x360], 16);
            if (t->f.is_int) {
                GLuint ui[4] = {(GLuint)cc[0], (GLuint)cc[1], (GLuint)cc[2], (GLuint)cc[3]};
                glClearBufferuiv(GL_COLOR, rt, ui);
            } else
                glClearBufferfv(GL_COLOR, rt, cc);
        }
    }
    if (g_tr_on && g_tr_n < 6000) {
        g_tr_n++;
        float cc[4];
        memcpy(cc, &r[0x360], 16);
        char nm[40];
        Tex* t = rt < 8 ? g_rt[rt] : NULL;
        {
            float dz;
            memcpy(&dz, &r[0x364], 4);
            trace_note("#%d CLEAR rt%u %llx mask=%x color=(%.2f,%.2f,%.2f,%.2f) depth=%.9g z=%llx\n", g_tr_n, rt,
                       (unsigned long long)(t ? t->gva : 0), v & 0x3F, cc[0], cc[1], cc[2], cc[3], dz,
                       (unsigned long long)(g_zt ? g_zt->gva : 0));
        }
        if (v & 0x3C) {
            tr_thumb(t, nm);
            trace_note("  result: %s\n", nm);
        }
    }
    if ((v & 3) && g_zt) {
        float dz;
        memcpy(&dz, &r[0x364], 4);
        glDepthMask(GL_TRUE);
        glStencilMaskSeparate(GL_FRONT_AND_BACK, 0xFF);
        if ((v & 3) == 3 && g_zt->f.fmt == GL_DEPTH_STENCIL)
            glClearBufferfi(GL_DEPTH_STENCIL, 0, dz, (GLint)r[0x368]);
        else if (v & 1)
            glClearBufferfv(GL_DEPTH, 0, &dz);
        else {
            GLint s = (GLint)r[0x368];
            glClearBufferiv(GL_STENCIL, 0, &s);
        }
    }
}
