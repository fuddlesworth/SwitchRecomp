# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
option(SNX_TEST_SANITIZERS "Run native regression tests with ASan and UBSan" OFF)
option(SNX_TEST_VULKAN "Run offscreen Vulkan integration tests (requires a Vulkan 1.3 ICD, e.g. lavapipe)" OFF)
if(SNX_TEST_SANITIZERS AND TARGET snx_driver)
    target_compile_options(snx_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(snx_driver INTERFACE -fsanitize=address,undefined)
endif()

function(snx_test_target target standard)
    set_target_properties(${target} PROPERTIES C_STANDARD ${standard} C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/common/utilities" "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler" "${PROJECT_SOURCE_DIR}/src/common"
        "${PROJECT_SOURCE_DIR}/src/nx" "${PROJECT_SOURCE_DIR}/src/nx/loader"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver")
    if(standard EQUAL 90)
        set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE -pedantic-errors)
    else()
        set_target_properties(${target} PROPERTIES C_EXTENSIONS ON)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(SNX_TEST_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# Validate the direct writer against SPIRV-Tools when the system tools are installed.
if(SNX_BUILD_NX)
    find_program(SNX_SPIRV_VAL spirv-val)
    find_program(SNX_SPIRV_DIS spirv-dis)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(SNX_SPIRV_VAL AND SNX_SPIRV_DIS AND Python3_Interpreter_FOUND)
        add_executable(snx-test-spirv-emitter spirv_emitter.c spirv_emitter_fixture.c
            "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/spirv_emit.c")
        snx_test_target(snx-test-spirv-emitter 11)
        add_test(NAME spirv_emitter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/spirv_emitter_test.py"
            --generator $<TARGET_FILE:snx-test-spirv-emitter> --validator "${SNX_SPIRV_VAL}" --disassembler "${SNX_SPIRV_DIS}")
    endif()
endif()

add_executable(snx-test-arm-decode arm_decode.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c")
snx_test_target(snx-test-arm-decode 11)
add_test(NAME arm_decode COMMAND snx-test-arm-decode)
add_executable(snx-test-arm-exec arm_exec_test.c)
snx_test_target(snx-test-arm-exec 11)
target_link_libraries(snx-test-arm-exec PRIVATE snx_arm_llvm)
add_test(NAME arm_exec_dry_svc COMMAND snx-test-arm-exec)

# Small architectural witnesses need neither game assets nor an emulator.
add_executable(snx-test-a64-fp a64_fp_test.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp 11)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp PRIVATE m)
endif()
# cpu_helpers.c's 16-byte CAS needs cmpxchg16b inline, not a libatomic call (the runtime's toolchain.c has the
# same flag for the same reason; the function-level target("cx16") is not enough on this Clang)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(snx-test-a64-fp PRIVATE -mcx16)
endif()
add_test(NAME a64_fp_witnesses COMMAND snx-test-a64-fp)

# one build per snx_crc32 path: the nibble table, PCLMULQDQ, PCLMULQDQ with SSE4.2
function(snx_crc32_test name)
    add_executable(snx-test-${name} crc32_helper.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_test_target(snx-test-${name} 11)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(snx-test-${name} PRIVATE -mcx16 ${ARGN})
    endif()
    if(NOT WIN32)
        target_link_libraries(snx-test-${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND snx-test-${name})
endfunction()
snx_crc32_test(crc32_helper)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    snx_crc32_test(crc32_helper_clmul -mpclmul)
    snx_crc32_test(crc32_helper_x86 -msse4.2 -mpclmul)
endif()

add_executable(snx-test-a64-fp-arithmetic a64_fp_arithmetic_test.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp-arithmetic 11)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp-arithmetic PRIVATE m)
endif()
add_test(NAME a64_fp_arithmetic_witnesses COMMAND snx-test-a64-fp-arithmetic)

option(SNX_TEST_A64_FP_DIFF "Run the Linux AArch64 FP differential harness (requires Python unicorn 2.1.4)" OFF)
if(SNX_TEST_A64_FP_DIFF)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "SNX_TEST_A64_FP_DIFF requires a native Linux host")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME a64_fp_differential COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/a64_fp_diff.py" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(a64_fp_differential PROPERTIES TIMEOUT 900)
endif()

# The emulated network stack over the host's loopback (src/common/network)
add_executable(snx-test-network-stack network_stack.c)
snx_test_target(snx-test-network-stack 23)
target_link_libraries(snx-test-network-stack PRIVATE scarletnx-net)
add_test(NAME network_stack COMMAND snx-test-network-stack)
set_tests_properties(network_stack PROPERTIES TIMEOUT 60)

add_executable(snx-test-network-icmp network_icmp.c)
snx_test_target(snx-test-network-icmp 23)
target_link_libraries(snx-test-network-icmp PRIVATE scarletnx-net)
add_test(NAME network_icmp COMMAND snx-test-network-icmp)
set_tests_properties(network_icmp PROPERTIES TIMEOUT 30)

# This IPC fixture stubs the kernel and needs no NX graphics/media dependencies.
add_executable(snx-test-bsd-service bsd_service.c)
snx_test_target(snx-test-bsd-service 23)
target_link_libraries(snx-test-bsd-service PRIVATE scarletnx-net)
add_test(NAME bsd_service COMMAND snx-test-bsd-service)
set_tests_properties(bsd_service PROPERTIES TIMEOUT 60)

if(SNX_BUILD_NX)
add_test(NAME llvm_dependency COMMAND "${CMAKE_COMMAND}"
    "-DSNX_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DSNX_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/llvm-dependency"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_dependency.cmake")

add_executable(snx-test-overlay overlay.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/overlay.c")
snx_test_target(snx-test-overlay 11)
target_link_libraries(snx-test-overlay PRIVATE scarletnx-common)
add_test(NAME overlay COMMAND snx-test-overlay)

add_executable(snx-test-startup startup_options.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/startup.c")
snx_test_target(snx-test-startup 90)
add_test(NAME startup_options COMMAND snx-test-startup)

add_library(snx_test_common STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/common.c")
snx_test_target(snx_test_common 90)
if(WIN32)
    target_link_libraries(snx_test_common PUBLIC advapi32)
endif()

add_executable(snx-test-driver driver_workflow.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/driver.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/process.c")
snx_test_target(snx-test-driver 90)
target_link_libraries(snx-test-driver PRIVATE snx_test_common)
add_test(NAME driver_workflow COMMAND snx-test-driver)
set_tests_properties(driver_workflow PROPERTIES TIMEOUT 60)

if(UNIX)
    add_executable(snx-test-gdbstub gdbstub.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/gdbstub.c")
    snx_test_target(snx-test-gdbstub 11)
    target_link_libraries(snx-test-gdbstub PRIVATE snx_test_common)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gdbstub_native COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdbstub.py" $<TARGET_FILE:snx-test-gdbstub>)
        set_tests_properties(gdbstub_native PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 RESOURCE_LOCK gdb_port_1234)
    endif()
endif()

add_executable(snx-test-extract extract.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/extract.c")
snx_test_target(snx-test-extract 90)
target_link_libraries(snx-test-extract PRIVATE snx_test_common)
add_test(NAME extraction_native COMMAND snx-test-extract)

add_executable(snx-test-loader loader_test.c "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c")
snx_test_target(snx-test-loader 90)
target_link_libraries(snx-test-loader PRIVATE snx_test_common)
add_test(NAME loader_native COMMAND snx-test-loader)

add_executable(snx-test-runtime-nro runtime_nro.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/loader.c")
snx_test_target(snx-test-runtime-nro 11)
add_test(NAME runtime_nro COMMAND snx-test-runtime-nro)

if(NOT WIN32)
    # Emit the real fallback interpreter without LLVM, a game dump, or a precompiled code module.
    add_executable(snx-test-ro-interp-gen "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler/llvm/rtlib_gen.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/a64.c")
    snx_test_target(snx-test-ro-interp-gen 11)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ldr_interp.inc"
        COMMAND snx-test-ro-interp-gen interp "${CMAKE_CURRENT_BINARY_DIR}/ldr_interp.inc"
        DEPENDS snx-test-ro-interp-gen VERBATIM)
    add_executable(snx-test-ldr-service ldr_service.c ldr_interp.c "${CMAKE_CURRENT_BINARY_DIR}/ldr_interp.inc"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/services/ldr.c" "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c"
        "${PROJECT_SOURCE_DIR}/src/common/utilities/sha256.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_test_target(snx-test-ldr-service 11)
    target_include_directories(snx-test-ldr-service PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(snx-test-ldr-service PRIVATE SNX_RO_INTERP_TEST)
    target_link_libraries(snx-test-ldr-service PRIVATE m)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        target_compile_options(snx-test-ldr-service PRIVATE -mcx16)
    endif()
    add_test(NAME ldr_service COMMAND snx-test-ldr-service)
    set_tests_properties(ldr_service PROPERTIES TIMEOUT 60)
endif()

add_executable(snx_recompiler_test recompiler_native.c)
snx_test_target(snx_recompiler_test 23)
target_include_directories(snx_recompiler_test PRIVATE "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64")
target_link_libraries(snx_recompiler_test PRIVATE snx_driver ${CMAKE_DL_LIBS})

# LLVM is always available to the recompiler.
add_test(NAME native_recompiler COMMAND snx_recompiler_test
    $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" "${CMAKE_C_COMPILER}")
add_executable(snx-test-llvm-differential llvm_differential.c)
snx_test_target(snx-test-llvm-differential 11)
target_link_libraries(snx-test-llvm-differential PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
add_test(NAME llvm_differential COMMAND snx-test-llvm-differential "${CMAKE_C_COMPILER}" "${PROJECT_SOURCE_DIR}"
    --quiet)
set_tests_properties(llvm_differential PROPERTIES TIMEOUT 900)
# not a test: timings of the generated code (llvm_bench SCARLETNX REPO CLANG)
add_executable(snx-llvm-bench llvm_bench.c)
snx_test_target(snx-llvm-bench 11)
target_include_directories(snx-llvm-bench PRIVATE "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64")
target_link_libraries(snx-llvm-bench PRIVATE snx_driver ${CMAKE_DL_LIBS})
# A32 emits self-contained host objects, without the A64 interpreter or RtAPI.
add_executable(snx-test-arm-emit arm_emit.c)
snx_test_target(snx-test-arm-emit 11)
target_link_libraries(snx-test-arm-emit PRIVATE snx_arm_llvm)
# Thumb16/32 emits self-contained host objects the same way.
add_executable(snx-test-thumb-emit thumb_emit.c)
snx_test_target(snx-test-thumb-emit 11)
target_link_libraries(snx-test-thumb-emit PRIVATE snx_arm_llvm)
option(SNX_TEST_ARM_DIFF "Compare A32 LLVM execution with Python unicorn 2.1.4" OFF)
if(UNIX AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME arm_llvm_witnesses COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
            --emitter $<TARGET_FILE:snx-test-arm-emit> --cc "${CMAKE_C_COMPILER}")
        if(SNX_TEST_ARM_DIFF)
            add_test(NAME arm_llvm_differential COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
                --emitter $<TARGET_FILE:snx-test-arm-emit> --cc "${CMAKE_C_COMPILER}" --unicorn)
            set_tests_properties(arm_llvm_differential PROPERTIES TIMEOUT 900)
            # VFP/NEON uses the same emitter plus the raw-bit FP and crypto runtime library.
            get_filename_component(snx_llvm_bin "${SNX_LLVM_CONFIG}" DIRECTORY)
            find_program(SNX_LLVM_MC llvm-mc HINTS "${snx_llvm_bin}" NO_DEFAULT_PATH)
            if(SNX_LLVM_MC)
                add_test(NAME arm_llvm_vector COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_vector_test.py"
                    --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt>
                    --mc "${SNX_LLVM_MC}" --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(arm_llvm_vector PROPERTIES TIMEOUT 1800)
                add_test(NAME thumb_llvm COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/thumb_test.py"
                    --emitter $<TARGET_FILE:snx-test-thumb-emit> --mc "${SNX_LLVM_MC}"
                    --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(thumb_llvm PROPERTIES TIMEOUT 900)
            endif()
        endif()
    endif()
endif()

add_executable(snx-test-llvm-module llvm_module.c)
snx_test_target(snx-test-llvm-module 11)
target_include_directories(snx-test-llvm-module PRIVATE "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64")
target_link_libraries(snx-test-llvm-module PRIVATE snx_driver ${CMAKE_DL_LIBS})
if(TARGET snx_jit)
    set(snx_extra_backends gcc)
    target_link_libraries(snx-test-llvm-differential PRIVATE snx_jit)
    target_compile_definitions(snx-test-llvm-differential PRIVATE SNX_TEST_GCCJIT)
    add_test(NAME gccjit_differential COMMAND snx-test-llvm-differential "${CMAKE_C_COMPILER}" "${PROJECT_SOURCE_DIR}"
        --gccjit --quiet --per-case 2 --simd 100 --states 4)
    set_tests_properties(gccjit_differential PROPERTIES TIMEOUT 900)
    add_test(NAME native_recompiler_gcc COMMAND snx_recompiler_test
        $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" "${CMAKE_C_COMPILER}" gcc)
endif()
add_test(NAME llvm_module COMMAND snx-test-llvm-module $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}"
    "${CMAKE_C_COMPILER}" ${snx_extra_backends})

add_executable(snx_codegen_test codegen_test.c)
snx_test_target(snx_codegen_test 90)
target_link_libraries(snx_codegen_test PRIVATE snx_driver)
add_test(NAME native_codegen COMMAND snx_codegen_test "${CMAKE_C_COMPILER}" "${PROJECT_SOURCE_DIR}")

add_executable(snx-test-astc astc_decode.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/astc.c")
snx_test_target(snx-test-astc 23)
add_test(NAME astc_decode COMMAND snx-test-astc)

add_executable(snx-test-audio-out audio_out.c)
snx_test_target(snx-test-audio-out 11)
# Isolate audout in this fixture without FFmpeg, also when testing a tools-only build.
# Production NX targets always link FFmpeg and never define SNX_NO_H264.
target_compile_definitions(snx-test-audio-out PRIVATE SNX_NO_H264)
add_test(NAME audio_out COMMAND snx-test-audio-out)

if(UNIX)
    add_executable(snx-test-audren audren_render.c)
    snx_test_target(snx-test-audren 11)
    target_link_libraries(snx-test-audren PRIVATE m)
    add_test(NAME audren_render COMMAND snx-test-audren)
endif()

add_executable(snx-test-render-enable render_enable.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c")
snx_test_target(snx-test-render-enable 23)
add_test(NAME render_enable COMMAND snx-test-render-enable)

add_executable(snx-test-clear-flags clear_flags.c)
snx_test_target(snx-test-clear-flags 23)
add_test(NAME clear_flags COMMAND snx-test-clear-flags)

add_executable(snx-test-instance-array instance_array.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c")
snx_test_target(snx-test-instance-array 23)
add_test(NAME instance_array COMMAND snx-test-instance-array)

add_executable(snx-test-draw-texture draw_texture.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c"
    "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c")
snx_test_target(snx-test-draw-texture 23)
add_test(NAME draw_texture COMMAND snx-test-draw-texture)

add_executable(snx-test-etc2 etc2_decode.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/etc2.c")
snx_test_target(snx-test-etc2 23)
add_test(NAME etc2_decode COMMAND snx-test-etc2)

if(UNIX)
    add_executable(snx-test-kernel-mutex kernel_mutex.c)
    snx_test_target(snx-test-kernel-mutex 11)
    add_test(NAME kernel_mutex COMMAND snx-test-kernel-mutex)
    add_executable(snx-test-kernel-cores kernel_cores.c)
    snx_test_target(snx-test-kernel-cores 11)
    add_test(NAME kernel_cores COMMAND snx-test-kernel-cores)
endif()

add_executable(snx-test-fs-fixture filesystem_fixture.c)
snx_test_target(snx-test-fs-fixture 90)
target_link_libraries(snx-test-fs-fixture PRIVATE snx_test_common)

add_library(snx_test_fs_tree OBJECT "${PROJECT_SOURCE_DIR}/src/nx/hle/filesystem/fs_tree.c")
snx_test_target(snx_test_fs_tree 11)
target_compile_options(snx_test_fs_tree PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fs_scan_probe.h")

foreach(suite tree service)
    add_executable(snx-test-fs-${suite} filesystem_${suite}.c $<TARGET_OBJECTS:snx_test_fs_tree>)
    snx_test_target(snx-test-fs-${suite} 11)
    add_test(NAME filesystem_${suite}
        COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-${suite}>)
endforeach()
add_test(NAME filesystem_raw
    COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> raw)

if(UNIX)
    add_executable(snx-test-memory memory_accounting.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c")
    snx_test_target(snx-test-memory 11)
    add_test(NAME memory_accounting COMMAND snx-test-memory)
    set_tests_properties(memory_accounting PROPERTIES TIMEOUT 60)
endif()

if(SNX_TEST_SANITIZERS AND WIN32)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE snx_clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(snx_sanitized_tests a64_fp_witnesses a64_fp_arithmetic_witnesses startup_options driver_workflow extraction_native loader_native runtime_nro native_codegen filesystem_tree filesystem_service filesystem_raw astc_decode audio_out render_enable clear_flags instance_array draw_texture etc2_decode crc32_helper)
    if(TEST native_recompiler)
        list(APPEND snx_sanitized_tests native_recompiler)
    endif()
    if(TEST crc32_helper_x86)
        list(APPEND snx_sanitized_tests crc32_helper_clmul crc32_helper_x86)
    endif()
    set_tests_properties(${snx_sanitized_tests}
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${snx_clang_resource}/lib/windows")
endif()

endif() # SNX_BUILD_NX

# dist/icons/ is what scripts/make-icons.py draws from dist/wimsy.svg: a changed SVG fails until it runs again
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME dist_icons COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/make-icons.py" --check)
endif()
if(NOT WIN32)
    include(GNUInstallDirs)
    add_test(NAME dist_install COMMAND "${CMAKE_COMMAND}" -DBUILD=${PROJECT_BINARY_DIR}
        -DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/dist-install -DDATADIR=${CMAKE_INSTALL_DATADIR} -DNX=${SNX_BUILD_NX}
        -DGUI=${SNX_BUILD_GUI} -DDIST=${PROJECT_SOURCE_DIR}/dist -P "${CMAKE_CURRENT_SOURCE_DIR}/dist_install.cmake")
endif()
include(architecture.cmake)
