/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* what the game module calls through RtAPI: undefined instructions, BRK, hints, ticks, FPCR */
#include "core/rtapi.h"
#include "core/clock.h"
#include "core/main.h"
#include "kernel/kernel.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_hints_path[1200];
static PMutex g_hint_lock;
static u64* g_hints; /* in the order they were found, for the file */
static int g_nhints, g_caphints, g_hints_dirty;
/* and a hash set of them, since every entry into the interpreter looks its address up */
static u64* g_hset;
static u32 g_hset_cap;        /* a power of two, at most half full */
static int hset_add(u64 pc) { /* 1 when pc is new. Hints are never 0 */
    if ((u64)(g_nhints + 1) * 2 > g_hset_cap) {
        u32 cap = g_hset_cap ? g_hset_cap * 2 : 1024;
        u64* n = calloc(cap, 8);
        for (u32 i = 0; i < g_hset_cap; i++)
            if (g_hset[i]) {
                u32 j = (u32)((g_hset[i] >> 2) * 0x9E3779B97F4A7C15ull >> 32) & (cap - 1);
                while (n[j]) j = (j + 1) & (cap - 1);
                n[j] = g_hset[i];
            }
        free(g_hset);
        g_hset = n;
        g_hset_cap = cap;
    }
    u32 j = (u32)((pc >> 2) * 0x9E3779B97F4A7C15ull >> 32) & (g_hset_cap - 1);
    while (g_hset[j]) {
        if (g_hset[j] == pc) return 0;
        j = (j + 1) & (g_hset_cap - 1);
    }
    g_hset[j] = pc;
    if (g_nhints == g_caphints) {
        g_caphints = g_caphints ? g_caphints * 2 : 256;
        g_hints = realloc(g_hints, 8 * g_caphints);
    }
    g_hints[g_nhints++] = pc;
    return 1;
}
static void rt_note_hint(u64 pc) {
    if (!pc) return;
    pm_lock(&g_hint_lock);
    if (hset_add(pc)) {
        g_hints_dirty = 1;
        LOGV("interpreter: entering %llx (not recompiled)\n", (unsigned long long)pc);
    }
    pm_unlock(&g_hint_lock);
}
/* the hints file, <Game>.hints next to the executable */
void hints_init(void) {
    snprintf(g_hints_path, sizeof g_hints_path, "%s%s.hints", g_exedir, g_exename);
    pm_init(&g_hint_lock);
}
/* called from the main loop and from the guest threads that stop the game: the lock comes first, so two of them can't
   truncate the file under each other */
void save_hints(void) {
    pm_lock(&g_hint_lock);
    FILE* f = g_hints_dirty ? fopen(g_hints_path, "w") : NULL;
    if (f) {
        for (int i = 0; i < g_nhints; i++) fprintf(f, "%llx\n", (unsigned long long)g_hints[i]);
        fclose(f);
        g_hints_dirty = 0;
    }
    pm_unlock(&g_hint_lock);
}
void load_hints(void) {
    FILE* f = fopen(g_hints_path, "r");
    if (!f) return;
    char ln[64];
    while (fgets(ln, sizeof ln, f)) {
        u64 v = strtoull(ln, NULL, 16);
        if (v) hset_add(v);
    }
    fclose(f);
}

static void rt_undef(CPU* c, u64 pc, u32 w) {
    WARN("\nUnsupported instruction %08x at %llx\n", w, (unsigned long long)pc);
    c->pc = pc;
    kernel_dump_regs(c);
    save_hints();
    fatal("Unsupported instruction %08x at %llx", w, (unsigned long long)pc);
}
static void rt_brk(CPU* c, u64 pc, u32 imm) {
    WARN("\nBRK #%u at %llx\n", imm, (unsigned long long)pc);
    c->pc = pc;
    kernel_dump_regs(c);
    save_hints();
    fatal("The game executed BRK at %llx", (unsigned long long)pc);
}
static u64 rt_ticks(void) { return guest_ticks(); }
/* what the host can't do the guest's way: said once each. The generated code is compiled for round-to-nearest, so a
   directed rounding mode only reaches the operations the compiler didn't fold (switchrecomp/toolchain.py) */
static void fpcr_warn(u32 fpcr) {
    static int said[3];
    if (((fpcr >> 22) & 3) && !__atomic_exchange_n(&said[0], 1, __ATOMIC_RELAXED))
        WARN("FPU: the game sets a directed rounding mode (FPCR %x), which the recompiled code only partly follows\n",
             fpcr);
#if !defined(__aarch64__)
    if ((fpcr & (1u << 25)) && !__atomic_exchange_n(&said[1], 1, __ATOMIC_RELAXED))
        WARN("FPU: the game sets default NaN mode (FPCR %x), which isn't emulated\n", fpcr);
    if ((fpcr & (1u << 26)) && !__atomic_exchange_n(&said[2], 1, __ATOMIC_RELAXED))
        WARN("FPU: the game sets alternative half precision (FPCR %x), which isn't emulated\n", fpcr);
#endif
}
void swr_fpcr_apply(CPU* c) {
    fpcr_warn(c->fpcr);
#if defined(__aarch64__)
    /* arm64 host: just load the guest FPCR (FZ, rounding, DN, AHP) */
    u64 v = (u64)(c->fpcr & 0x07C00000u);
    __asm__ __volatile__("msr fpcr, %0" ::"r"(v));
#elif defined(__x86_64__) || defined(_M_X64)
    u32 mx;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    mx &= ~(0x8040u | 0x6000u);
    if (c->fpcr & (1u << 24)) mx |= 0x8040; /* FZ -> FTZ+DAZ */
    static const u32 rm[4] = {0, 0x4000, 0x2000, 0x6000};
    mx |= rm[(c->fpcr >> 22) & 3];
    __asm__ __volatile__("ldmxcsr %0" ::"m"(mx));
#else
    (void)c;
#endif
}
static void rt_fpcr(CPU* c) { swr_fpcr_apply(c); }
static void rt_sev(void) {}
static void rt_wfe(CPU* c) {
    (void)c;
    plat_yield();
}
const RtAPI g_rtapi = {swr_svc, rt_undef, rt_note_hint, rt_ticks, rt_brk, rt_fpcr, rt_sev, rt_wfe};
