/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later (LICENSE). */
#define _POSIX_C_SOURCE 200809L
#include "recompile.h"
#include "loader.h"
#include "module.h"
#include "module_llvm.h"
#include "backend.h"
#include "toolchain.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("Usage: scarletnx -r INPUT [NAME] [OPTIONS]\n"
         "  INPUT                  Game/ExeFS directory or .nro\n"
         "  -o, --name, --out NAME  Output name (default: NPDM or input name)\n"
         "  --outdir DIR           Output directory (default: checkout/games, else ./games)\n"
         "  -O LEVEL               Optimization: 0,1,2,3,s,z (default: 2)\n"
         "  --jobs N               Parallel compiles (default: CPU count)");
    puts("  --backend NAME         Code generator: llvm (LLVM-C, the default and the recommended one),\n"
         "                         gcc (libgccjit, optional: for DEC Alpha EV67 hosts; elsewhere use llvm)");
    printf("  --chunk N              Instructions per code unit, whole functions batched (default: %d)\n",
           SNX_CHUNK_INSTRUCTIONS);
    puts("  --target auto|linux|win64  The host only: modules are built for the machine that builds them\n"
         "  --no-build             Stage data/INI only, generate no code");
    puts("  --interp-only          Generate fallback interpreter only\n"
         "  --runtime FILE         Deploy this runtime instead of building one\n"
         "  --keep-ir              Save the generated code (LLVM IR, libgccjit dumps) in <outdir>/<NAME>_build\n"
         "  -q, --quiet            Suppress progress");
    puts("Hints: <outdir>/<NAME>.hints (hex PCs, one per line).");
}
static unsigned number(const char* text, unsigned max) {
    char* end;
    unsigned long n;
    errno = 0;
    n = strtoul(text, &end, 10);
    return errno || !*text || *end || n == 0 || n > max ? 0 : (unsigned)n;
}
static char* title_name(const char* name, const SnxProgram* p, const char* input) {
    const char *start, *s;
    char *out, *dot;
    size_t n = 0;
    int input_name = 0;
    if (!name && p->npdm.name[0] && strcmp(p->npdm.name, "Application")) name = p->npdm.name;
    if (!name) {
        start = input;
        for (s = input; *s; ++s)
            if (*s == '/' || *s == '\\') start = s + 1;
        name = start;
        input_name = 1;
    }
    out = (char*)snx_alloc(strlen(name) + 1);
    /* Preserve UTF-8 names; exclude path delimiters and C/OS metacharacters. */
    for (s = name; *s; ++s)
        if ((unsigned char)*s >= 128 || isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == ' ' ||
            (input_name && *s == '.'))
            out[n++] = *s;
    out[n] = 0;
    if (input_name && (dot = strrchr(out, '.')) != NULL) *dot = 0;
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n) {
        free(out);
        return snx_strdup("Game");
    }
    return out;
}
/* This build's, not the checkout's cpu.h: the modules are laid out as this build's headers say, and an installed
   build has no checkout */
unsigned snx_game_abi(void) { return snx_module_abi(); }
#ifdef SNX_WITH_RUNTIME
/* In an AppImage this program is a launcher in its mount, which runs nothing once copied out: the AppImage is the
   runtime, and a copy named after the game runs it. APPIMAGE is inherited, so it counts only inside the mount */
static char* own_runtime(const SnxPaths* paths) {
    const char *appimage = getenv("APPIMAGE"), *appdir = getenv("APPDIR");
    if (appimage && *appimage && appdir && *appdir && snx_path_within(paths->executable, appdir))
        return snx_absolute(appimage);
    return snx_strdup(paths->executable);
}
#endif
#ifdef _WIN32
/* Windows finds a program's DLLs next to it: the game gets copies of those next to the runtime (the zip's LLVM),
   or it does not start */
static int copy_dlls(const char* run, const char* out) {
    char **names = NULL, *from = snx_dirname(run), *src, *dst;
    size_t i, count = 0, n;
    int rc = snx_list_dir(from, &names, &count);
    for (i = 0; !rc && i < count; ++i) {
        n = strlen(names[i]);
        if (n < 4 || names[i][n - 4] != '.' || tolower((unsigned char)names[i][n - 3]) != 'd' ||
            tolower((unsigned char)names[i][n - 2]) != 'l' || tolower((unsigned char)names[i][n - 1]) != 'l')
            continue;
        src = snx_path_join(from, names[i]);
        dst = snx_path_join(out, names[i]);
        rc = snx_is_file(src) && snx_copy_file(src, dst);
        free(src);
        free(dst);
    }
    snx_free_strings(names, count);
    free(from);
    return rc;
}
#endif
/* The game's settings: beside it, or in an installed build's config folder (snx_user_path), where the runtime reads
   them. An older build's beside the game stays, a line saying so. NULL when that folder cannot be made */
static char* settings_path(const char* out, const char* title) {
    char *folder = snx_user_path(out, SNX_USER_CONFIG), *base, *path = NULL, *old;
    if (!strcmp(folder, out) || !snx_mkdirs(folder)) {
        base = snx_path_join(folder, title);
        path = snx_format("%s.ini", base);
        free(base);
    }
    if (path && strcmp(folder, out)) {
        base = snx_path_join(out, title);
        old = snx_format("%s.ini", base);
        free(base);
        if (snx_left_behind(old, path))
            fprintf(stderr, "scarletnx: %s was left by an older version: this one keeps it in %s\n", old, path);
        free(old);
    }
    free(folder);
    return path;
}
static SnxU64* read_hints(const char* base, size_t* count) {
    char *path = snx_format("%s.hints", base), line[256], *s;
    FILE* f = snx_fopen(path, "r");
    SnxU64 *hints = NULL, value;
    size_t n = 0;
    int digits, invalid, c;
    free(path);
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            value = 0;
            digits = 0;
            invalid = 0;
            s = line;
            while (isspace((unsigned char)*s)) ++s;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            while (isxdigit((unsigned char)*s)) {
                if (++digits > 16) invalid = 1;
                c = tolower((unsigned char)*s++);
                value = (value << 4) | (SnxU64)(c <= '9' ? c - '0' : c - 'a' + 10);
            }
            while (isspace((unsigned char)*s)) ++s;
            if (!invalid && digits && !*s && !(value & 3)) {
                hints = (SnxU64*)snx_realloc(hints, (n + 1) * sizeof(*hints));
                hints[n++] = value;
            }
        }
        fclose(f);
    }
    *count = n;
    return hints;
}
static int copy_data(const SnxProgram* p, const char* dir, int quiet) {
    char *ex = snx_path_join(dir, "exefs"), *dst, *src, **names;
    size_t n, i;
    int rc = 0;
    if (snx_is_link(ex) || snx_mkdirs(ex)) {
        free(ex);
        return 1;
    }
    /* A cached mount of the other format can take precedence in the runtime. */
    dst = snx_path_join(ex, p->modules[0].is_nro ? "main" : "main.nro");
    if (snx_is_file(dst)) {
        snx_error("cached data has a different format; use another name or remove %s/exefs and its RomFS cache", dir);
        free(dst);
        free(ex);
        return 1;
    }
    free(dst);
    if (!p->modules[0].is_nro) {
        dst = snx_path_join(dir, "romfs.bin");
        if (snx_is_file(dst)) {
            snx_error("cached romfs.bin conflicts with NSO input; remove that cache before recompiling");
            free(dst);
            free(ex);
            return 1;
        }
        free(dst);
    }
    if (p->module_count == 1 && p->modules[0].is_nro) {
        dst = snx_path_join(ex, "main.nro");
        rc = snx_copy_file(p->entry_path, dst);
        free(dst);
        if (!rc) {
            dst = snx_path_join(dir, "romfs.bin");
            if (p->romfs_data)
                rc = snx_write_file(dst, p->romfs_data, p->romfs_size);
            else if (snx_is_file(dst))
                /* An updated NRO without embedded RomFS must not inherit the old image's assets. */
                rc = snx_remove_tree(dst);
            free(dst);
        }
    } else {
        if (snx_list_dir(p->exefs_path, &names, &n)) {
            free(ex);
            return 1;
        }
        for (i = 0; i < n && !rc; ++i) {
            src = snx_path_join(p->exefs_path, names[i]);
            dst = snx_path_join(ex, names[i]);
            if (snx_is_link(src) || snx_is_link(dst))
                rc = 1;
            else if (snx_is_file(src))
                rc = snx_copy_file(src, dst);
            free(src);
            free(dst);
        }
        snx_free_strings(names, n);
        if (!rc && p->romfs_path) {
            dst = snx_path_join(dir, "romfs");
            if (!snx_is_dir(dst)) {
                /* A stopped copy must not look like a complete cache on the next run. */
                char* pending = snx_format("%s.copying", dst);
                if (!quiet) puts("Copying RomFS...");
                rc = ((snx_is_dir(pending) || snx_is_file(pending) || snx_is_link(pending)) &&
                      snx_remove_tree(pending)) ||
                     snx_copy_tree(p->romfs_path, pending) || snx_replace(pending, dst);
                free(pending);
            }
            free(dst);
        }
    }
    if (!rc && !p->npdm_data) {
        dst = snx_path_join(ex, "main.npdm");
        if (snx_is_file(dst)) rc = snx_remove_tree(dst);
        free(dst);
    }
    free(ex);
    return rc;
}
int snx_recompile(int argc, char** argv, const SnxPaths* paths) {
    const char *input = NULL, *name = NULL, *outdir = NULL, *runtime = NULL, *target = "auto", *arg, *value;
    SnxBackend backend = SNX_BACKEND_LLVM;
    SnxLlvmBuild llvm;
    SnxCompileOptions compile;
    SnxCodegenOptions gen;
    SnxProgram program;
    char *title = NULL, *out = NULL, *base = NULL, *data = NULL, *code = NULL;
    char *ini = NULL, *ini_text = NULL, *run = NULL, *exe = NULL, *tmp = NULL;
    char error[1024], option[64];
    const char* equal;
    size_t option_size;
    int i, rc = 1, no_build = 0, keep_c = 0, host_win = 0, positional = 0;
    memset(&compile, 0, sizeof(compile));
    memset(&gen, 0, sizeof(gen));
    memset(&program, 0, sizeof(program));
    memset(&llvm, 0, sizeof(llvm));
    compile.opt = "3";
    compile.jobs = snx_cpu_count();
    gen.chunk_instrs = SNX_CHUNK_INSTRUCTIONS;
#ifdef _WIN32
    host_win = 1;
#endif
    compile.target_win = host_win;
    for (i = 0; i < argc; ++i) {
        arg = argv[i];
        if (positional) {
            if (!input)
                input = arg;
            else if (!name)
                name = arg;
            else
                return snx_error("unexpected argument: %s", arg);
            continue;
        }
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(arg, "--")) {
            positional = 1;
            continue;
        }
        if (!strcmp(arg, "--no-build"))
            no_build = 1;
        else if (!strcmp(arg, "--interp-only"))
            gen.interp_only = 1;
        else if (!strcmp(arg, "--keep-ir") || !strcmp(arg, "--keep-c"))
            keep_c = 1;
        else if (!strcmp(arg, "--quiet") || !strcmp(arg, "-q"))
            compile.quiet = gen.quiet = 1;
        else if (arg[0] == '-') {
            value = NULL;
            if (arg[1] == 'O' && arg[2]) {
                value = arg + 2;
                arg = "-O";
            } else if ((equal = strchr(arg, '=')) != NULL) {
                option_size = (size_t)(equal - arg);
                if (option_size >= sizeof(option)) return snx_error("unknown recompiler option: %s", arg);
                memcpy(option, arg, option_size);
                option[option_size] = 0;
                value = equal + 1;
                arg = option;
            }
            if (strcmp(arg, "-o") && strcmp(arg, "--name") && strcmp(arg, "--out") && strcmp(arg, "--outname") &&
                strcmp(arg, "--outdir") && strcmp(arg, "--runtime") && strcmp(arg, "--target") && strcmp(arg, "-O") &&
                strcmp(arg, "--jobs") && strcmp(arg, "--chunk") && strcmp(arg, "--backend"))
                return snx_error("unknown recompiler option: %s", arg);
            if (!value) {
                if (++i == argc) return snx_error("%s requires a value", arg);
                value = argv[i];
            }
            if (!*value) return snx_error("%s requires a nonempty value", arg);
            if (!strcmp(arg, "-o") || !strcmp(arg, "--name") || !strcmp(arg, "--out") || !strcmp(arg, "--outname"))
                name = value;
            else if (!strcmp(arg, "--outdir"))
                outdir = value;
            else if (!strcmp(arg, "--runtime"))
                runtime = value;
            else if (!strcmp(arg, "--target"))
                target = value;
            else if (!strcmp(arg, "--backend")) {
                if (!strcmp(value, "portable"))
                    return snx_error("the portable backend was removed: --backend=llvm (the default) or gcc");
                if (snx_backend_parse(value, &backend)) return snx_error("--backend expects llvm or gcc");
            } else if (!strcmp(arg, "-O")) {
                if (strlen(value) != 1 || !strchr("0123sz", value[0])) return snx_error("-O expects 0,1,2,3,s,z");
                compile.opt = value;
            } else if (!strcmp(arg, "--jobs")) {
                compile.jobs = number(value, 1024);
                if (!compile.jobs) return snx_error("--jobs expects 1..1024");
            } else {
                gen.chunk_instrs = number(value, INT_MAX);
                if (!gen.chunk_instrs) return snx_error("--chunk expects a positive instruction count");
            }
        } else if (!input)
            input = arg;
        else if (!name)
            name = arg;
        else
            return snx_error("unexpected argument: %s", arg);
    }
    if (!input) {
        usage();
        return 2;
    }
    if (!strcmp(target, "win64"))
        compile.target_win = 1;
    else if (!strcmp(target, "linux"))
        compile.target_win = 0;
    else if (strcmp(target, "auto"))
        return snx_error("--target expects auto, linux or win64");
    /* The backends write the module for the machine they run on (build == host == target) */
    if (compile.target_win != host_win) return snx_error("--target: modules are built for this host only");
    gen.abi = snx_game_abi();
    if (snx_load_program(input, &program, error, sizeof(error))) return snx_error("%s", error);
    if (!program.npdm.is64) {
        snx_error("AArch32 programs are unsupported");
        goto done;
    }
    title = title_name(name, &program, program.entry_path);
    out = outdir ? snx_absolute(outdir) : snx_path_join(paths->repo, "games");
    if (!out) {
        snx_error("cannot resolve output directory");
        goto done;
    }
    if ((program.romfs_path && snx_path_within(out, program.romfs_path)) ||
        (program.exefs_path && snx_path_within(out, program.exefs_path))) {
        snx_error("output directory must be outside the input ExeFS/RomFS");
        goto done;
    }
    if (snx_mkdirs(out)) {
        snx_error("cannot create output directory");
        goto done;
    }
    base = snx_path_join(out, title);
    /* Invalidate the completion stamp before touching an existing game's data
       or module. Failed recompilation must never leave a trusted checkpoint. */
    if (!no_build) {
        char* stamp = snx_format("%s.abi", base);
        int failed = snx_is_file(stamp) && snx_remove_tree(stamp);
        free(stamp);
        if (failed) goto done;
    }
    data = snx_format("%s_data", base);
    if (snx_is_link(data)) {
        snx_error("invalid output directory");
        goto done;
    }
    gen.title = title;
    gen.hints = read_hints(base, &gen.hint_count);
    if (!compile.quiet) printf("Recompiling %s (%lu modules)\n", title, (unsigned long)program.module_count);
    if (copy_data(&program, data, compile.quiet)) {
        snx_error("cannot stage game data in %s", data);
        goto done;
    }
    ini = settings_path(out, title);
    if (!ini) {
        snx_error("cannot create the settings folder for %s", title);
        goto done;
    }
    if (!snx_is_file(ini)) {
        ini_text = snx_format(
            "; ScarletNX settings for %s\n; Full option help is written on first run.\n[Display]\nlanguage=1\n", title);
        if (snx_write_file(ini, ini_text, strlen(ini_text))) {
            snx_error("cannot write %s", ini);
            goto done;
        }
    }
    if (no_build) {
        rc = 0;
        goto done;
    }
    code = snx_format("%s_code.%s", base, compile.target_win ? "dll" : "so");
    {
        char* kept = keep_c ? snx_format("%s_build", base) : NULL;
        llvm.output = code;
        llvm.opt = compile.opt[0];
        llvm.jobs = compile.jobs;
        llvm.quiet = compile.quiet;
        llvm.keep_ir = kept;
        i = kept && (snx_is_link(kept) || snx_mkdirs(kept));
        if (i) snx_error("cannot save the generated code in %s", kept);
        if (!i && backend == SNX_BACKEND_LLVM) i = snx_build_module_llvm(&program, &gen, &llvm);
        if (!i && backend == SNX_BACKEND_GCC) i = snx_build_module_gccjit(&program, &gen, &llvm);
        free(kept);
        if (i) goto done;
    }
    if (runtime) run = snx_absolute(runtime);
#ifdef SNX_WITH_RUNTIME
    else if (host_win == compile.target_win)
        run = own_runtime(paths);
#endif
    else
        run = snx_build_runtime(paths, compile.target_win, compile.quiet);
    if (!run || !snx_is_file(run)) {
        snx_error("runtime unavailable; specify --runtime FILE");
        goto done;
    }
#ifdef _WIN32
    if (copy_dlls(run, out)) {
        snx_error("cannot copy the runtime's DLLs to %s", out);
        goto done;
    }
#endif
    /* Deploy the executable last: automatic play uses it as the completion checkpoint. */
    exe = snx_format("%s%s", base, compile.target_win ? ".exe" : "");
    tmp = snx_format("%s.tmp", exe);
#ifdef __ANDROID__
    /* the shared storage runs nothing (the game runs on the app's own runtime), and before Android 11 it refuses a
       mode change: there the copy only marks the recompile done */
    i = snx_copy_file(run, tmp) || snx_replace(tmp, exe);
#else
    i = snx_copy_file(run, tmp) || snx_make_executable(tmp) || snx_replace(tmp, exe);
#endif
    if (i) {
        snx_error("cannot deploy runtime to %s", exe);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
        goto done;
    }
    {
        char* stamp = snx_format("%s.abi", base);
        char version[32];
        sprintf(version, "%u\n", gen.abi);
        i = snx_write_file(stamp, version, strlen(version));
        free(stamp);
        if (i) {
            snx_error("cannot record the compiled game's ABI");
            goto done;
        }
    }
    if (!compile.quiet) printf("Built %s\n", exe);
    rc = 0;
done:
    snx_free_program(&program);
    free((void*)gen.hints);
    free(title);
    free(out);
    free(base);
    free(data);
    free(code);
    free(ini);
    free(ini_text);
    free(run);
    free(exe);
    free(tmp);
    return rc;
}
