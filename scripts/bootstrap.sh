#!/usr/bin/env bash
# Builds the third-party libraries and tools from the pinned submodules in third_party/, so the repository ships no
# prebuilt binaries:
#
#   deps/<target>/lib/libdecoder.a, libcommon.a   OpenH264 decoder (NVDEC video)
#   deps/<target>/lib/libspvc.a                   SPIRV-Cross core + GLSL (LSFG shaders)
#   deps/<target>/include/{wels,spirv_cross}/     their headers
#   deps/host/bin/hactool                         extracts game dumps (scripts/extract.py)
#
#   scripts/bootstrap.sh                 Linux (needs gcc, g++, make, cmake, nasm)
#   scripts/bootstrap.sh --target win64  Windows, cross-built with MinGW-w64 (x86_64-w64-mingw32-gcc)
#
# Rerunning is cheap: each library is rebuilt only when its submodule commit changed.
set -euo pipefail

target=linux
jobs=$(nproc)
while [ $# -gt 0 ]; do
    case $1 in
        --target) target=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        *) echo "usage: $0 [--target linux|win64] [--jobs N]" >&2; exit 2 ;;
    esac
done

repo=$(cd "$(dirname "$0")/.." && pwd)
out="$repo/deps/$target"
work="$repo/build/deps/$target"
mkdir -p "$out/lib" "$out/include" "$work"

case $target in
    linux)
        cc=gcc; cxx=g++; ar=ar
        h264_make=(OS=linux ARCH=x86_64)
        cmake_cross=() ;;
    win64)
        cc=x86_64-w64-mingw32-gcc; cxx=x86_64-w64-mingw32-g++; ar=x86_64-w64-mingw32-ar
        h264_make=(OS=mingw_nt ARCH=x86_64 "CC=$cc" "CXX=$cxx" "AR=$ar")
        cmake_cross=(-DCMAKE_SYSTEM_NAME=Windows "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_CXX_COMPILER=$cxx") ;;
    *) echo "unknown target $target (linux or win64)" >&2; exit 2 ;;
esac
for tool in "$cc" "$cxx" "$ar" make cmake nasm git; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

git -C "$repo" submodule update --init third_party/openh264 third_party/SPIRV-Cross third_party/hactool

# Runs a build step with its output in a log, shown only when the step fails. Third-party code warns a lot.
log="$work/bootstrap.log"
quiet() {
    "$@" >"$log" 2>&1 || { tail -40 "$log" >&2; echo "failed: $* (full log: $log)" >&2; exit 1; }
}

# Builds only when the submodule moved since the last build. $1 = submodule name, $2 = stamp file
up_to_date() {
    [ -f "$2" ] && [ "$(cat "$2")" = "$(git -C "$repo/third_party/$1" rev-parse HEAD)" ]
}
stamp() {
    git -C "$repo/third_party/$1" rev-parse HEAD > "$2"
}

# A clean copy of a submodule's tree. OpenH264 and hactool build in their source folder, which would leave the
# submodules dirty.
export_tree() {
    rm -rf "$2"
    mkdir -p "$2"
    git -C "$repo/third_party/$1" archive HEAD | tar -x -C "$2"
}

# OpenH264: only the decoder and the common code, the runtime never encodes
if up_to_date openh264 "$out/.openh264"; then
    echo "OpenH264 is up to date"
else
    echo "Building OpenH264 ($target)"
    export_tree openh264 "$work/openh264"
    quiet make -C "$work/openh264" -j"$jobs" "${h264_make[@]}" USE_ASM=Yes BUILDTYPE=Release libdecoder.a libcommon.a
    cp "$work/openh264/libdecoder.a" "$work/openh264/libcommon.a" "$out/lib/"
    rm -rf "$out/include/wels"
    mkdir -p "$out/include/wels"
    cp "$work/openh264/codec/api/wels/"*.h "$out/include/wels/"
    cp "$work/openh264/LICENSE" "$out/LICENSE-OpenH264.txt"
    stamp openh264 "$out/.openh264"
fi

# SPIRV-Cross: core and GLSL merged into the one libspvc.a the runtime links
if up_to_date SPIRV-Cross "$out/.spirv-cross"; then
    echo "SPIRV-Cross is up to date"
else
    echo "Building SPIRV-Cross ($target)"
    quiet cmake -S "$repo/third_party/SPIRV-Cross" -B "$work/spirv-cross" -G "Unix Makefiles" "${cmake_cross[@]}" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON "-DCMAKE_INSTALL_PREFIX=$work/spirv-cross-install" \
        -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF \
        -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF \
        -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF
    quiet cmake --build "$work/spirv-cross" -j"$jobs"
    quiet cmake --install "$work/spirv-cross"
    lib="$work/spirv-cross-install/lib"
    rm -f "$out/lib/libspvc.a"
    printf 'create %s\naddlib %s\naddlib %s\nsave\nend\n' "$out/lib/libspvc.a" \
        "$lib/libspirv-cross-glsl.a" "$lib/libspirv-cross-core.a" | "$ar" -M
    rm -rf "$out/include/spirv_cross"
    cp -r "$work/spirv-cross-install/include/spirv_cross" "$out/include/"
    cp "$repo/third_party/SPIRV-Cross/LICENSE" "$out/LICENSE-SPIRV-Cross.txt"
    stamp SPIRV-Cross "$out/.spirv-cross"
fi

# hactool runs on the machine doing the extraction, whatever the target
host="$repo/deps/host"
if up_to_date hactool "$host/.hactool"; then
    echo "hactool is up to date"
else
    echo "Building hactool"
    export_tree hactool "$repo/build/deps/host/hactool"
    cp "$repo/build/deps/host/hactool/config.mk.template" "$repo/build/deps/host/hactool/config.mk"
    quiet make -C "$repo/build/deps/host/hactool" -j"$jobs"
    mkdir -p "$host/bin"
    cp "$repo/build/deps/host/hactool/hactool" "$host/bin/"
    cp "$repo/third_party/hactool/LICENSE" "$host/LICENSE-hactool.txt"
    stamp hactool "$host/.hactool"
fi

echo "Dependencies for $target are in deps/$target"
