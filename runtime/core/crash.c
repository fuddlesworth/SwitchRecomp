/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the crash report */
#include "core/loader.h"
#include "core/main.h"
#include "core/rtapi.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/log.h"
#if defined(_WIN32)
extern char __ImageBase; /* the linker's: where the executable is loaded */
#endif
void crash_report(void* host_pc, void* fault, int sig) {
    KThread* t = kthread_cur();
    u64 ga = (u64)((u8*)fault - g_mem);
    logf_(0, "\n*** Crash (signal %d) at host pc=%p, access to %p (game address %llx) ***\n", sig, host_pc, fault,
          (unsigned long long)ga);
    {
        char mr[200];
        mem_report(mr, sizeof mr);
        logf_(0, "  memory: %s\n", mr);
#ifdef _WIN32
        {
            u64 b = (u64)(size_t)&__ImageBase, p = (u64)(size_t)host_pc;
            if (p >= b && p < b + 0x10000000ull)
                logf_(0, "  in the runtime itself (exe offset +%llx), thread %s\n", (unsigned long long)(p - b),
                      kthread_cur() ? "of the game" : "of the runtime (GPU, audio or window)");
        }
#endif
        if (ga && ga < 0x1000000ull)
            logf_(
                0,
                "  the address is a null pointer plus an offset: the game probably got NULL from its allocator (out of memory?). Try memory_gb=6 in the .ini\n");
    }
    if (g_mod) { /* closest recompiled fn below host pc */
        u64 best = 0, ga2 = 0;
        for (u32 i = 0; i < g_mod->nentries; i++) {
            u64 f = (u64)(size_t)g_mod->entry_fn[i];
            if (f <= (u64)(size_t)host_pc && f > best) {
                best = f;
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
    save_hints();
    fatal("The game crashed (invalid memory access). See the .log.");
}
