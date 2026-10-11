/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the kernel's files share: result codes, the thread table, waiting and waking, and the SVCs svc.c dispatches to */
#ifndef SWR_KERNEL_INT_H
#define SWR_KERNEL_INT_H
#include "kernel/kernel.h"
#include "core/loader.h"
#include "kernel/ipc.h"
#include "kernel/mem.h"
#include "platform/log.h"
#include "platform/plat.h"
#include "core/rtapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_TIMEOUT     0xEA01
#define R_CANCELLED   0xEC01
#define R_BADHANDLE   0xE401
#define R_BADSTATE    0xFA01
#define R_BADADDR     0xCC01
#define R_OUTOFMEM    0xD001
#define R_BADENUM     0xF001
#define R_BADCOMBO    0xE801 /* InvalidCombination */
#define R_NOTFOUND    0xF201
#define R_OUTOFRANGE  0xEE01
#define R_BADMEMSTATE 0xD401
#define R_BADREGION   0xDC01
#define R_NOTIMPL     0xF601
#define HANDLE_WAIT   0x40000000u
#define H_CUR_PROC    0xFFFF8001u
#define H_CUR_THREAD  0xFFFF8000u
typedef struct {
    u32 imm, res;
    u64 lr, a[4], o1;
} SvcRec;

/* kernel.c */
extern PMutex g_klock;
extern u64 g_title_id;
extern u64 g_sysres_size;
extern int g_exiting;
extern u64 g_next_tid;
extern SvcRec g_svc_ring[64];
extern u32 g_svc_ring_n;
u32 h_new(KObj* o);
KObj* h_get_nl(u32 h);
KObj* h_get(u32 h);
KObj* h_get_type(u32 h, int t);
int h_close(u32 h);
void waiter_add(KObj* o, KThread* t);
void waiter_del(KObj* o, KThread* t);
void wake(KThread* t, int result);
void kobj_signal_nl(KObj* o);
int sleep_nl(KThread* t, i64 timeout);
KEvent* kevent_new(const char* name, int autoclear);
void kevent_signal(KEvent* e);

/* thread.c */
extern KThread* g_threads[256];
extern int g_nthreads;
KThread* kthread_cur(void);
KThread* kthread_create(u64 entry, u64 arg, u64 stack_top, int prio, int core);
void kthread_start(KThread* t);
void svc_exit_thread(KThread* t);
extern KThread* g_main;
void process_exit(int code);

/* sync.c */
u32 svc_arbitrate_lock(KThread* t, u32 owner, u64 addr, u32 tag);
u32 svc_arbitrate_unlock(u64 addr);
u32 svc_wait_cv(KThread* t, u64 maddr, u64 key, u32 tag, i64 timeout);
u32 svc_signal_cv(u64 key, i32 count);
u32 svc_wait_addr(KThread* t, u64 addr, u32 type, i32 value, i64 timeout);
u32 svc_signal_addr(u64 addr, u32 type, i32 value, i32 count);
u32 svc_wait_sync(KThread* t, u64 hptr, u32 n, i64 timeout, u32* out_idx);

/* svc_mem.c */
u32 svc_query(u64 info, u64 addr);
u32 svc_map_memory(u64 dst, u64 src, u64 size);
u32 svc_unmap_memory(u64 dst, u64 src, u64 size);
u32 svc_map_shm(u32 h, u64 addr, u64 size, u32 perm);
u32 svc_unmap_shm(u32 h, u64 addr, u64 size);
void tmem_destroy(KObj* o);
u64 mem_total(void);
extern u64 g_phys_mapped;
u64 used_memory(void);
u32 svc_get_info(u32 id0, u32 h, u64 id1, u64* out);

/* debug.c */
void kernel_dump_regs(CPU* c);

#endif
