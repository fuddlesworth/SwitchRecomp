# The runtime: one object library per component, linked into swr-runtime. Each component sees runtime/ as its include
# root (#include "kernel/kernel.h"), and the flags and third-party headers every component shares (swr_common).
#
#   core      startup, options, the game module, crash reports      kernel    threads, sync, memory, IPC
#   hle       the services the game talks to                         video     NVDEC and VIC (host1x)
#   gpu       the GPU front end: GPFIFO, engines, the renderer interface
#   shader    Maxwell shaders to GLSL                                 spirv     GLSL to SPIR-V (glslang)
#   gl / vk / null   the renderers                                    platform  the OS layer and a window backend

add_library(swr_common INTERFACE)
target_include_directories(swr_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(swr_common SYSTEM INTERFACE "${DEPS}/include")
# Every function and global used across files is declared once, in the header of the file that defines it, and
# everything else is static: a declaration that disagrees with the definition is then a compile error instead of a
# miscompile. Third-party sources (third_party/) keep their own warnings off.
target_compile_options(swr_common INTERFACE -fno-strict-aliasing
                       $<$<COMPILE_LANGUAGE:C>:-Werror=missing-prototypes -Werror=missing-variable-declarations>
                       $<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:GNU>>:-Werror=nested-externs>)
if(SWR_VULKAN)
    target_compile_definitions(swr_common INTERFACE SWR_VULKAN VK_NO_PROTOTYPES)
    target_include_directories(swr_common SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include"
                                                           "${CMAKE_SOURCE_DIR}/third_party/volk"
                                                           "${CMAKE_SOURCE_DIR}/third_party/VulkanMemoryAllocator/include")
endif()
if(SWR_H264)
    swr_require_dep(lib/libdecoder.a SWR_H264)
else()
    target_compile_definitions(swr_common INTERFACE SWR_NO_H264)
endif()

function(swr_component name)
    add_library(swr_${name} OBJECT ${ARGN})
    target_link_libraries(swr_${name} PUBLIC swr_common)
endfunction()

swr_component(core core/main.c core/entry.c core/loader.c)
swr_component(kernel kernel/kernel.c kernel/mem.c kernel/ipc.c)
swr_component(hle hle/services.c hle/fs.c hle/hid.c hle/vi.c hle/nvdrv.c)
swr_component(video video/host1x.c)
swr_component(gpu gpu/gpu.c gpu/engines.c gpu/maxwell3d.c $<$<BOOL:${SWR_VULKAN}>:gpu/renderer.c>)

set(platform platform/plat.c platform/backend/${SWR_BACKEND}.c)
if(WIN32 AND SWR_BACKEND MATCHES "^(win32|sdl3)$")
    list(APPEND platform platform/recorder_win32.c) # F7 video recorder, Media Foundation
endif()
swr_component(platform ${platform})

set(components core kernel hle video gpu platform)
if(SWR_BACKEND STREQUAL "headless")
    swr_component(null gpu/null/null.c)
    list(APPEND components null)
else()
    swr_component(shader gpu/shader/shader.c gpu/texture/astc.c)
    set(gl gpu/gl/renderer.c gpu/gl/formats.c gpu/gl/texture.c gpu/gl/programs.c gpu/gl/framebuffer.c gpu/gl/draw.c
           gpu/gl/trace.c gpu/gl/capture.c gpu/gl/framegen.c gpu/gl/compute.c gpu/gl/load.c gpu/gl/glq.c gpu/gl/glq_gen.c
           "${CMAKE_SOURCE_DIR}/third_party/bcn/bcn.cpp" "${CMAKE_SOURCE_DIR}/third_party/bcn/bcdec.cpp")
    if(SWR_BACKEND STREQUAL "x11es")
        list(APPEND gl gpu/gl/overlay.c)
    endif()
    if(SWR_LSFG AND SWR_BACKEND MATCHES "^(sdl3|x11|win32)$")
        swr_require_dep(lib/libspvc.a SWR_LSFG)
        list(APPEND gl gpu/gl/lsfg.c gpu/gl/lsfg_spvc.cpp)
    else()
        list(APPEND gl gpu/gl/lsfg_stub.c)
    endif()
    swr_component(gl ${gl})
    target_include_directories(swr_gl SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/bcn" "${DEPS}/include/spirv_cross")
    list(APPEND components shader gl)
endif()
if(SWR_VULKAN)
    swr_require_dep(lib/libglslang-all.a SWR_VULKAN)
    swr_component(spirv gpu/vk/spirv.c)
    # volk loads Vulkan through the window backend's loader, so nothing links against libvulkan
    swr_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                  gpu/vk/buffer.c gpu/vk/pipeline.c gpu/vk/render.c gpu/vk/compute.c gpu/vk/vma.cpp
                  "${CMAKE_SOURCE_DIR}/third_party/volk/volk.c")
    list(APPEND components spirv vk)
endif()
# third-party code (vma.cpp is VMA's implementation)
set_source_files_properties("${CMAKE_SOURCE_DIR}/third_party/bcn/bcn.cpp" "${CMAKE_SOURCE_DIR}/third_party/bcn/bcdec.cpp"
                            "${CMAKE_SOURCE_DIR}/third_party/volk/volk.c" gpu/vk/vma.cpp PROPERTIES COMPILE_OPTIONS -w)

add_executable(swr-runtime)
list(TRANSFORM components PREPEND swr_)
target_link_libraries(swr-runtime PRIVATE ${components})
# OpenH264, SPIRV-Cross and glslang are C++: link with the C++ driver. It goes in the build folder itself, where
# scripts/recomp.py and CI look for it
set_target_properties(swr-runtime PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    # Static, so the runtime runs without the MinGW DLLs next to it
    target_link_libraries(swr-runtime PRIVATE gdi32 winmm user32 opengl32 comdlg32 shell32)
    target_link_options(swr-runtime PRIVATE -static)
    if(SWR_BACKEND MATCHES "^(win32|sdl3)$")
        target_link_libraries(swr-runtime PRIVATE mfplat mfreadwrite mfuuid ole32)
        target_link_options(swr-runtime PRIVATE -mwindows)
    else()
        target_link_options(swr-runtime PRIVATE -mconsole)
    endif()
else()
    target_link_libraries(swr-runtime PRIVATE pthread dl m)
    target_link_options(swr-runtime PRIVATE -rdynamic)
endif()
if(SWR_H264)
    target_link_libraries(swr-runtime PRIVATE "${DEPS}/lib/libdecoder.a" "${DEPS}/lib/libcommon.a")
endif()
if(TARGET swr_gl AND SWR_LSFG AND SWR_BACKEND MATCHES "^(sdl3|x11|win32)$")
    target_link_libraries(swr-runtime PRIVATE "${DEPS}/lib/libspvc.a")
endif()
if(SWR_VULKAN)
    target_link_libraries(swr-runtime PRIVATE "${DEPS}/lib/libglslang-all.a")
endif()

if(SWR_BACKEND STREQUAL "sdl3" AND WIN32)
    # static, from scripts/bootstrap.py
    if(NOT EXISTS "${DEPS}/sdl3/lib/cmake/SDL3/SDL3Config.cmake")
        message(FATAL_ERROR "${DEPS}/sdl3 is missing. Run scripts/bootstrap.py (scripts\\bootstrap.ps1 on Windows), or configure with -DSWR_BACKEND=win32.")
    endif()
    find_package(SDL3 REQUIRED CONFIG PATHS "${DEPS}/sdl3" NO_DEFAULT_PATH)
    set(sdl SDL3::SDL3-static)
elseif(SWR_BACKEND STREQUAL "sdl3")
    find_package(SDL3 REQUIRED CONFIG)
    set(sdl SDL3::SDL3)
endif()
if(sdl)
    target_link_libraries(swr_platform PRIVATE ${sdl})
    target_link_libraries(swr-runtime PRIVATE ${sdl})
elseif(SWR_BACKEND STREQUAL "x11")
    target_link_libraries(swr-runtime PRIVATE X11 GL)
elseif(SWR_BACKEND STREQUAL "x11es")
    target_link_libraries(swr-runtime PRIVATE X11 EGL)
endif()

if(SWR_DEPLOY)
    add_custom_command(TARGET swr-runtime POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=$<TARGET_FILE:swr-runtime>" "-DGAMES=${SWR_GAMES_DIR}" "-DWINDOWS=${WIN32}"
                -P "${CMAKE_SOURCE_DIR}/cmake/deploy.cmake"
        VERBATIM)
endif()
