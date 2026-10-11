/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GL call queue.
   The GPU thread records GL calls into a ring (pointer args get copied) and a dedicated thread replays them.
   Main reason is Proton: every GL call crosses Wine, which ate ~26% of the GPU thread on the Deck, and
   Mesa's glthread can't help since it sits below Wine.
   Calls that return something (glGet*, glGen*, glMapBuffer...) sync with the GL thread.
   glFenceSync is virtualized, the GL thread owns the real fences.
   SWR_NO_GLQ=1 turns it off. */
#include "gpu/gl/glq_int.h"
#include "gpu/gl/glq.h"
__thread int t_glq_direct;
extern void* backend_gl_proc(const char*);
extern int backend_gl_create(void);
extern void backend_gl_swap(void);
extern int backend_gl_worker_current(void);
extern int backend_glq_ok(void);

#define Q_CAP (64u << 20)
#define Q_BIG (8u << 20)
typedef struct {
    u32 size, big;
    void (*ex)(void*);
    void* data;
} QHdr; /* record size in the ring, bytes */
static u8* g_q;
static volatile u64 g_qh, g_qt; /* head = producer, tail = consumer */
static u64 g_qh_pend;
static int g_active;
static volatile int g_ready;
static volatile int g_sleeping;
static PMutex g_qm;
static PCond g_qc;
static volatile u32 g_lock;
static volatile u64 g_swaps_sent, g_swaps_done;
static void q_wrap_ex(void* p) { (void)p; }
static void q_lock(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) { while (__atomic_load_n(&g_lock, __ATOMIC_RELAXED)); }
}
static void q_unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }
int glq_active(void) { return g_active; }

/* ---------- producer */
static u8* q_reserve(u32 total) {
    for (int spins = 0;; spins++) {
        u64 h = g_qh, t = __atomic_load_n(&g_qt, __ATOMIC_ACQUIRE);
        u64 pos = h % Q_CAP;
        u64 need = (pos + total > Q_CAP) ? (Q_CAP - pos) + total : total;
        if (h + need - t <= Q_CAP) {
            if (pos + total > Q_CAP) {
                QHdr* w = (QHdr*)(g_q + pos);
                w->size = (u32)(Q_CAP - pos);
                w->big = 0;
                w->ex = q_wrap_ex;
                w->data = NULL;
                h += Q_CAP - pos;
                pos = 0;
            }
            g_qh_pend = h + total;
            return g_q + pos;
        }
        if (spins > 64) plat_yield();
    }
}
void* glq_alloc(size_t sz, size_t extra, void (*ex)(void*)) {
    q_lock();
    size_t pay = q_al16(sz) + q_al16(extra);
    if (pay > Q_BIG) {
        QHdr* hd = (QHdr*)q_reserve((u32)sizeof(QHdr));
        u8* d = malloc(pay);
        hd->size = sizeof(QHdr);
        hd->big = 1;
        hd->ex = ex;
        hd->data = d;
        return d;
    }
    u32 total = (u32)(sizeof(QHdr) + pay);
    QHdr* hd = (QHdr*)q_reserve(total);
    hd->size = total;
    hd->big = 0;
    hd->ex = ex;
    hd->data = (u8*)(hd + 1);
    return hd->data;
}
static void q_wake(void) {
    if (__atomic_load_n(&g_sleeping, __ATOMIC_ACQUIRE)) {
        pm_lock(&g_qm);
        pc_signal(&g_qc);
        pm_unlock(&g_qm);
    }
}
void glq_commit(void) {
    __atomic_store_n(&g_qh, g_qh_pend, __ATOMIC_RELEASE);
    q_unlock();
    q_wake();
}

typedef struct {
    void (*ex)(void*);
    void* arg;
    volatile int done;
} QSync;
static void q_sync_ex(void* p) {
    QSync* s = *(QSync**)p;
    s->ex(s->arg);
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
}
void glq_sync(void (*ex)(void*), void* arg) {
    if (t_glq_direct || !g_active) {
        ex(arg);
        return;
    }
    QSync s = {ex, arg, 0};
    QSync** p = (QSync**)glq_alloc(sizeof(QSync*), 0, q_sync_ex);
    *p = &s;
    glq_commit();
    for (int i = 0; !__atomic_load_n(&s.done, __ATOMIC_ACQUIRE); i++) {
        if (i > 200) plat_yield();
    }
}
static void q_nop(void* arg) { (void)arg; }
void glq_finish_all(void) {
    if (g_active && !t_glq_direct) glq_sync(q_nop, NULL);
}

/* ---------- unpack state, needed to know how many bytes to copy */
static GLint g_un_align = 4, g_un_row = 0, g_un_imgh = 0, g_un_skp = 0, g_un_skr = 0, g_un_ski = 0;
static GLuint g_un_pbo;
int q_unpack_pbo(void) { return g_un_pbo != 0; }
static size_t q_px(GLenum f, GLenum t) {
    int c = 4;
    switch (f) {
        case GL_RED:
        case GL_RED_INTEGER:
        case GL_DEPTH_COMPONENT:
        case GL_STENCIL_INDEX:
        case GL_GREEN:
        case GL_BLUE: c = 1; break;
        case GL_RG:
        case GL_RG_INTEGER: c = 2; break;
        case GL_RGB:
        case GL_BGR:
        case GL_RGB_INTEGER:
        case GL_BGR_INTEGER: c = 3; break;
        case GL_DEPTH_STENCIL: c = 1; break;
        default: c = 4;
    }
    switch (t) {
        case GL_UNSIGNED_BYTE:
        case GL_BYTE: return c;
        case GL_UNSIGNED_SHORT:
        case GL_SHORT:
        case GL_HALF_FLOAT: return 2 * c;
        case GL_UNSIGNED_INT:
        case GL_INT:
        case GL_FLOAT: return 4 * c;
        case GL_UNSIGNED_SHORT_5_6_5:
        case GL_UNSIGNED_SHORT_5_6_5_REV:
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        case GL_UNSIGNED_SHORT_5_5_5_1:
        case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
        case GL_UNSIGNED_BYTE_3_3_2:
        case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
        default: return 4;
    }
} /* 8_8_8_8, 10_10_10_2, 2_10_10_10_REV, 24_8, 10F_11F_11F_REV, 5_9_9_9_REV */
size_t q_texel_size(GLenum f, GLenum t) { return q_px(f, t); }
size_t q_img_size(GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum t) {
    if (w <= 0 || h <= 0 || d <= 0) return 0;
    size_t px = q_px(f, t), rl = (size_t)(g_un_row > 0 ? g_un_row : w), al = (size_t)(g_un_align > 0 ? g_un_align : 1);
    size_t row = (rl * px + al - 1) / al * al, ih = (size_t)(g_un_imgh > 0 ? g_un_imgh : h);
    return ((size_t)g_un_ski * ih + (size_t)g_un_skr) * row + (size_t)g_un_skp * px +
           ((size_t)(d - 1) * ih + (size_t)(h - 1)) * row + (size_t)w * px;
}

/* ---------- hand-written wrappers (glq_gen.c has the r_ pointers) */
typedef void(APIENTRY* T_ps)(GLenum, GLint);
extern T_ps r_glPixelStorei;
typedef void(APIENTRY* T_bb)(GLenum, GLuint);
extern T_bb r_glBindBuffer;
typedef GLsync(APIENTRY* T_fs)(GLenum, GLbitfield);
extern T_fs r_glFenceSync;
typedef GLenum(APIENTRY* T_cws)(GLsync, GLbitfield, GLuint64);
extern T_cws r_glClientWaitSync;
typedef void(APIENTRY* T_ds)(GLsync);
extern T_ds r_glDeleteSync;
typedef struct {
    GLenum a;
    GLint b;
} A2;
static void e_ps(void* p) {
    A2* a = p;
    r_glPixelStorei(a->a, a->b);
}
void APIENTRY q_glPixelStorei(GLenum pname, GLint param) {
    if (t_glq_direct) {
        r_glPixelStorei(pname, param);
        return;
    }
    switch (pname) {
        case GL_UNPACK_ALIGNMENT: g_un_align = param; break;
        case GL_UNPACK_ROW_LENGTH: g_un_row = param; break;
        case GL_UNPACK_IMAGE_HEIGHT: g_un_imgh = param; break;
        case GL_UNPACK_SKIP_PIXELS: g_un_skp = param; break;
        case GL_UNPACK_SKIP_ROWS: g_un_skr = param; break;
        case GL_UNPACK_SKIP_IMAGES: g_un_ski = param; break;
    }
    A2* a = glq_alloc(sizeof *a, 0, e_ps);
    a->a = pname;
    a->b = param;
    glq_commit();
}
static void e_bb(void* p) {
    A2* a = p;
    r_glBindBuffer(a->a, (GLuint)a->b);
}
void APIENTRY q_glBindBuffer(GLenum target, GLuint buffer) {
    if (t_glq_direct) {
        r_glBindBuffer(target, buffer);
        return;
    }
    if (target == GL_PIXEL_UNPACK_BUFFER) g_un_pbo = buffer;
    A2* a = glq_alloc(sizeof *a, 0, e_bb);
    a->a = target;
    a->b = (GLint)buffer;
    glq_commit();
}
/* virtual fences */
typedef struct VF {
    GLsync real;
    volatile int sig;
    struct VF* next;
} VF;
static VF* g_vf_list; /* GL thread side: fences still pending */
typedef struct {
    VF* v;
    GLenum c;
    GLbitfield f;
} AF;
static void e_fs(void* p) {
    AF* a = p;
    a->v->real = r_glFenceSync(a->c, a->f);
    a->v->next = g_vf_list;
    g_vf_list = a->v;
}
GLsync APIENTRY q_glFenceSync(GLenum c, GLbitfield f) {
    VF* v = calloc(1, sizeof *v);
    if (t_glq_direct) {
        v->real = r_glFenceSync(c, f);
        return (GLsync)v;
    }
    AF* a = glq_alloc(sizeof *a, 0, e_fs);
    a->v = v;
    a->c = c;
    a->f = f;
    glq_commit();
    return (GLsync)v;
}
typedef struct {
    VF* v;
    GLbitfield f;
    GLuint64 t;
    GLenum r;
} AW;
static void e_cws(void* p) {
    AW* a = p;
    if (a->v->sig) {
        a->r = GL_ALREADY_SIGNALED;
        return;
    }
    a->r = a->v->real ? r_glClientWaitSync(a->v->real, a->f, a->t) : GL_WAIT_FAILED;
    if (a->r == GL_ALREADY_SIGNALED || a->r == GL_CONDITION_SATISFIED)
        __atomic_store_n(&a->v->sig, 1, __ATOMIC_RELEASE);
}
GLenum APIENTRY q_glClientWaitSync(GLsync s, GLbitfield f, GLuint64 t) {
    VF* v = (VF*)s;
    if (!v) return GL_WAIT_FAILED;
    if (__atomic_load_n(&v->sig, __ATOMIC_ACQUIRE)) return GL_ALREADY_SIGNALED;
    AW a = {v, f, t, 0};
    if (t_glq_direct) {
        e_cws(&a);
        return a.r;
    }
    if (t == 0) return GL_TIMEOUT_EXPIRED; /* GL thread polls it, don't block here */
    glq_sync(e_cws, &a);
    return a.r;
}
static void e_ds(void* p) {
    VF* v = *(VF**)p;
    for (VF** pp = &g_vf_list; *pp; pp = &(*pp)->next)
        if (*pp == v) {
            *pp = v->next;
            break;
        }
    if (v->real) r_glDeleteSync(v->real);
    free(v);
}
void APIENTRY q_glDeleteSync(GLsync s) {
    VF* v = (VF*)s;
    if (!v) return;
    if (t_glq_direct) {
        if (v->real) r_glDeleteSync(v->real);
        free(v);
        return;
    }
    VF** p = glq_alloc(sizeof *p, 0, e_ds);
    *p = v;
    glq_commit();
}
static void q_poll_fences(void) {
    for (VF* v = g_vf_list; v; v = v->next)
        if (!v->sig && v->real) {
            GLenum r = r_glClientWaitSync(v->real, 0, 0);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) __atomic_store_n(&v->sig, 1, __ATOMIC_RELEASE);
        }
}

/* ---------- swap */
static void e_swap(void* p) {
    (void)p;
    backend_gl_swap();
    __atomic_add_fetch(&g_swaps_done, 1, __ATOMIC_RELEASE);
}
void glq_swap(void) {
    if (!g_active || t_glq_direct) {
        backend_gl_swap();
        return;
    }
    g_swaps_sent++;
    glq_alloc(16, 0, e_swap);
    glq_commit();
    /* max one frame ahead, more just adds input lag */
    for (int i = 0; g_swaps_sent - __atomic_load_n(&g_swaps_done, __ATOMIC_ACQUIRE) > 1; i++) {
        if (i > 200) plat_yield();
    }
}
int glq_worker_current(void) {
    int r = backend_gl_worker_current();
    if (r) t_glq_direct = 1;
    return r;
}

/* ---------- proc lookup */
void* glq_proc(const char* n) {
    if (!g_active) return backend_gl_proc(n);
    for (const GlqEnt* e = g_glq_tab; e->name; e++)
        if (!strcmp(e->name, n)) return *e->real ? e->q : NULL;
    {
        static int said;
        if (said < 8) {
            said++;
            LOG("glq: %s is not in the queue table (called directly)\n", n);
        }
    }
    return backend_gl_proc(n);
}

/* ---------- GL thread */
static int g_start_ok;
static void gl_thread(void* arg) {
    (void)arg;
    {
        extern void plat_thread_high_priority(void);
        plat_thread_high_priority();
    }
    t_glq_direct = 1;
    int ok = backend_gl_create() && swr_gl_load(backend_gl_proc);
    extern int swr_gl_is_gles(void);
    if (ok && swr_gl_is_gles()) ok = 0; /* not on GLES */
    if (ok) {
        for (const GlqEnt* e = g_glq_tab; e->name; e++) {
            void* r = e->pvar ? *e->pvar : NULL;
            if (!r) r = backend_gl_proc(e->name);
            *e->real = r;
        }
        for (const GlqEnt* e = g_glq_tab; e->name; e++)
            if (e->pvar && *e->real) *e->pvar = e->q;
    }
    g_start_ok = ok;
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    if (!ok) return;
    u32 n = 0;
    for (;;) {
        u64 h = __atomic_load_n(&g_qh, __ATOMIC_ACQUIRE), t = g_qt;
        if (t == h) {
            if (g_vf_list) q_poll_fences();
            for (int i = 0; i < 2000 && __atomic_load_n(&g_qh, __ATOMIC_ACQUIRE) == t; i++) {
                if (i > 50) plat_yield();
            }
            if (__atomic_load_n(&g_qh, __ATOMIC_ACQUIRE) != t) continue;
            pm_lock(&g_qm);
            __atomic_store_n(&g_sleeping, 1, __ATOMIC_RELEASE);
            if (__atomic_load_n(&g_qh, __ATOMIC_ACQUIRE) == t) pc_wait(&g_qc, &g_qm, g_vf_list ? 500000 : 2000000);
            __atomic_store_n(&g_sleeping, 0, __ATOMIC_RELEASE);
            pm_unlock(&g_qm);
            continue;
        }
        while (t != h) {
            QHdr* hd = (QHdr*)(g_q + t % Q_CAP);
            u32 sz = hd->size;
            hd->ex(hd->data);
            if (hd->big) free(hd->data);
            t += sz;
            __atomic_store_n(&g_qt, t, __ATOMIC_RELEASE);
            if (((++n) & 255) == 0 && g_vf_list) q_poll_fences();
        }
    }
}
int glq_start(void) {
    if (getenv("SWR_NO_GLQ") || !backend_glq_ok()) return 0;
    g_q = malloc(Q_CAP + 256); /* slack so a header written at the end can't run past the buffer */
    if (!g_q) return 0;
    pm_init(&g_qm);
    pc_init(&g_qc);
    if (plat_thread(gl_thread, NULL, 8u << 20)) return 0;
    while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) plat_sleep_ns(1000000);
    if (!g_start_ok) return -1;
    g_active = 1;
    LOG("gpu: OpenGL calls run on their own thread (the GPU thread and the driver work at the same time)\n");
    return 1;
}
