/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the HLE kernel: objects and handles, events (kernel.c), threads (thread.c), shared memory and GetInfo (svc_mem.c),
   the dumps (debug.c). What only the kernel's own files use is in kernel_int.h */
#ifndef SWR_KERNEL_H
#define SWR_KERNEL_H
#include "abi/cpu.h"
#include "platform/plat.h"

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
    KO_SERVER,
    KO_WEVENT /* the writable end of an svcCreateEvent pair (svc.c) */
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
    int woken, cancel;
    /* svcSetThreadActivity (svc.c): suspended = it is paused, in_svc = it runs no game code because it is inside an
       SVC (both atomic: every SVC reads them without the lock), pause_at = where it waits while paused (0 nowhere or
       blocked inside the SVC, 1 before the SVC, 2 after it) */
    int suspended, in_svc, pause_at;
    /* what it's blocked on */
    int wait_kind; /* 0 none 1 objects 2 mutex 3 condvar 4 address 5 a thread being paused (svc.c) */
    u64 wait_addr, wait_key;
    u32 wait_tag;
    int wait_result;
    KThread* next_waiter;
    char name[32];
    const char* wait_name;
    i64 wait_timeout;
    u64 sleep_lr, sleep_n, sleep_x[31]; /* for threads.txt: catches SleepThread polling loops */
};

typedef struct KShmem {
    KObj o;
    u64 size;
    u8* host;
    u64 gaddr, gsize; /* where the game mapped it, 0 when it isn't (atomic), and the size it mapped */
    u32 perm;
} KShmem;
typedef struct KTmem {
    KObj o;
    u64 addr, size;
    u32 perm;
} KTmem;

void kernel_init(void);
u32 h_new(KObj* o); /* new handle, takes a ref */
void kobj_ref(KObj* o);
void kobj_unref(KObj* o);
KObj* h_get_ref(u32 h, int t); /* takes a ref the caller drops with kobj_unref. t 0 = any type */
KEvent* kevent_new(const char* name);
void kevent_signal(KEvent* e);
void kevent_clear(KEvent* e);
KThread* kthread_cur(void);
void kernel_run_main(u64 entry, u64 stack_size, int prio, int core);
void swr_svc(CPU* c, u32 imm);
extern PMutex g_klock;
extern u64 g_title_id;
extern u64 g_sysres_size;
extern int g_opt_mem_gb;
/* the cores and priorities the npdm's kernel capabilities give the process (svcGetInfo 0 and 1) */
extern u64 g_core_mask, g_prio_mask;
KShmem* kshmem_new(u64 size, const char* name);
u8* kshmem_ptr(KShmem* s);
void kernel_load_symbols(void); /* the game's exported symbols, for the dumps: call once its modules are loaded */
void kernel_dump_regs(CPU* c);
void kernel_dump_threads(void);
void mem_report(char* out, int n);

#endif
