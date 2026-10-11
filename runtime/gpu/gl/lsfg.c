/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* LSFG 3.1 frame generation in the GL renderer.

   The shaders belong to THS / Lossless Scaling and are not shipped. We pull the SPIR-V resources out of the
   user's own Lossless.dll at runtime, run them through SPIRV-Cross and dispatch them as GL compute.

   Pass order, sizes, formats, samplers and constants are taken from lsfg-vk v1.0
   (https://github.com/PancakeTAS/lsfg-vk, MIT license, Copyright (c) 2025 lsfg-vk), ported to GL. */
#include "gpu/gl/lsfg.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "abi/cpu.h"
#include "gpu/gl/glfn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ ini options */
static int g_lsfg_opt = 1;       /* lsfg=0 disables */
static char g_lsfg_dll[1024];    /* lsfg_dll=path */
static float g_lsfg_flow = 0.5f; /* lsfg_flow: flow scale, 1 quality / 0.5 fast */
static int g_lsfg_quality = 0;   /* perf mode only for now */
int g_lsfg_mult = 2;             /* lsfg_multiplier: 2 = x2, 3 = x3 */
int g_lsfg_height = 720;         /* lsfg_resolution: internal height */
float g_lsfg_target = 60.f;      /* lsfg_target: only generate below this fps */

void lsfg_ini(const char* k, const char* v) {
    if (!strcmp(k, "lsfg"))
        g_lsfg_opt = atoi(v);
    else if (!strcmp(k, "lsfg_dll")) {
        strncpy(g_lsfg_dll, v, 1023);
        g_lsfg_dll[1023] = 0;
    } else if (!strcmp(k, "lsfg_flow")) {
        g_lsfg_flow = (float)atof(v);
        if (g_lsfg_flow < 0.25f) g_lsfg_flow = 0.25f;
        if (g_lsfg_flow > 1) g_lsfg_flow = 1;
    } else if (!strcmp(k, "lsfg_multiplier")) {
        g_lsfg_mult = atoi(v);
        if (g_lsfg_mult < 2) g_lsfg_mult = 2;
        if (g_lsfg_mult > 4) g_lsfg_mult = 4;
    } else if (!strcmp(k, "lsfg_resolution")) {
        g_lsfg_height = atoi(v);
        if (g_lsfg_height < 240) g_lsfg_height = 240;
        if (g_lsfg_height > 2160) g_lsfg_height = 2160;
    } else if (!strcmp(k, "lsfg_target")) {
        g_lsfg_target = (float)atof(v);
        if (g_lsfg_target < 30) g_lsfg_target = 30;
    }
}

/* ------------------------------------------------------------ PE resource reader */
typedef struct {
    u32 id;
    const u8* p;
    u32 n;
} Res;
static u8* g_dll;
static Res g_res[512];
static int g_nres;
static u32 rd32(const u8* p) {
    u32 v;
    memcpy(&v, p, 4);
    return v;
}
static u16 rd16(const u8* p) {
    u16 v;
    memcpy(&v, p, 2);
    return v;
}
static int pe_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 4096 || n > (64l << 20)) {
        fclose(f);
        return 0;
    }
    g_dll = malloc((size_t)n);
    if (fread(g_dll, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(g_dll);
        g_dll = NULL;
        return 0;
    }
    fclose(f);
    const u8* d = g_dll;
    if (d[0] != 'M' || d[1] != 'Z') return 0;
    u32 pe = rd32(d + 0x3C);
    if (pe + 0x108 > (u32)n || rd32(d + pe) != 0x4550) return 0;
    u16 nsec = rd16(d + pe + 6), optsz = rd16(d + pe + 20);
    u32 opt = pe + 24;
    if (rd16(d + opt) != 0x20B) return 0;
    u32 rsrc = rd32(d + opt + 112 + 2 * 8);
    u32 sect = opt + optsz;
#define RVA2OFF(r_, o_)                                                                                                \
    do {                                                                                                               \
        o_ = 0;                                                                                                        \
        for (u32 s_ = 0; s_ < nsec; s_++) {                                                                            \
            const u8* q_ = d + sect + s_ * 40;                                                                         \
            u32 va = rd32(q_ + 12), vs = rd32(q_ + 8), ra = rd32(q_ + 20), rs = rd32(q_ + 16);                         \
            u32 m_ = vs > rs ? vs : rs;                                                                                \
            if ((r_) >= va && (r_) < va + m_) {                                                                        \
                o_ = (r_) - va + ra;                                                                                   \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)
    u32 base;
    RVA2OFF(rsrc, base);
    if (!base || base >= (u32)n) return 0;
    /* resource dir: type (RT_RCDATA=10) -> id -> language -> data */
    const u8* root = d + base;
    int nt = rd16(root + 12) + rd16(root + 14);
    for (int i = 0; i < nt; i++) {
        u32 nm = rd32(root + 16 + i * 8), ptr = rd32(root + 20 + i * 8);
        if (nm != 10 || !(ptr & 0x80000000u)) continue;
        const u8* t = d + base + (ptr & 0x7FFFFFFF);
        int ni = rd16(t + 12) + rd16(t + 14);
        for (int j = 0; j < ni && g_nres < 512; j++) {
            u32 id = rd32(t + 16 + j * 8), p2 = rd32(t + 20 + j * 8);
            if (!(p2 & 0x80000000u) || (id & 0x80000000u)) continue;
            const u8* l = d + base + (p2 & 0x7FFFFFFF);
            if (rd16(l + 12) + rd16(l + 14) < 1) continue;
            u32 de = rd32(l + 20);
            if (de & 0x80000000u) continue;
            const u8* e = d + base + de;
            u32 rva = rd32(e), sz = rd32(e + 4), off;
            RVA2OFF(rva, off);
            if (!off || off + sz > (u32)n) continue;
            g_res[g_nres].id = id;
            g_res[g_nres].p = d + off;
            g_res[g_nres].n = sz;
            g_nres++;
        }
    }
    return g_nres > 0;
}

/* what we need from a SPIR-V module: bindings (samplers/textures/outputs), local size, f16 */
typedef struct {
    u32 id;
    const u32* w;
    u32 nw;
    int f16, ns, ni, no, lx, ly;
} SpvInfo;
static int spv_info(const Res* r, SpvInfo* o) {
    memset(o, 0, sizeof *o);
    if (r->n < 20 || rd32(r->p) != 0x07230203u) return 0;
    o->id = r->id;
    o->w = (const u32*)r->p;
    o->nw = r->n / 4;
    u32 i = 5;
    int maxs = -1, maxi = -1, maxo = -1;
    while (i < o->nw) {
        u32 h = o->w[i], op = h & 0xFFFF, len = h >> 16;
        if (!len || i + len > o->nw) return 0;
        if (op == 17 && len >= 2 && o->w[i + 1] == 9) o->f16 = 1; /* OpCapability Float16 */
        if (op == 16 && len >= 5 && o->w[i + 2] == 17) {
            o->lx = (int)o->w[i + 3];
            o->ly = (int)o->w[i + 4];
        } /* LocalSize */
        if (op == 71 && len >= 4 && o->w[i + 2] == 33) {
            u32 b = o->w[i + 3]; /* OpDecorate Binding */
            if (b >= 16 && b < 32 && (int)(b - 16) > maxs)
                maxs = (int)(b - 16);
            else if (b >= 32 && b < 48 && (int)(b - 32) > maxi)
                maxi = (int)(b - 32);
            else if (b >= 48 && (int)(b - 48) > maxo)
                maxo = (int)(b - 48);
        }
        if (op == 54) break; /* OpFunction */
        i += len;
    }
    o->ns = maxs + 1;
    o->ni = maxi + 1;
    o->no = maxo + 1;
    return 1;
}

/* ------------------------------------------------------------ programs */
enum {
    P_MIP,
    P_GEN,
    P_G0,
    P_D5,
    P_G1,
    P_G2,
    P_G3,
    P_G4,
    P_D1,
    P_D2,
    P_D3,
    P_D4,
    P_A0,
    P_A1,
    P_A2,
    P_A3,
    P_D6,
    P_D7,
    P_D8,
    P_D9,
    P_B0,
    P_B1,
    P_B2,
    P_B3,
    P_B4,
    P_N
};
/* binding signatures for the perf set and the quality set */
static const signed char SIG_P[23][3] = {{2, 5, 3}, {2, 6, 1}, {1, 3, 2}, {1, 2, 2}, {1, 2, 2}, {2, 4, 1},
                                         {1, 3, 2}, {1, 2, 2}, {1, 2, 2}, {2, 4, 1}, {1, 1, 1}, {1, 1, 1},
                                         {1, 1, 2}, {1, 2, 2}, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, {2, 2, 1},
                                         {1, 6, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 6}};
static const signed char SIG_Q[23][3] = {{2, 9, 3},  {2, 10, 2}, {1, 3, 4}, {1, 4, 4}, {1, 4, 4}, {2, 6, 1},
                                         {1, 3, 4},  {1, 4, 4},  {1, 4, 4}, {2, 6, 1}, {1, 1, 2}, {1, 2, 2},
                                         {1, 2, 4},  {1, 4, 4},  {1, 2, 2}, {1, 2, 2}, {1, 2, 2}, {2, 3, 1},
                                         {1, 12, 2}, {1, 2, 2},  {1, 2, 2}, {1, 2, 2}, {1, 2, 6}};
typedef struct {
    GLuint prog;
    int nu;
    LsfgUnit u[24];
} LProg;
static LProg g_p[P_N];
static int g_ok = -1;
static GLuint g_smp[3]; /* 0 black border, 1 white border, 2 clamp */
static char g_why[256];

static GLuint cs_compile(const char* src, char* err, int errn) {
    GLuint s = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(s, errn, NULL, err);
        glDeleteShader(s);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, s);
    glLinkProgram(p);
    glDeleteShader(s);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(p, errn, NULL, err);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static int find_dll(char* out) {
    if (g_lsfg_dll[0]) {
        FILE* f = fopen(g_lsfg_dll, "rb");
        if (f) {
            fclose(f);
            strcpy(out, g_lsfg_dll);
            return 1;
        }
    }
    const char* local[] = {"Lossless.dll", "lossless.dll"};
    for (int i = 0; i < 2; i++) {
        FILE* f = fopen(local[i], "rb");
        if (f) {
            fclose(f);
            strcpy(out, local[i]);
            return 1;
        }
    }
#ifdef _WIN32
    {
        typedef long(__stdcall * RGV)(void*, const char*, const char*, unsigned long, unsigned long*, void*,
                                      unsigned long*);
        static char libs[10][600];
        int nl = 0;
        HMODULE a = LoadLibraryA("advapi32.dll");
        RGV rgv = a ? (RGV)(void*)GetProcAddress(a, "RegGetValueA") : NULL;
        char sp[600];
        unsigned long sz = sizeof sp;
        if (rgv &&
            rgv((void*)(uintptr_t)0x80000001u, "Software\\Valve\\Steam", "SteamPath", 0x0000ffff, NULL, sp, &sz) == 0 &&
            sp[0]) {
            strncpy(libs[nl], sp, 599);
            libs[nl][599] = 0;
            nl++;
        }
        strcpy(libs[nl++], "C:\\Program Files (x86)\\Steam");
        strcpy(libs[nl++], "C:\\Program Files\\Steam");
        {
            char vdf[700];
            snprintf(vdf, sizeof vdf, "%s\\steamapps\\libraryfolders.vdf", libs[0]);
            FILE* f = fopen(vdf, "r");
            if (f) {
                char ln[800];
                while (fgets(ln, sizeof ln, f) && nl < 10) {
                    char* p = strstr(ln, "\"path\"");
                    if (!p) continue;
                    p = strchr(p + 6, '"');
                    if (!p) continue;
                    p++;
                    char* e = strchr(p, '"');
                    if (!e) continue;
                    *e = 0;
                    int k = 0;
                    for (char* q = p; *q && k < 599; q++) {
                        if (q[0] == '\\' && q[1] == '\\') q++;
                        libs[nl][k++] = *q;
                    }
                    libs[nl][k] = 0;
                    nl++;
                }
                fclose(f);
            }
        }
        for (int i = 0; i < nl; i++) {
            char c[800];
            snprintf(c, sizeof c, "%s\\steamapps\\common\\Lossless Scaling\\Lossless.dll", libs[i]);
            FILE* f = fopen(c, "rb");
            if (f) {
                fclose(f);
                strcpy(out, c);
                return 1;
            }
        }
    }
#endif
    return 0;
}
/* frame generation would run: it is on and the user's Lossless.dll is there (renderer.c picks OpenGL for it) */
int lsfg_available(void) {
    char p[1100];
    return g_lsfg_opt && find_dll(p);
}

/* load + identify + compile everything, returns 1 when usable */
int lsfg_init(void) {
    if (g_ok >= 0) return g_ok;
    g_ok = 0;
    if (!g_lsfg_opt) { return 0; }
    char path[1024];
    if (!find_dll(path)) {
        LOG("lsfg: no Lossless.dll (copy it from your Lossless Scaling into the game folder, or set lsfg_dll=path in the .ini)\n");
        return 0;
    }
    if (!pe_load(path)) {
        LOG("lsfg: %s could not be read as a Lossless Scaling DLL\n", path);
        return 0;
    }
    /* modules sorted by id. dll has an fp16 set and an fp32 set, we take fp32 */
    static SpvInfo sv[256];
    int n = 0;
    for (int i = 0; i < g_nres; i++) {
        SpvInfo s;
        if (spv_info(&g_res[i], &s) && n < 256) sv[n++] = s;
    }
    for (int i = 1; i < n; i++) {
        SpvInfo t = sv[i];
        int j = i - 1;
        while (j >= 0 && sv[j].id > t.id) {
            sv[j + 1] = sv[j];
            j--;
        }
        sv[j + 1] = t;
    }
    int m = -1; /* mips: 1 tex -> 7 outputs, 32x32 groups. then generate + the two 23-shader sets */
    for (int i = 0; i + 47 < n && m < 0; i++) {
        if (!(sv[i].ni == 1 && sv[i].no == 7 && sv[i].lx == 32)) continue;
        int f16 = 0;
        for (int k = 0; k < 48; k++) f16 |= sv[i + k].f16;
        if (!f16) m = i;
    }
    if (m < 0) {
        LOG("lsfg: %s does not contain the expected LSFG 3.1 shaders (%d SPIR-V modules)\n", path, n);
        return 0;
    }
    const SpvInfo* gen = &sv[m + 1];
    if (gen->ni != 5 || gen->no != 1 || gen->ns != 2) {
        LOG("lsfg: the generation shader does not match\n");
        return 0;
    }
    int base = g_lsfg_quality ? m + 2 : m + 25;
    const signed char (*sig)[3] = g_lsfg_quality ? SIG_Q : SIG_P;
    for (int k = 0; k < 23; k++) {
        const SpvInfo* s = &sv[base + k];
        if (s->ns != sig[k][0] || s->ni != sig[k][1] || s->no != sig[k][2]) {
            LOG("lsfg: shader %u (pass %d) does not match LSFG 3.1 (%d/%d/%d): different Lossless Scaling version\n",
                s->id, k, s->ns, s->ni, s->no);
            return 0;
        }
    }
    const SpvInfo* pick[P_N];
    pick[P_MIP] = &sv[m];
    pick[P_GEN] = gen;
    for (int k = 0; k < 23; k++) pick[P_G0 + k] = &sv[base + k];
    u64 t0 = plat_ns();
    for (int k = 0; k < P_N; k++) {
        char err[1024];
        err[0] = 0;
        LProg* p = &g_p[k];
        char* src = lsfg_spv_to_glsl(pick[k]->w, pick[k]->nw, p->u, &p->nu, 24, err, sizeof err);
        if (!src) {
            LOG("lsfg: could not translate shader %u: %s\n", pick[k]->id, err);
            return 0;
        }
        {
            static int dmp = -1;
            if (dmp < 0) dmp = getenv("SWR_LSFG_DUMP") != NULL;
            if (dmp) {
                char fn[64];
                snprintf(fn, sizeof fn, "lsfg_%u.comp", pick[k]->id);
                FILE* f = fopen(fn, "w");
                if (f) {
                    fputs(src, f);
                    fclose(f);
                }
            }
        }
        p->prog = cs_compile(src, err, sizeof err);
        free(src);
        if (!p->prog) {
            LOG("lsfg: shader %u failed to compile: %.300s\n", pick[k]->id, err);
            return 0;
        }
    }
    /* samplers: linear with black border / white border / clamp */
    glGenSamplers(3, g_smp);
    for (int i = 0; i < 3; i++) {
        GLenum wr = i == 2 ? GL_CLAMP_TO_EDGE : GL_CLAMP_TO_BORDER;
        glSamplerParameteri(g_smp[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(g_smp[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(g_smp[i], GL_TEXTURE_WRAP_S, wr);
        glSamplerParameteri(g_smp[i], GL_TEXTURE_WRAP_T, wr);
        float bc[4] = {0, 0, 0, 0};
        if (i == 1) { bc[0] = bc[1] = bc[2] = bc[3] = 1; }
        glSamplerParameterfv(g_smp[i], GL_TEXTURE_BORDER_COLOR, bc);
    }
    g_ok = 1;
    free(g_dll);
    g_dll = NULL;
    g_nres = 0;
    LOG("lsfg: frame generation ready (%s, shaders %u.., mode %s, flow %.2f, x%d) in %.0f ms\n", path, pick[P_G0]->id,
        g_lsfg_quality ? "quality" : "performance", g_lsfg_flow, g_lsfg_mult, (plat_ns() - t0) / 1e6);
    return 1;
}

/* ------------------------------------------------------------ per-size resources */
typedef struct {
    GLuint t;
    u32 w, h;
    GLenum f;
} Img;
typedef struct {
    Img t1, t2, t3[2], out[3][2];
} AlphaR;
typedef struct {
    Img t1[2], t2[2], out[6];
} BetaR;
typedef struct {
    Img t1[3], t2[2], out;
} GammaR;
typedef struct {
    Img t1[3], t2[2], out1, out2;
} DeltaR;
static struct {
    int ok;
    u32 W, H;
    Img in[2], mip[7], gen[4];
    AlphaR a[7];
    BetaR b;
    GammaR g[7];
    DeltaR d[3];
    u64 fc;
} C;
static GLuint g_ubo[16];
static float g_ubo_key[16][3];
static int g_nubo;

static Img mk(u32 w, u32 h, GLenum f) {
    Img i;
    i.w = w ? w : 1;
    i.h = h ? h : 1;
    i.f = f;
    glGenTextures(1, &i.t);
    glBindTexture(GL_TEXTURE_2D, i.t);
    glTexStorage2D(GL_TEXTURE_2D, 1, f, i.w, i.h);
    return i;
}
static void rm(Img* i) {
    if (i->t) glDeleteTextures(1, &i->t);
    i->t = 0;
}
static void ctx_free(void) {
    for (int i = 0; i < 2; i++) rm(&C.in[i]);
    for (int i = 0; i < 7; i++) rm(&C.mip[i]);
    for (int i = 0; i < 4; i++) rm(&C.gen[i]);
    for (int i = 0; i < 7; i++) {
        AlphaR* a = &C.a[i];
        rm(&a->t1);
        rm(&a->t2);
        for (int j = 0; j < 2; j++) {
            rm(&a->t3[j]);
            for (int k = 0; k < 3; k++) rm(&a->out[k][j]);
        }
    }
    for (int j = 0; j < 2; j++) {
        rm(&C.b.t1[j]);
        rm(&C.b.t2[j]);
    }
    for (int i = 0; i < 6; i++) rm(&C.b.out[i]);
    for (int i = 0; i < 7; i++) {
        for (int j = 0; j < 3; j++) rm(&C.g[i].t1[j]);
        for (int j = 0; j < 2; j++) rm(&C.g[i].t2[j]);
        rm(&C.g[i].out);
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) rm(&C.d[i].t1[j]);
        for (int j = 0; j < 2; j++) rm(&C.d[i].t2[j]);
        rm(&C.d[i].out1);
        rm(&C.d[i].out2);
    }
    C.ok = 0;
}
static int q_(int n, int w) { return (n + w - 1) / w; }
int lsfg_context(u32 W, u32 H) {
    if (g_ok != 1) return 0;
    if (C.ok && C.W == W && C.H == H) return 1;
    ctx_free();
    C.W = W;
    C.H = H;
    C.fc = 0;
    for (int i = 0; i < 2; i++) C.in[i] = mk(W, H, GL_RGBA8);
    for (int i = 0; i < 4; i++) C.gen[i] = mk(W, H, GL_RGBA8);
    u32 fw = (u32)(W * g_lsfg_flow), fh = (u32)(H * g_lsfg_flow);
    for (int i = 0; i < 7; i++) C.mip[i] = mk(fw >> i, fh >> i, GL_R8);
    int P = !g_lsfg_quality; /* perf mode needs fewer temp images */
    for (int i = 0; i < 7; i++) {
        AlphaR* a = &C.a[i];
        u32 w = C.mip[i].w, h = C.mip[i].h, hw = (w + 1) >> 1, hh = (h + 1) >> 1, qw = (hw + 1) >> 1,
            qh = (hh + 1) >> 1;
        int n1 = P ? 1 : 2, n3 = 2, no = P ? 2 : 4;
        (void)n1;
        (void)no;
        a->t1 = mk(hw, hh, GL_RGBA8);
        a->t2 = mk(hw, hh, GL_RGBA8);
        for (int j = 0; j < n3; j++) {
            a->t3[j] = mk(qw, qh, GL_RGBA8);
            for (int k = 0; k < 3; k++) a->out[k][j] = mk(qw, qh, GL_RGBA8);
        }
    }
    {
        u32 w = C.a[0].out[0][0].w, h = C.a[0].out[0][0].h;
        for (int j = 0; j < 2; j++) {
            C.b.t1[j] = mk(w, h, GL_RGBA8);
            C.b.t2[j] = mk(w, h, GL_RGBA8);
        }
        for (int i = 0; i < 6; i++) C.b.out[i] = mk(w >> i, h >> i, GL_R8);
    }
    for (int i = 0; i < 7; i++) {
        u32 w = C.a[6 - i].out[0][0].w, h = C.a[6 - i].out[0][0].h;
        GammaR* g = &C.g[i];
        for (int j = 0; j < 3; j++) g->t1[j] = mk(w, h, GL_RGBA8);
        for (int j = 0; j < 2; j++) g->t2[j] = mk(w, h, GL_RGBA8);
        g->out = mk(w, h, GL_RGBA16F);
    }
    for (int j = 0; j < 3; j++) {
        int i = 4 + j;
        u32 w = C.a[6 - i].out[0][0].w, h = C.a[6 - i].out[0][0].h;
        DeltaR* d = &C.d[j];
        for (int k = 0; k < 3; k++) d->t1[k] = mk(w, h, GL_RGBA8);
        for (int k = 0; k < 2; k++) d->t2[k] = mk(w, h, GL_RGBA8);
        d->out1 = mk(w, h, GL_RGBA16F);
        d->out2 = mk(w, h, GL_RGBA16F);
    }
    C.ok = glGetError() == GL_NO_ERROR;
    if (!C.ok) {
        LOG("lsfg: could not create the images (%ux%u)\n", W, H);
        ctx_free();
    } else
        LOG("lsfg: context %ux%u (flow %ux%u)\n", W, H, fw, fh);
    return C.ok;
}

/* cbuf: offset, firstIter, firstIterS, colors, 1/flow scale, timestamp, ui threshold */
static GLuint ubo(float ts, int first, int firstS) {
    for (int i = 0; i < g_nubo; i++)
        if (g_ubo_key[i][0] == ts && g_ubo_key[i][1] == (float)first && g_ubo_key[i][2] == (float)firstS)
            return g_ubo[i];
    if (g_nubo >= 16) return g_ubo[0];
    struct {
        u32 off[2], first, firstS, ack, hdr;
        float inv, ts, ui;
        u32 pad[3];
    } cb = {{0, 0}, (u32)first, (u32)firstS, 0, 0, 1.0f / g_lsfg_flow, ts, 0.5f, {0, 0, 0}};
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_UNIFORM_BUFFER, b);
    glBufferData(GL_UNIFORM_BUFFER, sizeof cb, &cb, GL_STATIC_DRAW);
    g_ubo[g_nubo] = b;
    g_ubo_key[g_nubo][0] = ts;
    g_ubo_key[g_nubo][1] = (float)first;
    g_ubo_key[g_nubo][2] = (float)firstS;
    g_nubo++;
    return b;
}

/* one dispatch. textures/samplers/outputs are bound in declaration order */
static void D(int prog, GLuint cb, const Img* const* in, int nin, const int* smp, int nsmp, const Img* const* out,
              int nout, u32 gx, u32 gy) {
    LProg* p = &g_p[prog];
    glUseProgram(p->prog);
    if (cb) glBindBufferBase(GL_UNIFORM_BUFFER, 0, cb);
    for (int k = 0; k < p->nu; k++) {
        int ii = p->u[k].img - 32, si = p->u[k].smp - 16;
        GLuint t = ii >= 0 && ii < nin && in[ii] ? in[ii]->t : 0;
        GLuint s = g_smp[si >= 0 && si < nsmp ? smp[si] : (nsmp ? smp[0] : 0)];
        glActiveTexture(GL_TEXTURE0 + k);
        glBindTexture(GL_TEXTURE_2D, t);
        glBindSampler(k, s);
    }
    for (int k = 0; k < nout; k++) glBindImageTexture(k, out[k]->t, 0, GL_FALSE, 0, GL_WRITE_ONLY, out[k]->f);
    glDispatchCompute(gx ? gx : 1, gy ? gy : 1, 1);
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}
#define L(...)                                                                                                         \
    (const Img*[]) { __VA_ARGS__ }
#define S(...)                                                                                                         \
    (const int[]) { __VA_ARGS__ }

/* the caller copies the current frame here before lsfg_process */
GLuint lsfg_in_tex(void) { return C.ok ? C.in[C.fc % 2].t : 0; }
void lsfg_reset(void) { C.fc = 0; }

/* run on the frame just copied in and produce ngen frames between prev and current.
   returns how many were made (0 until the history is full) */
int lsfg_process(int ngen, GLuint* outs) {
    if (!C.ok) return 0;
    u64 fc = C.fc;
    int P = !g_lsfg_quality;
    if (ngen > 3) ngen = 3;
    /* 1) once per real frame: mips, alpha (7 levels), beta */
    {
        GLuint cb = ubo(0, 0, 0);
        u32 w = C.mip[0].w, h = C.mip[0].h;
        const Img* o[7];
        for (int i = 0; i < 7; i++) o[i] = &C.mip[i];
        D(P_MIP, cb, L(&C.in[fc % 2]), 1, S(0), 1, o, 7, (w + 63) >> 6, (h + 63) >> 6);
    }
    for (int ii = 0; ii < 7; ii++) {
        int i = 6 - ii;
        AlphaR* a = &C.a[i];
        u32 hw = a->t1.w, hh = a->t1.h, qw = a->t3[0].w, qh = a->t3[0].h;
        if (P) {
            D(P_A0, 0, L(&C.mip[i]), 1, S(0), 1, L(&a->t1), 1, q_(hw, 8), q_(hh, 8));
            D(P_A1, 0, L(&a->t1), 1, S(0), 1, L(&a->t2), 1, q_(hw, 8), q_(hh, 8));
            D(P_A2, 0, L(&a->t2), 1, S(0), 1, L(&a->t3[0], &a->t3[1]), 2, q_(qw, 8), q_(qh, 8));
            D(P_A3, 0, L(&a->t3[0], &a->t3[1]), 2, S(0), 1, L(&a->out[fc % 3][0], &a->out[fc % 3][1]), 2, q_(qw, 8),
              q_(qh, 8));
        }
    }
    {
        BetaR* b = &C.b;
        u32 w = b->t1[0].w, h = b->t1[0].h;
        AlphaR* a = &C.a[0];
        int i = (int)(fc % 3);
        D(P_B0, 0,
          L(&a->out[(i + 1) % 3][0], &a->out[(i + 1) % 3][1], &a->out[(i + 2) % 3][0], &a->out[(i + 2) % 3][1],
            &a->out[i][0], &a->out[i][1]),
          6, S(1), 1, L(&b->t1[0], &b->t1[1]), 2, q_(w, 8), q_(h, 8));
        D(P_B1, 0, L(&b->t1[0], &b->t1[1]), 2, S(0), 1, L(&b->t2[0], &b->t2[1]), 2, q_(w, 8), q_(h, 8));
        D(P_B2, 0, L(&b->t2[0], &b->t2[1]), 2, S(0), 1, L(&b->t1[0], &b->t1[1]), 2, q_(w, 8), q_(h, 8));
        D(P_B3, 0, L(&b->t1[0], &b->t1[1]), 2, S(0), 1, L(&b->t2[0], &b->t2[1]), 2, q_(w, 8), q_(h, 8));
        const Img* o[6];
        for (int k = 0; k < 6; k++) o[k] = &b->out[k];
        D(P_B4, ubo(0.5f, 0, 0), L(&b->t2[0], &b->t2[1]), 2, S(0), 1, o, 6, q_(w, 32), q_(h, 32));
    }
    C.fc++;
    if (fc < 2 || ngen <= 0) return 0; /* need 3 frames of history */
    /* 2) per generated frame: gamma (7) + delta (3 finest) + generate */
    int cur = (int)(fc % 3), prv = (int)((fc + 2) % 3);
    for (int pass = 0; pass < ngen; pass++) {
        float ts = (float)(pass + 1) / (float)(ngen + 1);
        for (int i = 0; i < 7; i++) {
            GammaR* g = &C.g[i];
            AlphaR* a = &C.a[6 - i];
            const Img* opt = i ? &C.g[i - 1].out : NULL;
            u32 w = g->t1[0].w, h = g->t1[0].h;
            const Img* inb = &C.b.out[(6 - i) < 5 ? 6 - i : 5];
            GLuint cb = ubo(ts, i == 0, 0);
            D(P_G0, cb, L(&a->out[prv][0], &a->out[prv][1], &a->out[cur][0], &a->out[cur][1], opt), 5, S(1, 2), 2,
              L(&g->t1[0], &g->t1[1], &g->t1[2]), 3, q_(w, 8), q_(h, 8));
            D(P_G1, 0, L(&g->t1[0], &g->t1[1], &g->t1[2]), 3, S(0), 1, L(&g->t2[0], &g->t2[1]), 2, q_(w, 8), q_(h, 8));
            D(P_G2, 0, L(&g->t2[0], &g->t2[1]), 2, S(0), 1, L(&g->t1[0], &g->t1[1]), 2, q_(w, 8), q_(h, 8));
            D(P_G3, 0, L(&g->t1[0], &g->t1[1]), 2, S(0), 1, L(&g->t2[0], &g->t2[1]), 2, q_(w, 8), q_(h, 8));
            D(P_G4, cb, L(&g->t2[0], &g->t2[1], opt, inb), 4, S(0, 2), 2, L(&g->out), 1, q_(w, 8), q_(h, 8));
            if (i < 4) continue;
            int j = i - 4;
            DeltaR* d = &C.d[j];
            const Img* o1 = j ? &C.g[i - 1].out : NULL;
            const Img* o2 = j ? &C.d[j - 1].out1 : NULL;
            const Img* o3 = j ? &C.d[j - 1].out2 : NULL;
            const Img* inb2 = &C.b.out[6 - i];
            GLuint cbd = ubo(ts, 0, j == 0);
            D(P_G0, cbd, L(&a->out[prv][0], &a->out[prv][1], &a->out[cur][0], &a->out[cur][1], o1), 5, S(1, 2), 2,
              L(&d->t1[0], &d->t1[1], &d->t1[2]), 3, q_(w, 8), q_(h, 8));
            D(P_D1, 0, L(&d->t1[0], &d->t1[1], &d->t1[2]), 3, S(0), 1, L(&d->t2[0], &d->t2[1]), 2, q_(w, 8), q_(h, 8));
            D(P_D2, 0, L(&d->t2[0], &d->t2[1]), 2, S(0), 1, L(&d->t1[0], &d->t1[1]), 2, q_(w, 8), q_(h, 8));
            D(P_D3, 0, L(&d->t1[0], &d->t1[1]), 2, S(0), 1, L(&d->t2[0], &d->t2[1]), 2, q_(w, 8), q_(h, 8));
            D(P_D4, cbd, L(&d->t2[0], &d->t2[1], o1, inb2), 4, S(0, 2), 2, L(&d->out1), 1, q_(w, 8), q_(h, 8));
            D(P_D5, cbd, L(&a->out[prv][0], &a->out[prv][1], &a->out[cur][0], &a->out[cur][1], o1, o2), 6, S(1, 2), 2,
              L(&d->t2[0]), 1, q_(w, 8), q_(h, 8));
            D(P_D6, 0, L(&d->t2[0]), 1, S(0), 1, L(&d->t1[0]), 1, q_(w, 8), q_(h, 8));
            D(P_D7, 0, L(&d->t1[0]), 1, S(0), 1, L(&d->t2[0]), 1, q_(w, 8), q_(h, 8));
            D(P_D8, 0, L(&d->t2[0]), 1, S(0), 1, L(&d->t1[0]), 1, q_(w, 8), q_(h, 8));
            D(P_D9, cbd, L(&d->t1[0], o3), 2, S(0, 2), 2, L(&d->out2), 1, q_(w, 8), q_(h, 8));
        }
        /* previous = in[(fc+1)%2], current = in[fc%2] */
        D(P_GEN, ubo(ts, 0, 0), L(&C.in[(fc + 1) % 2], &C.in[fc % 2], &C.g[6].out, &C.d[2].out1, &C.d[2].out2), 5,
          S(0, 2), 2, L(&C.gen[pass]), 1, q_(C.W, 16), q_(C.H, 16));
        outs[pass] = C.gen[pass].t;
    }
    return ngen;
}
int lsfg_ready(void) { return g_ok == 1; }
void lsfg_debug_tex(GLuint* prev, GLuint* cur, u32* W, u32* H) {
    *prev = C.in[C.fc % 2].t;
    *cur = C.in[(C.fc + 1) % 2].t;
    *W = C.W;
    *H = C.H;
}
