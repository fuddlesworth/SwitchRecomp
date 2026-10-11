/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* the F7 recorder (Windows), F8 full-resolution screenshots, screenshots and render target dumps */
#include "gpu/gl/gl.h"
#include "core/clock.h"

#ifdef _WIN32
/* ============================================================ F7 recorder
   each presented frame gets converted to NV12 (BT.709) on the GPU at window size, same letterboxing as on
   screen. read back through a PBO two frames later so the GPU thread never waits. Windows does the H.264
   encode on its own thread */
#define REC_N 3
static u64 g_rec_n, g_rec_vt0, g_rec_lastts;
int g_rec_on;
static int g_rec_w, g_rec_h;
static u64 g_rec_t0, g_rec_last;
static u32 g_rec_k;
static u64 g_rec_out, g_rec_base;
static int g_rec_have;
static GLuint g_rec_fbo[2], g_rec_tex[2], g_rec_prog[2], g_rec_pbo[REC_N];
static GLsync g_rec_sync[REC_N];
static u64 g_rec_ts[REC_N];
static PFNGLMAPBUFFERRANGEPROC p_rmap;
static PFNGLUNMAPBUFFERPROC p_runmap;
static PFNGLFENCESYNCPROC p_rfs;
static PFNGLCLIENTWAITSYNCPROC p_rcws;
static PFNGLDELETESYNCPROC p_rds;
static const char* REC_VS =
    "#version 430 core\nout vec2 uv; uniform vec4 xf; void main(){ vec2 p=vec2((gl_VertexID&1)*2.0-1.0,(gl_VertexID>>1)*2.0-1.0); vec2 q=p*0.5+0.5; q.y=1.0-q.y; uv=q*xf.zw+xf.xy; gl_Position=vec4(p,0.0,1.0); }";
static const char* REC_FS_Y =
    "#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; void main(){ vec3 c=clamp(texture(t,uv).rgb,0.0,1.0); o=vec4(16.0/255.0+dot(c,vec3(0.2126,0.7152,0.0722))*(219.0/255.0)); }";
static const char* REC_FS_UV =
    "#version 430 core\nin vec2 uv; out vec4 o; layout(binding=0) uniform sampler2D t; void main(){ vec3 c=clamp(texture(t,uv).rgb,0.0,1.0);\n"
    " o=vec4(128.0/255.0+dot(c,vec3(-0.1146,-0.3854,0.5))*(224.0/255.0), 128.0/255.0+dot(c,vec3(0.5,-0.4542,-0.0458))*(224.0/255.0), 0.0, 0.0); }";
static void rec_gl_free(void) {
    for (int i = 0; i < REC_N; i++) {
        if (g_rec_sync[i]) {
            p_rds(g_rec_sync[i]);
            g_rec_sync[i] = 0;
        }
    }
    glDeleteBuffers(REC_N, g_rec_pbo);
    glDeleteFramebuffers(2, g_rec_fbo);
    glDeleteTextures(2, g_rec_tex);
    memset(g_rec_pbo, 0, sizeof g_rec_pbo);
    memset(g_rec_fbo, 0, sizeof g_rec_fbo);
    memset(g_rec_tex, 0, sizeof g_rec_tex);
}
static void rec_start(void) {
    if (!p_rmap) {
        p_rmap = (PFNGLMAPBUFFERRANGEPROC)glq_proc("glMapBufferRange");
        p_runmap = (PFNGLUNMAPBUFFERPROC)glq_proc("glUnmapBuffer");
        p_rfs = (PFNGLFENCESYNCPROC)glq_proc("glFenceSync");
        p_rcws = (PFNGLCLIENTWAITSYNCPROC)glq_proc("glClientWaitSync");
        p_rds = (PFNGLDELETESYNCPROC)glq_proc("glDeleteSync");
    }
    if (!p_rmap || !p_runmap || !p_rfs || !p_rcws || !p_rds) {
        WARN("recorder: the video driver lacks the required features\n");
        return;
    }
    if (!g_rec_prog[0]) {
        GLuint v = compile(GL_VERTEX_SHADER, REC_VS, NULL, 0);
        g_rec_prog[0] = link2(v, compile(GL_FRAGMENT_SHADER, REC_FS_Y, NULL, 0), 0);
        g_rec_prog[1] =
            link2(compile(GL_VERTEX_SHADER, REC_VS, NULL, 0), compile(GL_FRAGMENT_SHADER, REC_FS_UV, NULL, 0), 0);
    }
    int ww, wh;
    backend_window_size(&ww, &wh);
    g_rec_w = ww & ~3;
    g_rec_h = wh & ~3;
    if (g_rec_w < 64 || g_rec_h < 64 || !g_rec_prog[0] || !g_rec_prog[1]) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* sl = wcsrchr(dir, L'\\');
    wchar_t name[128] = L"game";
    if (sl) {
        wcsncpy(name, sl + 1, 127);
        name[127] = 0;
        wchar_t* dot = wcsrchr(name, L'.');
        if (dot) *dot = 0;
        sl[1] = 0;
    } else
        dir[0] = 0;
    wcscat(dir, L"recordings");
    CreateDirectoryW(dir, NULL);
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(path, MAX_PATH, L"%ls\\%ls_%04d-%02d-%02d_%02d-%02d-%02d.mp4", dir, name, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    if (!rec_open(path, g_rec_w, g_rec_h, g_opt_rec_fps)) return;
    glGenTextures(2, g_rec_tex);
    glGenFramebuffers(2, g_rec_fbo);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, g_rec_tex[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, i ? GL_RG8 : GL_R8, i ? g_rec_w / 2 : g_rec_w, i ? g_rec_h / 2 : g_rec_h);
        glBindFramebuffer(GL_FRAMEBUFFER, g_rec_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rec_tex[i], 0);
    }
    glGenBuffers(REC_N, g_rec_pbo);
    for (int i = 0; i < REC_N; i++) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_rec_pbo[i]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)g_rec_w * g_rec_h * 3 / 2, NULL, GL_STREAM_READ);
        g_rec_sync[i] = 0;
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_rec_t0 = plat_ns();
    g_rec_last = 0;
    g_rec_k = 0;
    g_rec_on = 1;
    g_rec_n = 0;
    g_rec_out = 0;
    g_rec_base = 0;
    g_rec_have = 0;
    backend_rec_state(1);
    {
        g_rec_active = 1;
        vt_set(1);
        g_rec_vt0 = guest_ns();
        g_rec_lastts = 0;
        if (g_vt_on)
            LOG("recorder: smooth mode (the game runs in slow motion if the PC can't keep up; the video comes out at a steady %d FPS)\n",
                g_opt_rec_fps);
    }
}
/* PBO from REC_N-1 frames ago: hand it over if it's ready, otherwise skip (never wait) */
static void rec_drain(int all) {
    for (u32 n = 0; n < REC_N; n++) {
        u32 i = (g_rec_k + n) % REC_N;
        if (!g_rec_sync[i]) continue;
        GLenum r = p_rcws(g_rec_sync[i], 0, all ? 2000000000ull : 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
            if (all) {
                p_rds(g_rec_sync[i]);
                g_rec_sync[i] = 0;
                continue;
            } else
                return;
        }
        p_rds(g_rec_sync[i]);
        g_rec_sync[i] = 0;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_rec_pbo[i]);
        size_t n12 = (size_t)g_rec_w * g_rec_h * 3 / 2;
        const u8* p = (const u8*)p_rmap(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)n12, GL_MAP_READ_BIT);
        if (p) { /* constant-fps output: output frame k sits at k/fps. each captured frame covers its guest time up to the
         next one, so it gets repeated or dropped. weird clock jumps are clamped */
            u64 per = 1000000000ull / (u64)(g_opt_rec_fps > 0 ? g_opt_rec_fps : 60);
            u64 ts = g_rec_ts[i];
            if (g_rec_out == 0 && !g_rec_have) { g_rec_base = ts; }
            u64 rel = ts > g_rec_base ? ts - g_rec_base : 0;
            u64 want = rel / per + 1; /* output frames that should exist up to this one */
            if (g_rec_have && want > g_rec_out + 30) {
                g_rec_base += (want - (g_rec_out + 1)) * per;
                want = g_rec_out + 1;
            } /* > 0.5s jump: emit one frame */
            if (!g_rec_have) want = want > g_rec_out ? want : g_rec_out + 1;
            u64 fps = (u64)(g_opt_rec_fps > 0 ? g_opt_rec_fps : 60);
            while (g_rec_out < want) {
                rec_frame(p, (i64)(g_rec_out * 10000000ull / fps),
                          (i64)((g_rec_out + 1) * 10000000ull / fps - g_rec_out * 10000000ull / fps));
                g_rec_out++;
            }
            g_rec_have = 1;
            p_runmap(GL_PIXEL_PACK_BUFFER);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
}
void rec_stop(void) {
    if (!g_rec_on) return;
    rec_drain(1);
    rec_close();
    rec_gl_free();
    g_rec_on = 0;
    backend_rec_state(0);
    {
        vt_set(0);
        g_rec_active = 0;
    }
}
volatile int g_rec_exit; /* at exit: 1 = finalize, 2 = done */
void glr_rec_finish(void) {
    if (!g_rec_on) return;
    g_rec_exit = 1;
    u64 t0 = plat_ns();
    while (g_rec_exit != 2 && plat_ns() - t0 < 4000000000ull) plat_sleep_ns(10000000);
}
static void rec_capture(GLuint tex, const float* xf, int tw, int th) {
    if (g_rec_toggle) {
        g_rec_toggle = 0;
        if (g_rec_on)
            rec_stop();
        else
            rec_start();
    }
    if (!g_rec_on) return;
    u64 now = plat_ns();
    if (!g_vt_on && g_rec_last && now - g_rec_last < 15500000ull) return; /* max 60/s (smooth mode: every real frame) */
    g_rec_last = now;
    rec_drain(0);
    u32 i = g_rec_k % REC_N;
    if (g_rec_sync[i]) {
        if (!g_vt_on) return;
        rec_drain(1);
    } /* smooth mode never drops */
    int dw = g_rec_w, dh = g_rec_w * th / (tw ? tw : 1);
    if (dh > g_rec_h) {
        dh = g_rec_h;
        dw = g_rec_h * tw / (th ? th : 1);
    }
    dw &= ~1;
    dh &= ~1;
    int ox = ((g_rec_w - dw) / 2) & ~1, oy = ((g_rec_h - dh) / 2) & ~1;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    for (int k = 0; k < 2; k++) {
        int sx = k ? 2 : 1;
        glBindFramebuffer(GL_FRAMEBUFFER, g_rec_fbo[k]);
        {
            GLenum b = GL_COLOR_ATTACHMENT0;
            glDrawBuffers(1, &b);
        }
        glViewport(0, 0, g_rec_w / sx, g_rec_h / sx);
        if (k)
            glClearColor(128.0f / 255, 128.0f / 255, 0, 0);
        else
            glClearColor(16.0f / 255, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(ox / sx, oy / sx, dw / sx, dh / sx);
        glUseProgram(g_rec_prog[k]);
        glUniform4fv(glGetUniformLocation(g_rec_prog[k], "xf"), 1, xf);
        glBindVertexArray(g_scratch_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_rec_pbo[i]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rec_fbo[0]);
    glReadPixels(0, 0, g_rec_w, g_rec_h, GL_RED, GL_UNSIGNED_BYTE, (void*)0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_rec_fbo[1]);
    glReadPixels(0, 0, g_rec_w / 2, g_rec_h / 2, GL_RG, GL_UNSIGNED_BYTE, (void*)(size_t)((size_t)g_rec_w * g_rec_h));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    g_rec_sync[i] = p_rfs(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    {
        u64 ts = g_vt_on ? guest_ns() - g_rec_vt0 : now - g_rec_t0;
        if (g_rec_n && ts < g_rec_lastts) ts = g_rec_lastts;
        g_rec_lastts = ts;
        g_rec_ts[i] = ts;
    }
    g_rec_n++;
    g_rec_k++;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(g_blit_prog);
}
#else
static void rec_capture(GLuint tex, const float* xf, int tw, int th) {
    (void)tex;
    (void)xf;
    (void)tw;
    (void)th;
}
void glr_rec_finish(void) {}
#endif
/* ============================================================ F8 full-res screenshot (png)
   GPU copies the frame into a PBO, we map it a frame or two later and a worker thread writes the png */
volatile int g_shot8_req;
static GLuint g_s8_fbo, g_s8_tex, g_s8_pbo;
static GLsync g_s8_sync;
static int g_s8_w, g_s8_h, g_s8_tw, g_s8_th;
static PFNGLMAPBUFFERRANGEPROC p_s8map;
static PFNGLUNMAPBUFFERPROC p_s8unmap;
static PFNGLFENCESYNCPROC p_s8fs;
static PFNGLCLIENTWAITSYNCPROC p_s8cws;
static PFNGLDELETESYNCPROC p_s8ds;
typedef struct {
    u8* px;
    int w, h;
    char path[320];
} S8Job;
static u32 s8_crc_tab[256];
static u32 s8_crc(u32 c, const u8* d, size_t n) {
    if (!s8_crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 k = i;
            for (int j = 0; j < 8; j++) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            s8_crc_tab[i] = k;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = s8_crc_tab[(c ^ d[i]) & 255] ^ (c >> 8);
    return ~c;
}
static void s8_be32(u8* p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static void s8_chunk(FILE* f, const char* t, const u8* d, u32 n) {
    u8 h[8];
    s8_be32(h, n);
    memcpy(h + 4, t, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    u32 c = s8_crc(0, (const u8*)t, 4);
    c = s8_crc(c, d, n);
    u8 cc[4];
    s8_be32(cc, c);
    fwrite(cc, 1, 4, f);
}
/* RGB png, Sub filter per row, stored (uncompressed) deflate. fast, ~3 bytes/px */
static void s8_thread(void* arg) {
    S8Job* j = (S8Job*)arg;
    int w = j->w, h = j->h;
    size_t row = (size_t)w * 3 + 1, raw = row * h;
    u8* r = malloc(raw);
    if (!r) {
        free(j->px);
        free(j);
        return;
    }
    for (int y = 0; y < h; y++) {
        const u8* s = j->px + (size_t)(h - 1 - y) * w * 4;
        u8* o = r + (size_t)y * row;
        o[0] = 1;
        u8 pr = 0, pg = 0, pb = 0;
        for (int x = 0; x < w; x++) {
            u8 R = s[x * 4], G = s[x * 4 + 1], B = s[x * 4 + 2];
            o[1 + x * 3] = (u8)(R - pr);
            o[2 + x * 3] = (u8)(G - pg);
            o[3 + x * 3] = (u8)(B - pb);
            pr = R;
            pg = G;
            pb = B;
        }
    }
    free(j->px);
    size_t nb = (raw + 65534) / 65535;
    size_t zn = 2 + raw + nb * 5 + 4;
    u8* z = malloc(zn);
    if (z) {
        size_t o = 0;
        z[o++] = 0x78;
        z[o++] = 0x01;
        u32 a = 1, b = 0;
        for (size_t i = 0; i < raw; i += 65535) {
            size_t n = raw - i;
            if (n > 65535) n = 65535;
            z[o++] = (i + n >= raw) ? 1 : 0;
            z[o++] = (u8)n;
            z[o++] = (u8)(n >> 8);
            z[o++] = (u8)~n;
            z[o++] = (u8)(~n >> 8);
            memcpy(z + o, r + i, n);
            o += n;
            for (size_t k = 0; k < n; k++) {
                a = (a + r[i + k]) % 65521u;
                b = (b + a) % 65521u;
            }
        }
        s8_be32(z + o, (b << 16) | a);
        o += 4;
        FILE* f = fopen(j->path, "wb");
        if (f) {
            static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
            fwrite(sig, 1, 8, f);
            u8 ih[13];
            s8_be32(ih, (u32)w);
            s8_be32(ih + 4, (u32)h);
            ih[8] = 8;
            ih[9] = 2;
            ih[10] = 0;
            ih[11] = 0;
            ih[12] = 0;
            s8_chunk(f, "IHDR", ih, 13);
            s8_chunk(f, "IDAT", z, (u32)o);
            s8_chunk(f, "IEND", NULL, 0);
            fclose(f);
            LOG("F8 screenshot: %s (%dx%d)\n", j->path, w, h);
        } else
            WARN("F8 screenshot: could not write %s\n", j->path);
        free(z);
    }
    free(r);
    free(j);
}
static void shot8_poll(void) {
    if (!g_s8_sync) return;
    GLenum st = p_s8cws(g_s8_sync, 0, 0);
    if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) return;
    p_s8ds(g_s8_sync);
    g_s8_sync = 0;
    size_t n = (size_t)g_s8_w * g_s8_h * 4;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_s8_pbo);
    const u8* p = (const u8*)p_s8map(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)n, GL_MAP_READ_BIT);
    S8Job* j = calloc(1, sizeof *j);
    if (p && j && (j->px = malloc(n))) {
        memcpy(j->px, p, n);
        j->w = g_s8_w;
        j->h = g_s8_h;
        time_t tt = time(NULL);
        struct tm* tm = localtime(&tt);
        static int seq;
        plat_mkdir("captures");
        snprintf(j->path, sizeof j->path, "captures/F8_%04d%02d%02d_%02d%02d%02d_%d.png", tm->tm_year + 1900,
                 tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, ++seq);
        if (plat_thread(s8_thread, j, 4u << 20)) { s8_thread(j); }
        j = NULL;
    }
    if (j) {
        free(j->px);
        free(j);
    }
    if (p) p_s8unmap(GL_PIXEL_PACK_BUFFER);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
static void shot8_capture(GLuint tex, const float* xf, int tw, int th) {
    shot8_poll();
    {
        static int tf;
        if (!tf) { tf = getenv("SWR_SHOT8_TEST") ? 1 : -1; }
        if (tf > 0 && ++tf == 40) g_shot8_req = 1;
    } /* automated test hook */
    if (!g_shot8_req || g_s8_sync) return;
    g_shot8_req = 0;
    if (!p_s8map) {
        p_s8map = (PFNGLMAPBUFFERRANGEPROC)glq_proc("glMapBufferRange");
        p_s8unmap = (PFNGLUNMAPBUFFERPROC)glq_proc("glUnmapBuffer");
        p_s8fs = (PFNGLFENCESYNCPROC)glq_proc("glFenceSync");
        p_s8cws = (PFNGLCLIENTWAITSYNCPROC)glq_proc("glClientWaitSync");
        p_s8ds = (PFNGLDELETESYNCPROC)glq_proc("glDeleteSync");
    }
    if (!p_s8map || !p_s8unmap || !p_s8fs || !p_s8cws || !p_s8ds) {
        WARN("F8 screenshot: the video driver lacks the required features\n");
        return;
    }
    /* actual size of the shown content (visible part of the texture) */
    int w = (int)(tw * fabsf(xf[2]) + 0.5f), h = (int)(th * fabsf(xf[3]) + 0.5f);
    if (w < 16 || h < 16 || w > 8192 || h > 8192) return;
    if (w != g_s8_w || h != g_s8_h) {
        if (g_s8_tex) {
            glDeleteTextures(1, &g_s8_tex);
            glDeleteFramebuffers(1, &g_s8_fbo);
            glDeleteBuffers(1, &g_s8_pbo);
        }
        glGenTextures(1, &g_s8_tex);
        glBindTexture(GL_TEXTURE_2D, g_s8_tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
        glGenFramebuffers(1, &g_s8_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_s8_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_s8_tex, 0);
        glGenBuffers(1, &g_s8_pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, g_s8_pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)w * h * 4, NULL, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        g_s8_w = w;
        g_s8_h = h;
    }
    (void)g_s8_tw;
    (void)g_s8_th;
    glBindFramebuffer(GL_FRAMEBUFFER, g_s8_fbo);
    {
        GLenum b = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &b);
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMaski(0, 1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glUseProgram(g_blit_prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_s8_pbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_s8_fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void*)0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g_s8_sync = p_s8fs(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}
/* blit a texture to the window, 16:9 letterboxed. xf = uv transform (crop/flip) */
void screen_quad(GLuint tex, const float* xf, int tw, int th) {
    int ww, wh;
    backend_window_size(&ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, 0x8A48, 0x8A4A); /* TEXTURE_SRGB_DECODE = SKIP_DECODE */
    glColorMaski(0, 1, 1, 1, 1);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int dw = ww, dh = ww * th / (tw ? tw : 1);
    if (dh > wh) {
        dh = wh;
        dw = wh * tw / (th ? th : 1);
    }
    glViewport((ww - dw) / 2, (wh - dh) / 2, dw, dh);
    glUseProgram(g_blit_prog);
    g_pipe_on = 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
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
    rec_capture(tex, xf, tw, th);
    shot8_capture(tex, xf, tw, th);
    glUseProgram(g_blit_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, 0x8A48, 0x8A49);
} /* restore DECODE for when the game samples it */
void screen_swap(void) {
    TM(TM_SWAP, glq_swap());
    state_dirty();
    g_pipe_on = 0;
    mb_reset();
} /* Android overlay may touch state/program */

/* ============================================================ GPU thread */
u64 g_last_shot_hash;
void do_screenshot(const char* path) {
    int w = g_last_w ? g_last_w : 1280, h = g_last_h ? g_last_h : 720;
    if (g_last_is_rt) {
        w = (int)SC((u32)w);
        h = (int)SC((u32)h);
    }
    u8* px = malloc((size_t)w * h * 4);
    if (g_last_present_tex) {
        GLuint fb;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_last_present_tex, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fb);
    } else
        memset(px, 0, (size_t)w * h * 4);
    u64 hh = 1469598103934665603ull;
    for (int y = 0; y < h; y++) {
        int sy = g_last_flip ? y : h - 1 - y;
        for (int x = 0; x < w; x++) {
            u32 v;
            memcpy(&v, px + ((size_t)sy * w + x) * 4, 4);
            v |= 0xFF000000u;
            hh ^= v;
            hh *= 1099511628211ull;
        }
    }
    if (path) {
        char p[300];
        snprintf(p, sizeof p, "%s.ppm", path);
        FILE* f = fopen(p, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; y++) {
                int sy = g_last_flip ? y : h - 1 - y;
                for (int x = 0; x < w; x++) fwrite(px + ((size_t)sy * w + x) * 4, 1, 3, f);
            }
            fclose(f);
        }
    }
    free(px);
    g_last_shot_hash = hh;
}
/* debug: dump the recently used RTs to screenshots/ with some stats */
void dump_rts(void) {
    Tex* list[16];
    int n = 0;
    for (int k = 0; k < TEXH; k++)
        for (Tex* t = texh[k]; t; t = t->next)
            if (t->is_rt && !t->f.depth && !t->f.is_int) {
                if (n < 16) {
                    list[n++] = t;
                } else {
                    int mi = 0;
                    for (int i = 1; i < 16; i++)
                        if (list[i]->last_used < list[mi]->last_used) mi = i;
                    if (t->last_used > list[mi]->last_used) list[mi] = t;
                }
            }
    static int ndump;
    ndump++;
    LOG("---- render targets (dump %d, frame %u) ----\n", ndump, g_frame);
    plat_mkdir("captures");
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
    for (int i = 0; i < n; i++) {
        Tex* t = list[i];
        u32 w = SC(t->w), h = SC(t->h);
        if (!w || !h || w > 8192 || h > 8192) continue;
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->id, 0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOG("  rt %llx %ux%u fmt=%x: cannot be read\n", (unsigned long long)t->gva, t->w, t->h, t->fkey);
            continue;
        }
        u8* px = malloc((size_t)w * h * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        u64 sr = 0, sg = 0, sb = 0, sa = 0, nz = 0;
        size_t np = (size_t)w * h;
        for (size_t k = 0; k < np; k++) {
            u8* q = px + k * 4;
            sr += q[0];
            sg += q[1];
            sb += q[2];
            sa += q[3];
            if (q[0] + q[1] + q[2] > 12) nz++;
        }
        LOG("  rt %llx %ux%u fmt=%x used=%u  average color=(%llu,%llu,%llu,a%llu)  non-black=%llu%%\n",
            (unsigned long long)t->gva, t->w, t->h, t->fkey, t->last_used, (unsigned long long)(sr / np),
            (unsigned long long)(sg / np), (unsigned long long)(sb / np), (unsigned long long)(sa / np),
            (unsigned long long)(nz * 100 / np));
        /* downscaled, max 480 wide */
        u32 step = (w + 479) / 480;
        if (!step) step = 1;
        u32 ow = w / step, oh = h / step;
        char path[256];
        snprintf(path, sizeof path, "captures/rt_%d_%02d_%llx_%ux%u.ppm", ndump, i, (unsigned long long)t->gva, t->w,
                 t->h);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", ow, oh);
            for (u32 y = 0; y < oh; y++)
                for (u32 x = 0; x < ow; x++) {
                    u8* q = px + (((size_t)(h - 1 - y * step)) * w + x * step) * 4;
                    fwrite(q, 1, 3, f);
                }
            fclose(f);
        }
        free(px);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);
    LOG("--------------------------------------------\n");
    fflush(NULL);
}
void APIENTRY gl_dbg(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar* msg, const void* up) {
    (void)src;
    (void)id;
    (void)len;
    (void)up;
    if (sev == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type == GL_DEBUG_TYPE_ERROR) WARN("GL: %s\n", msg);
}
