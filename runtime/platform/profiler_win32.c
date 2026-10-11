/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the sampling profiler (profile=1, Windows): about every millisecond it suspends each thread, reads its instruction
   pointer and maps it to a game function, the runtime or another module. profile.txt is rewritten every 10 s */
#include "core/loader.h"
#include "gpu/renderer.h"
#include "kernel/mem.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
static HANDLE g_pth[256];
static volatile LONG g_npth;
void plat_prof_add_thread(HANDLE h) {
    HANDLE d = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
    LONG i = InterlockedIncrement(&g_npth) - 1;
    if (i < 256)
        g_pth[i] = d;
    else
        CloseHandle(d);
}
typedef struct {
    u64 host;
    u32 idx;
} PEnt;
static PEnt* g_pe;
static u32 g_npe;
static u64 g_plo, g_phi;
static u32* g_pcnt;
#define PMOD 32
static struct {
    u64 base;
    char name[64];
    u32 n;
} g_pmod[PMOD];
static int g_npmod;
#define PEXE 4096
static struct {
    u32 off;
    u32 n;
} g_pexe[PEXE];
static u64 g_ptot, g_pguest, g_pexe_n;
static int pe_cmp(const void* a, const void* b) {
    u64 x = ((const PEnt*)a)->host, y = ((const PEnt*)b)->host;
    return x < y ? -1 : x > y;
}
static void prof_note(u64 rip) {
    g_ptot++;
    if (rip >= g_plo && rip < g_phi + 0x4000) {
        u32 lo = 0, hi = g_npe;
        while (hi - lo > 1) {
            u32 m = (lo + hi) / 2;
            if (g_pe[m].host <= rip)
                lo = m;
            else
                hi = m;
        }
        g_pcnt[lo]++;
        g_pguest++;
        return;
    }
    HMODULE hm = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)rip, &hm) ||
        !hm) {
        hm = NULL;
    }
    u64 base = (u64)(uintptr_t)hm;
    if (hm == GetModuleHandleA(NULL)) {
        u32 off = (u32)((rip - base) & ~0x3Full);
        u32 k = (off * 2654435761u) >> 20;
        for (int t = 0; t < PEXE; t++) {
            u32 s = (k + t) & (PEXE - 1);
            if (!g_pexe[s].n || g_pexe[s].off == off) {
                g_pexe[s].off = off;
                g_pexe[s].n++;
                break;
            }
        }
        g_pexe_n++;
    }
    int i;
    for (i = 0; i < g_npmod; i++)
        if (g_pmod[i].base == base) break;
    if (i == g_npmod && g_npmod < PMOD) {
        g_pmod[i].base = base;
        g_pmod[i].n = 0;
        if (!hm)
            strcpy(g_pmod[i].name, "(no module: generated/JIT code)");
        else {
            char pth[MAX_PATH];
            GetModuleFileNameA(hm, pth, MAX_PATH);
            const char* s = strrchr(pth, '\\');
            snprintf(g_pmod[i].name, 64, "%s", s ? s + 1 : pth);
        }
        g_npmod++;
    }
    if (i < g_npmod) g_pmod[i].n++;
}
static const char* g_pfile = "profile.txt";
static int g_pboot;
static void prof_write(void) {
    FILE* f = fopen(g_pfile, "w");
    if (!f) return;
    /* idle threads sit in ntdll/kernelbase/win32u, so percentages are of non-idle samples */
    u64 idle = 0;
    for (int i = 0; i < g_npmod; i++) {
        const char* n = g_pmod[i].name;
        if (!_stricmp(n, "ntdll.dll") || !_stricmp(n, "kernelbase.dll") || !_stricmp(n, "win32u.dll") ||
            !_stricmp(n, "kernel32.dll"))
            idle += g_pmod[i].n;
    }
    u64 act = g_ptot > idle ? g_ptot - idle : 1;
    fprintf(f, "SwitchRecomp profile: %llu samples, %llu active (excluding waits). %% of active time\n\n",
            (unsigned long long)g_ptot, (unsigned long long)act);
    fprintf(f, "modules:\n");
    for (int i = 0; i < g_npmod; i++) {
        const char* n = g_pmod[i].name;
        int w = !_stricmp(n, "ntdll.dll") || !_stricmp(n, "kernelbase.dll") || !_stricmp(n, "win32u.dll") ||
                !_stricmp(n, "kernel32.dll");
        if (w)
            fprintf(f, "   (waits) %s: %llu samples\n", n, (unsigned long long)g_pmod[i].n);
        else
            fprintf(f, "  %6.2f%%  %s\n", 100.0 * g_pmod[i].n / act, n);
    }
    fprintf(f, "  %6.2f%%  game code (functions below)\n", 100.0 * g_pguest / act);
    fprintf(f, "\ngame functions (guest address):\n");
    u64 top[6];
    int ntop = 0;
    for (int r = 0; r < 60; r++) {
        u32 best = ~0u, bn = 0;
        for (u32 i = 0; i < g_npe; i++)
            if (!(g_pcnt[i] & 0x80000000u) && g_pcnt[i] > bn) {
                bn = g_pcnt[i];
                best = i;
            }
        if (best == ~0u || !bn || bn * 2000ull < act) break;
        fprintf(f, "  %6.2f%%  %llx\n", 100.0 * bn / act, (unsigned long long)g_mod->entry_addr[g_pe[best].idx]);
        if (r < 6) top[ntop++] = g_mod->entry_addr[g_pe[best].idx];
        g_pcnt[best] |= 0x80000000u;
    }
    for (u32 i = 0; i < g_npe; i++) g_pcnt[i] &= 0x7FFFFFFFu;
    /* raw A64 words of the top functions, for disassembling later */
    fprintf(f, "\ncode of the hottest game functions (first 96 instructions):\n");
    for (int t = 0; t < ntop; t++) {
        fprintf(f, "  %llx:", (unsigned long long)top[t]);
        const u32* w = (const u32*)GP(top[t]);
        for (int k = 0; k < 96; k++) {
            if (k % 16 == 0) fprintf(f, "\n   ");
            fprintf(f, " %08x", w[k]);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\nruntime (offset in the exe):\n");
    for (int r = 0; r < 40; r++) {
        int best = -1;
        u32 bn = 0;
        for (int s = 0; s < PEXE; s++)
            if (g_pexe[s].n > bn && !(g_pexe[s].n & 0x80000000u)) {
                bn = g_pexe[s].n;
                best = s;
            }
        if (best < 0 || bn * 2000ull < act) break;
        fprintf(f, "  %6.2f%%  +%x\n", 100.0 * bn / act, g_pexe[best].off);
        g_pexe[best].n |= 0x80000000u;
    }
    for (int s = 0; s < PEXE; s++) g_pexe[s].n &= 0x7FFFFFFFu;
    fclose(f);
}
static unsigned __stdcall prof_thread(void* a) {
    (void)a;
    u64 last = GetTickCount64();
    DWORD me = GetCurrentThreadId();
    u64 start = last;
    for (;;) {
        Sleep(g_pboot ? 2 : 1);
        LONG n = g_npth;
        if (n > 256) n = 256;
        for (LONG i = 0; i < n; i++) {
            HANDLE h = g_pth[i];
            if (!h || GetThreadId(h) == me) continue;
            if (SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT cx;
            memset(&cx, 0, sizeof cx);
            cx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &cx)) {
                ResumeThread(h);
                prof_note(cx.Rip);
            } else
                ResumeThread(h);
        }
        if (GetTickCount64() - last >= (g_pboot ? 5000u : 10000u)) {
            last = GetTickCount64();
            prof_write();
        }
        if (g_pboot && GetTickCount64() - start >= 90000) {
            prof_write();
            break;
        }
    }
    return 0;
}
void plat_prof_start(int boot) {
    if (!g_mod || !g_mod->nentries) return;
    g_pboot = boot;
    if (boot) g_pfile = "startup.txt";
    g_npe = g_mod->nentries;
    g_pe = malloc(sizeof(PEnt) * g_npe);
    g_pcnt = calloc(g_npe, 4);
    g_plo = ~0ull;
    g_phi = 0;
    for (u32 i = 0; i < g_npe; i++) {
        g_pe[i].host = (u64)(uintptr_t)g_mod->entry_fn[i];
        g_pe[i].idx = i;
        if (g_pe[i].host < g_plo) g_plo = g_pe[i].host;
        if (g_pe[i].host > g_phi) g_phi = g_pe[i].host;
    }
    qsort(g_pe, g_npe, sizeof(PEnt), pe_cmp);
    {
        HANDLE d = NULL;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &d, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        plat_prof_add_thread(d);
        CloseHandle(d);
    }
    uintptr_t h = _beginthreadex(NULL, 0, prof_thread, NULL, 0, NULL);
    if (h) {
        SetThreadPriority((HANDLE)h, THREAD_PRIORITY_TIME_CRITICAL);
        CloseHandle((HANDLE)h);
    }
}
