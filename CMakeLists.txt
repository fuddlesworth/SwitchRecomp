# Builds the SwitchRecomp runtime for Linux, or for Windows with MinGW-w64 (natively, or cross-built). The
# game module (<Game>_code.so or .dll) comes from the recompiler (python3 -m switchrecomp), see scripts/recomp.py. Run scripts/bootstrap.py
# once first: it builds OpenH264 and glslang (and SDL3 for Windows) into deps/linux or deps/win64.
#
#   cmake --preset gcc && cmake --build --preset gcc        (or clang, or mingw-cross; see CMakePresets.json)
#
# After each build the runtime is copied next to every recompiled game in games/ (SWR_DEPLOY), so a runtime fix
# reaches the games without recompiling them.
cmake_minimum_required(VERSION 3.20)
project(SwitchRecomp LANGUAGES C CXX)

# GNU dialects: the runtime uses GNU extensions (statement expressions, __atomic builtins)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if(WIN32)
    set(SWR_BACKEND sdl3 CACHE STRING "Window and input backend: sdl3 (SDL3 from scripts/bootstrap.py) or headless")
    set_property(CACHE SWR_BACKEND PROPERTY STRINGS sdl3 headless)
    set(DEPS "${CMAKE_SOURCE_DIR}/deps/win64")
else()
    set(SWR_BACKEND sdl3 CACHE STRING "Window and input backend: sdl3 (Wayland and X11) or headless")
    set_property(CACHE SWR_BACKEND PROPERTY STRINGS sdl3 headless)
    set(DEPS "${CMAKE_SOURCE_DIR}/deps/linux")
endif()
if(NOT SWR_BACKEND MATCHES "^(sdl3|headless)$")
    message(FATAL_ERROR "SWR_BACKEND=${SWR_BACKEND}: the x11, x11es and win32 backends were removed, sdl3 covers them. "
                        "Configure a new build folder, or pass -DSWR_BACKEND=sdl3.")
endif()
# The renderer is Vulkan (1.3), which the sdl3 backend draws with. The headless backend has none.
if(SWR_BACKEND STREQUAL "sdl3")
    set(SWR_VULKAN ON)
else()
    set(SWR_VULKAN OFF)
endif()
option(SWR_H264 "Decode NVDEC video with OpenH264 (deps/)" ON)
option(SWR_DEPLOY "Copy the runtime next to every recompiled game in SWR_GAMES_DIR after building" ON)
set(SWR_GAMES_DIR "${CMAKE_SOURCE_DIR}/games" CACHE PATH "Folder holding the recompiled games")

# Upstream builds with -O2. RelWithDebInfo keeps that and adds symbols for crash reports, without -DNDEBUG
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

function(swr_require_dep file option)
    if(NOT EXISTS "${DEPS}/${file}")
        message(FATAL_ERROR "${DEPS}/${file} is missing. Run scripts/bootstrap.py (scripts\\bootstrap.ps1 on Windows), or configure with -D${option}=OFF.")
    endif()
endfunction()

add_subdirectory(runtime)
if(SWR_VULKAN)
    add_subdirectory(tools)
endif()
