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
# Every function and global used across files is declared once, in the header of the file that defines it, and
# everything else is static: a declaration that disagrees with the definition is then a compile error instead of a
# miscompile. Third-party sources keep their own warnings off.
target_compile_options(swr_common INTERFACE -fno-strict-aliasing
                       $<$<COMPILE_LANGUAGE:C>:-Werror=missing-prototypes -Werror=missing-variable-declarations>)
if(SWR_VULKAN)
    target_compile_definitions(swr_common INTERFACE SWR_VULKAN VK_NO_PROTOTYPES)
    target_link_libraries(swr_common INTERFACE Vulkan::Headers)
    target_include_directories(swr_common SYSTEM INTERFACE "${volk_SOURCE_DIR}"
                                                           "${VulkanMemoryAllocator_SOURCE_DIR}/include")
endif()
if(NOT SWR_H264)
    target_compile_definitions(swr_common INTERFACE SWR_NO_H264)
endif()
# The GL headers come from libglvnd, which FreeBSD installs under /usr/local, outside the compiler's search path
if(NOT WIN32)
    find_path(SWR_GL_INCLUDE_DIR GL/glcorearb.h REQUIRED)
    target_include_directories(swr_common SYSTEM INTERFACE "${SWR_GL_INCLUDE_DIR}")
endif()

function(swr_component name)
    add_library(swr_${name} OBJECT ${ARGN})
    target_link_libraries(swr_${name} PUBLIC swr_common)
endfunction()

swr_component(core core/main.c core/entry.c core/loader.c core/rtapi.c core/clock.c core/crash.c core/options.c
              core/present.c)
swr_component(kernel kernel/kernel.c kernel/thread.c kernel/sync.c kernel/svc.c kernel/svc_mem.c kernel/debug.c
              kernel/mem.c kernel/ipc.c)
swr_component(hle hle/services.c hle/audren.c hle/pl.c hle/sm.c hle/am.c hle/set.c hle/time.c hle/acc.c hle/audio.c hle/misc.c hle/fs.c
              hle/hid.c hle/vi.c hle/nvdrv.c)
swr_component(video video/host1x.c)
if(SWR_H264)
    target_link_libraries(swr_video PRIVATE openh264)
endif()
swr_component(gpu gpu/gpu.c gpu/engines.c gpu/maxwell3d.c $<$<BOOL:${SWR_VULKAN}>:gpu/renderer.c>)

set(platform platform/plat.c platform/backend/${SWR_BACKEND}.c)
if(WIN32)
    list(APPEND platform platform/plat_win32.c platform/profiler_win32.c)
else()
    list(APPEND platform platform/plat_posix.c)
endif()
if(WIN32 AND SWR_BACKEND STREQUAL "sdl3")
    list(APPEND platform platform/recorder_win32.c) # F7 video recorder, Media Foundation
endif()
swr_component(platform ${platform})

set(components core kernel hle video gpu platform)
if(SWR_BACKEND STREQUAL "headless")
    swr_component(null gpu/null/null.c)
    list(APPEND components null)
else()
    swr_component(shader gpu/shader/shader.c gpu/shader/decode.c gpu/shader/translate.c gpu/shader/corpus.c
                  gpu/texture/astc.c)
    set(gl gpu/gl/renderer.c gpu/gl/formats.c gpu/gl/texture.c gpu/gl/programs.c gpu/gl/framebuffer.c gpu/gl/draw.c
           gpu/gl/trace.c gpu/gl/capture.c gpu/gl/framegen.c gpu/gl/compute.c gpu/gl/load.c gpu/gl/glq.c gpu/gl/glq_gen.c
           "${CMAKE_SOURCE_DIR}/third_party/bcn/bcn.cpp" "${CMAKE_SOURCE_DIR}/third_party/bcn/bcdec.cpp")
    if(SWR_LSFG_ENABLED)
        list(APPEND gl gpu/gl/lsfg.c gpu/gl/lsfg_spvc.cpp)
    else()
        list(APPEND gl gpu/gl/lsfg_stub.c)
    endif()
    swr_component(gl ${gl})
    target_include_directories(swr_gl SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/bcn")
    if(SWR_LSFG_ENABLED)
        target_link_libraries(swr_gl PRIVATE spirv-cross-glsl)
    endif()
    list(APPEND components shader gl)
endif()
if(SWR_VULKAN)
    swr_component(spirv gpu/vk/spirv.c)
    target_link_libraries(swr_spirv PUBLIC glslang::glslang glslang::glslang-default-resource-limits)
    # volk loads Vulkan through the window backend's loader, so nothing links against libvulkan
    swr_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                  gpu/vk/buffer.c gpu/vk/pipeline.c gpu/vk/render.c gpu/vk/compute.c gpu/vk/vma.cpp
                  "${volk_SOURCE_DIR}/volk.c")
    list(APPEND components spirv vk)
endif()
# third-party code (vma.cpp is VMA's implementation)
set_source_files_properties("${CMAKE_SOURCE_DIR}/third_party/bcn/bcn.cpp" "${CMAKE_SOURCE_DIR}/third_party/bcn/bcdec.cpp"
                            "${volk_SOURCE_DIR}/volk.c" gpu/vk/vma.cpp PROPERTIES COMPILE_OPTIONS -w)

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
    if(SWR_BACKEND STREQUAL "sdl3")
        target_link_libraries(swr-runtime PRIVATE mfplat mfreadwrite mfuuid ole32)
        target_link_options(swr-runtime PRIVATE -mwindows)
    else()
        target_link_options(swr-runtime PRIVATE -mconsole)
    endif()
else()
    # No -rdynamic: the game module gets the runtime through RtAPI and imports nothing from it, and exporting every
    # global would let a library loaded later (a GL driver) bind its own symbols to ours by name
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
