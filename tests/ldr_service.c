/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Real ldr:ro, memory block list and generated A64 interpreter; only platform and IPC transport are fakes.
   No game assets, RSA keys or precompiled NRO host libraries are required. */
#include "nx/hle/services/services_int.h"
#include "nx/gpu/gpu.h"
#include <openssl/evp.h>
#include <stdarg.h>
#include <sys/mman.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ldr_service:%d: %s\n", __LINE__, #x);                                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
#define RO(d) RC(22, d)
#define SRC   HEAP_BASE
#define BSS   (HEAP_BASE + 0x4000)
#define NRR   (HEAP_BASE + 0x5000)
#define SRC2  (HEAP_BASE + 0x8000)
static int fail_commit;
u64 g_title_id = 0x0100000000001000ull;
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
int g_verbose;

void pm_init(PMutex* m) { CHECK(!pthread_mutex_init(&m->l, NULL)); }
void pm_lock(PMutex* m) { CHECK(!pthread_mutex_lock(&m->l)); }
void pm_unlock(PMutex* m) { CHECK(!pthread_mutex_unlock(&m->l)); }
u8* plat_vm_reserve(u64 size) {
    void* p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
int plat_vm_commit(u8* p, u64 size) {
    if (fail_commit) {
        fail_commit = 0;
        return -1;
    }
    return mprotect(p, size, PROT_READ | PROT_WRITE);
}
void plat_vm_decommit(u8* p, u64 size) {
    CHECK(!madvise(p, size, MADV_DONTNEED));
    CHECK(!mprotect(p, size, PROT_NONE));
}
void gpu_mem_changed(u64 a, u64 size) {
    (void)a;
    (void)size;
}
void logf_(int level, const char* fmt, ...) {
    (void)level;
    (void)fmt;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}
static KObj process = {.type = KO_PROCESS};
KObj* h_get_ref(u32 h, int type) { return h == 0xFFFF8001u && type == KO_PROCESS ? &process : NULL; }
void kobj_unref(KObj* p) { CHECK(p == &process); }
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    SvcObj* o = calloc(1, sizeof *o);
    CHECK(o != NULL);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    o->data = calloc(1, size);
    CHECK(o->data != NULL);
    return o;
}
void svcobj_unref(SvcObj* o) {
    if (--o->refs) return;
    if (o->destroy) o->destroy(o);
    free(o->data);
    free(o);
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    CHECK(0);
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return RC(10, 235);
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    (void)o;
    (void)c;
    return RC(10, 221);
}
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}
static IpcCtx ctx;
static u64 raw[5];
static void request(u32 cmd, u64 a, u64 b, u64 c, u64 d) {
    memset(&ctx, 0, sizeof ctx);
    raw[0] = 0xdeadbeef; /* raw PID placeholder: not the trusted descriptor PID */
    raw[1] = a;
    raw[2] = b;
    raw[3] = c;
    raw[4] = d;
    ctx.cmd = cmd;
    ctx.raw = (const u8*)raw;
    ctx.raw_size = sizeof raw;
    ctx.has_pid = 1;
    ctx.pid = K_PROCESS_ID;
    ctx.ncopy = cmd == 4 || cmd == 10; /* the commands that take the process handle */
    ctx.copy_h[0] = 0xFFFF8001u;
}
static int call(SvcObj* o) { return o->dispatch(o, &ctx); }
static u64 output(void) {
    u64 v;
    CHECK(ctx.out_size == 8);
    memcpy(&v, ctx.out, 8);
    return v;
}
static void block_is(u64 a, u32 state, u32 perm, u32 attr) {
    MemBlock b;
    mem_query(a, &b);
    CHECK(b.state == state && b.perm == perm && b.attr == attr);
}
static void fixture(void) {
    memset(GP(SRC), 0, 0x6000);
    memcpy(GP(SRC + 0x10), "NRO0", 4);
    gw32(SRC + 0x18, 0x3000);
    gw32(SRC + 0x24, 0x1000);
    gw32(SRC + 0x28, 0x1000);
    gw32(SRC + 0x2c, 0x1000);
    gw32(SRC + 0x30, 0x2000);
    gw32(SRC + 0x34, 0x1000);
    gw32(SRC + 0x38, 0x1000);
    memset(GP(SRC + 0x40), 0x42, 32);
    /* A function absent from any AOT dispatch: mov x0,#42; ret. */
    gw32(SRC + 0x80, 0xd2800540);
    gw32(SRC + 0x84, 0xd65f03c0);
    /* A relative relocation witness. The service must leave it for the guest linker, not apply it itself. */
    gw32(SRC + 4, 0x100);
    memcpy(GP(SRC + 0x100), "MOD0", 4);
    gw32(SRC + 0x104, 0x100);
    gw64(SRC + 0x200, 7);
    gw64(SRC + 0x208, 0x280);
    gw64(SRC + 0x210, 8);
    gw64(SRC + 0x218, 24);
    gw64(SRC + 0x220, 9);
    gw64(SRC + 0x228, 24);
    gw64(SRC + 0x280, 0x2000);
    gw64(SRC + 0x288, 1027);
    gw64(SRC + 0x290, 0x80);
    gw64(SRC + 0x2000, 0x80);
    memset(GP(BSS), 0xa5, 0x1000); /* map preserves caller's BSS backing, as Eden does */
    memcpy(GP(NRR), "NRR0", 4);
    gw64(NRR + 0x330, g_title_id);
    gw32(NRR + 0x338, 0x1000);
    gw32(NRR + 0x340, 0x350);
    gw32(NRR + 0x344, 1);
    CHECK(EVP_Digest(GP(SRC), 0x3000, GP(NRR + 0x350), NULL, EVP_sha256(), NULL));
    memcpy(GP(SRC2), GP(SRC), 0x3000);
}
static void initialize(SvcObj* o) {
    request(4, 0, 0, 0, 0);
    CHECK(!call(o));
}
static u64 load(SvcObj* o) {
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(!call(o));
    return output();
}
#ifdef SNX_RO_INTERP_TEST
void snx_interp(CPU* c, u64 entry);
static void compiled_callee(CPU* cpu, u64 entry) {
    CHECK(entry == 0x200);
    cpu->x[0] = 41;
    cpu->pc = cpu->x[30];
}
static void execute(u64 base) {
    CPU cpu = {0};
    GuestFn dispatch[1] = {0};
    cpu.mem = g_mem;
    cpu.disp = dispatch;
    cpu.code_lo = 0;
    cpu.code_hi = 4;
    const RtAPI rt = {0};
    cpu.rt = &rt;
    cpu.x[30] = 0x1234;
    CHECK(snx_lookup(&cpu, base + 0x80) == snx_interp_entry);
    snx_lookup(&cpu, base + 0x80)(&cpu, base + 0x80);
    CHECK(cpu.x[0] == 42 && cpu.pc == 0x1234);
    /* NRO -> AOT dispatch -> NRO -> caller, with a saved link register. */
    const u32 code[] = {0xaa1e03f3, 0xd2804001, 0xd63f0020, 0xaa1303fe, 0x91000400, 0xd65f03c0};
    memcpy(GP(base + 0x90), code, sizeof code);
    dispatch[0] = compiled_callee;
    cpu.code_lo = 0x200;
    cpu.code_hi = 0x204;
    snx_lookup(&cpu, base + 0x90)(&cpu, base + 0x90);
    CHECK(cpu.x[0] == 42 && cpu.pc == 0x1234);
}
#endif
/* the two loads and the main thread start together, at a gate the three wait at once: a barrier, which Darwin's
   pthreads don't have */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t open;
    int waiting;
} Gate;
static void gate_wait(Gate* g) {
    pthread_mutex_lock(&g->lock);
    if (++g->waiting == 3)
        pthread_cond_broadcast(&g->open);
    else
        while (g->waiting < 3) pthread_cond_wait(&g->open, &g->lock);
    pthread_mutex_unlock(&g->lock);
}
typedef struct {
    SvcObj* service;
    Gate* gate;
    u64 source, bss, base;
    int result;
} LoadRace;
static void* race_load(void* arg) {
    LoadRace* r = arg;
    u64 args[] = {0, r->source, 0x3000, r->bss, 0x1000};
    IpcCtx c = {.cmd = 0, .raw = (const u8*)args, .raw_size = sizeof args, .has_pid = 1, .pid = K_PROCESS_ID};
    gate_wait(r->gate);
    r->result = r->service->dispatch(r->service, &c);
    if (!r->result) memcpy(&r->base, c.out, 8);
    return NULL;
}
static void concurrency(void) {
    fixture();
    SvcObj* o = new_ldr();
    initialize(o);
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    for (int repeat = 0; repeat < 8; repeat++) {
        Gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
        LoadRace a = {o, &gate, SRC, BSS, 0, 0}, b = {o, &gate, SRC2, SRC2 + 0x3000, 0, 0};
        pthread_t first, second;
        CHECK(!pthread_create(&first, NULL, race_load, &a));
        CHECK(!pthread_create(&second, NULL, race_load, &b));
        gate_wait(&gate);
        CHECK(!pthread_join(first, NULL));
        CHECK(!pthread_join(second, NULL));
        CHECK(!pthread_cond_destroy(&gate.open) && !pthread_mutex_destroy(&gate.lock));
        CHECK((!a.result && b.result == RO(3)) || (!b.result && a.result == RO(3)));
        request(1, a.base ? a.base : b.base, 0, 0, 0);
        CHECK(!call(o));
        block_is(SRC, MS_NORMAL, PERM_RW, 0);
        block_is(SRC2, MS_NORMAL, PERM_RW, 0);
    }
    svcobj_unref(o);
}
/* a request shorter than its command's arguments (0x28 bytes for MapManualLoadModuleMemory: the pid, then four u64s)
   is refused by sf before ldr sees it: nothing is mapped */
static void short_request(void) {
    fixture();
    SvcObj* o = new_ldr();
    initialize(o);
    request(0, SRC, 0x3000, BSS, 0x1000);
    ctx.raw_size = 0x20;
    CHECK(call(o) == RC(11, 420) && !ctx.out_size);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    request(0, SRC, 0x3000, BSS, 0x1000);
    ctx.raw_size = 8;
    CHECK(call(o) == RC(11, 420));
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    svcobj_unref(o);
}
static void limits(void) {
    CHECK(!mem_heap_set(0x200000, 0x200000));
    fixture();
    SvcObj* o = new_ldr();
    initialize(o);
    for (unsigned i = 0; i < 64; i++) {
        u64 a = HEAP_BASE + 0x10000 + i * 0x1000;
        memcpy(GP(a), GP(NRR), 0x1000);
        request(2, a, 0x1000, 0, 0);
        CHECK(!call(o));
    }
    request(2, NRR, 0x1000, 0, 0);
    CHECK(call(o) == RO(8));
    svcobj_unref(o);
    o = new_ldr();
    initialize(o);
    /* 64 independently authorized NROs; a full table must not lock a 65th source. */
    gw32(NRR + 0x344, 64);
    for (unsigned i = 0; i < 64; i++) {
        u64 a = HEAP_BASE + 0x100000 + i * 0x4000;
        memcpy(GP(a), GP(SRC), 0x3000);
        GP(a + 0x40)[0] = (u8)i;
        CHECK(EVP_Digest(GP(a), 0x3000, GP(NRR + 0x350 + i * 32), NULL, EVP_sha256(), NULL));
    }
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    for (unsigned i = 0; i < 64; i++) {
        u64 a = HEAP_BASE + 0x100000 + i * 0x4000;
        request(0, a, 0x3000, a + 0x3000, 0x1000);
        CHECK(!call(o));
    }
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(7));
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    svcobj_unref(o);
    CHECK(!mem_heap_set(0x10000, 0x200000));
}
int main(void) {
    mem_init();
    CHECK(!mem_heap_set(0x10000, 0x10000));
    fixture();
    SvcObj* o = new_ldr();
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(1031));
    request(4, 0, 0, 0, 0); /* no process id: sf refuses it before ldr sees it (InvalidCmifRequest) */
    ctx.has_pid = 0;
    CHECK(call(o) == RC(11, 420));
    request(4, 0, 0, 0, 0);
    ctx.copy_h[0] = 123;
    CHECK(call(o) == RO(1031));
    request(4, 0, 0, 0, 0); /* no process handle at all, or two: refused by sf as well */
    ctx.ncopy = 0;
    CHECK(call(o) == RC(11, 420));
    request(4, 0, 0, 0, 0);
    ctx.ncopy = 2;
    CHECK(call(o) == RC(11, 420));
    request(2, NRR, 0x1000, 0, 0); /* RegisterModuleInfo before RegisterProcessHandle */
    CHECK(call(o) == RO(1031));
    initialize(o);
    request(4, 0, 0, 0, 0);
    CHECK(call(o) == RO(1030));
    SvcObj* other = new_ro1();
    request(4, 0, 0, 0, 0);
    CHECK(call(other) == RO(1030));
    svcobj_unref(other);
    request(0, SRC, 0x3000, BSS, 0x1000);
    ctx.pid = 1;
    CHECK(call(o) == RO(1031));
    request(0, SRC + 1, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(1025));
    request(0, SRC, 0, BSS, 0x1000);
    CHECK(call(o) == RO(1026));
    request(0, ~0xfffull, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(1026));
    request(0, 0, 0x3000, 0, 0);
    CHECK(call(o) == RO(1027));
    request(0, AS_SIZE, 0x3000, 0, 0);
    CHECK(call(o) == RO(1027));
    request(0, SRC, 0x3000, SRC, 0x1000);
    CHECK(call(o) == RO(1027));
    request(0, HEAP_BASE + 0xe000, 0x3000, 0, 0);
    CHECK(call(o) == RO(1027)); /* source crosses into a hole */
    request(0, SRC, 0x3000, HEAP_BASE + 0x10000, 0x1000);
    CHECK(call(o) == RO(1027)); /* invalid BSS must not leave the image locked */
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(9) && !ctx.out_size);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    request(2, NRR, 0x1000, 0, 0);
    gw32(NRR + 0x344, ~0u);
    CHECK(call(o) == RO(6));
    gw32(NRR + 0x344, 1);
    gw32(NRR + 0x340, ~0u);
    CHECK(call(o) == RO(6));
    gw32(NRR + 0x340, 0x350);
    gw64(NRR + 0x330, 1);
    CHECK(call(o) == RO(6));
    gw64(NRR + 0x330, g_title_id);
    CHECK(!call(o)); /* RegisterModuleInfo has no process handle: the session's RegisterProcessHandle gave it */
    /* Hash list is a snapshot. */
    memset(GP(NRR + 0x350), 0, 32);
    request(0, SRC, 0x3000, BSS, 0x1000);
    fail_commit = 1;
    CHECK(call(o) == RO(1023));
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    gw32(SRC + 0x24, 0xfff);
    request(0, SRC, 0x3000, BSS, 0x1000);
    CHECK(call(o) == RO(4));
    gw32(SRC + 0x24, 0x1000);
    gw32(SRC + 0x1c, 2);
    CHECK(call(o) == RO(4));
    gw32(SRC + 0x1c, 0);
    gw32(SRC + 0x38, 0);
    CHECK(call(o) == RO(4));
    gw32(SRC + 0x38, 0x1000);
    GP(SRC + 0x180)[0] = 1;
    CHECK(call(o) == RO(9));
    GP(SRC + 0x180)[0] = 0;
    u64 base = load(o);
    CHECK(base != SRC && !memcmp(GP(base), GP(SRC), 0x3000));
    CHECK(gr64(base + 0x2000) == 0x80 && GP(base + 0x3000)[0] == 0xa5);
    block_is(base, MS_ALIASCODE, PERM_RX, 0);
    block_is(base + 0x1000, MS_ALIASCODE, PERM_R, 0);
    block_is(base + 0x2000, MS_ALIASCODEDATA, PERM_RW, 0);
    block_is(BSS, MS_NORMAL, PERM_NONE, 1);
    block_is(SRC, MS_NORMAL, PERM_NONE, 1);
    CHECK(g_code_memory == 0 && g_heap_size == 0x10000);
    CHECK(mem_heap_set(0, 0x10000) == MEM_BADSTATE);
#ifdef SNX_RO_INTERP_TEST
    execute(base);
#endif
    request(0, SRC2, 0x3000, SRC2 + 0x3000, 0x1000);
    CHECK(call(o) == RO(3));
    block_is(SRC2, MS_NORMAL, PERM_RW, 0);
    request(1, SRC, 0, 0, 0);
    CHECK(call(o) == RO(1028)); /* unload takes returned base, NOT source */
    request(1, base + 1, 0, 0, 0);
    CHECK(call(o) == RO(1025));
    gw64(base + 0x2008, 0xabcdef);
    gw64(base + 0x3000, 0x9876);
    request(3, NRR, 0, 0, 0);
    CHECK(!call(o)); /* unloading NRR must not unload NRO */
    request(1, base, 0, 0, 0);
    CHECK(!call(o));
    CHECK(call(o) == RO(1028));
    block_is(base, MS_FREE, PERM_NONE, 0);
    block_is(SRC, MS_NORMAL, PERM_RW, 0);
    CHECK(gr64(SRC + 0x2008) == 0xabcdef && gr64(BSS) == 0x9876);
    request(999, 0, 0, 0, 0);
    CHECK(call(o) == RC(10, 221)); /* sf's UnknownCommandId */
    fixture();
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    base = load(o);
    svcobj_unref(o);
    block_is(base, MS_FREE, PERM_NONE, 0);
    block_is(BSS, MS_NORMAL, PERM_RW, 0);
    o = new_ro1();
    initialize(o);
    fixture();
    request(10, NRR, 0x1000, 0, 0); /* RegisterProcessModuleInfo checks its process handle (sf, that there is one) */
    ctx.ncopy = 0;
    CHECK(call(o) == RC(11, 420));
    ctx.ncopy = 1;
    ctx.copy_h[0] = 123;
    CHECK(call(o) == RO(1031));
    request(10, NRR, 0x1000, 0, 0);
    CHECK(call(o) == RO(10));
    GP(NRR + 0x33c)[0] = 1;
    CHECK(!call(o));
    base = load(o);
    svcobj_unref(o);
    block_is(base, MS_FREE, PERM_NONE, 0);
    fixture();
    gw32(SRC + 0x38, 0);
    CHECK(EVP_Digest(GP(SRC), 0x3000, GP(NRR + 0x350), NULL, EVP_sha256(), NULL));
    o = new_ldr();
    initialize(o);
    request(2, NRR, 0x1000, 0, 0);
    CHECK(!call(o));
    request(0, SRC, 0x3000, 0, 0);
    CHECK(!call(o));
    base = output();
    block_is(BSS, MS_NORMAL, PERM_RW, 0); /* zero-sized BSS needs no backing */
    svcobj_unref(o);
    block_is(base, MS_FREE, PERM_NONE, 0);
    short_request();
    concurrency();
    limits();
    CHECK(!mem_heap_set(0, 0x10000));
    CHECK(!munmap(g_mem, AS_SIZE));
    puts("ldr:ro: IPC, authorization, mapping, rollback, unload, concurrency and capacity checks passed");
#ifdef SNX_RO_INTERP_TEST
    puts("ldr:ro: generated A64 interpreter and AOT callback execution checks passed");
#endif
    return 0;
}
