/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* IPC (ipc.c): service objects, sessions, the request and reply helpers, the service registry */
#ifndef SWR_IPC_H
#define SWR_IPC_H
#include "abi/cpu.h"
#include "kernel/kernel.h"
#include <string.h>

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
int ipc_warn_once(const char* name, u32 cmd); /* 1 the first time a service command is seen unhandled */
extern char g_ipc_ring[32][80];               /* the last requests, for crash reports */
extern u32 g_ipc_ring_n;

/* service registry */
typedef SvcObj* (*SvcCtor)(void);
void svc_register(const char* name, SvcCtor ctor);
SvcObj* svc_open(const char* name);

#endif
