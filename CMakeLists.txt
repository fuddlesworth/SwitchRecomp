# Builds the SwitchRecomp runtime for Linux or FreeBSD, or for Windows with llvm-mingw (natively, or cross-built). The
# game module (<Game>_code.so or .dll) comes from the recompiler (python3 -m switchrecomp), see scripts/recomp.py. The
# third-party libraries are fetched and built with the runtime, see cmake/dependencies.cmake.
#
#   cmake --preset default && cmake --build --preset default        (or windows-cross; see CMakePresets.json)
#   cmake -B build && cmake --build build                           (without the presets)
#
# After each build the runtime is copied next to every recompiled game in games/ (SWR_DEPLOY), so a runtime fix
# reaches the games without recompiling them.
cmake_minimum_required(VERSION 3.25)

# Clang only: the runtime, the generated game code and the third-party libraries are built and tested with one
# compiler. It is the default here, so a plain `cmake -B build` picks it over the system's cc
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER clang++)
endif()
project(SwitchRecomp LANGUAGES C CXX)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "SwitchRecomp builds with Clang only, and this is ${CMAKE_C_COMPILER_ID} "
                        "(${CMAKE_C_COMPILER}). Use a preset, or -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "
                        "in a new build folder.")
endif()

# GNU dialects: the runtime uses GNU extensions (statement expressions, __atomic builtins)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# SDL3 is the system's on Linux and FreeBSD (Wayland and X11), and built with the runtime on Windows
set(SWR_BACKEND sdl3 CACHE STRING "Window and input backend: sdl3 or headless")
set_property(CACHE SWR_BACKEND PROPERTY STRINGS sdl3 headless)
if(NOT SWR_BACKEND MATCHES "^(sdl3|headless)$")
    message(FATAL_ERROR "SWR_BACKEND=${SWR_BACKEND}: the x11, x11es and win32 backends were removed, sdl3 covers them. "
                        "Configure a new build folder, or pass -DSWR_BACKEND=sdl3.")
endif()
# OpenGL is always built in. The sdl3 backend can also have Vulkan, picked at startup (renderer= in the game's .ini)
if(SWR_BACKEND STREQUAL "sdl3")
    option(SWR_VULKAN "Build the Vulkan renderer in too (Vulkan 1.3, the default renderer when the GPU has it)" ON)
else()
    set(SWR_VULKAN OFF)
endif()
option(SWR_H264 "Decode NVDEC video with OpenH264" ON)
option(SWR_LSFG "LSFG frame generation (OpenGL renderer, needs SPIRV-Cross)" ON)
option(SWR_DEPLOY "Copy the runtime next to every recompiled game in SWR_GAMES_DIR after building" ON)
set(SWR_GAMES_DIR "${CMAKE_SOURCE_DIR}/games" CACHE PATH "Folder holding the recompiled games")
# LSFG is part of the OpenGL renderer, which the headless backend doesn't build
if(SWR_LSFG AND SWR_BACKEND STREQUAL "sdl3")
    set(SWR_LSFG_ENABLED ON)
else()
    set(SWR_LSFG_ENABLED OFF)
endif()

# Upstream builds with -O2. RelWithDebInfo keeps that and adds symbols for crash reports, without -DNDEBUG
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
include(cmake/dependencies.cmake)

add_subdirectory(runtime)
if(SWR_VULKAN)
    add_subdirectory(tools)
endif()

# `cmake --install` puts the runtime, hactool and the desktop entry under CMAKE_INSTALL_PREFIX, which is what the
# AppImage is made from (.github/workflows/build.yml)
include(GNUInstallDirs)
install(TARGETS swr-runtime RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET hactool)
    install(TARGETS hactool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NOT WIN32)
    install(FILES dist/switchrecomp.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES dist/switchrecomp.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()
