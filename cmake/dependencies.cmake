# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# The third-party libraries, fetched at configure time with CPM.cmake (https://github.com/cpm-cmake/CPM.cmake). Each
# package is pinned to a release archive and its SHA-256, and downloaded once into CPM_SOURCE_CACHE (.cache/cpm in the
# repository by default, shared by every build folder). Configuring offline works once the cache is filled.
#
# Bumping a package: change its version, URL and hash together, the hash taken from the release asset's digest or from
# a download checked by hand, never from what the next configure happens to fetch (swr-build skill).

set(CPM_VERSION 0.43.2)
set(CPM_HASH 49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa)
if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cache/cpm" CACHE PATH "Where CPM.cmake keeps downloaded packages")
endif()
if(DEFINED CPM_SOURCE_CACHE)
    set(cpm_file "${CPM_SOURCE_CACHE}/cpm/CPM_${CPM_VERSION}.cmake")
else()
    set(cpm_file "$ENV{CPM_SOURCE_CACHE}/cpm/CPM_${CPM_VERSION}.cmake")
endif()
# Checked on every configure, not only after downloading it: a download that failed its check or broke off leaves a
# file behind, and the next configure would include it. The download goes to a .part file, renamed once checked
set(cpm_have "")
if(EXISTS "${cpm_file}")
    file(SHA256 "${cpm_file}" cpm_have)
endif()
if(NOT cpm_have STREQUAL CPM_HASH)
    file(DOWNLOAD "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
         "${cpm_file}.part" EXPECTED_HASH SHA256=${CPM_HASH})
    file(RENAME "${cpm_file}.part" "${cpm_file}")
endif()
include("${cpm_file}")

# Packages whose headers a system may also have (Vulkan, glslang) are added without SYSTEM. The compiler searches every
# -I folder before any -isystem one, so as -isystem they lost to an -I/usr/local/include in CFLAGS, usual on FreeBSD,
# and the build mixed in the system's copies. Third-party code builds with its own flags and stays out of the warning
# output
function(swr_quiet)
    foreach(target IN LISTS ARGN)
        target_compile_options(${target} PRIVATE -w)
    endforeach()
endfunction()

if(SWR_H264)
    # FFmpeg builds with its own configure and make, which cmake/ffmpeg.cmake runs
    CPMAddPackage(NAME FFmpeg VERSION 9.0.2
                  URL https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz
                  URL_HASH SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
                  DOWNLOAD_ONLY YES)
    include("${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake")
endif()

if(SWR_VULKAN)
    CPMAddPackage(NAME Vulkan-Headers VERSION 1.4.365
                  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.365.tar.gz
                  URL_HASH SHA256=ed832b3292cce324ebba6522fd2d3004bf1b797fa49bfbcad0b302ff23d56abc
                  EXCLUDE_FROM_ALL YES)
    # volk.c is compiled into the vk component, VMA is a header
    CPMAddPackage(NAME volk VERSION 1.4.363
                  URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
                  URL_HASH SHA256=1547d8d74395d4048fb3f4a6313da56db626e89b55db238d0d7e8944c8a645f3
                  DOWNLOAD_ONLY YES)
    CPMAddPackage(NAME VulkanMemoryAllocator VERSION 3.4.0
                  URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
                  URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1
                  DOWNLOAD_ONLY YES)
    # The GLSL front end and SPIR-V back end, without the optimizer (SPIRV-Tools) or HLSL
    CPMAddPackage(NAME glslang VERSION 16.6.0
                  URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
                  URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2
                  EXCLUDE_FROM_ALL YES
                  OPTIONS "BUILD_SHARED_LIBS OFF" "ENABLE_OPT OFF" "ENABLE_HLSL OFF" "ENABLE_GLSLANG_BINARIES OFF"
                          "GLSLANG_TESTS OFF" "BUILD_EXTERNAL OFF" "ENABLE_PCH OFF" "GLSLANG_ENABLE_INSTALL OFF"
                          "ENABLE_SPVREMAPPER OFF")
    swr_quiet(glslang glslang-default-resource-limits)
endif()

if(SWR_BACKEND STREQUAL "sdl3")
    if(WIN32)
        # Static on Windows, so the runtime needs no DLL next to it
        CPMAddPackage(NAME SDL3 VERSION 3.4.18
                      URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
                      URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
                      EXCLUDE_FROM_ALL YES SYSTEM YES
                      OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF" "SDL_TESTS OFF"
                              "SDL_EXAMPLES OFF" "SDL_INSTALL OFF")
        swr_quiet(SDL3-static)
    else()
        # The system's SDL3 comes with the Wayland, X11 and input libraries it loads
        find_package(SDL3 REQUIRED CONFIG)
    endif()
endif()

# hactool runs on the machine doing the extraction (scripts/extract.py), so a cross build leaves it out
if(NOT CMAKE_CROSSCOMPILING)
    CPMAddPackage(NAME hactool VERSION 1.4.0.13
                  URL https://github.com/SciresM/hactool/archive/1d64a83450e025622f3468c28fc4164dad2c5ef6.tar.gz
                  URL_HASH SHA256=f7148daad624ba909f5ae253210734b1a0ce19e89a7914b6cf71401f7134ebbc
                  DOWNLOAD_ONLY YES)
    include("${CMAKE_SOURCE_DIR}/cmake/hactool.cmake")
endif()
