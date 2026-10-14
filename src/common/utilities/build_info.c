/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "build_info.h"
#include "snx_commit.h"

#define SNX_TEXT(x)   #x
#define SNX_NUMBER(x) SNX_TEXT(x)
#if defined(__clang__)
#define SNX_COMPILER                                                                                                   \
    "clang " SNX_NUMBER(__clang_major__) "." SNX_NUMBER(__clang_minor__) "." SNX_NUMBER(__clang_patchlevel__)
#elif defined(__GNUC__)
#define SNX_COMPILER "gcc " SNX_NUMBER(__GNUC__) "." SNX_NUMBER(__GNUC_MINOR__) "." SNX_NUMBER(__GNUC_PATCHLEVEL__)
#else
#define SNX_COMPILER "unknown compiler"
#endif
#if defined(_WIN32)
#define SNX_OS "Windows"
#elif defined(__ANDROID__)
#define SNX_OS "Android"
#elif defined(__linux__)
#define SNX_OS "Linux"
#elif defined(__FreeBSD__)
#define SNX_OS "FreeBSD"
#else
#define SNX_OS "unknown OS"
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define SNX_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SNX_ARCH "arm64"
#elif defined(__alpha__)
#define SNX_ARCH "alpha"
#else
#define SNX_ARCH "unknown arch"
#endif

const char* snx_build_info(void) {
    return "ScarletNX " SNX_BRANCH " " SNX_COMMIT " (" SNX_COMPILER ", " SNX_OS " " SNX_ARCH ")";
}
