#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The Android app (docs/ANDROID.md): each ABI's native build, installed into OUT/stage, then the APK from dist/android.
#
#   scripts/android-apk.sh OUT LLVM ABI... [-- cmake args]
#
# LLVM holds a scripts/android-llvm.sh prefix per ABI (LLVM/x86_64, LLVM/arm64-v8a). GRADLE names a Gradle 9
# (default: gradle). The APK lands in OUT/stage/gradle/outputs/apk/debug/.
set -eu
[ $# -ge 3 ] || { sed -n '4,9p' "$0"; exit 2; }
out=$1 llvm=$2
shift 2
abis=
while [ $# -gt 0 ] && [ "$1" != -- ]; do abis="$abis $1"; shift; done
[ "${1:-}" = -- ] && shift
src=$(cd "$(dirname "$0")/.." && pwd)
ndk=${ANDROID_NDK_ROOT:-$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/* 2>/dev/null | sort -V | tail -1)}
[ -f "$ndk/build/cmake/android.toolchain.cmake" ] || { echo "no NDK: set ANDROID_NDK_ROOT" >&2; exit 1; }
for abi in $abis; do
    cmake -S "$src" -B "$out/$abi" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-30 -DLLVM_DIR="$llvm/$abi/lib/cmake/llvm" \
        -DSNX_DEPLOY=OFF -DSNX_BUILD_GNUABI=OFF -DBUILD_TESTING=OFF "$@"
    cmake --build "$out/$abi"
    cmake --install "$out/$abi" --prefix "$out/stage" --strip
done
"${GRADLE:-gradle}" -p "$src/dist/android" --project-cache-dir "$out/gradle-cache" assembleDebug \
    -Pscarletnx.stage="$out/stage"
