/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
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
#elif defined(__ANDROID__)
    snprintf(p, sizeof p, "lib%s_code.so", g_exename); /* lives in the apk's lib dir */
#else
    snprintf(p, sizeof p, "%s%s_code.so", g_exedir, g_exename);
#endif
    void* h = plat_dl_open(p);
    if (!h) {
#if !defined(_WIN32)
        WARN("dlopen: %s\n", dlerror());
#endif
        WARN("Cannot find the recompiled code: %s\n", p);
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
    return 0;
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
    char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\'), *s = s1 > s2 ? s1 : s2;
    const char* base = s ? s + 1 : p;
    strncpy(g_exename, base, 255);
    char* dot = strrchr(g_exename, '.');
    if (dot && (!strcmp(dot, ".exe") || !strcmp(dot, ".EXE") || !strcmp(dot, ".AppImage"))) *dot = 0;
    if (s) {
        s[1] = 0;
        strncpy(g_exedir, p, 1023);
    } else
        g_exedir[0] = 0;
}

int swr_main(int argc, char** argv) {
    /* In an AppImage argv[0] points into its mount (APPDIR). APPIMAGE is the file that was run, so a copy named after
       the game (games/Catherine or games/Catherine.AppImage) finds games/Catherine_code.so like the bare runtime does.
       Both variables are inherited, so they only count when this runtime is the one inside the mount */
    const char *appimage = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    int in_appimage =
        appimage && *appimage && appdir && *appdir && argv[0] && !strncmp(argv[0], appdir, strlen(appdir));
    split_path(in_appimage ? appimage : argv[0]);
    char ini[1400];
    snprintf(ini, sizeof ini, "%s%s.ini", g_exedir, g_exename);
    ini_open(ini);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v"))
            g_verbose = 2;
        else if (!strncmp(argv[i], "--", 2) && strchr(argv[i], '=')) {
            char k[256];
            strncpy(k, argv[i] + 2, 255);
            char* e = strchr(k, '=');
            *e = 0;
            backend_ini(k, e + 1);
            if (!strcmp(k, "docked")) g_opt_docked = atoi(e + 1);
            if (!strcmp(k, "fps_limit")) g_opt_fps = atoi(e + 1);
        }
    }
    snprintf(g_datadir, sizeof g_datadir, "%s%s_data/", g_exedir, g_exename);
    hints_init();
    present_init();
    plat_install_crash_handler();
    clock_start();
    LOG("SwitchRecomp - %s\n", g_exename);
    if (load_code()) fatal("Could not load the game's recompiled code.");
    if (g_mod->abi != SWR_ABI) fatal("The recompiled code is from another SwitchRecomp version.");
    g_title_id = g_mod->title_id;
    mem_init();
    kernel_init();
    build_dispatch();
    if (g_mod->init) g_mod->init(&g_rtapi);
    load_hints();
    if (loader_load()) fatal("Could not load the game executables from %sexefs/", g_datadir);
    { /* npdm SystemResourceSize; newer SDKs only init their VM allocator when > 0 */
        char q[1200];
        snprintf(q, sizeof q, "%sexefs/main.npdm", g_datadir);
        FILE* f = fopen(q, "rb");
        if (f) {
            u8 h[0x20];
            if (fread(h, 1, 0x20, f) == 0x20 && !memcmp(h, "META", 4)) {
                u32 v;
                memcpy(&v, h + 0x14, 4);
                g_sysres_size = v;
            }
            fclose(f);
        }
        LOG("npdm: system resource %.1f MB\n", g_sysres_size / 1048576.0);
    }
    services_init();
    romfs_init();
    gpu_init();
    video_init(g_mod->title ? g_mod->title : g_exename);
    {
        if (g_opt_prof) {
            plat_prof_start(0);
            LOG("profile: active (profile.txt every 10 s)\n");
        }
    }
    kernel_run_main(g_mod->mods[0].base + g_mod->mods[0].text_off, g_mod->main_stack ? g_mod->main_stack : 0x100000,
                    (int)g_mod->main_prio, (int)g_mod->main_core);
    /* 60Hz frontend loop */
    u64 next = plat_ns();
    int tick = 0;
    while (!g_quit) {
        video_poll();
        hid_update();
        vsync_tick_vt();
        if (tick % 60 == 0) ini_poll();
        { kernel_heal_mutexes(); }
        /* bail out of the slow clock if nothing was presented for 3s */
        {
            static int lf = -1, still;
            if (g_vt_on) {
                if (g_frames != lf) {
                    lf = g_frames;
                    still = 0;
                } else if (++still == 180) {
                    vt_set(0);
                    WARN(
                        "smooth recording: the game stopped presenting images; back to the normal clock (recording continues)\n");
                }
            } else {
                lf = -1;
                still = 0;
            }
        }
        frontend_frame();
        if (++tick % 300 == 0) save_hints();
        /* dump thread states to threads.txt every 15s, useful when something hangs */
        if (tick % 900 == 0) {
            char p[1200];
            snprintf(p, sizeof p, "%sthreads.txt", g_exedir);
            FILE* f = fopen(p, "w");
            if (f) {
                fprintf(f, "second %d, frames %d\n", tick / 60, g_frames);
                g_log_redirect = f;
                kernel_dump_threads();
                g_log_redirect = NULL;
                fclose(f);
            }
        }
        /* auto diag only with diag=1, F5 always works */
        {
            if (g_opt_diag && (tick == 60 * 20 || tick == 60 * 40)) { g_renderer->dump_rts(); }
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
    { g_renderer->rec_finish(); }
    LOG("Exit.\n");
    fflush(NULL);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0); /* don't join the guest threads */
#endif
    _exit(0);
}
