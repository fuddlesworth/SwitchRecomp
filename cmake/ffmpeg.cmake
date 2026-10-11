# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
#
# FFmpeg's libavcodec and libavutil as static libraries (targets FFmpeg::avcodec and FFmpeg::avutil), built from the
# release source in cmake/dependencies.cmake with FFmpeg's own configure and make, which it has instead of a CMake
# build. Only the decoders NVDEC handles are in (H.264, VP8, VP9): no programs, formats, filters, devices, network or
# autodetected system libraries, so the result needs nothing but libc and builds the same everywhere. It needs a POSIX
# shell and make (gmake on the BSDs), and NASM for the x86-64 assembly.
set(src "${FFmpeg_SOURCE_DIR}")
set(prefix "${CMAKE_BINARY_DIR}/ffmpeg")
# sh before bash: on Windows a bash.exe in System32 is WSL's launcher, and w64devkit's sh is the one to use
find_program(SWR_FFMPEG_SH NAMES sh bash REQUIRED)
find_program(SWR_FFMPEG_MAKE NAMES gmake make REQUIRED)

set(args
    --prefix=${prefix}
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
    --disable-swresample
    --disable-iconv
    --enable-decoder=h264,vp8,vp9
    --cc=${CMAKE_C_COMPILER}
    --cxx=${CMAKE_CXX_COMPILER}
    --ar=${CMAKE_AR}
    --ranlib=${CMAKE_RANLIB}
    --nm=${CMAKE_NM}
    # FFmpeg's own warnings stay out of the build output
    --extra-cflags=-w)
if(WIN32)
    # CMake names the architecture AMD64 on Windows, which configure doesn't know
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(arch x86_64)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
    endif()
    list(APPEND args --target-os=mingw32 --arch=${arch})
endif()
if(CMAKE_CROSSCOMPILING)
    # configure builds a few helper programs for the machine doing the build, which needs its own C compiler
    find_program(SWR_HOST_CC NAMES clang cc)
    if(NOT SWR_HOST_CC)
        message(FATAL_ERROR "Cross-building FFmpeg needs a C compiler for this machine too (clang or cc). Install one, "
                            "or configure with -DSWR_H264=OFF.")
    endif()
    list(APPEND args --enable-cross-compile --host-cc=${SWR_HOST_CC})
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    find_program(SWR_NASM nasm)
    if(SWR_NASM)
        list(APPEND args --x86asmexe=${SWR_NASM})
    else()
        list(APPEND args --disable-x86asm)
        message(WARNING "NASM was not found: FFmpeg is built without its assembly and decodes movies more slowly")
    endif()
endif()

cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)
include(ExternalProject)
ExternalProject_Add(ffmpeg_build
    SOURCE_DIR "${src}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/ffmpeg-build"
    INSTALL_DIR "${prefix}"
    CONFIGURE_COMMAND "${SWR_FFMPEG_SH}" "${src}/configure" ${args}
    BUILD_COMMAND "${SWR_FFMPEG_MAKE}" -j${jobs}
    INSTALL_COMMAND "${SWR_FFMPEG_MAKE}" install
    BUILD_BYPRODUCTS "${prefix}/lib/libavcodec.a" "${prefix}/lib/libavutil.a"
    # configure and make print a lot: keep it in log files, shown when a step fails
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

# The include folder is filled by the install step, but has to exist for the imported targets at configure time
file(MAKE_DIRECTORY "${prefix}/include")
add_library(FFmpeg::avutil STATIC IMPORTED GLOBAL)
set_target_properties(FFmpeg::avutil PROPERTIES IMPORTED_LOCATION "${prefix}/lib/libavutil.a"
                                                INTERFACE_INCLUDE_DIRECTORIES "${prefix}/include")
add_dependencies(FFmpeg::avutil ffmpeg_build)
if(WIN32)
    # libavutil's random seed uses BCryptGenRandom
    target_link_libraries(FFmpeg::avutil INTERFACE bcrypt)
else()
    target_link_libraries(FFmpeg::avutil INTERFACE Threads::Threads m)
endif()
add_library(FFmpeg::avcodec STATIC IMPORTED GLOBAL)
set_target_properties(FFmpeg::avcodec PROPERTIES IMPORTED_LOCATION "${prefix}/lib/libavcodec.a")
target_link_libraries(FFmpeg::avcodec INTERFACE FFmpeg::avutil)
add_dependencies(FFmpeg::avcodec ffmpeg_build)
