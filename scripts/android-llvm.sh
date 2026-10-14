#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's libraries for one Android ABI, which the recompiler links (docs/ANDROID.md). Only the libraries and headers,
# one code generator: the host's of that ABI.
#
#   scripts/android-llvm.sh ABI PREFIX [SOURCE] [-- cmake args]
#
# ABI is x86_64 or arm64-v8a; SOURCE an llvm-project folder or .tar.xz (default: the release named by LLVM_VERSION,
# downloaded). The NDK is ANDROID_NDK_ROOT, else the newest under ANDROID_HOME/ndk. Configure ScarletNX with
# -DLLVM_DIR=PREFIX/lib/cmake/llvm afterwards.
set -eu
[ $# -ge 2 ] || { sed -n '4,11p' "$0"; exit 2; }
abi=$1 prefix=$2
shift 2
source=
if [ $# -gt 0 ] && [ "$1" != -- ]; then source=$1; shift; fi
[ "${1:-}" = -- ] && shift
case $abi in
    x86_64) target=X86 triple=x86_64-none-linux-android ;;
    arm64-v8a) target=AArch64 triple=aarch64-none-linux-android ;;
    *) echo "ABI must be x86_64 or arm64-v8a, not $abi" >&2; exit 2 ;;
esac
api=${ANDROID_API:-30}
ndk=${ANDROID_NDK_ROOT:-$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/* 2>/dev/null | sort -V | tail -1)}
[ -f "$ndk/build/cmake/android.toolchain.cmake" ] || { echo "no NDK: set ANDROID_NDK_ROOT" >&2; exit 1; }
version=${LLVM_VERSION:-22.1.8}
work=$prefix.build
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
cmake -S "$source/llvm" -B "$work/$abi" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$api" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DLLVM_TARGETS_TO_BUILD="$target" -DLLVM_HOST_TRIPLE="$triple$api" -DLLVM_ENABLE_PIC=ON \
    -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF "$@"
cmake --build "$work/$abi" --target install
echo "LLVM for $abi in $prefix: configure ScarletNX with -DLLVM_DIR=$prefix/lib/cmake/llvm"
