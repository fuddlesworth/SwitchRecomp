# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The runtime: one object library per component, linked into scarletnx. Each component sees src/nx/ as its include
# root (#include "hle/kernel/kernel.h"), and the flags and third-party headers every component shares (snx_nx_flags).
#
#   core      startup, options, the game module, crash reports      kernel    threads, sync, memory, IPC
#   hle       the services the game talks to                         video     NVDEC and VIC (host1x)
#   gpu       the GPU front end: GPFIFO, engines, the renderer interface
#   shader    Maxwell shaders to SPIR-V
#   vk        the renderer                                            platform  the OS layer and a window backend

add_library(snx_nx_flags INTERFACE)
target_link_libraries(snx_nx_flags INTERFACE scarletnx-common scarletnx-net scarletnx-filesystem)
# Every function and global used across files is declared once, in the header of the file that defines it, and
# everything else is static: a declaration that disagrees with the definition is then a compile error instead of a
# miscompile. Third-party sources keep their own warnings off.
target_compile_options(snx_nx_flags INTERFACE -fno-strict-aliasing
                       $<$<COMPILE_LANGUAGE:C>:-Werror=missing-prototypes -Werror=missing-variable-declarations>)
target_compile_definitions(snx_nx_flags INTERFACE VK_NO_PROTOTYPES)
target_link_libraries(snx_nx_flags INTERFACE Vulkan::Headers)
target_include_directories(snx_nx_flags BEFORE INTERFACE "${volk_SOURCE_DIR}"
                                                "${VulkanMemoryAllocator_SOURCE_DIR}/include")

function(snx_component name)
    add_library(snx_${name} OBJECT ${ARGN})
    target_link_libraries(snx_${name} PUBLIC snx_nx_flags)
endfunction()

snx_component(core runtime/main.c runtime/arm.c runtime/loader.c runtime/rtapi.c runtime/clock.c runtime/crash.c runtime/options.c
              runtime/ini.c runtime/startup.c runtime/gdbstub.c runtime/present.c)
target_link_libraries(snx_core PRIVATE snx_driver snx_lz4)
snx_component(kernel hle/kernel/kernel.c hle/kernel/thread.c hle/kernel/sync.c hle/svc/svc.c hle/svc/svc_mem.c hle/kernel/debug.c
              memory/mem.c hle/kernel/ipc.c)
snx_component(hle hle/services/services.c hle/services/audren.c hle/services/audren_dsp.c hle/services/pl.c hle/services/sm.c hle/services/am.c hle/services/set.c hle/services/time.c hle/services/acc.c hle/services/audio.c
              hle/services/misc.c hle/services/ldr.c hle/services/network/bsd.c hle/services/network/dns.c
              hle/services/network/nifm.c hle/services/network/ssl.c hle/filesystem/fs.c hle/services/hid.c hle/services/hid_motion.c hle/services/hid_touch.c hle/services/vi.c hle/services/nvdrv.c)
# the shared fonts' stand-ins (cmake/shared_fonts.cmake), which hle/services/pl.c hands out
target_sources(snx_hle PRIVATE "${CMAKE_BINARY_DIR}/generated/shared_fonts.c")
# the IPC interfaces of the services (docs/IPC.md): their dispatchers are generated, the handlers are in the services'
# files. A type is defined before the files that use it
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
snx_swipc(snx_hle ROOT "${CMAKE_CURRENT_SOURCE_DIR}" FILES
          hle/services/types.swipc hle/services/sm.swipc hle/services/set.swipc hle/services/acc.swipc
          hle/services/audio.swipc hle/services/ldr.swipc hle/services/pl.swipc hle/services/time.swipc
          hle/services/misc.swipc hle/services/am.swipc hle/services/hid.swipc
          hle/services/vi.swipc hle/services/nvdrv.swipc hle/services/network/bsd.swipc
          hle/services/network/dns.swipc hle/services/network/nifm.swipc hle/services/network/ssl.swipc
          hle/filesystem/fs.swipc)
snx_component(video gpu/video/host1x.c gpu/video/nvdec.c gpu/video/nvdec_h264.c gpu/video/nvdec_rec.c
              gpu/video/nvdec_vp9.c gpu/video/vic.c gpu/video/vp9_head.c)
target_link_libraries(snx_video PRIVATE FFmpeg::avcodec)
# hwopus (hle/services/audio.c) decodes Opus with FFmpeg too
target_link_libraries(snx_hle PRIVATE FFmpeg::avcodec OpenSSL::SSL)
snx_component(gpu gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c gpu/maxwell/maxwell3d.c gpu/renderer.c)

include("${PROJECT_SOURCE_DIR}/cmake/controls_atlas.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
set(platform runtime/platform/plat.c runtime/platform/unattended.c runtime/platform/backend/sdl3.c
             runtime/platform/backend/gamepad.c
             runtime/platform/overlay.c runtime/platform/touch.c runtime/platform/ui.c runtime/platform/vpad.c
             runtime/platform/fatal_screen.c "${CMAKE_BINARY_DIR}/generated/controls_png.c")
if(WIN32)
    list(APPEND platform runtime/platform/plat_win32.c runtime/platform/profiler_win32.c)
else()
    list(APPEND platform runtime/platform/plat_posix.c)
endif()
snx_component(platform ${platform})

snx_component(shader gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c
                     gpu/shader/spirv_translate.c gpu/shader/spirv_compute.c gpu/shader/spirv_emit.c)
snx_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                 gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c gpu/vk/render.c gpu/vk/compute.c
                 gpu/vk/astc.c gpu/vk/ui.c gpu/texture/etc2.c gpu/vk/vic.c gpu/vk/vma.cpp "${volk_SOURCE_DIR}/volk.c")
set(components core kernel hle video gpu platform shader vk)
# third-party code (vma.cpp is VMA's implementation)
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" gpu/vk/vma.cpp PROPERTIES COMPILE_OPTIONS -w)

if(ANDROID)
    # the app (dist/android): its game activity loads libscarletnx.so and calls its main; the menu runs the driver as
    # libscarletnx-run.so, an executable named for the app's libraries folder, which takes only lib*.so
    add_library(scarletnx SHARED runtime/entry.c)
    # FFmpeg's assembly reaches its tables PC-relative, as its own shared libraries allow with this
    target_link_options(scarletnx PRIVATE -Wl,-Bsymbolic)
    add_executable(scarletnx-run runtime/platform/android_run.c)
    set_target_properties(scarletnx-run PROPERTIES OUTPUT_NAME libscarletnx-run.so RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(scarletnx-run PRIVATE ${CMAKE_DL_LIBS})
    install(TARGETS scarletnx scarletnx-run LIBRARY DESTINATION lib/${ANDROID_ABI} RUNTIME DESTINATION lib/${ANDROID_ABI})
else()
    add_executable(scarletnx runtime/entry.c)
    # started with nothing to run, it opens the menu (src/gui), which runs games with it
    if(TARGET scarletnx-gui)
        target_link_libraries(scarletnx PRIVATE scarletnx-gui)
        target_compile_definitions(scarletnx PRIVATE SNX_WITH_GUI)
    endif()
endif()
list(TRANSFORM components PREPEND snx_)
add_library(scarletnx-nx STATIC)
set_target_properties(scarletnx-nx PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(scarletnx-nx PUBLIC snx_nx_flags ${components} snx_driver)
target_link_libraries(scarletnx PRIVATE scarletnx-nx)
# VMA is C++: link with the C++ driver. It goes in the build folder itself, where
# the native driver and CI look for it
set_target_properties(scarletnx PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    # Prefer static toolchain/SDL libraries. LLVM may still use a DLL import library,
    # resolved by cmake/llvm.cmake and bundled by CI. user32 provides message boxes;
    # winmm is loaded at run time, and the static SDL3 brings the libraries it needs
    target_link_libraries(scarletnx PRIVATE user32)
    # The application manifest (runtime/platform/runtime.manifest): UTF-8 as the process's code page, long paths
    enable_language(RC)
    target_sources(scarletnx PRIVATE runtime/platform/runtime.rc)
    target_link_options(scarletnx PRIVATE -static)
    target_link_options(scarletnx PRIVATE -mwindows)
else()
    # No -rdynamic: the game module gets the runtime through RtAPI and imports nothing from it, and exporting every
    # global would let a library loaded later (a GPU driver) bind its own symbols to ours by name
    target_link_libraries(scarletnx PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# SDL3 is either the system package or the pinned static source build (cmake/dependencies.cmake)
include("${PROJECT_SOURCE_DIR}/cmake/window_icon.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/build_info.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/pad_sdl.cmake")
target_link_libraries(snx_platform PRIVATE SDL3::SDL3 snx_window_icon snx_build_info snx_pad_sdl)
# the game's menu in YoRHa's colours, from src/gui/themes/yorha/theme.ini (cmake/gui_themes.cmake)
target_include_directories(snx_platform PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(scarletnx PRIVATE SDL3::SDL3)

if(BUILD_TESTING)
    add_executable(snx-test-shader-direct "${PROJECT_SOURCE_DIR}/tests/shader_direct.c")
    snx_test_target(snx-test-shader-direct 11)
    target_link_libraries(snx-test-shader-direct PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-direct PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_direct_structure COMMAND $<TARGET_FILE:snx-test-shader-direct> --check-only)
    add_executable(snx-test-shader-direct-regressions "${PROJECT_SOURCE_DIR}/tests/shader_direct_regressions.c")
    snx_test_target(snx-test-shader-direct-regressions 11)
    target_link_libraries(snx-test-shader-direct-regressions PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-direct-regressions PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_direct_regressions COMMAND $<TARGET_FILE:snx-test-shader-direct-regressions>)
    add_test(NAME runtime_settings_list COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             -P "${PROJECT_SOURCE_DIR}/tests/settings_list.cmake")
    add_test(NAME runtime_unknown_option COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
             -P "${PROJECT_SOURCE_DIR}/tests/unknown_option.cmake")
    if(NOT WIN32) # Windows' error box would wait for a click
        add_test(NAME runtime_game_copy COMMAND "${CMAKE_COMMAND}" -DRUNTIME=$<TARGET_FILE:scarletnx>
                 -P "${PROJECT_SOURCE_DIR}/tests/game_copy.cmake")
    endif()
    find_program(SNX_SPIRV_VAL spirv-val)
    find_program(SNX_SPIRV_DIS spirv-dis)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(SNX_SPIRV_VAL AND SNX_SPIRV_DIS AND Python3_Interpreter_FOUND)
        add_test(NAME shader_direct COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/shader_direct_test.py"
            --generator $<TARGET_FILE:snx-test-shader-direct> --validator "${SNX_SPIRV_VAL}"
            --disassembler "${SNX_SPIRV_DIS}"
            --regressions $<TARGET_FILE:snx-test-shader-direct-regressions>)
    else()
        message(STATUS "Skipping shader_direct: Python3, spirv-val, and spirv-dis are required")
    endif()
endif()

# Opt-in because normal unit-test runs need neither an ICD nor a GPU. All rendering uses the real runtime.
if(BUILD_TESTING AND SNX_TEST_VULKAN)
    add_executable(snx-test-vulkan "${PROJECT_SOURCE_DIR}/tests/vulkan_headless.c"
        "${PROJECT_SOURCE_DIR}/tests/spirv_emitter_fixture.c")
    target_link_libraries(snx-test-vulkan PRIVATE snx_nx_flags ${components} snx_driver Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(snx-test-vulkan PROPERTIES LINKER_LANGUAGE CXX)
    if(WIN32)
        target_link_libraries(snx-test-vulkan PRIVATE user32)
    else()
        target_link_libraries(snx-test-vulkan PRIVATE m)
    endif()
    target_link_libraries(snx-test-vulkan PRIVATE SDL3::SDL3)
    add_test(NAME vulkan_headless COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_headless PROPERTIES TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
endif()
if(BUILD_TESTING)
    snx_test_sanitize(snx_shader) # tests/CMakeLists.txt: made after it looked
    snx_sanitizer_path()          # this folder's tests
endif()

# a sanitized build (SNX_TEST_SANITIZERS) is a test build, its runtime instrumented: it doesn't go next to the games
if(SNX_DEPLOY AND SNX_TEST_SANITIZERS AND BUILD_TESTING)
    message(STATUS "SNX_TEST_SANITIZERS: the runtime is not deployed to ${SNX_GAMES_DIR}")
elseif(SNX_DEPLOY)
    add_custom_command(TARGET scarletnx POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=$<TARGET_FILE:scarletnx>" "-DGAMES=${SNX_GAMES_DIR}" "-DWINDOWS=${WIN32}"
                -P "${CMAKE_SOURCE_DIR}/cmake/deploy.cmake"
        VERBATIM)
endif()

# All model names share the same Horizon/Maxwell implementation.
if(ANDROID)
    set(snx_models "")
else()
    set(snx_models erista mariko aula)
endif()
foreach(model ${snx_models})
    if(WIN32)
        add_custom_command(TARGET scarletnx POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:scarletnx>"
                    "${CMAKE_BINARY_DIR}/scarletnx-${model}.exe" VERBATIM)
        install(PROGRAMS "${CMAKE_BINARY_DIR}/scarletnx-${model}.exe" DESTINATION bin)
    else()
        add_custom_command(TARGET scarletnx POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E create_symlink "$<TARGET_FILE_NAME:scarletnx>"
                    "${CMAKE_BINARY_DIR}/scarletnx-${model}" VERBATIM)
        install(PROGRAMS "${CMAKE_BINARY_DIR}/scarletnx-${model}" DESTINATION bin)
    endif()
endforeach()
