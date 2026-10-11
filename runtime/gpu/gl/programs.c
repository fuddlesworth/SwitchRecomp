/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* shaders and programs: compiling (on worker threads), separable programs, the shader pack and warming it at start */
#include "gpu/gl/gl.h"

static Sh* shh[1024];
/* per-address cache. shader_hash walks the whole program (~50us per stage per draw on big shaders),
   within a submit a quick hash of the same range is enough */
typedef struct {
    u64 gva, gva_a;
    u32 key2, epoch;
    int stage;
    Sh* s;
} ShA;
static ShA sha[4096];
u64 st_sh_fast;
int g_sh_bind;
u32* g_last_r3d;
u32 g_fs_rtk, g_fs_rtm, g_fs_rts; /* FS integer output key */
static void aw_write(const char* path, const void* d, size_t n, int append);
/* common epilogue stuff: alpha test for FS, Y flip for VS */
static char* sh_post(char* src, int stage) {
    /* alpha test: discard before every return */
    if (stage == ST_FS && src && strstr(src, "out vec4 frag_color0")) {
        char* p = strstr(src, "void main(){");
        if (p) {
            size_t pre = p - src;
            const char* fn =
                "uniform vec4 swr_at;\nbool swr_atest(float a){ int f=int(swr_at.y); float r=swr_at.z;\n"
                "  if(f==0) return false; if(f==1) return a<r; if(f==2) return a==r; if(f==3) return a<=r; if(f==4) return a>r; if(f==5) return a!=r; if(f==6) return a>=r; return true; }\n";
            char* out = malloc(strlen(src) * 2 + strlen(fn) + 64);
            memcpy(out, src, pre);
            out[pre] = 0;
            strcat(out, fn);
            char* o = out + strlen(out);
            const char* q2 = p;
            while (*q2) {
                if (!strncmp(q2, "return;", 7)) {
                    o += sprintf(o, "{ if(swr_at.x!=0.0 && !swr_atest(frag_color0.w)) discard; return; }");
                    q2 += 7;
                } else
                    *o++ = *q2++;
            }
            *o = 0;
            free(src);
            src = out;
        }
    }
    /* Y flip */
    if (stage == ST_VS && src) {
        char* p = strstr(src, "void main(){");
        if (p) {
            size_t pre = p - src;
            char* ns = malloc(strlen(src) + 512);
            memcpy(ns, src, pre);
            ns[pre] = 0;
            strcat(ns, "uniform vec4 swr_vp;\n");
            strcat(ns, p);
            free(src);
            src = ns;
            /* patch every return */
            char* out = malloc(strlen(src) * 3 + 64);
            char* o = out;
            const char* q2 = src;
            while (*q2) {
                if (!strncmp(q2, "return;", 7)) {
                    o += sprintf(
                        o,
                        "{ gl_Position.y *= swr_vp.x; if(swr_vp.y!=0.0) gl_Position.z = gl_Position.z*2.0 - gl_Position.w; return; }");
                    q2 += 7;
                } else
                    *o++ = *q2++;
            }
            *o = 0;
            free(src);
            src = out;
        }
    }
    return src;
}
/* code hash at gva over len bytes (fast revalidation) */
static u64 sh_mem_hash(u64 gva, u32 len) {
    u8* p = gmem_ptr(gva, len);
    if (p) return fast_hash(p, len, 0);
    u8* t = malloc(len ? len : 1);
    gmem_read(gva, t, len);
    u64 q = fast_hash(t, len, 0);
    free(t);
    return q;
}
/* gva_a: VertexA program that runs before this VertexB (0 = none) */
Sh* shader_get(u64 gva_code, u64 gva_a, int stage, u32 attr_int, u32 attr_sint) {
    st_shaders++;
    g_sh_bind = stage == ST_VS ? 0 : 4;
    g_shader_cb_read = sh_cb_read; /* BRX table is only used to mark targets, length doesn't change */
    u32 key2 =
        stage == ST_VS ? (attr_int ^ (attr_sint * 31)) : (attr_int * 0x9E3779B1u ^ attr_sint * 0x85EBCA6Bu ^ g_fs_rtk);
    ShA* ca =
        &sha[(u32)((gva_code >> 6) ^ (gva_code >> 18) ^ (gva_a >> 4) ^ ((u64)key2 * 0x9E3779B1u) ^ ((u32)stage << 11)) &
             4095];
    if (ca->s && ca->gva == gva_code && ca->gva_a == gva_a && ca->stage == stage && ca->key2 == key2) {
        if (ca->epoch == g_submit_epoch) {
            st_sh_fast++;
            return ca->s;
        }
        Sh* s = ca->s;
        if (sh_mem_hash(gva_code, s->clen) == s->qh && (!gva_a || sh_mem_hash(gva_a, s->clen_a) == s->qh_a)) {
            ca->epoch = g_submit_epoch;
            st_sh_fast++;
            return s;
        }
    }
    u8* code = gmem_ptr(gva_code, 0x10000);
    u8* tmp = NULL;
    u32 len, len_a = 0;
    u64 qh, qh_a = 0;
    if (!code) {
        tmp = malloc(0x10000);
        gmem_read(gva_code, tmp, 0x10000);
        code = tmp;
    }
    shader_hash(code, 0x10000, &len);
    qh = fast_hash(code, len, 0);
    if (gva_a) { /* VertexA + VertexB: translate the merged program */
        u8* ca_ = malloc(0x10000);
        gmem_read(gva_a, ca_, 0x10000);
        shader_hash(ca_, 0x10000, &len_a);
        qh_a = fast_hash(ca_, len_a, 0);
        u8* m = malloc(0x10000);
        if (shader_merge_dual(ca_, code, m, 0x10000)) {
            free(tmp);
            tmp = m;
            code = m;
        } else
            free(m);
        free(ca_);
    }
    u32 mlen;
    u64 h = shader_hash(code, 0x10000, &mlen);
    for (Sh* s = shh[h & 1023]; s; s = s->next)
        if (s->hash == h && s->stage == stage && s->key2 == key2) {
            ca->gva = gva_code;
            ca->gva_a = gva_a;
            ca->key2 = key2;
            ca->stage = stage;
            ca->epoch = g_submit_epoch;
            ca->s = s;
            free(tmp);
            return s;
        }
    Sh* s = calloc(1, sizeof *s);
    s->hash = h;
    s->stage = stage;
    s->key2 = key2;
    st_sh_new++;
    s->clen = len;
    s->qh = qh;
    s->clen_a = len_a;
    s->qh_a = qh_a;
    ca->gva = gva_code;
    ca->gva_a = gva_a;
    ca->key2 = key2;
    ca->stage = stage;
    ca->epoch = g_submit_epoch;
    ca->s = s;
    if (stage == ST_FS) {
        s->si.tex_int_mask = attr_int;
        s->si.tex_sint_mask = attr_sint;
        s->si.rt_int_mask = g_fs_rtm;
        s->si.rt_sint_mask = g_fs_rts;
    } else {
        s->si.attr_int_mask = attr_int;
        s->si.attr_sint_mask = attr_sint;
    }
    {
        static const char* corpus = (const char*)-1;
        if (corpus == (const char*)-1) corpus = getenv("SWR_SHADER_CORPUS");
        if (corpus) shader_corpus_save(corpus, code, mlen, 0x10000, stage, &s->si, h ^ key2);
    }
    char* src = shader_to_glsl(code, 0x10000, stage, &s->si);
    /* dump shaders with indirect branches (code + cbufs) for offline analysis */
    {
        if (g_opt_diag && src && strstr(src, "uint t_ = uint(")) {
            static int nb;
            if (nb < 24) {
                nb++;
                plat_mkdir("captures");
                plat_mkdir("captures/failed");
                char fp[200];
                snprintf(fp, sizeof fp, "captures/failed/%016llx_%s.bin", (unsigned long long)h,
                         stage == ST_VS ? "vs" : "fs");
                FILE* f = fopen(fp, "wb");
                if (f) {
                    fwrite(code, 1, 0x10000, f);
                    fclose(f);
                }
                sh_dump_cbs(h);
                u32* rr = g_last_r3d;
                if (rr) {
                    snprintf(fp, sizeof fp, "captures/failed/%016llx_stages.txt", (unsigned long long)h);
                    f = fopen(fp, "w");
                    if (f) {
                        for (int k = 0; k < 6; k++)
                            fprintf(f, "slot %d: cfg=%08x offset=%08x\n", k, rr[0x800 + k * 0x10],
                                    rr[0x801 + k * 0x10]);
                        fclose(f);
                    }
                }
            }
        }
    }
    if (stage != ST_FS) s->si.attr_sint_mask = attr_sint;
    if (s->si.unsupported)
        WARN("shader %016llx: %d untranslated instructions (first: %s)\n", (unsigned long long)h, s->si.unsupported,
             s->si.first_unsupported);
    if (s->si.unsupported && strstr(s->si.first_unsupported, "untracked")) {
        static int nd;
        if (nd < 8) {
            nd++;
            char fp[200];
            plat_mkdir("captures");
            plat_mkdir("captures/failed");
            snprintf(fp, sizeof fp, "captures/failed/%016llx_%s.bin", (unsigned long long)h,
                     stage == ST_VS ? "vs" : "fs");
            aw_write(fp, code, 0x10000, 0);
            LOG("shader %016llx saved to %s (send it so the missing texture can be analyzed)\n", (unsigned long long)h,
                fp);
        }
    }
    src = sh_post(src, stage);
    GLenum ty = stage == ST_VS    ? GL_VERTEX_SHADER
                : stage == ST_FS  ? GL_FRAGMENT_SHADER
                : stage == ST_GS  ? GL_GEOMETRY_SHADER
                : stage == ST_CS  ? GL_COMPUTE_SHADER
                : stage == ST_TCS ? GL_TESS_CONTROL_SHADER
                                  : GL_TESS_EVALUATION_SHADER;
    (void)ty;
    if (!src) s->failed = 1;
    s->src = src;
    src = NULL; /* only compiled if it isn't in the disk cache */
    {
        static int dump = -1;
        if (dump < 0) { dump = getenv("SWR_DUMP_SHADERS") != NULL; }
        if (dump && s->src) {
            char pth[256];
            snprintf(pth, sizeof pth, "shader_%016llx_%d.glsl", (unsigned long long)h, stage);
            FILE* f = fopen(pth, "w");
            if (f) {
                fputs(s->src, f);
                fclose(f);
            }
        }
    }
    free(src);
    free(tmp);
    s->next = shh[h & 1023];
    shh[h & 1023] = s;
    return s;
}
/* deferred compile + on-disk program binaries */
static int sh_ensure(Sh* s) {
    if (s->gl) return 1;
    if (s->failed || !s->src) return 0;
    GLenum ty = s->stage == ST_VS ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    char err[4096];
    err[0] = 0;
    s->gl = compile(ty, s->src, err, sizeof err);
    if (!s->gl) {
        s->failed = 1;
        WARN("shader %016llx (stage %d) failed to compile:\n%s\n", (unsigned long long)s->hash, s->stage, err);
        if (g_verbose >= 2) WARN("---- GLSL ----\n%s\n", s->src);
        return 0;
    }
    return 1;
}
static int g_pcache = -1;
static char g_pc_tag[64];
#define TR_VERSION 0x5173u /* bump when the GLSL output changes, invalidates caches */
/* binary cache key is the GLSL text, so a new exe that emits the same source reuses old binaries */
static u64 src_key(Sh* s) {
    if (!s->skey) {
        u64 h = s->src ? fast_hash((const u8*)s->src, strlen(s->src), 0) : s->hash;
        s->skey = (h ^ ((u64)(s->stage + 1) << 58)) | 1;
    }
    return s->skey;
}
static u64 prog_key(Sh* vs, Sh* fs) {
    return src_key(vs) * 0x9E3779B97F4A7C15ull ^ src_key(fs) * 0xC2B2AE3D27D4EB4Full ^ 0x5E9A0001ull;
}
static u64 sh_key(Sh* s) { return src_key(s) * 0x9E3779B97F4A7C15ull ^ 0x5E9A0000ull; }
int g_par = -1;
static void pcache_init(void) {
    if (g_pcache < 0) {
        g_pcache = p_glProgramBinary && p_glGetProgramBinary && p_glProgramParameteri;
        if (g_pcache) {
            plat_mkdir("shader_cache");
            const char* rn = (const char*)glGetString(GL_RENDERER);
            const char* vv = (const char*)glGetString(GL_VERSION);
            u64 h = 1469598103934665603ull;
            for (const char* q = rn; q && *q; q++) {
                h ^= (u8)*q;
                h *= 1099511628211ull;
            }
            for (const char* q = vv; q && *q; q++) {
                h ^= (u8)*q;
                h *= 1099511628211ull;
            }
            snprintf(g_pc_tag, sizeof g_pc_tag, "%08x", (u32)(h ^ (h >> 32)));
        }
    }
}
static void pack_add(u64 key, Sh* vs, Sh* fs);
static void pack_add_sh(u64 key, Sh* s);
static int sh_start(Sh* s) {
    if (s->gl) return 1;
    if (s->failed || !s->src) return 0;
    s->gl = glCreateShader(s->stage == ST_VS ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
    const char* src = s->src;
    glShaderSource(s->gl, 1, &src, NULL);
    glCompileShader(s->gl);
    return 1;
}
static GLuint prog_finish(Sh* vs, Sh* fs, GLuint p);
static u64 prog_key(Sh* vs, Sh* fs);
static int prog_cached(Sh* vs, Sh* fs);
static GLuint prog_build(Sh* vs, Sh* fs, int* pending) {
    if (!vs || !fs || vs->failed || fs->failed) return 0;
    pcache_init();
    char path[160];
    u64 key = prog_key(vs, fs);
    if (g_pcache) {
        snprintf(path, sizeof path, "shader_cache/%s_%016llx.bin", g_pc_tag, (unsigned long long)key);
        FILE* f = fopen(path, "rb");
        if (f) {
            u32 fmt = 0, len = 0;
            if (fread(&fmt, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && len && len < (64u << 20)) {
                void* b = malloc(len);
                if (fread(b, 1, len, f) == len) {
                    GLuint p = glCreateProgram();
                    p_glProgramBinary(p, fmt, b, (GLsizei)len);
                    GLint ok = 0;
                    glGetProgramiv(p, GL_LINK_STATUS, &ok);
                    if (ok) {
                        free(b);
                        fclose(f);
                        pack_add(key, vs, fs);
                        return p;
                    }
                    glDeleteProgram(p);
                }
                free(b);
            }
            fclose(f);
        }
    }
    if (g_par > 0 && pending) { /* parallel compile: kick it off, poll on later draws */
        if (!sh_start(vs) || !sh_start(fs)) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, vs->gl);
        glAttachShader(p, fs->gl);
        if (g_pcache) p_glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(p);
        *pending = 1;
        return p;
    }
    if (!sh_ensure(vs) || !sh_ensure(fs)) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs->gl);
    glAttachShader(p, fs->gl);
    if (g_pcache) p_glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(p);
    return prog_finish(vs, fs, p);
}
static GLuint prog_finish(Sh* vs, Sh* fs, GLuint p) {
    char path[160];
    u64 key = prog_key(vs, fs);
    if (g_pcache) snprintf(path, sizeof path, "shader_cache/%s_%016llx.bin", g_pc_tag, (unsigned long long)key);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        Sh* ss[2] = {vs, fs};
        for (int k = 0; k < 2; k++) {
            GLint cok = 1;
            glGetShaderiv(ss[k]->gl, GL_COMPILE_STATUS, &cok);
            if (!cok) {
                char e[4096];
                glGetShaderInfoLog(ss[k]->gl, sizeof e, NULL, e);
                WARN("shader %016llx (stage %d) failed to compile:\n%s\n", (unsigned long long)ss[k]->hash,
                     ss[k]->stage, e);
                ss[k]->failed = 1;
            }
        }
    }
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, 2048, NULL, log);
        WARN("GL link: %s\n", log);
        glDeleteProgram(p);
        static int nd;
        if (nd < 12 && vs->src && fs->src) {
            nd++;
            plat_mkdir("captures");
            plat_mkdir("captures/failed");
            char fp[200];
            snprintf(fp, sizeof fp, "captures/failed/%016llx_vs.glsl", (unsigned long long)vs->hash);
            FILE* f = fopen(fp, "w");
            if (f) {
                fputs(vs->src, f);
                fclose(f);
            }
            snprintf(fp, sizeof fp, "captures/failed/%016llx_fs.glsl", (unsigned long long)fs->hash);
            f = fopen(fp, "w");
            if (f) {
                fputs(fs->src, f);
                fclose(f);
            }
        }
        return 0;
    }
    pack_add(key, vs, fs);
    if (g_pcache) {
        GLint len = 0;
        glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
        if (len > 0) {
            void* b = malloc(len);
            GLenum fmt = 0;
            GLsizei got = 0;
            p_glGetProgramBinary(p, len, &got, &fmt, b);
            if (got > 0) {
                FILE* f = fopen(path, "wb");
                if (f) {
                    u32 fm = fmt, ln = (u32)got;
                    fwrite(&fm, 4, 1, f);
                    fwrite(&ln, 4, 1, f);
                    fwrite(b, 1, got, f);
                    fclose(f);
                }
            }
            free(b);
        }
    }
    return p;
}
/* ============================================================ shader compile thread
   second shared context that compiles + links, so the GPU thread never blocks on the driver compiler.
   draws whose program isn't ready yet get skipped for a few frames */
static int prog_cached(Sh* vs, Sh* fs) {
    pcache_init();
    if (g_pcache <= 0) return 0;
    char path[160];
    snprintf(path, sizeof path, "shader_cache/%s_%016llx.bin", g_pc_tag, (unsigned long long)prog_key(vs, fs));
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
WRing wq_hi, wq_lo;
static volatile int g_worker;
volatile u64 g_cmp_ns, g_cmp_n, g_cmp_max, g_bin_n, g_bin_ns; /* 0 not started, 1 running, -1 unavailable */
static int wr_push(WRing* r, WJob* j) {
    u32 t = __atomic_load_n(&r->t, __ATOMIC_RELAXED), h = __atomic_load_n(&r->h, __ATOMIC_ACQUIRE);
    if (t - h >= WQN) return 0;
    r->e[t % WQN] = j;
    __atomic_store_n(&r->t, t + 1, __ATOMIC_RELEASE);
    return 1;
}
static WJob* wr_pop(WRing* r) {
    u32 h = __atomic_load_n(&r->h, __ATOMIC_RELAXED), t = __atomic_load_n(&r->t, __ATOMIC_ACQUIRE);
    if (h == t) return NULL;
    WJob* j = r->e[h % WQN];
    __atomic_store_n(&r->h, h + 1, __ATOMIC_RELEASE);
    return j;
}
static void shader_worker(void* arg) {
    (void)arg;
    if (!backend_gl_worker_current()) {
        __atomic_store_n(&g_worker, -1, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&g_worker, 1, __ATOMIC_RELEASE);
    {
        if (g_opt_cmp_low) plat_thread_low_priority();
    } /* only use the CPU the game isn't using */
    for (;;) {
        WJob* j = wr_pop(&wq_hi);
        int lo = 0;
        if (!j) {
            j = wr_pop(&wq_lo);
            lo = 1;
        }
        if (!j) {
            plat_sleep_ns(1000000);
            continue;
        }
        u64 tc0 = plat_ns();
        if (j->try_bin) { /* disk binary, loaded here instead of on the GPU thread */
            FILE* f = fopen(j->bin_path, "rb");
            GLuint p = 0;
            GLint ok = 0;
            if (f) {
                u32 fmt = 0, len = 0;
                if (fread(&fmt, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && len && len < (64u << 20)) {
                    void* b = malloc(len);
                    if (fread(b, 1, len, f) == len) {
                        p = glCreateProgram();
                        p_glProgramParameteri(p, 0x8258, GL_TRUE);
                        p_glProgramBinary(p, fmt, b, (GLsizei)len);
                        glGetProgramiv(p, GL_LINK_STATUS, &ok);
                        if (!ok) {
                            glDeleteProgram(p);
                            p = 0;
                        }
                    }
                    free(b);
                }
                fclose(f);
            }
            if (ok) {
                glFinish();
                u64 dt = plat_ns() - tc0;
                __atomic_add_fetch(&g_bin_ns, dt, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_bin_n, 1, __ATOMIC_RELAXED);
                j->prog = p;
                __atomic_store_n(&j->state, 1, __ATOMIC_RELEASE);
                continue;
            }
            while (glGetError() != GL_NO_ERROR) {}
        }
        GLuint a = glCreateShader(j->sep ? j->stype : GL_VERTEX_SHADER),
               b = j->sep ? 0 : glCreateShader(GL_FRAGMENT_SHADER);
        const char* s1 = j->vs;
        const char* s2 = j->fs;
        glShaderSource(a, 1, &s1, NULL);
        glCompileShader(a);
        if (b) {
            glShaderSource(b, 1, &s2, NULL);
            glCompileShader(b);
        }
        GLuint p = glCreateProgram();
        glAttachShader(p, a);
        if (b) glAttachShader(p, b);
        if (j->sep) p_glProgramParameteri(p, 0x8258 /* GL_PROGRAM_SEPARABLE */, GL_TRUE);
        if (g_pcache > 0) p_glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            GLint c = 1;
            char e[1024];
            e[0] = 0;
            glGetShaderiv(a, GL_COMPILE_STATUS, &c);
            if (!c) {
                glGetShaderInfoLog(a, sizeof e, NULL, e);
                snprintf(j->log, sizeof j->log, "%s: %s", j->sep ? (j->stype == GL_VERTEX_SHADER ? "VS" : "FS") : "VS",
                         e);
            } else if (b && (glGetShaderiv(b, GL_COMPILE_STATUS, &c), !c)) {
                glGetShaderInfoLog(b, sizeof e, NULL, e);
                snprintf(j->log, sizeof j->log, "FS: %s", e);
            } else
                glGetProgramInfoLog(p, sizeof j->log, NULL, j->log);
            glDeleteProgram(p);
            p = 0;
        } else if (g_pcache > 0) {
            GLint len = 0;
            glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
            if (len > 0) {
                void* bb = malloc(len);
                GLenum fmt = 0;
                GLsizei got = 0;
                p_glGetProgramBinary(p, len, &got, &fmt, bb);
                char path[160];
                snprintf(path, sizeof path, "shader_cache/%s_%s%016llx.bin", g_pc_tag, j->sep ? "s" : "",
                         (unsigned long long)j->key);
                if (got > 0) {
                    FILE* f = fopen(path, "wb");
                    if (f) {
                        u32 fm = fmt, ln = (u32)got;
                        fwrite(&fm, 4, 1, f);
                        fwrite(&ln, 4, 1, f);
                        fwrite(bb, 1, got, f);
                        fclose(f);
                    }
                }
                free(bb);
            }
        }
        glDeleteShader(a);
        if (b) glDeleteShader(b); /* flagged for deletion, freed together with the program */
        if (p && !j->keep) {
            glDeleteProgram(p);
            p = 0;
        }
        glFinish();
        {
            u64 dt = plat_ns() - tc0;
            __atomic_add_fetch(&g_cmp_ns, dt, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_cmp_n, 1, __ATOMIC_RELAXED);
            if (dt > g_cmp_max) g_cmp_max = dt;
        }
        j->prog = p;
        __atomic_store_n(&j->state, ok ? 1 : 2, __ATOMIC_RELEASE);
        if (lo) plat_sleep_ns(2000000); /* background work yields to the game */
    }
}
void worker_start(void) {
    if (getenv("SWR_SYNC_SHADERS")) {
        g_worker = -1;
        return;
    }
    if (plat_thread(shader_worker, NULL, 8u << 20)) {
        g_worker = -1;
        return;
    }
    u64 t0 = plat_ns();
    while (!__atomic_load_n(&g_worker, __ATOMIC_ACQUIRE) && plat_ns() - t0 < 2000000000ull) plat_sleep_ns(1000000);
    if (g_worker != 1) g_worker = -1;
    LOG("shaders: compiler thread %s\n", g_worker == 1
                                             ? "active (shaders compile in the background without freezing the game)"
                                             : "not available (shaders compile on the GPU thread)");
}
/* ============================================================ separable programs
   one program per shader stage, compiled once; draws just bind the stages instead of linking every
   vs+fs combination */
int g_sep = -1;
GLuint g_pipe;
int g_pipe_on;
GLuint g_pipe_vs, g_pipe_fs;
typedef void(APIENTRY* PFN_GPP_)(GLsizei, GLuint*);
PFN_UPS_ p_ups;
PFN_BPP_ p_bpp;
static void sh_uloc(Sh* s) {
    s->uloc = s->sp ? glGetUniformLocation(s->sp, s->stage == ST_VS ? "swr_vp" : "swr_at") : -1;
}
static int pk_has(u64 k);
int sh_sep_ready(Sh* s) {
    if (s->sps >= 2) return 1;
    if (s->sps == 1) {
        int st = __atomic_load_n(&s->sj->state, __ATOMIC_ACQUIRE);
        if (!st) return 0;
        struct WJob* j = s->sj;
        s->sj = NULL;
        if (st == 1) {
            s->sp = j->prog;
            s->sps = 2;
            pack_add_sh(j->key, s);
        } else {
            s->sp = 0;
            s->sps = 3;
            WARN("shader %016llx (stage %d) failed to compile: %s\n", (unsigned long long)s->hash, s->stage, j->log);
            static int nd;
            if (nd < 16 && s->src) {
                nd++;
                plat_mkdir("captures");
                plat_mkdir("captures/failed");
                char fp[200];
                snprintf(fp, sizeof fp, "captures/failed/%016llx_%s.glsl", (unsigned long long)s->hash,
                         s->stage == ST_VS ? "vs" : "fs");
                FILE* f = fopen(fp, "w");
                if (f) {
                    fputs(s->src, f);
                    fclose(f);
                }
            }
        }
        free(j);
        sh_uloc(s);
        return 1;
    }
    s->sps = 3;
    if (s->failed || !s->src) return 1;
    u64 key = sh_key(s);
    pcache_init();
    /* a program the shader pack has seen before has a binary on disk. Outside Windows it loads here, in well under a
       millisecond, so its draws don't skip frames waiting for the compile thread (on Windows a file open can take tens
       of ms with antivirus scanning, so it stays on that thread) */
    int bin_here = 0;
#ifndef _WIN32
    bin_here = g_pcache > 0 && pk_has(key);
#endif
    if (g_worker == 1 && !bin_here) { /* disk binary or compile, both on the compile thread */
        struct WJob* j = calloc(1, sizeof *j);
        j->vs = s->src;
        j->key = key;
        j->keep = 1;
        j->sep = 1;
        j->stype = s->stage == ST_VS ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
        if (g_pcache > 0) {
            j->try_bin = 1;
            snprintf(j->bin_path, sizeof j->bin_path, "shader_cache/%s_s%016llx.bin", g_pc_tag,
                     (unsigned long long)key);
        }
        if (wr_push(&wq_hi, j)) {
            s->sj = j;
            s->sps = 1;
            return 0;
        }
        free(j);
    }
    if (g_pcache > 0) {
        char path[160];
        snprintf(path, sizeof path, "shader_cache/%s_s%016llx.bin", g_pc_tag, (unsigned long long)key);
        FILE* f = fopen(path, "rb");
        if (f) {
            u32 fmt = 0, len = 0;
            if (fread(&fmt, 4, 1, f) == 1 && fread(&len, 4, 1, f) == 1 && len && len < (64u << 20)) {
                void* b = malloc(len);
                if (fread(b, 1, len, f) == len) {
                    GLuint p = glCreateProgram();
                    p_glProgramParameteri(p, 0x8258, GL_TRUE);
                    p_glProgramBinary(p, fmt, b, (GLsizei)len);
                    GLint ok = 0;
                    glGetProgramiv(p, GL_LINK_STATUS, &ok);
                    if (ok) {
                        free(b);
                        fclose(f);
                        s->sp = p;
                        s->sps = 2;
                        sh_uloc(s);
                        pack_add_sh(key, s);
                        return 1;
                    }
                    glDeleteProgram(p);
                }
                free(b);
            }
            fclose(f);
        }
    }
    struct WJob* j = calloc(1, sizeof *j);
    j->vs = s->src;
    j->key = key;
    j->keep = 1;
    j->sep = 1;
    j->stype = s->stage == ST_VS ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
    if (g_worker == 1 && wr_push(&wq_hi, j)) {
        s->sj = j;
        s->sps = 1;
        return 0;
    }
    /* no compile thread, do it inline */
    GLuint a = glCreateShader(j->stype);
    const char* s1 = s->src;
    glShaderSource(a, 1, &s1, NULL);
    glCompileShader(a);
    GLuint p = glCreateProgram();
    glAttachShader(p, a);
    p_glProgramParameteri(p, 0x8258, GL_TRUE);
    if (g_pcache > 0) p_glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(p);
    glDeleteShader(a);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, sizeof j->log, NULL, j->log);
        WARN("shader %016llx failed to compile: %s\n", (unsigned long long)s->hash, j->log);
        glDeleteProgram(p);
        p = 0;
    } else if (g_pcache > 0) {
        GLint len = 0;
        glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
        if (len > 0) {
            void* bb = malloc(len);
            GLenum fmt = 0;
            GLsizei got = 0;
            p_glGetProgramBinary(p, len, &got, &fmt, bb);
            char path[160];
            snprintf(path, sizeof path, "shader_cache/%s_s%016llx.bin", g_pc_tag, (unsigned long long)key);
            if (got > 0) {
                FILE* f = fopen(path, "wb");
                if (f) {
                    u32 fm = fmt, ln = (u32)got;
                    fwrite(&fm, 4, 1, f);
                    fwrite(&ln, 4, 1, f);
                    fwrite(bb, 1, got, f);
                    fclose(f);
                }
            }
            free(bb);
        }
    }
    free(j);
    s->sp = p;
    s->sps = p ? 2 : 3;
    sh_uloc(s);
    if (p) pack_add_sh(key, s);
    return 1;
}
void sep_init(void) {
    g_sep = 0;
    if (g_gles || getenv("SWR_NO_SEP") || !p_glProgramParameteri) goto out;
    p_ups = (PFN_UPS_)backend_gl_proc("glUseProgramStages");
    p_bpp = (PFN_BPP_)backend_gl_proc("glBindProgramPipeline");
    PFN_GPP_ gpp = (PFN_GPP_)backend_gl_proc("glGenProgramPipelines");
    if (!p_ups || !p_bpp || !gpp) goto out;
    gpp(1, &g_pipe);
    if (g_pipe) g_sep = 1;
out:
    LOG("shaders: %s\n",
        g_sep ? "one program per shader (each shader is compiled only once)" : "one program per combination");
}
static Prog* progs[512];
Prog* program_get(Sh* vs, Sh* fs) {
    u32 k = (u32)((((size_t)vs) >> 4) ^ (((size_t)fs) >> 6)) & 511;
    for (Prog* p = progs[k]; p; p = p->next)
        if (p->vs == vs && p->fs == fs) return p;
    Prog* p = calloc(1, sizeof *p);
    p->vs = vs;
    p->fs = fs;
    if (g_worker == 1 && vs && fs && !vs->failed && !fs->failed && vs->src && fs->src && !prog_cached(vs, fs)) {
        WJob* j = calloc(1, sizeof *j);
        j->vs = vs->src;
        j->fs = fs->src;
        j->key = prog_key(vs, fs);
        j->keep = 1;
        if (wr_push(&wq_hi, j)) {
            p->job = j;
            p->next = progs[k];
            progs[k] = p;
            return p;
        }
        free(j);
    }
    p->p = prog_build(vs, fs, &p->pending);
    if (!p->pending) {
        p->vp_loc = p->p ? glGetUniformLocation(p->p, "swr_vp") : -1;
        p->at_loc = p->p ? glGetUniformLocation(p->p, "swr_at") : -1;
    }
    p->next = progs[k];
    progs[k] = p;
    return p;
}
/* 1 = ready, 0 = driver still compiling (skip the draw) */
int program_ready(Prog* p) {
    if (p->job) {
        int s = __atomic_load_n(&p->job->state, __ATOMIC_ACQUIRE);
        if (!s) return 0;
        WJob* j = p->job;
        p->job = NULL;
        if (s == 1) {
            p->p = j->prog;
            pack_add(j->key, p->vs, p->fs);
            p->vp_loc = glGetUniformLocation(p->p, "swr_vp");
            p->at_loc = glGetUniformLocation(p->p, "swr_at");
        } else {
            p->p = 0;
            WARN("GL link (thread): %s\n", j->log);
            static int nd;
            if (nd < 12) {
                nd++;
                plat_mkdir("captures");
                plat_mkdir("captures/failed");
                char fp[200];
                snprintf(fp, sizeof fp, "captures/failed/%016llx_vs.glsl", (unsigned long long)p->vs->hash);
                FILE* f = fopen(fp, "w");
                if (f) {
                    fputs(p->vs->src, f);
                    fclose(f);
                }
                snprintf(fp, sizeof fp, "captures/failed/%016llx_fs.glsl", (unsigned long long)p->fs->hash);
                f = fopen(fp, "w");
                if (f) {
                    fputs(p->fs->src, f);
                    fclose(f);
                }
            }
        }
        free(j);
        return 1;
    }
    if (!p->pending) return 1;
    GLint done = 0;
    glGetProgramiv(p->p, 0x91B1 /* GL_COMPLETION_STATUS_KHR */, &done);
    if (!done) return 0;
    p->pending = 0;
    p->p = prog_finish(p->vs, p->fs, p->p);
    p->vp_loc = p->p ? glGetUniformLocation(p->p, "swr_vp") : -1;
    p->at_loc = p->p ? glGetUniformLocation(p->p, "swr_at") : -1;
    return 1;
}

/* ============================================================ shader pack
   every new program's GLSL (driver independent) gets appended to shader_cache/shader_pack.bin. at startup
   whatever isn't in the binary cache yet is compiled in the background. the pack can be moved to another PC */
#define PACK_PATH "shader_cache/shader_pack_v3.bin" /* versioned with the translator */
static u64* pk_keys;
static u32 pk_n, pk_cap;
static int pk_has(u64 k) {
    if (!pk_cap) return 0;
    u32 i = (u32)(k ^ (k >> 29)) & (pk_cap - 1);
    while (pk_keys[i]) {
        if (pk_keys[i] == k) return 1;
        i = (i + 1) & (pk_cap - 1);
    }
    return 0;
}
static void pk_put(u64 k) {
    if (!k) k = 1;
    if (pk_n * 2 >= pk_cap) {
        u32 oc = pk_cap;
        u64* ok = pk_keys;
        pk_cap = oc ? oc * 2 : 4096;
        pk_keys = calloc(pk_cap, 8);
        pk_n = 0;
        for (u32 i = 0; i < oc; i++)
            if (ok[i]) pk_put(ok[i]);
        free(ok);
    }
    u32 i = (u32)(k ^ (k >> 29)) & (pk_cap - 1);
    while (pk_keys[i]) {
        if (pk_keys[i] == k) return;
        i = (i + 1) & (pk_cap - 1);
    }
    pk_keys[i] = k;
    pk_n++;
}
/* disk writes happen on a background thread; with AV scanning, an fopen on Windows can take tens of ms */
typedef struct AwJob {
    struct AwJob* next;
    char path[200];
    u8* data;
    size_t n;
    int append;
} AwJob;
static AwJob* volatile g_aw_head;
static AwJob* g_aw_tail;
static PMutex g_aw_m;
static volatile int g_aw_on;
static void aw_thread(void* arg) {
    (void)arg;
    plat_thread_low_priority();
    for (;;) {
        pm_lock(&g_aw_m);
        AwJob* j = g_aw_head;
        if (j) {
            g_aw_head = j->next;
            if (!g_aw_head) g_aw_tail = NULL;
        }
        pm_unlock(&g_aw_m);
        if (!j) {
            plat_sleep_ns(20000000);
            continue;
        }
        FILE* f = fopen(j->path, j->append ? "ab" : "wb");
        if (f) {
            fwrite(j->data, 1, j->n, f);
            fclose(f);
        }
        free(j->data);
        free(j);
    }
}
static void aw_write(const char* path, const void* d, size_t n, int append) {
    if (!g_aw_on) {
        pm_init(&g_aw_m);
        g_aw_on = plat_thread(aw_thread, NULL, 256 * 1024) ? -1 : 1;
    }
    if (g_aw_on < 0) {
        FILE* f = fopen(path, append ? "ab" : "wb");
        if (f) {
            fwrite(d, 1, n, f);
            fclose(f);
        }
        return;
    }
    AwJob* j = calloc(1, sizeof *j);
    snprintf(j->path, sizeof j->path, "%s", path);
    j->data = malloc(n ? n : 1);
    memcpy(j->data, d, n);
    j->n = n;
    j->append = append;
    pm_lock(&g_aw_m);
    if (g_aw_tail)
        g_aw_tail->next = j;
    else
        g_aw_head = j;
    g_aw_tail = j;
    pm_unlock(&g_aw_m);
}
static void pack_add(u64 key, Sh* vs, Sh* fs) {
    if (!vs->src || !fs->src || pk_has(key)) return;
    pk_put(key);
    u32 a = (u32)strlen(vs->src), b = (u32)strlen(fs->src), mg = 0x4B435053;
    size_t n = 4 + 8 + 4 + a + 4 + b;
    u8 *d = malloc(n), *q = d;
    memcpy(q, &mg, 4);
    q += 4;
    memcpy(q, &key, 8);
    q += 8;
    memcpy(q, &a, 4);
    q += 4;
    memcpy(q, vs->src, a);
    q += a;
    memcpy(q, &b, 4);
    q += 4;
    memcpy(q, fs->src, b);
    aw_write(PACK_PATH, d, n, 1);
    free(d);
}
static void pack_add_sh(u64 key, Sh* s) {
    if (!s->src || pk_has(key)) return;
    pk_put(key);
    u32 a = (u32)strlen(s->src), mg = 0x33504853, st = (u32)s->stage;
    u8* d = malloc(20 + a);
    {
        u8* q = d;
        memcpy(q, &mg, 4);
        q += 4;
        memcpy(q, &key, 8);
        q += 8;
        memcpy(q, &st, 4);
        q += 4;
        memcpy(q, &a, 4);
        q += 4;
        memcpy(q, s->src, a);
    }
    aw_write(PACK_PATH, d, 20 + a, 1);
    free(d);
}
typedef struct {
    u64 key;
    char* vs;
    char* fs;
    int sep;
    u32 stage;
} PackEnt;
static PackEnt* wq;
static u32 wq_n, wq_i, wq_done;
typedef struct {
    GLuint p, a, b;
    u64 key;
} WarmJob;
static WarmJob wj[6];
static int wj_n;
void warm_load(void) {
    pcache_init();
    FILE* f = fopen(PACK_PATH, "rb");
    if (!f) return;
    u32 cap = 0;
    for (;;) {
        u32 mg, a, b;
        u64 key;
        if (fread(&mg, 4, 1, f) != 1 || (mg != 0x4B435053 && mg != 0x33504853) || fread(&key, 8, 1, f) != 1) break;
        if (mg == 0x33504853) {
            u32 st = 0;
            if (fread(&st, 4, 1, f) != 1 || fread(&a, 4, 1, f) != 1 || a > (8u << 20)) break;
            char* src = malloc(a + 1);
            if (fread(src, 1, a, f) != a) {
                free(src);
                break;
            }
            src[a] = 0;
            if (pk_has(key) || g_worker != 1) {
                free(src);
                continue;
            }
            pk_put(key);
            char path[160];
            snprintf(path, sizeof path, "shader_cache/%s_s%016llx.bin", g_pc_tag, (unsigned long long)key);
            FILE* t = g_pcache ? fopen(path, "rb") : NULL;
            if (t) {
                fclose(t);
                free(src);
                continue;
            }
            if (wq_n >= cap) {
                cap = cap ? cap * 2 : 256;
                wq = realloc(wq, cap * sizeof *wq);
            }
            wq[wq_n].key = key;
            wq[wq_n].vs = src;
            wq[wq_n].fs = NULL;
            wq[wq_n].sep = 1;
            wq[wq_n].stage = st;
            wq_n++;
            continue;
        }
        if (fread(&a, 4, 1, f) != 1 || a > (8u << 20)) break;
        char* vs = malloc(a + 1);
        if (fread(vs, 1, a, f) != a) {
            free(vs);
            break;
        }
        vs[a] = 0;
        if (fread(&b, 4, 1, f) != 1 || b > (8u << 20)) {
            free(vs);
            break;
        }
        char* fs = malloc(b + 1);
        if (fread(fs, 1, b, f) != b) {
            free(vs);
            free(fs);
            break;
        }
        fs[b] = 0;
        if (pk_has(key)) {
            free(vs);
            free(fs);
            continue;
        }
        pk_put(key);
        char path[160];
        snprintf(path, sizeof path, "shader_cache/%s_%016llx.bin", g_pc_tag, (unsigned long long)key);
        FILE* t = g_pcache ? fopen(path, "rb") : NULL;
        if (t) {
            fclose(t);
            free(vs);
            free(fs);
            continue;
        } /* already have a binary for this PC */
        if (wq_n >= cap) {
            cap = cap ? cap * 2 : 256;
            wq = realloc(wq, cap * sizeof *wq);
        }
        wq[wq_n].key = key;
        wq[wq_n].vs = vs;
        wq[wq_n].fs = fs;
        wq[wq_n].sep = 0;
        wq[wq_n].stage = 0;
        wq_n++;
    }
    fclose(f);
    if (wq_n) LOG("shaders: %u from the pack are precompiled in the background while you play\n", wq_n);
}
static WJob* wlo[16];
static int wlo_n;
void warm_step(void) {
    if (g_worker == 1) { /* compile thread available: feed it a few low priority jobs at a time */
        for (int j = 0; j < wlo_n;) {
            if (__atomic_load_n(&wlo[j]->state, __ATOMIC_ACQUIRE)) {
                free(wlo[j]->own_vs);
                free(wlo[j]->own_fs);
                free(wlo[j]);
                wlo[j] = wlo[--wlo_n];
                wq_done++;
                if (wq_done == wq_n) LOG("shaders: precompiled pack (%u programs)\n", wq_n);
            } else
                j++;
        }
        while (wlo_n < 8 && wq_i < wq_n) {
            PackEnt* e = &wq[wq_i];
            WJob* j = calloc(1, sizeof *j);
            j->own_vs = e->vs;
            j->own_fs = e->fs;
            j->vs = e->vs;
            j->fs = e->fs;
            j->key = e->key;
            j->keep = 0;
            j->sep = e->sep;
            j->stype = e->stage == ST_VS ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
            if (!wr_push(&wq_lo, j)) {
                free(j);
                break;
            }
            e->vs = e->fs = NULL;
            wq_i++;
            wlo[wlo_n++] = j;
        }
        return;
    }
    if (g_par <= 0 || !g_pcache) return;
    for (int j = 0; j < wj_n;) {
        GLint done = 0;
        glGetProgramiv(wj[j].p, 0x91B1, &done);
        if (!done) {
            j++;
            continue;
        }
        GLint ok = 0;
        glGetProgramiv(wj[j].p, GL_LINK_STATUS, &ok);
        if (ok) {
            GLint len = 0;
            glGetProgramiv(wj[j].p, GL_PROGRAM_BINARY_LENGTH, &len);
            if (len > 0) {
                void* bb = malloc(len);
                GLenum fmt = 0;
                GLsizei got = 0;
                p_glGetProgramBinary(wj[j].p, len, &got, &fmt, bb);
                char path[160];
                snprintf(path, sizeof path, "shader_cache/%s_%016llx.bin", g_pc_tag, (unsigned long long)wj[j].key);
                if (got > 0) {
                    FILE* f = fopen(path, "wb");
                    if (f) {
                        u32 fm = fmt, ln = (u32)got;
                        fwrite(&fm, 4, 1, f);
                        fwrite(&ln, 4, 1, f);
                        fwrite(bb, 1, got, f);
                        fclose(f);
                    }
                }
                free(bb);
            }
        }
        glDeleteProgram(wj[j].p);
        glDeleteShader(wj[j].a);
        glDeleteShader(wj[j].b);
        wj[j] = wj[--wj_n];
        wq_done++;
        if (wq_done == wq_n) LOG("shaders: precompiled pack (%u programs)\n", wq_n);
    }
    while (wj_n < 4 && wq_i < wq_n) {
        PackEnt* e = &wq[wq_i++];
        if (e->sep) {
            free(e->vs);
            e->vs = NULL;
            wq_done++;
            continue;
        }
        WarmJob* j = &wj[wj_n++];
        j->key = e->key;
        j->a = glCreateShader(GL_VERTEX_SHADER);
        const char* s1 = e->vs;
        glShaderSource(j->a, 1, &s1, NULL);
        glCompileShader(j->a);
        j->b = glCreateShader(GL_FRAGMENT_SHADER);
        const char* s2 = e->fs;
        glShaderSource(j->b, 1, &s2, NULL);
        glCompileShader(j->b);
        j->p = glCreateProgram();
        glAttachShader(j->p, j->a);
        glAttachShader(j->p, j->b);
        p_glProgramParameteri(j->p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glLinkProgram(j->p);
        free(e->vs);
        free(e->fs);
        e->vs = e->fs = NULL;
    }
}
FILE* g_rf;
u64 g_rf_t0;
u64 g_fr_t0, g_fr_max;
int g_fr_slow;
u64 g_fr_shnew;
/* ---- dynamic res: move every RT to the new scale with a GPU copy */
/* UBO with 1/scale for gl_FragCoord math (shaders do uv = pos * 1/native size) */
GLuint g_rs_ubo;
