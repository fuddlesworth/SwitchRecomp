/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The line the window titles carry: the branch and commit the build read, this compiler's version, this host. */
#include "common/utilities/build_info.h"
#include "snx_commit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

int main(void) {
    char want[256];
    const char *os, *arch;
#if defined(_WIN32)
    os = "Windows";
#elif defined(__linux__)
    os = "Linux";
#elif defined(__FreeBSD__)
    os = "FreeBSD";
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    os = "iOS";
#elif defined(__APPLE__)
    os = "macOS";
#else
    os = "unknown OS";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    arch = "arm64";
#else
    arch = NULL;
#endif
#if defined(__clang__)
    snprintf(want, sizeof want, "ScarletNX %s %s (clang %d.%d.%d, %s %s)", SNX_BRANCH, SNX_COMMIT, __clang_major__,
             __clang_minor__, __clang_patchlevel__, os, arch ? arch : "");
#else
    snprintf(want, sizeof want, "ScarletNX %s %s (gcc %d.%d.%d, %s %s)", SNX_BRANCH, SNX_COMMIT, __GNUC__,
             __GNUC_MINOR__, __GNUC_PATCHLEVEL__, os, arch ? arch : "");
#endif
    printf("%s\n", snx_build_info());
    assert(!arch || !strcmp(snx_build_info(), want));
    assert(strlen(SNX_COMMIT) >= 7); /* a hash, or "unknown" */
    assert(!strcmp(snx_build_commit(), SNX_COMMIT));
    assert(SNX_BRANCH[0]); /* a branch, "detached" or "unknown" */
    puts("build info: commit, compiler and host in the window titles' line passed");
    return 0;
}
