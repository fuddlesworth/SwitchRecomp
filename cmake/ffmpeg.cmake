# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# FFmpeg's libavcodec, libswresample and libavutil as static libraries (targets FFmpeg::avcodec, FFmpeg::swresample
# and FFmpeg::avutil), built from the release source in cmake/dependencies.cmake with FFmpeg's own configure and make,
# which it has instead of a CMake build. Only the decoders NVDEC handles are in (H.264, VP8, VP9), and Opus for hwopus
# (FFmpeg's Opus decoder resamples its SILK layer with libswresample, which is why that library is in): no programs,
# formats, filters, devices, network or autodetected system libraries, so the result needs nothing but libc and builds
# the same everywhere. All of it is LGPL (no --enable-gpl). It needs a POSIX shell and make (gmake on the BSDs), and
# NASM for the x86-64 assembly.
# This file is included into the top-level scope: its variables carry the snx_ffmpeg_ prefix.
set(snx_ffmpeg_src "${FFmpeg_SOURCE_DIR}")
set(snx_ffmpeg_prefix "${CMAKE_BINARY_DIR}/ffmpeg")
# FFmpeg's configure stops on a source path with a space in it, which would only show at build time
if(snx_ffmpeg_src MATCHES "[ \t]")
    message(FATAL_ERROR "FFmpeg can't be built from a path with spaces in it (${snx_ffmpeg_src}). Move the checkout "
                        "to a path without spaces, set CPM_SOURCE_CACHE to one, or configure with -DSNX_H264=OFF.")
endif()
# sh before bash: on Windows a bash.exe in System32 is WSL's launcher, and w64devkit's sh is the one to use
find_program(SNX_FFMPEG_SH NAMES sh bash)
find_program(SNX_FFMPEG_MAKE NAMES gmake make)
if(NOT SNX_FFMPEG_SH OR NOT SNX_FFMPEG_MAKE)
    message(FATAL_ERROR "Building FFmpeg needs a POSIX shell (sh) and GNU make (gmake on the BSDs) on the PATH. On "
                        "Windows, scripts\\bootstrap.ps1 installs them with w64devkit. Or configure with "
                        "-DSNX_H264=OFF, and the game's videos stay black.")
endif()

set(snx_ffmpeg_args
    --prefix=${snx_ffmpeg_prefix}
    --enable-static
    --disable-shared
    --enable-pic
    --disable-autodetect
    --disable-everything
    --disable-programs
    --disable-doc
    --disable-network
    --disable-avdevice
    --disable-avformat
    --disable-avfilter
    --disable-swscale
    --disable-iconv
    --enable-decoder=h264,vp8,vp9,opus
    --cc=${CMAKE_C_COMPILER}
    --cxx=${CMAKE_CXX_COMPILER}
    --ar=${CMAKE_AR}
    --ranlib=${CMAKE_RANLIB}
    --nm=${CMAKE_NM}
    # FFmpeg's own warnings stay out of the build output
    --extra-cflags=-w)
if(WIN32 OR CMAKE_CROSSCOMPILING)
    # configure guesses the target from the machine doing the build, so a cross build names it. CMake names the
    # architecture AMD64 or ARM64 on Windows, which configure doesn't know
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(snx_ffmpeg_arch x86_64)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(snx_ffmpeg_arch aarch64)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" snx_ffmpeg_arch)
    endif()
    if(WIN32)
        set(snx_ffmpeg_os mingw32)
    else()
        # configure's names for Linux, Darwin, FreeBSD, Android are CMake's in lower case
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" snx_ffmpeg_os)
    endif()
    list(APPEND snx_ffmpeg_args --target-os=${snx_ffmpeg_os} --arch=${snx_ffmpeg_arch})
endif()
if(CMAKE_CROSSCOMPILING)
    # configure builds a few helper programs for the machine doing the build, which needs its own C compiler
    find_program(SNX_HOST_CC NAMES clang cc)
    if(NOT SNX_HOST_CC)
        message(FATAL_ERROR "Cross-building FFmpeg needs a C compiler for this machine too (clang or cc). Install one, "
                            "or configure with -DSNX_H264=OFF.")
    endif()
    list(APPEND snx_ffmpeg_args --enable-cross-compile --host-cc=${SNX_HOST_CC})
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # without its assembly FFmpeg decodes movies several times slower, which a build shouldn't do without being asked
    option(SNX_FFMPEG_ASM "Build FFmpeg's x86-64 assembly (needs NASM)" ON)
    if(SNX_FFMPEG_ASM)
        find_program(SNX_NASM nasm)
        if(NOT SNX_NASM)
            message(FATAL_ERROR "NASM was not found: FFmpeg needs it for its x86-64 assembly. Install nasm, or configure "
                                "with -DSNX_FFMPEG_ASM=OFF to build FFmpeg without it (movies decode more slowly).")
        endif()
        list(APPEND snx_ffmpeg_args --x86asmexe=${SNX_NASM})
    else()
        list(APPEND snx_ffmpeg_args --disable-x86asm)
    endif()
endif()

cmake_host_system_information(RESULT snx_ffmpeg_jobs QUERY NUMBER_OF_LOGICAL_CORES)
include(ExternalProject)
ExternalProject_Add(ffmpeg_build
    SOURCE_DIR "${snx_ffmpeg_src}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ffmpeg-build"
    INSTALL_DIR "${snx_ffmpeg_prefix}"
    CONFIGURE_COMMAND "${SNX_FFMPEG_SH}" "${snx_ffmpeg_src}/configure" ${snx_ffmpeg_args}
    BUILD_COMMAND "${SNX_FFMPEG_MAKE}" -j${snx_ffmpeg_jobs}
    INSTALL_COMMAND "${SNX_FFMPEG_MAKE}" install
    BUILD_BYPRODUCTS "${snx_ffmpeg_prefix}/lib/libavcodec.a" "${snx_ffmpeg_prefix}/lib/libswresample.a"
                     "${snx_ffmpeg_prefix}/lib/libavutil.a"
    # configure and make print a lot: keep it in log files, shown when a step fails
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

# The include folder is filled by the install step, but has to exist for the imported targets at configure time
file(MAKE_DIRECTORY "${snx_ffmpeg_prefix}/include")
add_library(FFmpeg::avutil STATIC IMPORTED GLOBAL)
# SYSTEM OFF: as a system folder (-isystem) the headers would come after every -I, and a -I that holds another
# FFmpeg's headers (/usr/local/include on the BSDs) would win over the ones these libraries were built with
set_target_properties(FFmpeg::avutil PROPERTIES IMPORTED_LOCATION "${snx_ffmpeg_prefix}/lib/libavutil.a"
                                                INTERFACE_INCLUDE_DIRECTORIES "${snx_ffmpeg_prefix}/include"
                                                SYSTEM OFF)
add_dependencies(FFmpeg::avutil ffmpeg_build)
if(WIN32)
    # libavutil's random seed uses BCryptGenRandom
    target_link_libraries(FFmpeg::avutil INTERFACE bcrypt)
else()
    target_link_libraries(FFmpeg::avutil INTERFACE Threads::Threads m)
endif()
add_library(FFmpeg::swresample STATIC IMPORTED GLOBAL)
set_target_properties(FFmpeg::swresample PROPERTIES IMPORTED_LOCATION "${snx_ffmpeg_prefix}/lib/libswresample.a"
                                                    SYSTEM OFF)
target_link_libraries(FFmpeg::swresample INTERFACE FFmpeg::avutil)
add_dependencies(FFmpeg::swresample ffmpeg_build)
add_library(FFmpeg::avcodec STATIC IMPORTED GLOBAL)
set_target_properties(FFmpeg::avcodec PROPERTIES IMPORTED_LOCATION "${snx_ffmpeg_prefix}/lib/libavcodec.a" SYSTEM OFF)
# the Opus decoder in libavcodec calls libswresample
target_link_libraries(FFmpeg::avcodec INTERFACE FFmpeg::swresample FFmpeg::avutil)
add_dependencies(FFmpeg::avcodec ffmpeg_build)
