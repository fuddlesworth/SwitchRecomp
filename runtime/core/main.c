/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#define _GNU_SOURCE
/* startup: load GAME_code.dll/.so, set up memory + services + main thread, then run the frontend
   loop (window, input, vsync). */
#include "core/main.h"
#include "core/clock.h"
#include "core/loader.h"
#include "core/options.h"
#include "core/rtapi.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "hle/fs.h"
#include "hle/hid.h"
#include "hle/nvdrv.h"
#include "hle/services.h"
#include "hle/vi.h"
#include "kernel/kernel.h"
#include "kernel/mem.h"
#include "platform/backend.h"
#include "platform/log.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

const SwrModule* g_mod;
GuestFn* g_disp;
GuestFn swr_interp_entry;
char g_datadir[1024], g_exename[256], g_exedir[1024];
volatile int g_quit;
int g_frames;
/* ============================================================ code module */
typedef const SwrModule* (*ModFn)(void);
#if !defined(_WIN32)
__attribute__((weak)) const SwrModule* swr_module_static(void);
#else
const SwrModule* swr_module_static(void) __attribute__((weak));
#endif
int g_code_tcc;
static int load_code(void) {
    if (swr_module_static) {
        g_mod = swr_module_static();
        return g_mod ? 0 : -1;
    }
    char p[1200];
#if defined(_WIN32)
    snprintf(p, sizeof p, "%s%s_code.dll", g_exedir, g_exename);
#else
    snprintf(p, sizeof p, "%s%s_code.so", g_exedir, g_exename);
#endif
    FILE* probe = fopen(p, "rb");
    if (!probe) {
        WARN("Cannot find the recompiled code: %s\n", p);
        WARN("The runtime runs the game it is named after, from the game's folder: a copy named Game next to "
             "Game_code.so runs that game. Or name the game with --game=PATH, for example "
             "--game=games/Game.\n");
        return -1;
    }
    fclose(probe);
    void* h = plat_dl_open(p);
    if (!h) { /* it's there but doesn't load: a library it needs is missing, or it was built for another system */
#if !defined(_WIN32)
        WARN("dlopen: %s\n", dlerror());
#endif
        WARN("Cannot load the recompiled code: %s\n", p);
        return -1;
    }
    ModFn f = (ModFn)plat_dl_sym(h, "swr_module");
    if (!f) {
        WARN("%s is not a SwitchRecomp module\n", p);
        return -1;
    }
#if defined(_WIN32)
    { /* tell clang from tcc builds by the PE linker version (lld 14.0 vs tcc 6.0). tcc code is a lot slower */
        const u8* b = (const u8*)h;
        u32 lf;
        memcpy(&lf, b + 0x3C, 4);
        u8 mj = b[lf + 24 + 2], mn = b[lf + 24 + 3];
        g_code_tcc = mj == 6 && mn == 0;
        if (g_code_tcc)
            LOG("code: %s_code.dll was compiled with TCC. It runs several times slower than with Clang: recompile it with Clang for much better performance\n",
                g_exename);
        else
            LOG("code: %s_code.dll compiled with Clang (linker %u.%u)\n", g_exename, mj, mn);
    }
#endif
    g_mod = f();
    return g_mod ? 0 : -1;
}

static void build_dispatch(void) {
    g_code_lo = g_mod->code_lo;
    g_code_hi = g_mod->code_hi;
    u64 n = (g_code_hi - g_code_lo) / 4;
    g_disp = calloc(n ? n : 1, sizeof(GuestFn));
    for (u32 i = 0; i < g_mod->nentries; i++) {
        u64 a = g_mod->entry_addr[i];
        if (a >= g_code_lo && a < g_code_hi) g_disp[(a - g_code_lo) / 4] = g_mod->entry_fn[i];
    }
    swr_interp_entry = g_mod->interp;
}

static void services_init(void) {
    svc_core_init();
    svc_hid_init();
    svc_fs_init();
    svc_vi_init();
    nv_init();
    svc_pl_init();
}

static void split_path(const char* argv0) {
    char p[1024];
    strncpy(p, argv0 ? argv0 : "", 1023);
    p[1023] = 0;
    /* --game=games/Catherine/ names games/Catherine */
    for (size_t n = strlen(p); n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\'); n--) p[n - 1] = 0;
    char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\'), *s = s1 > s2 ? s1 : s2;
    const char* base = s ? s + 1 : p;
    strncpy(g_exename, base, 255);
    char* dot = strrchr(g_exename, '.');
    if (dot && (!strcmp(dot, ".exe") || !strcmp(dot, ".EXE") || !strcmp(dot, ".AppImage"))) *dot = 0;
    /* --game=games/Catherine_code.so names the same game as --game=games/Catherine */
    size_t n = strlen(g_exename);
    if (n > 8 && (!strcmp(g_exename + n - 8, "_code.so"))) g_exename[n - 8] = 0;
    if (n > 9 && (!strcmp(g_exename + n - 9, "_code.dll"))) g_exename[n - 9] = 0;
    if (s) {
        s[1] = 0;
        strncpy(g_exedir, p, 1023);
    } else {
#if defined(_WIN32)
        g_exedir[0] = 0;
#else
        strcpy(g_exedir, "./"); /* a bare file name would make dlopen search the library path, not this folder */
#endif
    }
}

/* players point --game at the dump itself (--game=ARMS.xci), and the runtime then looks for ARMS.xci_code.so. It only
   runs recompiled code, so name what is missing instead of the .so it would have looked for */
/* main.npdm: the SystemResourceSize (newer SDKs only init their VM allocator when > 0), and the cores and
   priorities the kernel capabilities of its ACI0 give the process */
static void read_npdm(void) {
    char q[1200];
    snprintf(q, sizeof q, "%sexefs/main.npdm", g_datadir);
    FILE* f = fopen(q, "rb");
    u8 h[0x4000];
    size_t n = 0;
    if (f) {
        n = fread(h, 1, sizeof h, f);
        fclose(f);
    }
    if (n >= 0x80 && !memcmp(h, "META", 4)) {
        u32 v, aci, acisz;
        memcpy(&v, h + 0x14, 4);
        g_sysres_size = v;
        memcpy(&aci, h + 0x70, 4);
        memcpy(&acisz, h + 0x74, 4);
        u32 ko = 0, ks = 0;
        if (acisz >= 0x38 && aci <= n && acisz <= n - aci && !memcmp(h + aci, "ACI0", 4)) {
            memcpy(&ko, h + aci + 0x30, 4);
            memcpy(&ks, h + aci + 0x34, 4);
        }
        for (u32 o = 0; ks <= acisz && ko <= acisz - ks && o + 4 <= ks; o += 4) {
            u32 cap;
            memcpy(&cap, h + aci + ko + o, 4);
            if ((cap & 0xF) != 0x7) continue; /* ThreadInfo: the lowest and highest priority and core */
            u32 lo_prio = (cap >> 4) & 63, hi_prio = (cap >> 10) & 63, min_core = (cap >> 16) & 255,
                max_core = (cap >> 24) & 255;
            if (hi_prio > lo_prio || min_core > max_core || max_core > 63) break;
            g_core_mask = (~0ull >> (63 - max_core)) & (~0ull << min_core);
            g_prio_mask = (~0ull >> (63 - lo_prio)) & (~0ull << hi_prio);
            break;
        }
    }
    LOG("npdm: system resource %.1f MB, cores %llx, priorities %llx\n", g_sysres_size / 1048576.0,
        (unsigned long long)g_core_mask, (unsigned long long)g_prio_mask);
}

static int is_dump(const char* path) {
    static const char* const ext[] = {".xci", ".xcz", ".nsp", ".nsz", ".nca", ".nro", ".nso"};
    const char* dot = strrchr(path, '.');
    if (!dot || strpbrk(dot, "/\\")) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        size_t k = 0;
        while (ext[i][k] && (dot[k] | 0x20) == ext[i][k]) k++;
        if (!ext[i][k] && !dot[k]) return 1;
    }
    return 0;
}

int swr_main(int argc, char** argv) {
    /* In an AppImage argv[0] points into its mount (APPDIR). APPIMAGE is the file that was run, so a copy named after
       the game (games/Catherine or games/Catherine.AppImage) finds games/Catherine_code.so like the bare runtime does.
       Both variables are inherited, so they only count when this runtime is the one inside the mount */
    const char *appimage = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    int in_appimage =
        appimage && *appimage && appdir && *appdir && argv[0] && !strncmp(argv[0], appdir, strlen(appdir));
    /* --game=PATH names the game the way the runtime's own name does (games/Catherine runs games/Catherine_code.so
       with games/Catherine_data/), so one runtime or AppImage runs any game without a copy named after it */
    /* the dump can be named with --game or on its own */
    const char *game = NULL, *dump = NULL;
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], "--game=", 7) && argv[i][7])
            game = argv[i] + 7;
        else if (argv[i][0] != '-' && is_dump(argv[i]))
            dump = argv[i];
    if (game && is_dump(game)) dump = game;
    /* named after the runtime when it's a dump, so the log the fatal error writes lands next to the runtime */
    split_path(game && !dump ? game : in_appimage ? appimage : argv[0]);
    if (dump)
        fatal("%s is a game dump. SwitchRecomp does not run dumps: it runs a game recompiled from one. Extract and "
              "recompile it first with a clone of the repository (scripts/extract.py, then scripts/recomp.py, as "
              "the README describes), then point --game at the result, for example --game=games/Game.",
              dump);
    /* loaded before the .ini is opened, which writes one: a mistyped --game leaves no stray settings file */
    if (load_code()) fatal("Could not load the game's recompiled code.");
    if (g_mod->abi != SWR_ABI) fatal("The recompiled code is from another SwitchRecomp version.");
    /* --key=value sets a setting for this run, over the .ini. The value is used where it is, so a long --script
       isn't cut */
    for (int i = 1; i < argc; i++) {
        const char* e = strchr(argv[i], '=');
        if (!strcmp(argv[i], "-v"))
            ini_override("log", "2");
        else if (!strncmp(argv[i], "--", 2) && e && e - argv[i] - 2 < 64) {
            char k[64];
            memcpy(k, argv[i] + 2, (size_t)(e - argv[i] - 2));
            k[e - argv[i] - 2] = 0;
            ini_override(k, e + 1);
        }
    }
    char ini[1400];
    snprintf(ini, sizeof ini, "%s%s.ini", g_exedir, g_exename);
    ini_open(ini);
    snprintf(g_datadir, sizeof g_datadir, "%s%s_data/", g_exedir, g_exename);
    hints_init();
    present_init();
    plat_install_crash_handler();
    clock_start();
    LOG("SwitchRecomp - %s\n", g_exename);
    g_title_id = g_mod->title_id;
    mem_init();
    kernel_init();
    build_dispatch();
    load_hints();
    if (loader_load()) fatal("Could not load the game executables from %sexefs/", g_datadir);
    kernel_load_symbols();
    read_npdm();
    services_init();
    romfs_init();
    gpu_init();
    video_init(g_mod->title ? g_mod->title : g_exename);
    if (g_opt_prof) {
#if defined(_WIN32)
        plat_prof_start();
        LOG("profile: active (profile.txt every 10 s)\n");
#else
        LOG("profile: the CPU profiler is Windows only, profile=1 does nothing here\n");
#endif
    }
    kernel_run_main(g_mod->mods[0].base + g_mod->mods[0].text_off, g_mod->main_stack ? g_mod->main_stack : 0x100000,
                    (int)g_mod->main_prio, (int)g_mod->main_core);
    /* 60Hz frontend loop */
    u64 next = plat_ns();
    int tick = 0;
    while (!g_quit) {
        video_poll();
        hid_update();
        vi_vsync_tick();
        if (tick % 60 == 0) ini_poll();
        frontend_frame();
        if (++tick % 300 == 0) save_hints();
        /* dump thread states to threads.txt every 15s, useful when something hangs */
        if (tick % 900 == 0) {
            char p[1200];
            snprintf(p, sizeof p, "%sthreads.txt", g_exedir);
            FILE* f = fopen(p, "w");
            if (f) {
                fprintf(f, "second %d, frames %d\n", tick / 60, g_frames);
                tls_log_redirect = f;
                kernel_dump_threads();
                tls_log_redirect = NULL;
                fclose(f);
            }
        }
        {
            static int last_frames = -1, still = 0, dumps = 0;
            if (g_frames != last_frames) {
                last_frames = g_frames;
                still = 0;
            } else if (++still == 60 * 15 && dumps < 4) {
                dumps++;
                still = 0;
                LOG("warning: %d s without a new image (frames=%d, GPU submits=%llu)\n", 15, g_frames,
                    (unsigned long long)g_gpu_submits);
                kernel_dump_threads();
            }
        }
        next += 16666667ull;
        u64 now = plat_ns();
        if (next > now)
            plat_sleep_ns((i64)(next - now));
        else if (now - next > 100000000ull)
            next = now;
    }
    save_hints();
    LOG("Exit.\n");
    fflush(NULL);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0); /* don't join the guest threads */
#endif
    _exit(0);
}
