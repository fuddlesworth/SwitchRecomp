#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's libraries for macOS or iOS on arm64, which the recompiler links (docs/APPLE.md). Only the libraries, their
# headers and CMake package, and one code generator: AArch64's, the Mac's or the phone's own. Run on a Mac with Xcode.
#
#   scripts/apple-llvm.sh macos|ios PREFIX [SOURCE] [-- cmake args]
#
# SOURCE is an llvm-project folder or .tar.xz (default: the release named by LLVM_VERSION, downloaded). MACOS_VERSION
# (default 13.0) and IOS_VERSION (default 16.0), the apps' oldest, are the oldest systems it runs on. Configure
# ScarletNX with -DLLVM_DIR=PREFIX/lib/cmake/llvm afterwards.
set -eu
[ $# -ge 2 ] || { sed -n '4,11p' "$0"; exit 2; }
system=$1 prefix=$2
shift 2
source=
if [ $# -gt 0 ] && [ "$1" != -- ]; then source=$1; shift; fi
[ "${1:-}" = -- ] && shift
case $system in
    # the Mac's own build
    macos) os=${MACOS_VERSION:-13.0}; set -- -DCMAKE_OSX_DEPLOYMENT_TARGET="$os" "$@" ;;
    # a cross build: LLVM builds the tablegens it runs for this Mac in a tree of their own, for the same one target.
    # Its CMake takes only a CMAKE_SYSTEM_NAME of Darwin for Apple's linker, and gives iOS GNU ld's options
    # (--gc-sections, -z defs): no executable or plugin of it is linked here, and LLVM_NO_DEAD_STRIP keeps the first
    # off any that would be
    ios)
        os=${IOS_VERSION:-16.0}
        set -- -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET="$os" \
            -DLLVM_HOST_TRIPLE=arm64-apple-ios"$os" "$@"
        ;;
    *) echo "macos or ios, not $system" >&2; exit 2 ;;
esac
version=${LLVM_VERSION:-23.1.3}
work=$prefix.build/$version
mkdir -p "$work"
if [ -z "$source" ]; then
    source=$work/llvm-project-$version.src.tar.xz
    [ -f "$source" ] || curl -fL -o "$source" \
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/llvm-project-$version.src.tar.xz"
fi
if [ -f "$source" ]; then
    [ -d "$work/src/llvm" ] || { mkdir -p "$work/src"; tar -xJf "$source" -C "$work/src" --strip-components=1; }
    source=$work/src
fi
cmake -S "$source/llvm" -B "$work/$system" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DLLVM_TARGETS_TO_BUILD=AArch64 -DLLVM_ENABLE_PIC=ON \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_NO_DEAD_STRIP=ON "$@"
# the libraries, their headers and the CMake package ScarletNX finds them with, which names no tool or utility
cmake --build "$work/$system" --target install-llvm-libraries install-llvm-headers install-cmake-exports
echo "LLVM for $system in $prefix: configure ScarletNX with -DLLVM_DIR=$prefix/lib/cmake/llvm"
