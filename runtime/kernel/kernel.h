/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the HLE kernel (kernel.c): objects, handles, threads, events, shared memory */
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
    int prio;
    int core;       /* the core it runs on, what svcGetCurrentProcessorNumber reports */
    int ideal_core; /* as svcSetThreadCoreMask set it: -1 when the game doesn't care */
    u32 yields;     /* svcSleepThread yields, every 64th of which is a host one */
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
extern u64 g_sysres_size;
extern int g_opt_mem_gb;
void svc_exit_thread(KThread* t);
void kernel_heal_mutexes(void);
KShmem* kshmem_new(u64 size, const char* name);
u8* kshmem_ptr(KShmem* s);
void kernel_dump_regs(CPU* c);
void kernel_dump_threads(void);
void mem_report(char* out, int n);

#endif
