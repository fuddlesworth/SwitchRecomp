# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Builds the ScarletNX runtime for Linux or FreeBSD, for Windows with llvm-mingw (natively, or cross-built), or for
# macOS and iOS with Xcode's Clang (the macos and ios presets, docs/APPLE.md). The
# game module (<Game>_code.so or .dll) comes from `scarletnx -r`. The
# third-party libraries are fetched and built with the runtime, see cmake/dependencies.cmake.
#
#   cmake --preset default && cmake --build --preset default        (or windows-cross; see CMakePresets.json)
#   cmake -B build && cmake --build build                           (without the presets)
#
# After each build the runtime is copied next to every recompiled game in the data folder's games/ (SNX_DEPLOY), so a
# runtime fix reaches the games without recompiling them.
cmake_minimum_required(VERSION 3.25)

# Clang only: the runtime, the generated game code and the third-party libraries are built and tested with one
# compiler. It is the default here, so a plain `cmake -B build` picks it over the system's cc
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER clang++)
endif()
project(ScarletNX LANGUAGES C CXX)
# Xcode's Clang names itself AppleClang: the same compiler, with Apple's version numbers
if(NOT CMAKE_C_COMPILER_ID MATCHES "^(Apple)?Clang$" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
    message(FATAL_ERROR "ScarletNX builds with Clang only, and this is ${CMAKE_C_COMPILER_ID} "
                        "(${CMAKE_C_COMPILER}). Use a preset, or -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "
                        "in a new build folder.")
endif()
# the iOS and macOS apps' frontend (src/gui/apple.m and dist/apple's Swift, docs/APPLE.md), which the runtime links
# with the menu: the languages are enabled where every target that links them is below
if(APPLE AND (NOT DEFINED SNX_BUILD_GUI OR SNX_BUILD_GUI))
    enable_language(OBJC)
    enable_language(Swift)
endif()

# GNU dialects: the runtime uses GNU extensions (statement expressions, __atomic builtins)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
# The strict ANSI C (RFC #142: the driver, swipc, the build info and their tests) builds as C90 with -pedantic-errors,
# but as C11 on macOS and iOS: Apple's headers use long long where C90 has none (stdint.h's UINT64_C), which
# -pedantic-errors refuses. Still ISO C, without extensions
if(APPLE)
    set(SNX_C90_STANDARD 11)
else()
    set(SNX_C90_STANDARD 90)
endif()

option(SNX_BUILD_NX "Build the Switch Horizon/Maxwell runtime" ON)
option(SNX_BUILD_GNUABI "Build the CPU-only Linux AArch64 runtime" ON)
option(SNX_BUILD_3DS "Build 3DS extraction/memory support" ON)
# Reject stale backend selections instead of silently changing an existing build's behavior.
if(SNX_BUILD_NX AND DEFINED SNX_BACKEND AND NOT SNX_BACKEND STREQUAL "sdl3")
    message(FATAL_ERROR "SNX_BACKEND=${SNX_BACKEND}: SDL3 is now mandatory for the NX runtime. "
                        "Configure a new build folder, or remove SNX_BACKEND with -USNX_BACKEND. "
                        "Use -H/--headless at runtime to run without a window.")
endif()
option(SNX_DEPLOY "Copy the runtime next to every recompiled game in SNX_GAMES_DIR after building" ON)
set(SNX_GAMES_DIR "" CACHE PATH "Folder holding the recompiled games (empty: games/ of the drivers' data folder)")
# the checkout's games/ was the default while the drivers of a build tree worked in it (docs/DEPRECATIONS.md)
if(SNX_GAMES_DIR STREQUAL "${CMAKE_SOURCE_DIR}/games")
    message(STATUS "SNX_GAMES_DIR was the checkout's games/, an older version's default: the data folder's from now on")
    set_property(CACHE SNX_GAMES_DIR PROPERTY VALUE "")
endif()

# Upstream builds with -O2. RelWithDebInfo keeps that and adds symbols for crash reports. It leaves out -DNDEBUG, as
# upstream does: the runtime has no assert(), so this keeps VMA's assertions on.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
option(SNX_TOOLS_ONLY "Build the C90 extraction/recompilation driver without the runtime" OFF)
option(SNX_HACTOOL_ONLY "Build only the native container extractor" OFF)
set(snx_gui_default OFF)
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND NOT SNX_HACTOOL_ONLY)
    set(snx_gui_default ON)
endif()
option(SNX_BUILD_GUI "Build the keyboard-driven SDL3 launcher" ${snx_gui_default})
if(SNX_BUILD_GUI AND (SNX_TOOLS_ONLY OR SNX_HACTOOL_ONLY))
    message(FATAL_ERROR "SNX_BUILD_GUI requires a runtime build, not a tools-only build")
endif()
# Before the sources: the C90 driver of a tools-only build links LZ4 too
include(cmake/dependencies.cmake)
if(SNX_HACTOOL_ONLY)
    return()
endif()
# every local include is a path from src/ ("nx/hle/kernel/ipc.h"); generated and third-party headers come with their
# targets
include_directories("${PROJECT_SOURCE_DIR}/src")
add_subdirectory(src/common)
if(SNX_BUILD_NX)
    add_subdirectory(src/nx/runtime/driver host-tools)
endif()
include(CTest)
include(cmake/build_info.cmake) # the window titles' line; the driver stamps games with its commit
if(SNX_BUILD_3DS)
    add_subdirectory(src/3ds)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(SNX_TOOLS_ONLY AND SNX_BUILD_NX)
    include(GNUInstallDirs)
    install(TARGETS scarletnx RUNTIME DESTINATION bin)
    install(FILES LICENSE LICENSE.apache NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    return()
endif()
if(SNX_BUILD_GNUABI)
    add_subdirectory(src/gnuabi)
endif()
if(SNX_BUILD_GUI)
    add_subdirectory(src/gui)
endif()
# The icon both programs' desktop entries name, in every size (scripts/make-icons.py); Windows has it in the executables
if(NOT WIN32)
    include(GNUInstallDirs)
    install(FILES dist/wimsy.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps COMPONENT desktop)
    install(DIRECTORY dist/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons COMPONENT desktop)
endif()
if(NOT SNX_BUILD_NX)
    include(GNUInstallDirs)
    install(FILES LICENSE LICENSE.apache NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    return()
endif()
if(BUILD_TESTING AND TARGET hactool_lib)
    add_executable(snx-test-hactool-decrypted tests/hactool_decrypted.c)
    snx_test_target(snx-test-hactool-decrypted 11)
    target_link_libraries(snx-test-hactool-decrypted PRIVATE hactool_lib)
    add_test(NAME hactool_decrypted COMMAND snx-test-hactool-decrypted)
    # the storages cmake/hactool-libhac.patch ports from LibHac, in-process and through the executable
    add_executable(snx-test-hactool-layers tests/hactool_layers.c)
    snx_test_target(snx-test-hactool-layers 11)
    target_link_libraries(snx-test-hactool-layers PRIVATE hactool_lib)
    add_test(NAME hactool_layers COMMAND snx-test-hactool-layers $<TARGET_FILE:hactool>)
    snx_sanitizer_path() # tests/CMakeLists.txt
endif()
add_subdirectory(src/nx)
add_subdirectory(tools)

# `cmake --install` puts the runtime, hactool, the desktop entry and the licenses under CMAKE_INSTALL_PREFIX, which is
# what the AppImage and the CI artifacts are made from (.github/workflows/build.yml)
include(GNUInstallDirs)
if(IOS) # the app itself (dist/apple), which scripts/ios-app.sh packs
    install(TARGETS scarletnx BUNDLE DESTINATION .)
elseif(NOT ANDROID) # src/nx puts Android's in the app's libraries
    install(TARGETS scarletnx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET hactool AND ANDROID)
    install(TARGETS hactool RUNTIME DESTINATION lib/${ANDROID_ABI})
elseif(TARGET hactool)
    install(TARGETS hactool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NOT WIN32)
    install(FILES dist/scarletnx.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT desktop)
endif()
if(ANDROID) # the app's launcher icon (dist/android)
    install(FILES dist/icons/hicolor/256x256/apps/wimsy.png DESTINATION res/mipmap-xxxhdpi RENAME ic_launcher.png)
endif()

# The licenses go with the binaries: the fork's (GPL-3.0), MZ Labs' (Apache 2.0, with its NOTICE), and those of the
# libraries built into the runtime and hactool, each in a folder named after it
set(licenses "${CMAKE_INSTALL_DATADIR}/licenses/scarletnx")
install(FILES LICENSE LICENSE.apache NOTICE DESTINATION ${licenses})
# FFmpeg's with the change made to it (cmake/ffmpeg-vp9-nvdec.patch), which its LGPL wants carried with the binaries
install(FILES "${FFmpeg_SOURCE_DIR}/COPYING.LGPLv2.1" "${FFmpeg_SOURCE_DIR}/LICENSE.md" cmake/ffmpeg-vp9-nvdec.patch
        DESTINATION ${licenses}/FFmpeg)
install(FILES LICENSE.apache DESTINATION ${licenses}/OpenSans RENAME LICENSE) # the shared fonts' stand-in (pl.c)
install(FILES "${Vulkan-Headers_SOURCE_DIR}/LICENSE.md" DESTINATION ${licenses}/Vulkan-Headers)
install(DIRECTORY "${Vulkan-Headers_SOURCE_DIR}/LICENSES" DESTINATION ${licenses}/Vulkan-Headers)
install(FILES "${volk_SOURCE_DIR}/LICENSE.md" DESTINATION ${licenses}/volk)
install(FILES "${VulkanMemoryAllocator_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/VulkanMemoryAllocator)
install(FILES "${OpenSSL_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/OpenSSL)
# lib/LICENSE is the library's BSD-2-Clause. The top-level LICENSE covers the rest of LZ4 (GPL-2.0-or-later), none
# of which is built here
install(FILES "${lz4_SOURCE_DIR}/lib/LICENSE" DESTINATION ${licenses}/lz4)
if(SNX_SDL3_FROM_SOURCE)
    install(FILES "${SDL3_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/SDL3)
endif()
if(TARGET hactool)
    install(FILES "${hactool_SOURCE_DIR}/LICENSE" DESTINATION ${licenses}/hactool)
    # cJSON's license is the comment at the top of its header
    file(READ "${hactool_SOURCE_DIR}/cJSON.h" cjson)
    string(FIND "${cjson}" "*/" cjson_end)
    string(SUBSTRING "${cjson}" 0 ${cjson_end} cjson)
    file(WRITE "${CMAKE_BINARY_DIR}/cJSON-LICENSE.txt" "${cjson}*/\n")
    install(FILES "${CMAKE_BINARY_DIR}/cJSON-LICENSE.txt" DESTINATION ${licenses}/hactool/cJSON RENAME LICENSE.txt)
endif()
