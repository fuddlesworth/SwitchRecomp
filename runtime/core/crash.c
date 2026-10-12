/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the crash report. It runs in the crash handler: the log is in its crash mode by then (log_crash_mode), and the
   hints aren't saved here, the main loop saves them every few seconds */
#include "core/loader.h"
#include "core/main.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#if defined(_WIN32)
extern char __ImageBase; /* the linker's: where the executable is loaded */
#else
#include <signal.h>
#endif
/* fault: the address a memory fault touched, NULL for any other kind of crash. sig: the signal, or on Windows the
   exception code */
void crash_report(void* host_pc, void* fault, int sig) {
    KThread* t = kthread_cur();
#if defined(_WIN32)
    const char* kind = fault ? "invalid memory access" : "exception";
    logf_(0, "\n*** Crash (exception %x) at host pc=%p", (unsigned)sig, host_pc);
#else
    const char* kind = fault           ? "invalid memory access"
                       : sig == SIGILL ? "illegal instruction"
                       : sig == SIGFPE ? "arithmetic error"
                                       : "crash";
    logf_(0, "\n*** Crash (signal %d) at host pc=%p", sig, host_pc);
#endif
    /* the game address only for an access inside the guest's address space */
    u8* f = (u8*)fault;
    int in_guest = f && g_mem && f >= g_mem && f < g_mem + AS_SIZE;
    u64 ga = in_guest ? (u64)(f - g_mem) : 0;
    if (in_guest)
        logf_(0, ", access to %p (game address %llx) ***\n", fault, (unsigned long long)ga);
    else if (fault)
        logf_(0, ", access to %p ***\n", fault);
    else
        logf_(0, " (%s) ***\n", kind);
    if (g_mem) {
        char mr[200];
        mem_report(mr, sizeof mr);
        logf_(0, "  memory: %s\n", mr);
    }
#ifdef _WIN32
    {
        u64 b = (u64)(size_t)&__ImageBase, p = (u64)(size_t)host_pc;
        if (p >= b && p < b + 0x10000000ull)
            logf_(0, "  in the runtime itself (exe offset +%llx), thread %s\n", (unsigned long long)(p - b),
                  t ? "of the game" : "of the runtime (GPU, audio or window)");
    }
#endif
    if (in_guest && ga < 0x1000000ull) {
        if (g_opt_mem_gb < 6)
            logf_(0,
                  "  the address is a null pointer plus an offset: the game probably got NULL from its allocator (out "
                  "of memory?). Try memory_gb=6 in the .ini\n");
        else
            logf_(0, "  the address is a null pointer plus an offset\n");
    }
    if (g_mod) { /* closest recompiled fn below host pc */
        u64 best = 0, ga2 = 0;
        for (u32 i = 0; i < g_mod->nentries; i++) {
            u64 fn = (u64)(size_t)g_mod->entry_fn[i];
            if (fn <= (u64)(size_t)host_pc && fn > best) {
                best = fn;
                ga2 = g_mod->entry_addr[i];
            }
        }
        if (best && (u64)(size_t)host_pc - best < 0x100000)
            logf_(0, "  inside the recompiled code of game function %llx (+%llx on the host)\n",
                  (unsigned long long)ga2, (unsigned long long)((u64)(size_t)host_pc - best));
    }
    if (t) {
        logf_(0, "  thread %llu (%s)\n", (unsigned long long)t->id, t->name);
        kernel_dump_regs(&t->cpu);
    }
    fatal("The game crashed (%s). See the .log.", kind);
}
