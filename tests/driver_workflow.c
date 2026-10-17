/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real driver/supervisor with synthetic extraction, compilation and game children. */
#define _POSIX_C_SOURCE 200809L
#include "common/utilities/common.h"
#include "nx/runtime/driver/driver.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/runtime/driver/recompile.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static unsigned checks;
static void check(int good, const char* expression, int line) {
    ++checks;
    if (!good) {
        fprintf(stderr, "driver_workflow.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}
static int event(char stage) {
    FILE* log = snx_fopen(getenv("SNX_WORKFLOW_LOG"), "a");
    const char *fail = getenv("SNX_WORKFLOW_FAIL"), *mode = getenv("SNX_WORKFLOW_MODE");
    CHECK(log != NULL);
    CHECK(fputc(stage, log) != EOF);
    CHECK(!fclose(log));
    if (!fail || fail[0] != stage || !strcmp(mode, "missing")) return 0;
#ifndef _WIN32
    if (!strcmp(mode, "interrupt")) raise(SIGINT);
    if (!strcmp(mode, "terminate")) raise(SIGTERM);
    if (!strcmp(mode, "stop")) raise(SIGSTOP);
    if (!strcmp(mode, "parent")) {
        int ready[2];
        char byte;
        pid_t worker;
        CHECK(!pipe(ready));
        worker = fork();
        CHECK(worker >= 0);
        if (!worker) {
            close(ready[0]);
            signal(SIGTERM, SIG_IGN);
            CHECK(write(ready[1], "x", 1) == 1);
            close(ready[1]);
            sleep(2);
            /* A surviving compiler grandchild would keep the capture pipe open and log X. */
            event('X');
            _exit(0);
        }
        close(ready[1]);
        CHECK(read(ready[0], &byte, 1) == 1);
        close(ready[0]);
        CHECK(!kill(getppid(), SIGTERM));
        for (;;) pause();
    }
#else
    (void)mode;
#endif
    return 7;
}
int snx_extract(int argc, char** argv, const SnxPaths* paths) {
    char* partial;
    int rc;
    (void)paths;
    CHECK(argc == 4 && !strcmp(argv[0], "--out") && !strcmp(argv[2], "--"));
    CHECK(snx_is_file(argv[3]));
    CHECK(!snx_is_dir(argv[1]));
    CHECK(!snx_mkdirs(argv[1]));
    partial = snx_path_join(argv[1], "partial");
    CHECK(!snx_write_file(partial, "partial", 7));
    free(partial);
    /* an ExeFS, with the NPDM SNX_WORKFLOW_NPDM names: "id" one with a title ID, "bad" a broken one */
    partial = snx_path_join(argv[1], "exefs");
    CHECK(!snx_mkdirs(partial));
    {
        char *main = snx_path_join(partial, "main"), *npdm = snx_path_join(partial, "main.npdm");
        const char* kind = getenv("SNX_WORKFLOW_NPDM");
        unsigned char meta[0xa0];
        SnxU64 id = ((SnxU64)0x0100ABCDUL << 32) | 0x12345000UL;
        int i;
        memset(meta, 0, sizeof meta);
        memcpy(meta, "META", 4);
        meta[12] = 1;
        meta[0x70] = 0x80;                                   /* the ACI0 at 0x80 */
        meta[0x74] = 0x20;                                   /* its size */
        if (kind && !strcmp(kind, "bad")) meta[0x75] = 0x10; /* past the file's end */
        memcpy(meta + 0x80, "ACI0", 4);
        for (i = 0; i < 8; ++i) meta[0x90 + i] = (unsigned char)(id >> (8 * i));
        CHECK(!snx_write_file(main, "main", 4));
        if (kind && *kind) CHECK(!snx_write_file(npdm, meta, sizeof meta));
        free(main);
        free(npdm);
    }
    free(partial);
    rc = event('E');
    if (!strcmp(getenv("SNX_WORKFLOW_MODE"), "missing") && getenv("SNX_WORKFLOW_FAIL") &&
        !strcmp(getenv("SNX_WORKFLOW_FAIL"), "E"))
        CHECK(!snx_remove_tree(argv[1]));
    return rc;
}
/* this build's stamp: ABI 4, made from commit abc1234 */
#define SNX_WORKFLOW_STAMP "4\nabc1234\n"
char* snx_game_stamp(void) { return snx_strdup(SNX_WORKFLOW_STAMP); }
int snx_recompile(int argc, char** argv, const SnxPaths* paths) {
    char *game, *partial, *name, *folder;
    int rc;
    if (getenv("SNX_WORKFLOW_BACKEND") && *getenv("SNX_WORKFLOW_BACKEND")) {
        CHECK((argc == 6 || argc == 7) && !strcmp(argv[2], "--backend") &&
              !strcmp(argv[3], getenv("SNX_WORKFLOW_BACKEND")));
        /* Normalize for the shared synthetic compilation assertions below. */
        argv[2] = argv[4];
        argv[3] = argv[5];
        if (argc == 7) argv[4] = argv[6];
        argc -= 2;
    }
    /* a dump's: no name given, the recompile names the game; a game made already: its folder's name */
    CHECK((argc == 4 || argc == 5) && !strcmp(argv[0], "--outdir") && !strcmp(argv[2], "--"));
    if (getenv("SNX_WORKFLOW_SOURCE") && *getenv("SNX_WORKFLOW_SOURCE")) {
        char *given = snx_absolute(argv[3]), *expected = snx_absolute(getenv("SNX_WORKFLOW_SOURCE"));
        const char* named = getenv("SNX_WORKFLOW_NAMED"); /* the name a game made already is given, if any */
        CHECK(named && *named ? argc == 5 && !strcmp(argv[4], named) : argc == 4);
        CHECK(given && expected && !strcmp(given, expected));
        free(given);
        free(expected);
    } else if (getenv("SNX_WORKFLOW_NRO") && *getenv("SNX_WORKFLOW_NRO")) {
        char *given, *expected;
        CHECK(snx_is_file(argv[3]));
        /* The driver hands the stage a resolved path (Windows: backslashes and long
           names), so compare canonical spellings like the game's cwd check does. */
        given = snx_absolute(argv[3]);
        expected = snx_absolute(getenv("SNX_WORKFLOW_NRO"));
        CHECK(given && expected && !strcmp(given, expected));
        free(given);
        free(expected);
    } else
        CHECK(snx_is_dir(argv[3]));
    CHECK(!snx_mkdirs(argv[1]));
    partial = snx_path_join(argv[1], "partial-build");
    CHECK(!snx_write_file(partial, "partial", 7));
    free(partial);
    rc = event('R');
    if (rc || (!strcmp(getenv("SNX_WORKFLOW_MODE"), "missing") && getenv("SNX_WORKFLOW_FAIL") &&
               !strcmp(getenv("SNX_WORKFLOW_FAIL"), "R")))
        return rc;
    if (argc == 5)
        name = snx_strdup(argv[4]);
    else {
        SnxNpdm npdm;
        char error[256];
        CHECK(!snx_load_npdm(argv[3], &npdm, error, sizeof error));
        name = snx_game_name(NULL, &npdm, argv[3]);
    }
    /* the game in a folder of its own, with its data, as the recompile makes it */
    folder = snx_path_join(argv[1], name);
    CHECK(!snx_mkdirs(folder));
    partial = snx_format("%s/%s_data/exefs", folder, name);
    CHECK(!snx_mkdirs(partial));
    { /* the extraction's ExeFS in it, as the recompile stages it */
        static const char* const files[] = {"main", "main.npdm"};
        size_t k;
        for (k = 0; k < 2; ++k) {
            char *from = snx_format("%s/exefs/%s", argv[3], files[k]), *to = snx_path_join(partial, files[k]);
            if (snx_is_file(from) && strcmp(from, to)) CHECK(!snx_copy_file(from, to));
            free(from);
            free(to);
        }
    }
    free(partial);
#ifdef _WIN32
    game = snx_format("%s/%s.exe", folder, name);
#else
    game = snx_path_join(folder, name);
#endif
    CHECK(!snx_copy_file(paths->executable, game));
    CHECK(!snx_make_executable(game));
    {
        char* stamp = snx_format("%s/%s.abi", folder, name);
        char* code = snx_format("%s/%s_code.%s", folder, name,
#ifdef _WIN32
                                "dll"
#else
                                "so"
#endif
        );
        CHECK(!snx_write_file(stamp, SNX_WORKFLOW_STAMP, strlen(SNX_WORKFLOW_STAMP)));
        CHECK(!snx_write_file(code, "synthetic", 9));
        free(stamp);
        free(code);
    }
    free(game);
    free(folder);
    free(name);
    return 0;
}
static void run(const char* exe, const char* repo, const char* dump, const char* log, int expected, const char* events,
                const char* diagnostic) {
    const char* args[24];
    const char *headless = getenv("SNX_WORKFLOW_HEADLESS"), *flags = getenv("SNX_WORKFLOW_FLAGS");
    const char *outdir = getenv("SNX_WORKFLOW_OUTDIR"), *out = getenv("SNX_WORKFLOW_OUT");
    int n = 5;
    char* output = NULL;
    unsigned char* recorded;
    size_t size;
    int rc;
    CHECK(!snx_write_file(log, "", 0));
    args[0] = exe;
    args[1] = "--repo";
    args[2] = repo;
    args[3] = "--build-dir";
    args[4] = repo;
    if (flags && *flags && strncmp(flags, "--backend=", 10)) args[n++] = flags;
    if (headless && *headless && !strcmp(getenv("SNX_WORKFLOW_PREFIX"), "1")) args[n++] = headless;
    args[n++] = dump;
    if (flags && !strncmp(flags, "--backend=", 10)) args[n++] = flags;
    if (headless && *headless && strcmp(getenv("SNX_WORKFLOW_PREFIX"), "1")) args[n++] = headless;
    if (outdir && *outdir) { /* "none": the option without its folder */
        args[n++] = "--outdir";
        if (strcmp(outdir, "none")) args[n++] = outdir;
    }
    if (out && *out) {
        args[n++] = "--out";
        args[n++] = out;
    }
    if (getenv("SNX_WORKFLOW_ARGS") && *getenv("SNX_WORKFLOW_ARGS")) {
        args[n++] = "--";
        args[n++] = "some arg";
        args[n++] = "";
        args[n++] = "-rx";
        args[n++] = "--headless=0";
    }
    args[n] = NULL;
    rc = snx_run(args, NULL, NULL, &output);
    if (rc != expected) fprintf(stderr, "expected %d, got %d: %s\n", expected, rc, output);
    CHECK(rc == expected);
    if (diagnostic) CHECK(strstr(output, diagnostic) != NULL);
    recorded = snx_read_file(log, &size);
    CHECK(recorded && !strcmp((const char*)recorded, events));
    free(recorded);
    free(output);
}
/* the runtime's settings (runtime/options.c): those this test gives */
int snx_setting_known(const char* key) {
    static const char* const known[] = {"touch", "controls", "resolution", "controls_a", "seconds"};
    size_t i;
    for (i = 0; i < sizeof known / sizeof *known; ++i)
        if (!strcmp(key, known[i])) return 1;
    return 0;
}
static void reset(const char* repo, const char* dump) {
    CHECK(!snx_remove_tree(repo));
    CHECK(!snx_mkdirs(repo));
    CHECK(!snx_write_file(dump, "synthetic", 9));
    set_env("SNX_WORKFLOW_FAIL", "");
}
int main(int argc, char** argv) {
    char *exe, *root, *repo, *dump, *log, *extracted, *game, *cwd, *expected_cwd;
    const char* args[3];
    char* output;
    int rc;
#ifndef _WIN32
    const char* modes[] = {"interrupt", "terminate", "stop", "parent"};
    int statuses[] = {128 + SIGINT, 128 + SIGTERM, 128 + SIGSTOP, 128 + SIGTERM};
    size_t i;
#endif
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        args[0] = argv[0];
        args[1] = NULL;
        if (!strcmp(argv[2], "exec")) args[0] = "/nonexistent/scarletnx-test";
        return snx_run_stage(args, !strcmp(argv[2], "cwd") ? "/nonexistent/scarletnx-test" : NULL, "Probe");
    }
    if (argc > 1) {
        rc = snx_driver(argc, argv);
        if (rc >= 0) return rc;
    }
    if (getenv("SNX_WORKFLOW_CHILD")) {
        const char* flag = getenv("SNX_WORKFLOW_HEADLESS");
        const char* fullscreen = getenv("SNX_WORKFLOW_FLAGS");
        if (fullscreen &&
            (!strcmp(fullscreen, "--fullscreen") || !strcmp(fullscreen, "--fullscreen=1") || !strcmp(fullscreen, "-f")))
            CHECK(argc == 2 && !strcmp(argv[1], "-f"));
        if (fullscreen && !strcmp(fullscreen, "--fullscreen=0")) CHECK(argc == 2 && !strcmp(argv[1], "--fullscreen=0"));
        /* the game's own settings, any --key=value of its .ini, reach it as they were given */
        if (fullscreen && strchr(fullscreen, '=') && strncmp(fullscreen, "--fullscreen=", 13) &&
            strncmp(fullscreen, "--backend=", 10))
            CHECK(argc == 2 && !strcmp(argv[1], fullscreen));
        if (getenv("SNX_WORKFLOW_BACKEND") && *getenv("SNX_WORKFLOW_BACKEND")) CHECK(argc == 1);
        if (flag && *flag) CHECK(argc == 2 && !strcmp(argv[1], "-H"));
        if (getenv("SNX_WORKFLOW_ARGS") && *getenv("SNX_WORKFLOW_ARGS")) {
            CHECK(argc == 6 && !strcmp(argv[1], "--"));
            CHECK(!strcmp(argv[2], "some arg") && !strcmp(argv[3], ""));
            CHECK(!strcmp(argv[4], "-rx") && !strcmp(argv[5], "--headless=0"));
        }
        /* All game invocations must chdir, including repeat launches. */
        cwd = snx_absolute(".");
        expected_cwd = snx_absolute(getenv("SNX_WORKFLOW_GAMES"));
        CHECK(cwd && expected_cwd && !strcmp(cwd, expected_cwd));
        free(cwd);
        free(expected_cwd);
        return event('G');
    }
    exe = snx_executable(argv[0]);
    root = snx_temp_dir(NULL);
    CHECK(exe && root);
    repo = snx_path_join(root, "checkout with spaces");
    CHECK(!snx_mkdirs(repo));
    dump = snx_path_join(repo, "Test [game];.NSP");
    log = snx_path_join(root, "events");
    extracted = snx_path_join(repo, "work/Test game");
#ifdef _WIN32
    game = snx_path_join(repo, "games/Test game/Test game.exe");
#else
    game = snx_path_join(repo, "games/Test game/Test game");
#endif
    cwd = snx_path_join(repo, "games/Test game"); /* the game runs in its folder */
    set_env("SNX_WORKFLOW_GAMES", cwd);
    free(cwd);
    set_env("SNX_WORKFLOW_CHILD", "1");
    set_env("SNX_WORKFLOW_LOG", log);
    set_env("SNX_WORKFLOW_MODE", "exit");
    set_env("SNX_WORKFLOW_HEADLESS", "");
    set_env("SNX_WORKFLOW_PREFIX", "0");
    set_env("SNX_WORKFLOW_NRO", "");
    set_env("SNX_WORKFLOW_SOURCE", "");
    set_env("SNX_WORKFLOW_NAMED", "");
    set_env("SNX_WORKFLOW_OUTDIR", "");
    set_env("SNX_WORKFLOW_OUT", "");
    reset(repo, dump);
    run(exe, repo, dump, log, 0, "ERG", NULL);
    CHECK(snx_is_dir(extracted) && snx_is_file(game));
    run(exe, repo, dump, log, 0, "G", NULL);
    CHECK(!snx_remove_tree(game));
    run(exe, repo, dump, log, 0, "RG", NULL);
    set_env("SNX_WORKFLOW_FAIL", "G");
    run(exe, repo, dump, log, 7, "G", "Game failed (exit status 7)");
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "G", NULL);

    /* Missing/mismatched/malformed ABI stamps are never trusted, nor one another build wrote with the same ABI: an
       older version's ABI alone, another commit's, or this one's cut short or with more after it */
    {
        const char* const stale[] = {
            "3\nabc1234\n",      "broken", "4", "4garbage\n", "4\n", "4\ndef5678\n", "4\nabc1234", "4\nabc1234\n\n",
            "4\nabc1234-dirty\n"};
        char* stamp = snx_path_join(repo, "games/Test game/Test game.abi");
        size_t j;
        for (j = 0; j < sizeof stale / sizeof *stale; ++j) {
            CHECK(!snx_write_file(stamp, stale[j], strlen(stale[j])));
            run(exe, repo, dump, log, 0, "RG", NULL);
            run(exe, repo, dump, log, 0, "G", NULL); /* the recompile stamped it as this build */
        }
        CHECK(!snx_remove_tree(stamp));
        run(exe, repo, dump, log, 0, "RG", NULL);
        free(stamp);
    }
    {
        const char* flags[] = {"--fullscreen", "-f"};
        size_t j;
        for (j = 0; j < sizeof(flags) / sizeof(flags[0]); ++j) {
            set_env("SNX_WORKFLOW_FLAGS", flags[j]);
            run(exe, repo, dump, log, 0, "G", NULL);
        }
        set_env("SNX_WORKFLOW_FLAGS", "--touch=2");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--controls=1");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--resolution=2");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--seconds=5");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--resoltion=2");
        run(exe, repo, dump, log, 1, "", "unknown option: --resoltion=2");
        set_env("SNX_WORKFLOW_FLAGS", "--controls_a=0.5 0.5 100 1");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=invalid");
        run(exe, repo, dump, log, 1, "", "unknown option");
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=0");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=1");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--backend=gcc");
        set_env("SNX_WORKFLOW_BACKEND", "gcc");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--backend=llvm");
        set_env("SNX_WORKFLOW_BACKEND", "llvm");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_BACKEND", "");
        set_env("SNX_WORKFLOW_FLAGS", "--backend=invalid");
        run(exe, repo, dump, log, 1, "", "unknown backend");
        set_env("SNX_WORKFLOW_FLAGS", "");
    }
    set_env("SNX_WORKFLOW_ARGS", "1");
    run(exe, repo, dump, log, 0, "G", NULL);
    set_env("SNX_WORKFLOW_ARGS", "");
    {
        const char* flags[] = {
            "-r",
            "--recomp",
#ifndef _WIN32
            "-rx",
            "-xr",
#endif
        };
        size_t j;
        for (j = 0; j < sizeof(flags) / sizeof(flags[0]); ++j) {
            set_env("SNX_WORKFLOW_FLAGS", flags[j]);
            reset(repo, dump);
            run(exe, repo, dump, log, 0, "ER", NULL);
            run(exe, repo, dump, log, 0, "R", NULL);
        }
        set_env("SNX_WORKFLOW_FLAGS", "");
    }
    reset(repo, dump);
    set_env("SNX_WORKFLOW_FAIL", "E");
    run(exe, repo, dump, log, 7, "E", "Extraction failed");
    CHECK(!snx_is_dir(extracted) && !snx_is_file(game));
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "ERG", NULL);

    reset(repo, dump);
    set_env("SNX_WORKFLOW_FAIL", "R");
    run(exe, repo, dump, log, 7, "ER", "Recompilation failed");
    CHECK(snx_is_dir(extracted) && !snx_is_file(game));
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "RG", NULL);
#ifndef _WIN32
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        reset(repo, dump);
        set_env("SNX_WORKFLOW_MODE", modes[i]);
        set_env("SNX_WORKFLOW_FAIL", "E");
        run(exe, repo, dump, log, statuses[i], "E", "Extraction interrupted by signal");
        CHECK(!snx_is_dir(extracted) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_FAIL", "R");
        run(exe, repo, dump, log, statuses[i], "ER", "Recompilation interrupted by signal");
        CHECK(snx_is_dir(extracted) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_FAIL", "");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FAIL", "G");
        run(exe, repo, dump, log, statuses[i], "G", "Game interrupted by signal");
        set_env("SNX_WORKFLOW_FAIL", "");
        run(exe, repo, dump, log, 0, "G", NULL);
    }
#endif
    reset(repo, dump);
    set_env("SNX_WORKFLOW_MODE", "missing");
    set_env("SNX_WORKFLOW_FAIL", "E");
    run(exe, repo, dump, log, 1, "E", "Extraction finished without creating");
    set_env("SNX_WORKFLOW_FAIL", "R");
    run(exe, repo, dump, log, 1, "ER", "Recompilation finished without creating");
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "RG", NULL);
    run(exe, repo, "/nonexistent/dump.xci", log, 1, "", "Input dump is missing");
    {
        const char* flags[] = {"--headless", "-H"};
        unsigned j;
        for (j = 0; j < 2; ++j) {
            set_env("SNX_WORKFLOW_HEADLESS", flags[j]);
            set_env("SNX_WORKFLOW_PREFIX", "0");
            run(exe, repo, dump, log, 0, "G", NULL);
            set_env("SNX_WORKFLOW_PREFIX", "1");
            reset(repo, dump);
            run(exe, repo, dump, log, 0, "ERG", NULL);
        }
        set_env("SNX_WORKFLOW_HEADLESS", "");
    }
    /* Decrypted containers share the extract/recompile checkpoints, including quoted dump paths. */
    {
        const char* names[] = {"My Switch DXCI's/Test [game];.dnsp", "My Switch DXCI's/Test [game];.dNsP",
                               "My Switch DXCI's/Test [game];.dxci", "My Switch DXCI's/Test [game];.dXcI"};
        unsigned j;
        for (j = 0; j < sizeof names / sizeof names[0]; ++j) {
            char *decrypted = snx_path_join(repo, names[j]), *directory = snx_path_join(repo, "My Switch DXCI's");
            CHECK(!snx_remove_tree(repo) && !snx_mkdirs(directory));
            CHECK(!snx_write_file(decrypted, "synthetic", 9));
            set_env("SNX_WORKFLOW_MODE", "exit");
            set_env("SNX_WORKFLOW_FAIL", "");
            run(exe, repo, decrypted, log, 0, "ERG", NULL);
            CHECK(snx_is_dir(extracted) && snx_is_file(game));
            run(exe, repo, decrypted, log, 0, "G", NULL);
            CHECK(!snx_remove_tree(game));
            run(exe, repo, decrypted, log, 0, "RG", NULL);
            /* -r prepares a decrypted dump too, by its extension */
            CHECK(!snx_remove_tree(extracted) && !snx_remove_tree(game));
            set_env("SNX_WORKFLOW_FLAGS", "-r");
            run(exe, repo, decrypted, log, 0, "ER", NULL);
            set_env("SNX_WORKFLOW_FLAGS", "");
            free(decrypted);
            free(directory);
        }
    }
    /* a game with a title ID of its own is named by it, whatever its dump is called; a broken NPDM stops the run */
    {
#ifdef _WIN32
        char* named = snx_path_join(repo, "games/0100ABCD12345000/0100ABCD12345000.exe");
#else
        char* named = snx_path_join(repo, "games/0100ABCD12345000/0100ABCD12345000");
#endif
        char* folder = snx_path_join(repo, "games/0100ABCD12345000");
        reset(repo, dump);
        set_env("SNX_WORKFLOW_MODE", "exit");
        set_env("SNX_WORKFLOW_NPDM", "id");
        set_env("SNX_WORKFLOW_GAMES", folder);
        run(exe, repo, dump, log, 0, "ERG", NULL);
        CHECK(snx_is_file(named) && !snx_is_file(game));
        run(exe, repo, dump, log, 0, "G", NULL);
        { /* made already, recompiled where it is with no name given: shown again by its control.nacp, not its ID */
            char *stamp = snx_format("%s/0100ABCD12345000.abi", folder),
                 *data = snx_format("%s/0100ABCD12345000_data", folder);
            CHECK(!snx_write_file(stamp, "4\n", 2));
            set_env("SNX_WORKFLOW_SOURCE", data);
            run(exe, repo, named, log, 0, "RG", NULL);
            set_env("SNX_WORKFLOW_SOURCE", "");
            free(data);
            free(stamp);
        }
        reset(repo, dump);
        set_env("SNX_WORKFLOW_NPDM", "bad");
        run(exe, repo, dump, log, 1, "E", "NPDM: access-control region exceeds file");
        CHECK(!snx_is_file(named) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_NPDM", "");
        free(folder);
        folder = snx_path_join(repo, "games/Test game");
        set_env("SNX_WORKFLOW_GAMES", folder);
        free(folder);
        free(named);
    }
    /* NROs bypass extraction even with a stale same-stem work directory or game executable. */
    {
        const char* names[] = {"Test [game];.nro", "Test [game];.NRO"};
        unsigned j;
        for (j = 0; j < 2; ++j) {
            char* nro = snx_path_join(repo, names[j]);
            reset(repo, nro);
            set_env("SNX_WORKFLOW_MODE", "exit");
            set_env("SNX_WORKFLOW_NRO", nro);
            set_env("SNX_WORKFLOW_HEADLESS", "--headless");
            run(exe, repo, nro, log, 0, "RG", NULL);
            CHECK(!snx_is_dir(extracted) && snx_is_file(game));
            set_env("SNX_WORKFLOW_OUT", extracted); /* an extraction an NRO does not have */
            run(exe, repo, nro, log, 1, "", "--out names a dump's extraction, and an NRO has none");
            set_env("SNX_WORKFLOW_OUT", "");
            CHECK(!snx_mkdirs(extracted));
            run(exe, repo, nro, log, 0, "RG", NULL);
            set_env("SNX_WORKFLOW_FAIL", "R");
            run(exe, repo, nro, log, 7, "R", "Recompilation failed");
            set_env("SNX_WORKFLOW_FAIL", "");
            run(exe, repo, nro, log, 0, "RG", NULL);
            CHECK(!snx_remove_tree(nro));
            run(exe, repo, nro, log, 1, "", "Input dump is missing");
            free(nro);
        }
        set_env("SNX_WORKFLOW_NRO", "");
        set_env("SNX_WORKFLOW_HEADLESS", "");
    }
    /* a game made already, named by its program or its folder: played as it is when this build made it, else
       recompiled where it is from its _data (an NRO's from the NRO there), its name kept; -r recompiles it without
       playing. A program an older version laid out beside the others is neither: the driver says what to do */
    {
        char *folder = snx_path_join(repo, "games/Test game"),
             *data = snx_path_join(repo, "games/Test game/Test game_data");
        char *stamp = snx_path_join(repo, "games/Test game/Test game.abi"), *slash = snx_format("%s/", folder);
        char *nro = snx_path_join(data, "exefs/main.nro"), *old = snx_path_join(repo, "games/Old"), *old_data;
#ifdef _WIN32
        char* code = snx_path_join(folder, "Test game_code.dll");
#else
        char* code = snx_path_join(folder, "Test game_code.so");
#endif
        reset(repo, dump);
        run(exe, repo, dump, log, 0, "ERG", NULL);
        set_env("SNX_WORKFLOW_SOURCE", data);
        set_env("SNX_WORKFLOW_NAMED", "Test game"); /* by its extraction's folder: the name kept is given */
        run(exe, repo, game, log, 0, "G", NULL);
        run(exe, repo, folder, log, 0, "G", NULL);
        run(exe, repo, slash, log, 0, "G", NULL);
        CHECK(!snx_write_file(stamp, "4\n", 2)); /* an older version's */
        run(exe, repo, game, log, 0, "RG", NULL);
        run(exe, repo, game, log, 0, "G", NULL);
        CHECK(!snx_write_file(stamp, "4\ndef5678\n", 10)); /* another build's */
        run(exe, repo, folder, log, 0, "RG", NULL);
        CHECK(!snx_remove_tree(code));
        run(exe, repo, game, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "-r");
        run(exe, repo, game, log, 0, "R", NULL);
        run(exe, repo, folder, log, 0, "R", NULL);
#ifndef _WIN32
        set_env("SNX_WORKFLOW_FLAGS", "-rx");
        run(exe, repo, game, log, 1, "", "-rx requires");
#endif
        set_env("SNX_WORKFLOW_FLAGS", "--backend=gcc");
        set_env("SNX_WORKFLOW_BACKEND", "gcc");
        run(exe, repo, game, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_BACKEND", "");
        set_env("SNX_WORKFLOW_FLAGS", "");
        set_env("SNX_WORKFLOW_OUTDIR", repo);
        run(exe, repo, game, log, 1, "", "is a game");
        set_env("SNX_WORKFLOW_OUTDIR", "");
        set_env("SNX_WORKFLOW_OUT", extracted);
        run(exe, repo, folder, log, 1, "", "is a game");
        set_env("SNX_WORKFLOW_OUT", "");
        CHECK(!snx_mkdirs(data) && !snx_write_file(stamp, "4\n", 2));
        { /* an NRO's game: recompiled from the NRO its data keeps */
            char* exefs = snx_path_join(data, "exefs");
            CHECK(!snx_mkdirs(exefs) && !snx_write_file(nro, "NRO", 3));
            set_env("SNX_WORKFLOW_SOURCE", nro);
            run(exe, repo, game, log, 0, "RG", NULL);
            free(exefs);
        }
        set_env("SNX_WORKFLOW_SOURCE", "");
        set_env("SNX_WORKFLOW_NAMED", "");
        old_data = snx_format("%s_data", old);
        CHECK(!snx_copy_file(game, old) && !snx_mkdirs(old_data));
        run(exe, repo, old, log, 1, "", "laid out by an older version: move its files as docs/DEPRECATIONS.md says");
        set_env("SNX_WORKFLOW_FLAGS", "-r");
        run(exe, repo, old, log, 1, "", "laid out by an older version");
        set_env("SNX_WORKFLOW_FLAGS", "");
        free(old_data);
        free(old);
        free(nro);
        free(code);
        free(slash);
        free(stamp);
        free(data);
        free(folder);
    }
    /* --outdir and --out, as the menu passes the games and the extractions of Settings > App > Folders: the game and
       the extraction go there, not in the data folder, and are used again there */
    {
        char *outdir = snx_path_join(root, "games elsewhere"), *out = snx_path_join(root, "work elsewhere/Test game");
        char *folder = snx_path_join(outdir, "Test game"), *moved;
#ifdef _WIN32
        moved = snx_path_join(folder, "Test game.exe");
#else
        moved = snx_path_join(folder, "Test game");
#endif
        reset(repo, dump);
        set_env("SNX_WORKFLOW_GAMES", folder);
        set_env("SNX_WORKFLOW_OUTDIR", outdir);
        set_env("SNX_WORKFLOW_OUT", out);
        run(exe, repo, dump, log, 0, "ERG", NULL);
        CHECK(snx_is_dir(out) && snx_is_file(moved) && !snx_is_dir(extracted) && !snx_is_file(game));
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_OUT", "");
        set_env("SNX_WORKFLOW_OUTDIR", "none");
        run(exe, repo, dump, log, 1, "", "--outdir requires a directory");
        set_env("SNX_WORKFLOW_OUTDIR", "");
        free(folder);
        folder = snx_path_join(repo, "games/Test game");
        set_env("SNX_WORKFLOW_GAMES", folder);
        free(folder);
        free(moved);
        free(out);
        free(outdir);
    }
    /* The probe takes a third argument, so use a separate, terminated vector. */
    {
        const char* probe[4];
        probe[0] = exe;
        probe[1] = "--probe";
        probe[2] = "cwd";
        probe[3] = NULL;
        output = NULL;
        rc = snx_run(probe, NULL, NULL, &output);
        CHECK(rc != 0 && strstr(output, "Probe"));
#ifndef _WIN32
        CHECK(strstr(output, "chdir to game directory"));
#endif
        free(output);
        probe[2] = "exec";
        output = NULL;
        rc = snx_run(probe, NULL, NULL, &output);
        CHECK(rc != 0 && strstr(output, "Probe"));
#ifndef _WIN32
        CHECK(strstr(output, "executing child process"));
#endif
        free(output);
    }
    CHECK(!snx_remove_tree(root));
    free(exe);
    free(root);
    free(repo);
    free(dump);
    free(log);
    free(extracted);
    free(game);
    printf("Driver workflow: %u checks passed\n", checks);
    return 0;
}
