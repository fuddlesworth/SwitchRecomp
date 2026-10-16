/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_left_behind, which tells an older build's file this one keeps elsewhere, and moves nothing; and snx_work_dir,
   the drivers' extractions, which leaves an older build's work/ where it is. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void put(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
static int holds(const char* path, const char* text) {
    size_t n;
    unsigned char* data = snx_read_file(path, &n);
    int same = data && n == strlen(text) && !memcmp(data, text, n);
    free(data);
    return same;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *old, *now, *folder, *work;
    assert(root);
    old = join(root, "old/games/Game.ini");
    now = join(root, "new/games/Game.ini");
    folder = snx_dirname(old);
    assert(!snx_mkdirs(folder));
    free(folder);

    /* nothing there, or the same path: nothing to say */
    assert(!snx_left_behind(old, now));
    put(old, "old");
    assert(!snx_left_behind(old, old));
    /* there: said, and left alone, as long as it stays, whether or not this build made its own */
    assert(snx_left_behind(old, now) && holds(old, "old") && !snx_path_exists(now));
    folder = snx_dirname(now);
    assert(!snx_mkdirs(folder));
    free(folder);
    put(now, "new");
    assert(snx_left_behind(old, now) && holds(old, "old") && holds(now, "new"));
    /* gone: nothing more to say */
    assert(!remove(old) && !snx_left_behind(old, now));
    free(old);
    free(now);

    /* work/ of a checkout or a portable copy is its own */
    folder = join(root, "checkout");
    work = snx_work_dir(folder);
    old = join(folder, "work");
    assert(!strcmp(work, old));
    free(old);
    free(work);
    free(folder);
#ifdef SNX_XDG
    /* an installed build's is in the cache folder; an older build's extractions stay in the data folder */
    folder = join(root, "data");
    setenv("XDG_DATA_HOME", folder, 1);
    free(folder);
    folder = join(root, "cache");
    setenv("XDG_CACHE_HOME", folder, 1);
    free(folder);
    folder = join(root, "data/scarletnx/work/Game/exefs");
    assert(!snx_mkdirs(folder));
    free(folder);
    folder = join(root, "data/scarletnx");
    work = snx_work_dir(folder);
    free(folder);
    now = join(root, "cache/scarletnx/work");
    assert(!strcmp(work, now));
    old = join(root, "data/scarletnx/work");
    assert(snx_left_behind(old, work) && snx_is_dir(old) && !snx_path_exists(now));
    free(old);
    free(now);
    free(work);
#endif

    puts("snx_left_behind: an older build's files are told, never moved, while they stay; snx_work_dir: an installed "
         "build's extractions in its cache folder, an older build's work/ left in place");
    assert(!snx_remove_tree(root));
    free(root);
    return 0;
}
