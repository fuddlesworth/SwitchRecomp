/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_user_dir and snx_user_path: an installed build's config, state and cache folders (the XDG Base Directory spec),
   and where a file of its data folder goes in them. tests/data_dir.c has the data folder itself. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void expect_dir(int kind, const char* expected) {
    char* dir = snx_user_dir(kind);
    assert(expected ? dir && !strcmp(dir, expected) : !dir);
    free(dir);
}
static void expect_path(const char* path, int kind, const char* expected) {
    char* moved = snx_user_path(path, kind);
    assert(moved && !strcmp(moved, expected));
    free(moved);
}

int main(void) {
    static const char* const vars[] = {"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
    char *root = snx_temp_dir(NULL), *home, *dir[4], *base, *games, *path;
    int kind;
    assert(root);
    home = join(root, "home");
    setenv("HOME", home, 1);
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) unsetenv(vars[kind]);
#ifdef SNX_XDG
    {
        static const char* const defaults[] = {".local/share", ".config", ".local/state", ".cache"};
        char* expected;
        for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) {
            /* unset, empty or relative: the spec's default under HOME */
            base = join(home, defaults[kind]);
            expected = join(base, "scarletnx");
            expect_dir(kind, expected);
            setenv(vars[kind], "", 1);
            expect_dir(kind, expected);
            setenv(vars[kind], "relative/folder", 1);
            expect_dir(kind, expected);
            free(expected);
            free(base);
            /* without a home only an absolute variable names the folder */
            unsetenv("HOME");
            expect_dir(kind, NULL);
            base = join(root, vars[kind]);
            expected = join(base, "scarletnx");
            setenv(vars[kind], base, 1);
            expect_dir(kind, expected);
            setenv("HOME", home, 1);
            expect_dir(kind, expected);
            free(expected);
            free(base);
        }
    }
#else
    /* one folder for everything where the spec does not apply */
    base = snx_user_data_dir();
    assert(base);
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) expect_dir(kind, base);
    free(base);
#endif

    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) {
        base = join(root, vars[kind]);
        setenv(vars[kind], base, 1);
        dir[kind] = snx_user_dir(kind);
        assert(dir[kind]);
        free(base);
    }
    games = join(dir[SNX_USER_DATA], "games");
    assert(!snx_mkdirs(games));
#ifdef SNX_XDG
    {
        char *expected, *link, *cwd;
        /* the installed games/: each kind in its folder, under the same name */
        path = join(dir[SNX_USER_CONFIG], "games");
        expect_path(games, SNX_USER_CONFIG, path);
        free(path);
        path = join(games, "Game.ini");
        expected = join(dir[SNX_USER_CONFIG], "games/Game.ini");
        expect_path(path, SNX_USER_CONFIG, expected);
        free(expected);
        free(path);
        path = join(dir[SNX_USER_STATE], "games");
        expect_path(games, SNX_USER_STATE, path);
        free(path);
        path = join(dir[SNX_USER_DATA], "work");
        expected = join(dir[SNX_USER_CACHE], "work");
        expect_path(path, SNX_USER_CACHE, expected);
        free(expected);
        free(path);
        expect_path(dir[SNX_USER_DATA], SNX_USER_CACHE, dir[SNX_USER_CACHE]);
        /* a trailing separator, and the folder reached through a link */
        path = snx_format("%s/", games);
        expected = join(dir[SNX_USER_STATE], "games");
        expect_path(path, SNX_USER_STATE, expected);
        free(path);
        link = join(root, "link");
        assert(!symlink(dir[SNX_USER_DATA], link));
        path = join(link, "games");
        expect_path(path, SNX_USER_STATE, expected);
        free(path);
        free(link);
        /* the runner started in games/ as ./Game */
        cwd = getcwd(NULL, 0);
        assert(cwd && !chdir(games));
        expect_path("./", SNX_USER_STATE, expected);
        expect_path(".", SNX_USER_STATE, expected);
        assert(!chdir(cwd));
        free(cwd);
        free(expected);
    }
#endif
    /* data stays; a checkout's, a portable copy's or an --outdir's games/ keep everything beside the game */
    expect_path(games, SNX_USER_DATA, games);
    path = join(root, "portable/games");
    expect_path(path, SNX_USER_CONFIG, path);
    expect_path(path, SNX_USER_CACHE, path);
    free(path);
    path = snx_format("%s-other/games", dir[SNX_USER_DATA]);
    expect_path(path, SNX_USER_STATE, path);
    free(path);
    expect_path("games", SNX_USER_CONFIG, "games");
    /* a config folder that is the data folder divides nothing */
    setenv("XDG_CONFIG_HOME", getenv("XDG_DATA_HOME"), 1);
    expect_path(games, SNX_USER_CONFIG, games);

    puts("snx_user_dir: the XDG variables, their defaults, a relative path ignored; snx_user_path: the installed "
         "games/ divided, any other folder kept");
    for (kind = SNX_USER_DATA; kind <= SNX_USER_CACHE; ++kind) free(dir[kind]);
    free(games);
    free(home);
    assert(!snx_remove_tree(root));
    free(root);
    return 0;
}
