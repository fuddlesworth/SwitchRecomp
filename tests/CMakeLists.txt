# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
option(SNX_TEST_SANITIZERS
       "Build the native regression tests, and the in-tree libraries they exercise, with ASan and UBSan (a test build: \
the runtime links those libraries too, and isn't deployed)" OFF)
option(SNX_TEST_VULKAN "Run offscreen Vulkan integration tests (requires a Vulkan 1.3 ICD, e.g. lavapipe)" OFF)
option(SNX_TEST_ARM_AOT "Build asset-free static ARM11/checked-memory witnesses (requires native LLVM SDK)" OFF)
# the in-tree libraries the tests exercise (the network stack, the loaders and LZ4, the recompiler and its backends):
# an overflow in them shows only in code built with the sanitizers. The runtime links them too, so src/nx doesn't
# deploy a sanitized one. A folder that makes one after this one calls snx_test_sanitize on it: the ARM backend of a
# GNU ABI-only or an ARM AOT build, the GNU ABI runtime, the shader translator, the GUI's library
function(snx_test_sanitize)
    if(NOT SNX_TEST_SANITIZERS)
        return()
    endif()
    foreach(target ${ARGN})
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            get_target_property(type ${target} TYPE)
            if(type STREQUAL "SHARED_LIBRARY" OR type STREQUAL "MODULE_LIBRARY")
                target_link_options(${target} PUBLIC -fsanitize=address,undefined) # its own link too
            else()
                target_link_options(${target} INTERFACE -fsanitize=address,undefined)
            endif()
        endif()
    endforeach()
endfunction()
snx_test_sanitize(snx_driver snx_nx_module snx_driver_llvm snx_driver_gccjit scarletnx-common scarletnx-net snx_lz4
                  snx_llvm snx_arm_llvm snx_jit scarletnx-recompiler scarletnx-recompiler-shared)
# one Python for the scripted tests: without it they are left out
find_package(Python3 QUIET COMPONENTS Interpreter)
# x86-64, by any of the names hosts give it
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(snx_test_x86 ON)
else()
    set(snx_test_x86 OFF)
endif()

# On Windows a sanitized test needs the sanitizer runtime's DLLs on PATH. Each folder that adds tests (this one, the
# top level, src/nx, src/gnuabi, src/gui) calls this for the ones it has added (the others don't mind the DLLs)
function(snx_sanitizer_path)
    if(NOT (SNX_TEST_SANITIZERS AND WIN32))
        return()
    endif()
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    get_property(tests DIRECTORY PROPERTY TESTS)
    if(tests)
        set_tests_properties(${tests} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${resource}/lib/windows")
    endif()
endfunction()
# cpu_helpers.c built as the game's helpers are (snx_rtlib_flags in src/common/cpu/recompiler/llvm/CMakeLists.txt),
# with -mcx16 on x86-64: its 16-byte compare-and-swap is then lock cmpxchg16b, not a libatomic call
function(snx_cpu_helpers_flags target)
    if(snx_test_x86)
        target_compile_options(${target} PRIVATE -mcx16)
    endif()
endfunction()
# a service's dispatchers, generated from types.swipc and the interfaces named (paths under src/nx). A handler takes
# every argument of its command, used or not, so -Wextra's unused-parameter warning is off
function(snx_service_swipc target)
    target_compile_options(${target} PRIVATE -Wno-unused-parameter)
    snx_swipc(${target} ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc ${ARGN})
endfunction()

# a test's checks are asserts too: -UNDEBUG keeps them in any build type
function(snx_test_target target standard)
    set(strict OFF)
    if(standard EQUAL 90) # strict ANSI C: C11 on Apple systems (SNX_C90_STANDARD, CMakeLists.txt)
        set(strict ON)
        set(standard ${SNX_C90_STANDARD})
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD ${standard} C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    if(strict)
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

if(TARGET snx_ctr_board)
    if(SNX_TEST_SANITIZERS)
        snx_test_sanitize(snx_ctr_board snx_ctr_memory snx_ctr_loader snx_ctr_cpu)
        foreach(target snx_ctr_board snx_ctr_memory snx_ctr_loader snx_ctr_cpu snx_ctr_process)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} INTERFACE -fsanitize=address,undefined)
        endforeach()
        target_compile_options(scarletnx-3ds PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        if(TARGET snx_ctr_native)
            target_compile_options(snx_ctr_native PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(snx_ctr_native INTERFACE -fsanitize=address,undefined)
        endif()
    endif()
    add_executable(snx-test-3ds-board 3ds_board.c)
    snx_test_target(snx-test-3ds-board 11)
    target_link_libraries(snx-test-3ds-board PRIVATE snx_ctr_board)
    add_test(NAME 3ds_board COMMAND snx-test-3ds-board)
    add_executable(snx-test-3ds-memory 3ds_memory.c)
    snx_test_target(snx-test-3ds-memory 11)
        target_link_libraries(snx-test-3ds-memory PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory COMMAND snx-test-3ds-memory)
    add_executable(snx-test-3ds-kernel 3ds_kernel.c)
    snx_test_target(snx-test-3ds-kernel 11)
    target_link_libraries(snx-test-3ds-kernel PRIVATE snx_ctr_process)
    add_test(NAME 3ds_kernel COMMAND snx-test-3ds-kernel)
    foreach(test services threads)
        add_executable(snx-test-3ds-${test} 3ds_${test}.c)
        snx_test_target(snx-test-3ds-${test} 11)
        target_link_libraries(snx-test-3ds-${test} PRIVATE snx_ctr_process)
        add_test(NAME 3ds_${test} COMMAND snx-test-3ds-${test})
    endforeach()
    add_executable(snx-test-3ds-ipc 3ds_ipc.c)
    snx_test_target(snx-test-3ds-ipc 11)
    target_link_libraries(snx-test-3ds-ipc PRIVATE snx_ctr_process)
    add_test(NAME 3ds_ipc COMMAND snx-test-3ds-ipc)
    add_executable(snx-test-ctr-swipc ctr_swipc.c)
    if(NOT WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        snx_test_target(snx-test-ctr-swipc 90)
            else()
        snx_test_target(snx-test-ctr-swipc 11)
    endif()
    snx_swipc(snx-test-ctr-swipc CTR ROOT "${PROJECT_SOURCE_DIR}" FILES tests/ctr_swipc.swipc)
    add_test(NAME ctr_swipc_dispatch COMMAND snx-test-ctr-swipc)
    add_executable(snx-test-3ds-process 3ds_process.c)
    snx_test_target(snx-test-3ds-process 11)
    target_link_libraries(snx-test-3ds-process PRIVATE snx_ctr_process)
    add_test(NAME 3ds_process COMMAND snx-test-3ds-process)
    add_executable(snx-test-3ds-discover 3ds_discover.c "${PROJECT_SOURCE_DIR}/src/3ds/aot/discover.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c" "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c" "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/profile.c")
    snx_test_target(snx-test-3ds-discover 11)
    add_test(NAME 3ds_static_discovery COMMAND snx-test-3ds-discover)
    if(TARGET snx_ctr_native)
        add_executable(snx-test-3ds-native 3ds_native.c)
        snx_test_target(snx-test-3ds-native 11)
        target_link_libraries(snx-test-3ds-native PRIVATE snx_ctr_native)
    endif()
    add_executable(snx-test-3ds-blz 3ds_blz.c)
    snx_test_target(snx-test-3ds-blz 11)
        target_link_libraries(snx-test-3ds-blz PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_blz COMMAND snx-test-3ds-blz)

    if(NOT WIN32)
        if(Python3_Interpreter_FOUND)
            add_executable(snx-test-3ds-fs 3ds_fs.c)
            snx_test_target(snx-test-3ds-fs 11)
            target_link_libraries(snx-test-3ds-fs PRIVATE snx_ctr_process)
            add_test(NAME 3ds_filesystem COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_fs.py" $<TARGET_FILE:snx-test-3ds-fs>)
            add_executable(snx-test-3ds-gsp 3ds_gsp.c)
            snx_test_target(snx-test-3ds-gsp 11)
            target_link_libraries(snx-test-3ds-gsp PRIVATE snx_ctr_process)
            add_test(NAME 3ds_gsp_gpu COMMAND snx-test-3ds-gsp "${CMAKE_CURRENT_BINARY_DIR}/gsp-frames")
            # the transfer engine alone: fills, display transfers, texture copies against hand-written goldens
            add_executable(snx-test-3ds-transfer 3ds_transfer.c)
            snx_test_target(snx-test-3ds-transfer 11)
            target_link_libraries(snx-test-3ds-transfer PRIVATE snx_ctr_gpu)
            add_test(NAME 3ds_transfer COMMAND snx-test-3ds-transfer)
            # the PICA command processor: lists in FCRAM, uploads, interrupts, jumps, draw and immediate hooks
            add_executable(snx-test-3ds-pica 3ds_pica.c)
            snx_test_target(snx-test-3ds-pica 11)
            target_link_libraries(snx-test-3ds-pica PRIVATE snx_ctr_gpu m)
            add_test(NAME 3ds_pica COMMAND snx-test-3ds-pica)
            # the HLE DSP and dsp::DSP: pipes, regions, sources, mixing, decoding, the service as ndsp drives it
            add_executable(snx-test-3ds-dsp 3ds_dsp.c)
            snx_test_target(snx-test-3ds-dsp 11)
            target_link_libraries(snx-test-3ds-dsp PRIVATE snx_ctr_process)
            add_test(NAME 3ds_dsp COMMAND snx-test-3ds-dsp)
    # No Vulkan loader/ICD: presentation cannot run here; the probe skips, never fakes a frame.
    set_tests_properties(3ds_gsp_gpu PROPERTIES SKIP_RETURN_CODE 77)
            add_test(NAME ctr_swipc_validation COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ctr_swipc.py"
                $<TARGET_FILE:snx-swipc>)
            add_test(NAME 3ds_driver COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_driver.py"
                $<TARGET_FILE:scarletnx-3ds>)
            set(snx_ctr_extract_test_args "")

            set(snx_ctr_extract_test_args --aot)
            add_test(NAME 3ds_static_pipeline COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_aot.py"
                $<TARGET_FILE:scarletnx-3ds> $<TARGET_FILE:snx-test-3ds-native>)
            set_tests_properties(3ds_static_pipeline PROPERTIES TIMEOUT 180)
            if(SNX_TEST_3DS_SUPERTUX)
                get_filename_component(snx_supertux_cia "${SNX_TEST_3DS_SUPERTUX}" ABSOLUTE)
                set(snx_supertux_oracle_args "")
                if(SNX_TEST_ARM_DIFF)
                    set(snx_supertux_oracle_args --oracle)
                endif()
                add_test(NAME 3ds_supertux_bootstrap COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_supertux.py"
                    $<TARGET_FILE:scarletnx-3ds> "${snx_supertux_cia}" $<TARGET_FILE:snx-test-3ds-native> ${snx_supertux_oracle_args})
                set_tests_properties(3ds_supertux_bootstrap PROPERTIES TIMEOUT 1800 LABELS "external;bootstrap")
            endif()
            
            add_test(NAME 3ds_extract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_extract.py"
                $<TARGET_FILE:scarletnx-3ds> ${snx_ctr_extract_test_args})
            set_tests_properties(3ds_extract PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()

add_executable(snx-test-arm-profile arm_profile.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/profile.c")
snx_test_target(snx-test-arm-profile 11)
add_test(NAME arm_isa_profiles COMMAND snx-test-arm-profile)
if(SNX_TEST_ARM_AOT)
    include(arm_checked.cmake)
endif()

# Board/GUI-only builds do not configure either AOT runtime or LLVM. Do not
# create CPU execution tests that link the absent backend targets in that case.
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
# Exercise NX's real FS tree and service adapters even in the independent CTR
# build. These fixtures supply their own platform/kernel boundary stubs.
add_executable(snx-test-fs-fixture filesystem_fixture.c)
snx_test_target(snx-test-fs-fixture 90)
target_link_libraries(snx-test-fs-fixture PRIVATE scarletnx-common)
add_library(snx_test_fs_tree OBJECT "${PROJECT_SOURCE_DIR}/src/common/filesystem/tree.c")
snx_test_target(snx_test_fs_tree 11)
target_compile_options(snx_test_fs_tree PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fs_scan_probe.h")
foreach(suite tree service)
    add_executable(snx-test-fs-${suite} filesystem_${suite}.c $<TARGET_OBJECTS:snx_test_fs_tree>)
    snx_test_target(snx-test-fs-${suite} 11)
    add_test(NAME filesystem_${suite}
        COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-${suite}>)
endforeach()
target_compile_options(snx-test-fs-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-fs-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc hle/filesystem/fs.swipc)
add_test(NAME filesystem_raw COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> raw)

if(UNIX)
    add_executable(snx-test-data-dir data_dir.c)
    snx_test_target(snx-test-data-dir 90)
    target_link_libraries(snx-test-data-dir PRIVATE scarletnx-common)
    add_test(NAME data_dir COMMAND snx-test-data-dir)
endif()
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    include(architecture.cmake)
    snx_sanitizer_path()
    return()
endif()

# Validate the direct writer against SPIRV-Tools when the system tools are installed.
if(SNX_BUILD_NX)
    find_program(SNX_SPIRV_VAL spirv-val)
    find_program(SNX_SPIRV_DIS spirv-dis)
    if(SNX_SPIRV_VAL AND SNX_SPIRV_DIS AND Python3_Interpreter_FOUND)
        add_executable(snx-test-spirv-emitter spirv_emitter.c spirv_emitter_fixture.c
            "${PROJECT_SOURCE_DIR}/src/nx/gpu/shader/spirv_emit.c")
        snx_test_target(snx-test-spirv-emitter 11)
                add_test(NAME spirv_emitter COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/spirv_emitter_test.py"
            --generator $<TARGET_FILE:snx-test-spirv-emitter> --validator "${SNX_SPIRV_VAL}" --disassembler "${SNX_SPIRV_DIS}")
    else()
        message(STATUS "Skipping spirv_emitter: Python3, spirv-val, and spirv-dis are required")
    endif()
endif()

add_executable(snx-test-arm-decode arm_decode.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/rt_helpers.c"
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
snx_cpu_helpers_flags(snx-test-a64-fp)
add_test(NAME a64_fp_witnesses COMMAND snx-test-a64-fp)

# one build per snx_crc32 path: the nibble table, PCLMULQDQ, PCLMULQDQ with SSE4.2
function(snx_crc32_test name)
    add_executable(snx-test-${name} crc32_helper.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_test_target(snx-test-${name} 11)
    snx_cpu_helpers_flags(snx-test-${name})
    target_compile_options(snx-test-${name} PRIVATE ${ARGN})
    if(NOT WIN32)
        target_link_libraries(snx-test-${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND snx-test-${name})
endfunction()
snx_crc32_test(crc32_helper)
if(snx_test_x86)
    snx_crc32_test(crc32_helper_clmul -mpclmul)
    snx_crc32_test(crc32_helper_x86 -msse4.2 -mpclmul)
endif()

add_executable(snx-test-a64-fp-arithmetic a64_fp_arithmetic_test.c
    "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp-arithmetic 11)
snx_cpu_helpers_flags(snx-test-a64-fp-arithmetic)
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

# swipc (tools/swipc): the dispatchers generated from tests/swipc/test.swipc and other.swipc, driven with hand-made
# requests; the same output compiled as C90 against a stand-in for ipc.h (LP64 hosts, where its u64 is unsigned long);
# and definitions the generator has to refuse, each with its line and message
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
add_executable(snx-test-swipc-dispatch swipc_dispatch.c)
snx_test_target(snx-test-swipc-dispatch 23)
snx_swipc(snx-test-swipc-dispatch ROOT "${PROJECT_SOURCE_DIR}" FILES tests/swipc/test.swipc tests/swipc/other.swipc)
add_test(NAME swipc_dispatch COMMAND snx-test-swipc-dispatch)
if(NOT WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(snx-test-swipc-c90 OBJECT swipc/c90/c90.c)
    set_target_properties(snx-test-swipc-c90 PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-test-swipc-c90 PRIVATE -pedantic-errors -Wall -Wextra -Werror)
    target_include_directories(snx-test-swipc-c90 BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/swipc/c90")
    snx_swipc(snx-test-swipc-c90 ROOT "${PROJECT_SOURCE_DIR}" FILES tests/swipc/test.swipc tests/swipc/other.swipc)
endif()
foreach(bad IN ITEMS
        array_of_arrays 5 "give the element a name"
        big_command_id 4 "number too large"
        buffer_two_modes 5 "exactly one of MapAlias"
        duplicate_argument 4 "two arguments named a"
        duplicate_command 6 "command 0 is defined twice"
        duplicate_field 5 "two fields named x"
        duplicate_service 5 "service test:a is served twice"
        exchange_buffer 5 "exchange \\(W\\) buffers"
        fixed_size_array 4 "it is not FixedSize"
        keyword_argument 4 "a C keyword"
        long_service_name 3 "at most 8 characters"
        move_handle_in 5 "a client can't send a move handle"
        output_too_big 5 "the reply takes 260 bytes"
        padding_name 4 "padN_ names"
        pid_field 4 "not struct fields"
        pid_output 4 "pid is an input"
        reply_with_handles 5 "the reply takes 272 bytes"
        request_too_big 5 "the request takes 280 bytes"
        reserved_command 5 "the name of a generated function"
        scoped_argument 4 "a scoped name"
        struct_in_place 5 "give it a name with type"
        stub_handle 5 "a @stub answers zeros"
        stub_object 5 "a @stub answers zeros"
        too_many_moves 5 "more than 15 move handles"
        two_pids 5 "one pid or aruid at most"
        type_before_definition 5 "unknown type test::Later"
        unknown_interface 5 "object<test::INope>: no such interface"
        unnamed_argument 5 "has a name"
        zero_alignment 4 "bad alignment 0")
    if(NOT DEFINED bad_name)
        set(bad_name "${bad}")
    elseif(NOT DEFINED bad_line)
        set(bad_line "${bad}")
    else()
        add_test(NAME swipc_refuses_${bad_name} COMMAND snx-swipc --root "${CMAKE_CURRENT_SOURCE_DIR}/swipc/bad"
                 --out "${CMAKE_CURRENT_BINARY_DIR}" ${bad_name}.swipc)
        set_tests_properties(swipc_refuses_${bad_name} PROPERTIES
                             PASS_REGULAR_EXPRESSION "${bad_name}.swipc:${bad_line}: error: .*${bad}")
        unset(bad_name)
        unset(bad_line)
    endif()
endforeach()

# This IPC fixture stubs the kernel and needs no NX graphics/media dependencies
add_executable(snx-test-bsd-service bsd_service.c)
snx_test_target(snx-test-bsd-service 23)
snx_service_swipc(snx-test-bsd-service hle/services/network/bsd.swipc)
target_link_libraries(snx-test-bsd-service PRIVATE scarletnx-net)
add_test(NAME bsd_service COMMAND snx-test-bsd-service)
set_tests_properties(bsd_service PROPERTIES TIMEOUT 60)

# sfdnsres and nifm (dns.c, nifm.c) offline, with bsd.c beside them for whether the console is online
add_executable(snx-test-network-service network_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c")
snx_test_target(snx-test-network-service 23)
snx_service_swipc(snx-test-network-service hle/services/network/bsd.swipc hle/services/network/dns.swipc
                  hle/services/network/nifm.swipc)
target_link_libraries(snx-test-network-service PRIVATE scarletnx-net)
add_test(NAME network_service COMMAND snx-test-network-service)
set_tests_properties(network_service PROPERTIES TIMEOUT 60)

# ssl (ssl.c) against a TLS server in the test, on the game's bsd sockets (bsd.c, built beside). OpenSSL comes with
# the NX, 3DS and hactool builds (cmake/dependencies.cmake), not with a GNU ABI-only one
if(TARGET OpenSSL::SSL)
    add_executable(snx-test-ssl-service ssl_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c")
    snx_test_target(snx-test-ssl-service 23)
        snx_service_swipc(snx-test-ssl-service hle/services/network/bsd.swipc hle/services/network/ssl.swipc)
    target_link_libraries(snx-test-ssl-service PRIVATE scarletnx-net OpenSSL::SSL)
    add_test(NAME ssl_service COMMAND snx-test-ssl-service)
    set_tests_properties(ssl_service PROPERTIES TIMEOUT 60)
endif()

# mm:u and hid's IHidServer (misc.c, hid.c), with the kernel objects stubbed
if(TARGET OpenSSL::Crypto)
    add_executable(snx-test-misc-service misc_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c"
        "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_motion.c")
    snx_test_target(snx-test-misc-service 23)
        snx_service_swipc(snx-test-misc-service hle/services/hid.swipc hle/services/misc.swipc)
    target_link_libraries(snx-test-misc-service PRIVATE OpenSSL::Crypto)
    if(NOT WIN32)
        target_link_libraries(snx-test-misc-service PRIVATE m)
    endif()
    add_test(NAME misc_service COMMAND snx-test-misc-service)
endif()

# nvdrv's IPC side (nvdrv.c), with the GPU and host1x stand-ins in the test
add_executable(snx-test-nvdrv-service nvdrv_service.c)
snx_test_target(snx-test-nvdrv-service 23)
snx_service_swipc(snx-test-nvdrv-service hle/services/nvdrv.swipc)
add_test(NAME nvdrv_service COMMAND snx-test-nvdrv-service)

# ipc.c's parsing and replies (hle/kernel/ipc.c, included by the test) with raw HIPC messages laid out as libnx
# writes them, a test service, and the kernel's handles and objects stubbed
add_executable(snx-test-ipc-message ipc_message.c)
snx_test_target(snx-test-ipc-message 23)
add_test(NAME ipc_message COMMAND snx-test-ipc-message)

if(SNX_BUILD_NX)
add_test(NAME llvm_dependency COMMAND "${CMAKE_COMMAND}"
    "-DSNX_SOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DSNX_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/llvm-dependency"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/llvm_dependency.cmake")

add_executable(snx-test-overlay overlay.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/overlay.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/fatal_screen.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/vpad.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-overlay 11)
target_link_libraries(snx-test-overlay PRIVATE scarletnx-common m)
include("${PROJECT_SOURCE_DIR}/cmake/gui_themes.cmake")
target_include_directories(snx-test-overlay PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_test(NAME overlay COMMAND snx-test-overlay)

add_executable(snx-test-vpad vpad.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/vpad.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-vpad 11)
target_link_libraries(snx-test-vpad PRIVATE scarletnx-common m)
add_test(NAME vpad COMMAND snx-test-vpad)

add_executable(snx-test-ini-deprecated ini_deprecated.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini-deprecated 11)
add_test(NAME ini_deprecated COMMAND snx-test-ini-deprecated)

add_executable(snx-test-pad-map pad_map.c)
snx_test_target(snx-test-pad-map 11)
target_link_libraries(snx-test-pad-map PRIVATE scarletnx-common)
add_test(NAME pad_map COMMAND snx-test-pad-map)
add_executable(snx-test-pad-screen pad_screen.c)
snx_test_target(snx-test-pad-screen 11)
target_link_libraries(snx-test-pad-screen PRIVATE scarletnx-common)
add_test(NAME pad_screen COMMAND snx-test-pad-screen)

include("${PROJECT_SOURCE_DIR}/cmake/pad_sdl.cmake")
add_executable(snx-test-gamepad gamepad.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/backend/gamepad.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/ui.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-gamepad 11)
target_link_libraries(snx-test-gamepad PRIVATE scarletnx-common snx_pad_sdl m)
add_test(NAME gamepad COMMAND snx-test-gamepad)

add_executable(snx-test-fatal-screen fatal_screen.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/fatal_screen.c")
snx_test_target(snx-test-fatal-screen 11)
target_link_libraries(snx-test-fatal-screen PRIVATE scarletnx-common)
add_test(NAME fatal_screen COMMAND snx-test-fatal-screen)

add_executable(snx-test-ini ini.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini 11)
add_test(NAME ini COMMAND snx-test-ini)
add_executable(snx-test-ini-lines ini_lines.c "${PROJECT_SOURCE_DIR}/src/common/utilities/ini_lines.c")
snx_test_target(snx-test-ini-lines 90)
add_test(NAME ini_lines COMMAND snx-test-ini-lines)

# generated and checked in (the controls' atlas, their shaders, the VIC's): their sources must still make them
if(Python3_Interpreter_FOUND)
    add_test(NAME controls_atlas_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-controls.py" --check)
    add_test(NAME ui_shaders_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-ui-shaders.py" --check)
    add_test(NAME vic_shader_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-vic-shader.py" --check)
    add_test(NAME generators_crlf COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generators_crlf.py")
    # which GitHub Actions caches the cleanup workflow deletes (scripts/trim-caches.py)
    add_test(NAME trim_caches COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/trim_caches_test.py")
    # the nightly release's title and notes (scripts/nightly-notes.py)
    add_test(NAME nightly_notes COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nightly_notes_test.py")
    # the builds published as releases and on Discord (scripts/publish-builds.py), against fakes. Where publish.yml
    # runs it, Linux: the fake gh is a script Windows does not run by its name
    if(NOT WIN32)
        add_test(NAME publish_builds
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/publish_builds_test.py")
        # the Discord cards of pull requests, issues and pushes to main (scripts/discord-cards.py), against the same
        add_test(NAME discord_cards
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/discord_cards_test.py")
    endif()
endif()

# a package patched in the build tree from the cache's copy as released (cmake/patched_copy.cmake)
add_test(NAME patched_copy COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/patched-copy-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/patched_copy_test.cmake")
# the dependency cache's keys and how a build is published to it (cmake/deps_cache.cmake)
add_test(NAME deps_cache COMMAND "${CMAKE_COMMAND}" "-DCC=${CMAKE_C_COMPILER}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/deps-cache-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/deps_cache_test.cmake")

# hid's shared memory layout (src/nx/hle/services/hid_layout.h) is checked against Eden's declarations at a pinned
# revision by check_hid_layout.py, run by hand: `python3 tests/check_hid_layout.py --eden <an Eden checkout>`. It
# needs that checkout, so it isn't a test here
add_executable(snx-test-hid-touch hid_touch.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/touch.c"
    "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c")
snx_test_target(snx-test-hid-touch 11)
target_link_libraries(snx-test-hid-touch PRIVATE scarletnx-common)
add_test(NAME hid_touch COMMAND snx-test-hid-touch)

add_executable(snx-test-hid-motion hid_motion.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_motion.c")
snx_test_target(snx-test-hid-motion 11)
target_link_libraries(snx-test-hid-motion PRIVATE scarletnx-common)
if(NOT WIN32)
    target_link_libraries(snx-test-hid-motion PRIVATE m)
endif()
add_test(NAME hid_motion COMMAND snx-test-hid-motion)

add_executable(snx-test-startup startup_options.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/startup.c")
snx_test_target(snx-test-startup 90)
add_test(NAME startup_options COMMAND snx-test-startup)

add_library(snx_test_common STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/common.c")
snx_test_target(snx_test_common 90)
if(WIN32)
    target_link_libraries(snx_test_common PUBLIC advapi32)
endif()

add_executable(snx-test-driver driver_workflow.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/driver.c" "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c"
    "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/process.c")
snx_test_target(snx-test-driver 90)
target_link_libraries(snx-test-driver PRIVATE snx_test_common snx_lz4 OpenSSL::Crypto)
add_test(NAME driver_workflow COMMAND snx-test-driver)
set_tests_properties(driver_workflow PROPERTIES TIMEOUT 60)
add_executable(snx-test-link-file link_file.c)
snx_test_target(snx-test-link-file 90)
target_link_libraries(snx-test-link-file PRIVATE snx_test_common)
add_test(NAME link_file COMMAND snx-test-link-file)
add_executable(snx-test-launcher-name launcher_name.c)
snx_test_target(snx-test-launcher-name 90)
target_link_libraries(snx-test-launcher-name PRIVATE scarletnx-common)
add_test(NAME launcher_name COMMAND snx-test-launcher-name)
add_executable(snx-test-game-title game_title.c)
snx_test_target(snx-test-game-title 90)
target_link_libraries(snx-test-game-title PRIVATE scarletnx-common)
add_test(NAME game_title COMMAND snx-test-game-title)
if(UNIX)
    foreach(name user_dirs left_behind)
        string(REPLACE "_" "-" target "snx-test-${name}")
        add_executable(${target} ${name}.c)
        snx_test_target(${target} 90)
        target_link_libraries(${target} PRIVATE scarletnx-common)
        add_test(NAME ${name} COMMAND ${target})
    endforeach()
endif()

if(UNIX)
    add_executable(snx-test-gdbstub gdbstub.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/gdbstub.c")
    snx_test_target(snx-test-gdbstub 11)
        target_link_libraries(snx-test-gdbstub PRIVATE snx_test_common)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gdbstub_native COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/gdbstub.py" $<TARGET_FILE:snx-test-gdbstub>)
        set_tests_properties(gdbstub_native PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(snx-test-run-ticking run_ticking.c)
snx_test_target(snx-test-run-ticking 90)
target_link_libraries(snx-test-run-ticking PRIVATE snx_test_common)
add_test(NAME run_ticking COMMAND snx-test-run-ticking)

add_executable(snx-test-extract extract.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/extract.c")
snx_test_target(snx-test-extract 90)
target_link_libraries(snx-test-extract PRIVATE snx_test_common)
add_test(NAME extraction_native COMMAND snx-test-extract)

add_executable(snx-test-loader loader_test.c "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c"
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/game_name.c")
snx_test_target(snx-test-loader 90)
target_link_libraries(snx-test-loader PRIVATE snx_test_common snx_lz4 OpenSSL::Crypto)
add_test(NAME loader_native COMMAND snx-test-loader)

add_executable(snx-test-runtime-nro runtime_nro.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/loader.c")
snx_test_target(snx-test-runtime-nro 11)
target_link_libraries(snx-test-runtime-nro PRIVATE snx_lz4)
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
        "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
    snx_test_target(snx-test-ldr-service 11)
        snx_service_swipc(snx-test-ldr-service hle/services/ldr.swipc)
    target_include_directories(snx-test-ldr-service PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(snx-test-ldr-service PRIVATE SNX_RO_INTERP_TEST)
    target_link_libraries(snx-test-ldr-service PRIVATE OpenSSL::Crypto m)
    snx_cpu_helpers_flags(snx-test-ldr-service)
    add_test(NAME ldr_service COMMAND snx-test-ldr-service)
    set_tests_properties(ldr_service PROPERTIES TIMEOUT 60)
endif()

# Deterministic FFmpeg API double: no decoder dependency in tools-only builds.
add_executable(snx-test-nvdec-packets nvdec_packets.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp")
snx_test_target(snx-test-nvdec-packets 11)
target_include_directories(snx-test-nvdec-packets BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_stub")
add_test(NAME nvdec_packets COMMAND snx-test-nvdec-packets)
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-nvdec-decode nvdec_packets.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp")
    snx_test_target(snx-test-nvdec-decode 11)
    target_compile_definitions(snx-test-nvdec-decode PRIVATE SNX_TEST_REAL_FFMPEG)
    target_link_libraries(snx-test-nvdec-decode PRIVATE FFmpeg::avcodec)
    add_test(NAME nvdec_decode COMMAND snx-test-nvdec-decode)
endif()
# VP9 packets rebuilt from NVDEC jobs (gpu/video/vp9_rebuild.cpp): the destination of the frame each emits, and
# setups with invalid tile layouts refused before a header is built
add_executable(snx-test-vp9-rebuild vp9_rebuild.cpp "${PROJECT_SOURCE_DIR}/src/nx/gpu/video/vp9_rebuild.cpp")
snx_test_target(snx-test-vp9-rebuild 23)
add_test(NAME vp9_rebuild COMMAND snx-test-vp9-rebuild)
# the 2D engine's blit in guest memory (fermi2d.c, dma.c's bl_span_ptr): block linear scaling, an odd window, the
# renderer's hooks once a blit
add_executable(snx-test-fermi2d-blit fermi2d_blit.c)
snx_test_target(snx-test-fermi2d-blit 23)
add_test(NAME fermi2d_blit COMMAND snx-test-fermi2d-blit)
# vic.c's CPU composition, through vic_execute, with a renderer that declines the GPU
add_executable(snx-test-vic-compose vic_compose.c)
snx_test_target(snx-test-vic-compose 23)
add_test(NAME vic_compose COMMAND snx-test-vic-compose)

add_executable(snx_recompiler_test recompiler_native.c)
snx_test_target(snx_recompiler_test 23)
target_link_libraries(snx_recompiler_test PRIVATE snx_driver ${CMAKE_DL_LIBS})

# LLVM is always available to the recompiler.
add_test(NAME native_recompiler COMMAND snx_recompiler_test
    $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" "${CMAKE_C_COMPILER}")
add_executable(snx-test-llvm-differential llvm_differential.c)
snx_test_target(snx-test-llvm-differential 11)
target_link_libraries(snx-test-llvm-differential PRIVATE snx_llvm snx_driver ${CMAKE_DL_LIBS})
# In shards that run side by side, each a share of the words, all of them started first (COST): a tree with no times
# of its own (CI's) starts tests in their order, and these are the longest. Fewer, and one runs alone at the end;
# more, and each pays the compiles' start again
foreach(shard RANGE 2)
    add_test(NAME llvm_differential_${shard} COMMAND snx-test-llvm-differential "${CMAKE_C_COMPILER}"
        "${PROJECT_SOURCE_DIR}" --quiet --shard ${shard}/3)
    set_tests_properties(llvm_differential_${shard} PROPERTIES TIMEOUT 900 COST 100)
endforeach()
# not a test: timings of the generated code (llvm_bench SCARLETNX REPO CLANG)
add_executable(snx-llvm-bench llvm_bench.c)
snx_test_target(snx-llvm-bench 11)
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
    if(Python3_Interpreter_FOUND)
        add_test(NAME arm_llvm_witnesses COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
            --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}")
        if(SNX_TEST_ARM_DIFF)
            add_test(NAME arm_llvm_differential COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/arm_test.py"
                --emitter $<TARGET_FILE:snx-test-arm-emit> --runtime $<TARGET_FILE:snx_arm_rt> --cc "${CMAKE_C_COMPILER}" --unicorn)
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
                    --emitter $<TARGET_FILE:snx-test-thumb-emit> --runtime $<TARGET_FILE:snx_arm_rt> --mc "${SNX_LLVM_MC}"
                    --cc "${CMAKE_C_COMPILER}")
                set_tests_properties(thumb_llvm PROPERTIES TIMEOUT 900)
            endif()
        endif()
    endif()
endif()

add_executable(snx-test-llvm-module llvm_module.c)
snx_test_target(snx-test-llvm-module 11)
target_link_libraries(snx-test-llvm-module PRIVATE snx_driver ${CMAKE_DL_LIBS})
if(TARGET snx_jit)
    set(snx_extra_backends gcc)
    target_link_libraries(snx-test-llvm-differential PRIVATE snx_jit)
    target_compile_definitions(snx-test-llvm-differential PRIVATE SNX_TEST_GCCJIT)
    foreach(shard RANGE 1) # as llvm_differential's
        add_test(NAME gccjit_differential_${shard} COMMAND snx-test-llvm-differential "${CMAKE_C_COMPILER}"
            "${PROJECT_SOURCE_DIR}" --gccjit --quiet --per-case 2 --simd 100 --states 4 --shard ${shard}/2)
        set_tests_properties(gccjit_differential_${shard} PROPERTIES TIMEOUT 900 COST 100)
    endforeach()
    add_test(NAME native_recompiler_gcc COMMAND snx_recompiler_test
        $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" "${CMAKE_C_COMPILER}" gcc)
    # code units in this process and in processes of their own (gccjit/jit_units.c)
    add_executable(snx-test-gccjit-units gccjit_units.c)
    snx_test_target(snx-test-gccjit-units 11)
        target_link_libraries(snx-test-gccjit-units PRIVATE snx_jit)
    add_test(NAME gccjit_units COMMAND snx-test-gccjit-units)
endif()
add_test(NAME llvm_module COMMAND snx-test-llvm-module $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}"
    "${CMAKE_C_COMPILER}" ${snx_extra_backends})
# the recompiler as a library (snx_recompiler.h): A64 on every backend built, A32, from code in memory
add_executable(snx-test-recompiler-library recompiler_library.c)
snx_test_target(snx-test-recompiler-library 11)
target_link_libraries(snx-test-recompiler-library PRIVATE scarletnx-recompiler)
add_test(NAME recompiler_library COMMAND snx-test-recompiler-library)

add_executable(snx_codegen_test codegen_test.c)
snx_test_target(snx_codegen_test 90)
target_link_libraries(snx_codegen_test PRIVATE snx_driver)
add_test(NAME native_codegen COMMAND snx_codegen_test "${CMAKE_C_COMPILER}" "${PROJECT_SOURCE_DIR}")

add_executable(snx-test-astc astc_decode.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/astc.c")
snx_test_target(snx-test-astc 23)
add_test(NAME astc_decode COMMAND snx-test-astc)

# the shared fonts pl.c hands out when a game folder has none: the stand-ins compiled in (cmake/shared_fonts.cmake)
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-pl-fonts pl_fonts.c "${CMAKE_BINARY_DIR}/generated/shared_fonts.c")
    snx_test_target(snx-test-pl-fonts 23)
    snx_service_swipc(snx-test-pl-fonts hle/services/pl.swipc)
    add_test(NAME pl_fonts COMMAND snx-test-pl-fonts)
endif()

# audout and hwopus in a fixture, with FFmpeg's Opus decoder for the hwopus decoders that open. FFmpeg::avcodec comes
# from cmake/dependencies.cmake, before this folder, in every build but a tools-only one, which checks the fixture
# without it (SNX_NO_H264, which no runtime build defines)
if(SNX_TOOLS_ONLY)
    add_executable(snx-test-audio-out audio_out.c)
    snx_test_target(snx-test-audio-out 11)
    snx_service_swipc(snx-test-audio-out hle/services/audio.swipc)
    target_compile_definitions(snx-test-audio-out PRIVATE SNX_NO_H264)
    add_test(NAME audio_out COMMAND snx-test-audio-out)
else()
    add_executable(snx-test-audio-opus audio_out.c)
    snx_test_target(snx-test-audio-opus 11)
    snx_service_swipc(snx-test-audio-opus hle/services/audio.swipc)
    target_link_libraries(snx-test-audio-opus PRIVATE FFmpeg::avcodec)
    add_test(NAME audio_opus COMMAND snx-test-audio-opus)
endif()

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

# gpu.c's queue over the real puller, 3D engine and macros: a channel waiting on a semaphore, a syncpoint, a fence or
# inside a macro lets the others run, and goes on where it stopped (its platform stand-ins are pthreads)
if(UNIX)
    add_executable(snx-test-gpu-park gpu_park.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/gpu.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/engines.c" "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/maxwell3d.c"
        "${PROJECT_SOURCE_DIR}/src/nx/gpu/maxwell/mme.c")
    snx_test_target(snx-test-gpu-park 23)
        add_test(NAME gpu_park COMMAND snx-test-gpu-park)
    set_tests_properties(gpu_park PROPERTIES TIMEOUT 30) # a deadlock in the queue would otherwise hang ctest
endif()

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


if(UNIX)
    add_executable(snx-test-memory memory_accounting.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c")
    snx_test_target(snx-test-memory 11)
        add_test(NAME memory_accounting COMMAND snx-test-memory)
    set_tests_properties(memory_accounting PROPERTIES TIMEOUT 60)
endif()

endif() # SNX_BUILD_NX

# dist/icons/ is what scripts/make-icons.py draws from dist/wimsy.svg: a changed SVG fails until it runs again
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
snx_sanitizer_path()
