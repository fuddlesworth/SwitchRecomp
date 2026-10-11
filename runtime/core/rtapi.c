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
static u64* g_hints;
static int g_nhints, g_caphints, g_hints_dirty;
static void rt_note_hint(u64 pc) {
    pm_lock(&g_hint_lock);
    for (int i = g_nhints - 1; i >= 0 && i >= g_nhints - 64; i--)
        if (g_hints[i] == pc) {
            pm_unlock(&g_hint_lock);
            return;
        }
    int found = 0;
    for (int i = 0; i < g_nhints; i++)
        if (g_hints[i] == pc) {
            found = 1;
            break;
        }
    if (!found) {
        if (g_nhints == g_caphints) {
            g_caphints = g_caphints ? g_caphints * 2 : 256;
            g_hints = realloc(g_hints, 8 * g_caphints);
        }
        g_hints[g_nhints++] = pc;
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
void save_hints(void) {
    if (!g_hints_dirty) return;
    FILE* f = fopen(g_hints_path, "w");
    if (!f) return;
    pm_lock(&g_hint_lock);
    for (int i = 0; i < g_nhints; i++) fprintf(f, "%llx\n", (unsigned long long)g_hints[i]);
    g_hints_dirty = 0;
    pm_unlock(&g_hint_lock);
    fclose(f);
}
void load_hints(void) {
    FILE* f = fopen(g_hints_path, "r");
    if (!f) return;
    char ln[64];
    while (fgets(ln, sizeof ln, f)) {
        u64 v = strtoull(ln, NULL, 16);
        if (v) {
            if (g_nhints == g_caphints) {
                g_caphints = g_caphints ? g_caphints * 2 : 256;
                g_hints = realloc(g_hints, 8 * g_caphints);
            }
            g_hints[g_nhints++] = v;
        }
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
static u64 rt_ticks(void) { return (u64)((double)guest_ns() * 0.0192); }
void swr_fpcr_apply(CPU* c) {
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
