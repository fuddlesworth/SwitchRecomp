/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The driver as the Android app's menu runs it (dist/android): the runtime library beside this executable, called as
   the scarletnx executable would be */
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 32);
    char* slash;
    void* runtime;
    int (*entry)(int, char**);
    if (n <= 0) return 127;
    path[n] = 0;
    slash = strrchr(path, '/');
    strcpy(slash ? slash + 1 : path, "libscarletnx.so");
    runtime = dlopen(path, RTLD_NOW);
    entry = runtime ? (int (*)(int, char**))dlsym(runtime, "main") : NULL;
    if (!entry) {
        fprintf(stderr, "scarletnx: %s\n", dlerror());
        return 127;
    }
    return entry(argc, argv);
}
