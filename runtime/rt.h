/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* runtime internal header: HLE kernel, IPC, services, GPU, frontend */
#ifndef SWR_RT_H
#define SWR_RT_H
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

/* ======================================================= platform */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct {
    SRWLOCK l;
} PMutex;
typedef struct {
    CONDITION_VARIABLE c;
} PCond;
#else
#include <pthread.h>
typedef struct {
    pthread_mutex_t l;
} PMutex;
typedef struct {
    pthread_cond_t c;
} PCond;
#endif
void pm_init(PMutex* m);
void pm_lock(PMutex* m);
void pm_unlock(PMutex* m);
void pc_init(PCond* c);
void pc_signal(PCond* c);
void pc_broadcast(PCond* c);
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns); /* 0 woken, 1 timed out */
typedef void (*PThreadFn)(void* arg);
int plat_thread(PThreadFn fn, void* arg, size_t stack);
void plat_thread_low_priority(void);
void plat_thread_exit(void);
void plat_sleep_ns(i64 ns);
void plat_yield(void);
u64 plat_ns(void);             /* monotonic */
u64 plat_thread_cpu_ns(void);  /* cpu time, this thread */
u64 plat_process_cpu_ns(void); /* cpu time, whole process */
i64 plat_unix_time(void);
i64 plat_tz_offset(void); /* local - UTC, seconds */
u8* plat_vm_reserve(u64 size);
int plat_ww_collect(u8* base, u64 size, void (*cb)(u64));
extern int g_ww_ok;
/* written-page tracking by range: calls cb for each run of pages in [p, p+size) written since the range was last
   scanned, and starts tracking them again. -1 when the platform can't (then everything must be checked by content).
   Linux: userfaultfd write protection (async) and PAGEMAP_SCAN; Windows: GetWriteWatch, with SWR_WW set */
int plat_ww_scan(u8* p, u64 size, void (*cb)(u8* p, u64 size));
int plat_vm_commit(u8* p, u64 size);
void plat_vm_decommit(u8* p, u64 size);
void* plat_dl_open(const char* path);
void* plat_dl_sym(void* h, const char* name);
int plat_mkdir(const char* path);
int plat_is_dir(const char* path);
typedef struct {
    void* h;
    int first;
    char name[512];
    int is_dir;
    u64 size;
} PDir;
int plat_dir_open(PDir* d, const char* path);
int plat_dir_next(PDir* d);
void plat_dir_close(PDir* d);
u64 plat_file_size(const char* path);
int plat_pread(FILE* f, void* buf, u64 size, u64 off);
void plat_install_crash_handler(void);
int plat_ncpu(void);
extern __thread void* tls_thread;

/* ======================================================= log */
extern int g_verbose;
void logf_(int lvl, const char* fmt, ...);
#define LOG(...) logf_(1, __VA_ARGS__)
#define LOGV(...)                                                                                                      \
    do {                                                                                                               \
        if (g_verbose >= 2) logf_(2, __VA_ARGS__);                                                                     \
    } while (0)
#define WARN(...) logf_(0, __VA_ARGS__)
void fatal(const char* fmt, ...);

/* ======================================================= address space */
#define AS_SIZE    0x2000000000ull
#define AS_BASE    0x08000000ull
#define HEAP_BASE  0x100000000ull
#define HEAP_SIZE  0x200000000ull
#define ALIAS_BASE 0x300000000ull
#define ALIAS_SIZE 0x1000000000ull
#define STACK_BASE 0x1300000000ull
#define STACK_SIZE 0x100000000ull
#define KERN_BASE  0x1400000000ull /* TLS, kernel stacks, our own shmem */
#define KERN_END   0x1800000000ull

enum {
    MS_FREE = 0x00000000,
    MS_IO = 0x00182001,
    MS_STATIC = 0x00042002,
    MS_CODE = 0x04DC7E03,
    MS_CODEDATA = 0x07FEBD04,
    MS_NORMAL = 0x077EBD05,
    MS_SHARED = 0x04402006,
    MS_ALIASCODE = 0x04DD7E08,
    MS_ALIASCODEDATA = 0x07FFBD09,
    MS_IPC = 0x045C3C0A,
    MS_STACK = 0x045C3C0B,
    MS_TLS = 0x0400000C,
    MS_TRANSFERED = 0x055C3C0D,
    MS_SHAREDTRANSFERED = 0x045C380E,
    MS_SHAREDCODE = 0x0440380F,
    MS_INACCESSIBLE = 0x00000010,
    MS_KERNEL = 0x00000013,
    MS_GENCODE = 0x04402214,
    MS_CODEOUT = 0x04402015
};
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_RW = 3, PERM_X = 4, PERM_RX = 5, PERM_DONTCARE = 0x10000000 };

extern u8* g_mem;
typedef struct {
    u64 addr, size;
    u32 state, perm, attr, ipc, dev;
} MemBlock;
void mem_init(void);
int mem_map(u64 addr, u64 size, u32 state, u32 perm); /* 0 ok */
int mem_unmap(u64 addr, u64 size);
int mem_is_free(u64 addr, u64 size);
void mem_query(u64 addr, MemBlock* out);
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm);
int mem_set_attr(u64 addr, u64 size, u32 mask, u32 attr);
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align);
u64 kmem_alloc(u64 size, u32 state, u32 perm); /* reserved for the kernel */
u64 mem_heap_set(u64 size);
extern u64 g_heap_size, g_heap_max;
#define GP(a) (g_mem + (u64)(a))
static inline u32 gr32(u64 a) {
    u32 v;
    memcpy(&v, GP(a), 4);
    return v;
}
static inline u64 gr64(u64 a) {
    u64 v;
    memcpy(&v, GP(a), 8);
    return v;
}
static inline void gw32(u64 a, u32 v) { memcpy(GP(a), &v, 4); }
static inline void gw64(u64 a, u64 v) { memcpy(GP(a), &v, 8); }

/* ======================================================= kernel */
enum {
    KO_NONE,
    KO_EVENT,
    KO_THREAD,
    KO_SESSION,
    KO_PORT,
    KO_SHMEM,
    KO_TMEM,
    KO_PROCESS,
    KO_RESLIMIT,
    KO_CODEMEM,
    KO_SERVER
};
typedef struct KThread KThread;
typedef struct KObj {
    int type;
    int refs;
    void (*destroy)(struct KObj*);
    int (*is_signaled)(struct KObj*);
    KThread** waiters;
    int nwait, capwait;
    const char* name;
} KObj;

typedef struct KEvent {
    KObj o;
    int signaled;
    int autoclear;
} KEvent;

struct KThread {
    KObj o;
    CPU cpu;
    u64 id;
    int prio, core;
    u64 affinity;
    u64 tls;
    u64 entry, arg, stack_top;
    int state; /* 0 created, 1 running, 2 exited */
    PCond cv;
    int woken, cancel, suspended;
    /* what it's blocked on */
    int wait_kind; /* 0 none 1 objects 2 mutex 3 condvar 4 address */
    u64 wait_addr, wait_key;
    u32 wait_tag;
    int wait_result;
    int synced_idx;
    KThread* next_waiter;
    char name[32];
    const char* wait_name;
    i64 wait_timeout;
    u64 cpu_ns0;
    int is_main;
    void* jmpbuf;
    u64 sleep_lr, sleep_n, sleep_x[31]; /* for threads.txt: catches SleepThread polling loops */
};

typedef struct KShmem {
    KObj o;
    u64 size;
    u8* host;
    u64 gaddr;
    u32 perm;
} KShmem;
typedef struct KTmem {
    KObj o;
    u64 addr, size;
    u32 perm;
} KTmem;

void kernel_init(void);
u32 h_new(KObj* o); /* new handle, takes a ref */
KObj* h_get(u32 h); /* no ref taken */
KObj* h_get_type(u32 h, int type);
int h_close(u32 h);
void kobj_ref(KObj* o);
void kobj_unref(KObj* o);
KEvent* kevent_new(const char* name, int autoclear);
void kevent_signal(KEvent* e);
void kevent_clear(KEvent* e);
void kobj_signal(KObj* o); /* wakes waiters */
KThread* kthread_cur(void);
KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core);
void kthread_start(KThread* t);
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core);
void swr_svc(CPU* c, u32 imm);
extern PMutex g_klock;
extern u64 g_title_id;
extern int g_exiting;
void process_exit(int code);

/* ======================================================= IPC */
#define MAX_BUFS 8
typedef struct {
    u64 addr, size;
    u32 flags;
} IpcBuf;
struct SvcObj;
typedef struct IpcCtx {
    KThread* th;
    u8* msg;
    u64 msg_addr;
    u32 type;
    u32 cmd;
    u32 token;
    const u8* raw;
    u32 raw_size; /* raw args after the CMIF header */
    u64 pid;
    int has_pid;
    u32 copy_h[8], move_h[8];
    int ncopy, nmove;
    IpcBuf a[MAX_BUFS], b[MAX_BUFS], w[MAX_BUFS], x[MAX_BUFS], c[MAX_BUFS];
    int na, nb, nw, nx, nc;
    u32 x_counters[MAX_BUFS];
    int is_domain;
    u32 domain_obj;
    u32 in_objs[8];
    int nin_objs;
    int tipc;
    /* reply */
    u32 rc;
    u8 out[0x100];
    u32 out_size;
    u32 out_copy[8], out_move[8];
    int nout_copy, nout_move;
    struct SvcObj* out_objs[8];
    int nout_objs;
    u64 x_out_addr[MAX_BUFS], x_out_size[MAX_BUFS];
    int nx_out;
    struct KSession* sess;
} IpcCtx;

typedef struct SvcObj {
    const char* name;
    int (*dispatch)(struct SvcObj* self, IpcCtx* ctx);
    void (*destroy)(struct SvcObj* self);
    int refs;
    void* data;
    u64 u[8];
} SvcObj;

typedef struct KSession {
    KObj o;
    SvcObj* obj;
    int is_domain;
    struct Domain* domain; /* shared by cloned sessions, same as HOS */
} KSession;
typedef struct Domain {
    SvcObj** obj;
    int n, cap, refs;
    PMutex lk;
} Domain;

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t datasize);
void svcobj_unref(SvcObj* o);
u32 session_new(SvcObj* obj);
void ipc_handle(KThread* t, u32 handle, u64 msg_addr);
/* reading args */
static inline u32 in32(IpcCtx* c, u32 off) {
    u32 v = 0;
    if (off + 4 <= c->raw_size) memcpy(&v, c->raw + off, 4);
    return v;
}
static inline u64 in64(IpcCtx* c, u32 off) {
    u64 v = 0;
    if (off + 8 <= c->raw_size) memcpy(&v, c->raw + off, 8);
    return v;
}
static inline u8 in8(IpcCtx* c, u32 off) { return off < c->raw_size ? c->raw[off] : 0; }
/* building the reply */
static inline void out32(IpcCtx* c, u32 v) {
    memcpy(c->out + c->out_size, &v, 4);
    c->out_size += 4;
}
static inline void out64(IpcCtx* c, u64 v) {
    c->out_size = (c->out_size + 7) & ~7u;
    memcpy(c->out + c->out_size, &v, 8);
    c->out_size += 8;
}
static inline void out8(IpcCtx* c, u8 v) { c->out[c->out_size++] = v; }
static inline void outbytes(IpcCtx* c, const void* p, u32 n) {
    memcpy(c->out + c->out_size, p, n);
    c->out_size += n;
}
static inline void outpad(IpcCtx* c, u32 al) { c->out_size = (c->out_size + al - 1) & ~(al - 1); }
void out_copy_h(IpcCtx* c, u32 h);
void out_move_h(IpcCtx* c, u32 h);
void out_obj(IpcCtx* c, SvcObj* o);
/* buffers: in = A or X, out = B or C */
int buf_in(IpcCtx* c, int idx, u64* addr, u64* size);
int buf_out(IpcCtx* c, int idx, u64* addr, u64* size);
void buf_x_out(IpcCtx* c, int idx, u64 addr, u64 size); /* mark a C buffer used (needed for the reply X descriptor) */
int buf_a(IpcCtx* c, int idx, u64* addr, u64* size);
int buf_b(IpcCtx* c, int idx, u64* addr, u64* size);
int buf_xs(IpcCtx* c, int idx, u64* addr, u64* size);
int buf_cs(IpcCtx* c, int idx, u64* addr, u64* size);
#define RC(mod, desc) ((mod) | ((desc) << 9))
#define RC_OK         0
int stub_ok(SvcObj* o, IpcCtx* c);
SvcObj* svc_generic(const char* name); /* empty success reply, logged */

/* service registry */
typedef SvcObj* (*SvcCtor)(void);
void svc_register(const char* name, SvcCtor ctor);
SvcObj* svc_open(const char* name);
void services_init(void);

/* ======================================================= game */
typedef struct SwrModInfo {
    const char* name;
    u64 base, size;
    u8 build_id[32];
    u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    u32 is_nro;
} SwrModInfo;
typedef struct SwrModule {
    u32 abi;
    u32 nmods;
    const SwrModInfo* mods;
    u32 nentries;
    const u64* entry_addr;
    const GuestFn* entry_fn;
    GuestFn interp;
    u64 code_lo, code_hi;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
    void (*init)(const RtAPI* rt);
    u64 (*step)(CPU* c, u32 w, u64 pc);
} SwrModule;
extern const SwrModule* g_mod;
extern GuestFn* g_disp;
extern char g_datadir[1024], g_exename[256], g_exedir[1024];
extern const RtAPI g_rtapi;
int loader_load(void);
extern u64 g_code_lo, g_code_hi;

/* romfs / files */
void romfs_init(void);
u64 romfs_size(void);
int romfs_read(u64 off, void* dst, u64 size);

/* ======================================================= frontend and input */
typedef struct {
    u64 buttons;
    i32 lx, ly, rx, ry;
    int touch, tx, ty;
} PadState;
extern PadState g_pad;
extern volatile int g_quit;
void video_init(const char* title);
void video_present_rgba(const u32* px, int w, int h, int stride); /* final RGBA8 image */
void video_poll(void);
int video_has_gl(void);
void backend_ini(const char* k, const char* v);
void frontend_frame(void);
extern int g_frames;
extern volatile u64 g_gpu_submits;
void ini_load(const char* path);
extern int g_opt_docked, g_opt_lang, g_opt_res, g_opt_fps, g_opt_vsync;

/* ======================================================= hid / vi / nv */
void hid_update(void);
void vi_init(void);
void vi_vsync_tick(void);
void nv_init(void);
/* gpu */
void gpu_init(void);
void gpu_submit(u64* entries, u32 n, u32 channel, u32 incs);
void gpu_present(u64 gpu_addr_or_cpu, u32 nvmap_handle, u32 w, u32 h, u32 stride, u32 fmt, int is_blocklinear,
                 u32 block_h_log2, u64 cpu_addr);
u64 nvmap_addr(u32 handle);
u64 gmmu_translate(u64 gva);
u32 syncpt_alloc(void);
u32 syncpt_read(u32 id);
void syncpt_incr(u32 id);
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns);
#endif
