# Builds the SwitchRecomp runtime for Linux. The game module (<Game>_code.so) comes from switchrecomp.py, see
# scripts/recomp.sh. Run scripts/bootstrap.sh once first: it builds OpenH264 and SPIRV-Cross into deps/linux.
#
#   cmake --preset gcc && cmake --build --preset gcc        (or clang; see CMakePresets.json)
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

set(SWR_BACKEND x11 CACHE STRING "Window and input backend: x11, x11es or headless")
set_property(CACHE SWR_BACKEND PROPERTY STRINGS x11 x11es headless)
option(SWR_H264 "Decode NVDEC video with OpenH264 (deps/linux)" ON)
option(SWR_LSFG "LSFG frame generation, needs SPIRV-Cross (deps/linux)" ON)
option(SWR_DEPLOY "Copy the runtime next to every recompiled game in SWR_GAMES_DIR after building" ON)
set(SWR_GAMES_DIR "${CMAKE_SOURCE_DIR}/games" CACHE PATH "Folder holding the recompiled games")

# Upstream builds with -O2. RelWithDebInfo keeps that and adds symbols for crash reports, without -DNDEBUG
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

set(RT "${CMAKE_SOURCE_DIR}/runtime")
set(DEPS "${CMAKE_SOURCE_DIR}/deps/linux")

# The same source list as switchrecomp.py's build_runtime
set(sources plat mem kernel ipc svc_core svc_hid svc_fs svc_vi nvdrv gpu gpu_engines gpu_3d loader main host1x entry)
if(SWR_BACKEND STREQUAL "headless")
    list(APPEND sources gpu_nogl)
else()
    list(APPEND sources gpu_gl gl_load shader astc glq glq_gen)
    if(SWR_BACKEND STREQUAL "x11es")
        list(APPEND sources overlay)
    endif()
endif()
list(APPEND sources backend_${SWR_BACKEND})
list(TRANSFORM sources PREPEND "${RT}/")
list(TRANSFORM sources APPEND ".c")

add_executable(swr-runtime ${sources})
target_include_directories(swr-runtime PRIVATE "${RT}")
target_link_libraries(swr-runtime PRIVATE pthread dl m)
target_link_options(swr-runtime PRIVATE -rdynamic)
# Upstream's flags. -w hides the warnings for now; turning them back on is in ROADMAP.md (Phase 2)
target_compile_options(swr-runtime PRIVATE -w -fno-strict-aliasing)

if(NOT SWR_BACKEND STREQUAL "headless")
    target_sources(swr-runtime PRIVATE "${RT}/bcn/bcn.cpp" "${RT}/bcn/bcdec.cpp")
    target_include_directories(swr-runtime PRIVATE "${RT}/bcn")
endif()

if(SWR_BACKEND STREQUAL "x11")
    target_link_libraries(swr-runtime PRIVATE X11 GL)
elseif(SWR_BACKEND STREQUAL "x11es")
    target_link_libraries(swr-runtime PRIVATE X11 EGL)
endif()

function(swr_require_dep file option)
    if(NOT EXISTS "${DEPS}/${file}")
        message(FATAL_ERROR "${DEPS}/${file} is missing. Run scripts/bootstrap.sh, or configure with -D${option}=OFF.")
    endif()
endfunction()

if(SWR_H264)
    swr_require_dep(lib/libdecoder.a SWR_H264)
    target_include_directories(swr-runtime PRIVATE "${DEPS}/include")
    target_link_libraries(swr-runtime PRIVATE "${DEPS}/lib/libdecoder.a" "${DEPS}/lib/libcommon.a")
else()
    target_compile_definitions(swr-runtime PRIVATE SWR_NO_H264)
endif()

if(SWR_LSFG AND SWR_BACKEND STREQUAL "x11")
    swr_require_dep(lib/libspvc.a SWR_LSFG)
    target_sources(swr-runtime PRIVATE "${RT}/lsfg.c" "${RT}/lsfg_spvc.cpp")
    target_include_directories(swr-runtime PRIVATE "${DEPS}/include/spirv_cross")
    target_link_libraries(swr-runtime PRIVATE "${DEPS}/lib/libspvc.a")
elseif(NOT SWR_BACKEND STREQUAL "headless")
    target_sources(swr-runtime PRIVATE "${RT}/lsfg_stub.c")
endif()

if(SWR_DEPLOY)
    add_custom_command(TARGET swr-runtime POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=$<TARGET_FILE:swr-runtime>" "-DGAMES=${SWR_GAMES_DIR}"
                -P "${CMAKE_SOURCE_DIR}/cmake/deploy.cmake"
        VERBATIM)
endif()
