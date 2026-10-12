# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# The runtime: one object library per component, linked into swr-runtime. Each component sees runtime/ as its include
# root (#include "kernel/kernel.h"), and the flags and third-party headers every component shares (swr_common).
#
#   core      startup, options, the game module, crash reports      kernel    threads, sync, memory, IPC
#   hle       the services the game talks to                         video     NVDEC and VIC (host1x)
#   gpu       the GPU front end: GPFIFO, engines, the renderer interface
#   shader    Maxwell shaders to GLSL                                 spirv     GLSL to SPIR-V (glslang)
#   vk / null        the renderers                                    platform  the OS layer and a window backend

add_library(swr_common INTERFACE)
target_include_directories(swr_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
# Every function and global used across files is declared once, in the header of the file that defines it, and
# everything else is static: a declaration that disagrees with the definition is then a compile error instead of a
# miscompile. Third-party sources keep their own warnings off.
target_compile_options(swr_common INTERFACE -fno-strict-aliasing
                       $<$<COMPILE_LANGUAGE:C>:-Werror=missing-prototypes -Werror=missing-variable-declarations>)
if(SWR_VULKAN)
    target_compile_definitions(swr_common INTERFACE SWR_VULKAN VK_NO_PROTOTYPES)
    target_link_libraries(swr_common INTERFACE Vulkan::Headers)
    target_include_directories(swr_common BEFORE INTERFACE "${volk_SOURCE_DIR}"
                                                    "${VulkanMemoryAllocator_SOURCE_DIR}/include")
endif()
if(NOT SWR_H264)
    target_compile_definitions(swr_common INTERFACE SWR_NO_H264)
endif()

function(swr_component name)
    add_library(swr_${name} OBJECT ${ARGN})
    target_link_libraries(swr_${name} PUBLIC swr_common)
endfunction()

swr_component(core core/main.c core/entry.c core/loader.c core/rtapi.c core/clock.c core/crash.c core/options.c
              core/present.c)
swr_component(kernel kernel/kernel.c kernel/thread.c kernel/sync.c kernel/svc.c kernel/svc_mem.c kernel/debug.c
              kernel/mem.c kernel/ipc.c)
swr_component(hle hle/services.c hle/audren.c hle/pl.c hle/sm.c hle/am.c hle/set.c hle/time.c hle/acc.c hle/audio.c
              hle/misc.c hle/fs.c hle/hid.c hle/vi.c hle/nvdrv.c)
swr_component(video video/host1x.c video/nvdec.c video/vic.c)
if(SWR_H264)
    target_link_libraries(swr_video PRIVATE FFmpeg::avcodec)
    # hwopus (hle/audio.c) decodes Opus with FFmpeg too
    target_link_libraries(swr_hle PRIVATE FFmpeg::avcodec)
endif()
swr_component(gpu gpu/gpu.c gpu/engines.c gpu/mme.c gpu/compute.c gpu/fermi2d.c gpu/dma.c gpu/maxwell3d.c gpu/renderer.c)

set(platform platform/plat.c platform/unattended.c platform/backend/${SWR_BACKEND}.c)
if(WIN32)
    list(APPEND platform platform/plat_win32.c platform/profiler_win32.c)
else()
    list(APPEND platform platform/plat_posix.c)
endif()
swr_component(platform ${platform})

set(components core kernel hle video gpu platform)
if(SWR_BACKEND STREQUAL "headless")
    swr_component(null gpu/null/null.c)
    list(APPEND components null)
endif()
if(SWR_VULKAN)
    swr_component(shader gpu/shader/shader.c gpu/shader/decode.c gpu/shader/translate.c gpu/shader/corpus.c
                  gpu/texture/astc.c)
    swr_component(spirv gpu/vk/spirv.c)
    target_link_libraries(swr_spirv PUBLIC glslang::glslang glslang::glslang-default-resource-limits)
    # volk loads Vulkan through the window backend's loader, so nothing links against libvulkan
    swr_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                  gpu/vk/buffer.c gpu/vk/pipeline.c gpu/vk/render.c gpu/vk/compute.c gpu/vk/vma.cpp
                  "${volk_SOURCE_DIR}/volk.c")
    list(APPEND components shader spirv vk)
    # third-party code (vma.cpp is VMA's implementation)
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" gpu/vk/vma.cpp PROPERTIES COMPILE_OPTIONS -w)
endif()

add_executable(swr-runtime)
list(TRANSFORM components PREPEND swr_)
target_link_libraries(swr-runtime PRIVATE ${components})
# glslang is C++: link with the C++ driver. It goes in the build folder itself, where
# scripts/recomp.py and CI look for it
set_target_properties(swr-runtime PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    # Static, so the runtime runs without the MinGW DLLs next to it. user32 has the backends' message boxes:
    # winmm is loaded at run time, and the static SDL3 brings the libraries it needs
    target_link_libraries(swr-runtime PRIVATE user32)
    # The application manifest (platform/runtime.manifest): UTF-8 as the process's code page, long paths
    enable_language(RC)
    target_sources(swr-runtime PRIVATE platform/runtime.rc)
    target_link_options(swr-runtime PRIVATE -static)
    if(SWR_BACKEND STREQUAL "sdl3")
        target_link_options(swr-runtime PRIVATE -mwindows)
    else()
        target_link_options(swr-runtime PRIVATE -mconsole)
    endif()
else()
    # No -rdynamic: the game module gets the runtime through RtAPI and imports nothing from it, and exporting every
    # global would let a library loaded later (a GPU driver) bind its own symbols to ours by name
    target_link_libraries(swr-runtime PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# SDL3 is the system's, or on Windows built with the runtime (cmake/dependencies.cmake)
if(SWR_BACKEND STREQUAL "sdl3")
    target_link_libraries(swr_platform PRIVATE SDL3::SDL3)
    target_link_libraries(swr-runtime PRIVATE SDL3::SDL3)
endif()

if(SWR_DEPLOY)
    add_custom_command(TARGET swr-runtime POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=$<TARGET_FILE:swr-runtime>" "-DGAMES=${SWR_GAMES_DIR}" "-DWINDOWS=${WIN32}"
                -P "${CMAKE_SOURCE_DIR}/cmake/deploy.cmake"
        VERBATIM)
endif()
