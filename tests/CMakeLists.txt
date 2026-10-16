# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
option(SNX_TEST_SANITIZERS "Run native regression tests with ASan and UBSan" OFF)
option(SNX_TEST_VULKAN "Run offscreen Vulkan integration tests (requires a Vulkan 1.3 ICD, e.g. lavapipe)" OFF)
option(SNX_TEST_ARM_AOT "Build asset-free static ARM11/checked-memory witnesses (requires native LLVM SDK)" OFF)
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

if(TARGET snx_ctr_board)
    if(SNX_TEST_SANITIZERS)
        foreach(target snx_ctr_board snx_ctr_memory snx_ctr_loader snx_ctr_cpu)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} INTERFACE -fsanitize=address,undefined)
        endforeach()
        target_compile_options(scarletnx-3ds PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endif()
    add_executable(snx-test-3ds-board 3ds_board.c)
    snx_test_target(snx-test-3ds-board 11)
    target_compile_options(snx-test-3ds-board PRIVATE -UNDEBUG)
    target_link_libraries(snx-test-3ds-board PRIVATE snx_ctr_board)
    add_test(NAME 3ds_board COMMAND snx-test-3ds-board)
    add_executable(snx-test-3ds-memory 3ds_memory.c)
    snx_test_target(snx-test-3ds-memory 11)
    target_link_libraries(snx-test-3ds-memory PRIVATE snx_ctr_memory)
    add_test(NAME 3ds_memory COMMAND snx-test-3ds-memory)
    add_executable(snx-test-3ds-blz 3ds_blz.c)
    snx_test_target(snx-test-3ds-blz 11)
    target_link_libraries(snx-test-3ds-blz PRIVATE snx_ctr_loader)
    add_test(NAME 3ds_blz COMMAND snx-test-3ds-blz)
    foreach(target snx-test-3ds-memory snx-test-3ds-blz)
        target_compile_options(${target} PRIVATE -UNDEBUG)
    endforeach()

    if (NOT WIN32)
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME 3ds_driver COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_driver.py"
                $<TARGET_FILE:scarletnx-3ds>)
            add_test(NAME 3ds_extract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/3ds_extract.py"
                $<TARGET_FILE:scarletnx-3ds>)
            set_tests_properties(3ds_extract PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()

add_executable(snx-test-arm-profile arm_profile.c "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/a32.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/thumb_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/vector_decode.c"
    "${PROJECT_SOURCE_DIR}/src/common/cpu/arm/profile.c")
snx_test_target(snx-test-arm-profile 11)
target_compile_options(snx-test-arm-profile PRIVATE -UNDEBUG)
add_test(NAME arm_isa_profiles COMMAND snx-test-arm-profile)
if(SNX_TEST_ARM_AOT)
    include(arm_checked.cmake)
endif()

# Board/GUI-only builds do not configure either AOT runtime or LLVM. Do not
# create CPU execution tests that link the absent backend targets in that case.
if(NOT SNX_BUILD_NX AND NOT SNX_BUILD_GNUABI)
    include(architecture.cmake)
    return()
endif()

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

# This IPC fixture stubs the kernel and needs no NX graphics/media dependencies. The tests that include a service's
# file turn off -Wextra's unused-parameter warning: a handler takes every argument of its command, used or not
add_executable(snx-test-bsd-service bsd_service.c)
snx_test_target(snx-test-bsd-service 23)
target_compile_options(snx-test-bsd-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-bsd-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
          hle/services/network/bsd.swipc)
target_link_libraries(snx-test-bsd-service PRIVATE scarletnx-net)
add_test(NAME bsd_service COMMAND snx-test-bsd-service)
set_tests_properties(bsd_service PROPERTIES TIMEOUT 60)

# sfdnsres and nifm (dns.c, nifm.c) offline, with bsd.c beside them for whether the console is online
add_executable(snx-test-network-service network_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c")
snx_test_target(snx-test-network-service 23)
target_compile_options(snx-test-network-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-network-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
          hle/services/network/bsd.swipc hle/services/network/dns.swipc hle/services/network/nifm.swipc)
target_link_libraries(snx-test-network-service PRIVATE scarletnx-net)
add_test(NAME network_service COMMAND snx-test-network-service)
set_tests_properties(network_service PROPERTIES TIMEOUT 60)

# ssl (ssl.c) against a TLS server in the test, on the game's bsd sockets (bsd.c, built beside)
add_executable(snx-test-ssl-service ssl_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/network/bsd.c")
snx_test_target(snx-test-ssl-service 23)
target_compile_options(snx-test-ssl-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-ssl-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
          hle/services/network/bsd.swipc hle/services/network/ssl.swipc)
target_link_libraries(snx-test-ssl-service PRIVATE scarletnx-net OpenSSL::SSL)
add_test(NAME ssl_service COMMAND snx-test-ssl-service)
set_tests_properties(ssl_service PROPERTIES TIMEOUT 60)

# mm:u and hid's IHidServer (misc.c, hid.c), with the kernel objects stubbed
add_executable(snx-test-misc-service misc_service.c "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c")
snx_test_target(snx-test-misc-service 23)
target_compile_options(snx-test-misc-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-misc-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
          hle/services/hid.swipc hle/services/misc.swipc)
target_link_libraries(snx-test-misc-service PRIVATE OpenSSL::Crypto)
add_test(NAME misc_service COMMAND snx-test-misc-service)

# nvdrv's IPC side (nvdrv.c), with the GPU and host1x stand-ins in the test
add_executable(snx-test-nvdrv-service nvdrv_service.c)
snx_test_target(snx-test-nvdrv-service 23)
target_compile_options(snx-test-nvdrv-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-nvdrv-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
          hle/services/nvdrv.swipc)
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

add_executable(snx-test-fatal-screen fatal_screen.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/fatal_screen.c")
snx_test_target(snx-test-fatal-screen 11)
target_link_libraries(snx-test-fatal-screen PRIVATE scarletnx-common)
add_test(NAME fatal_screen COMMAND snx-test-fatal-screen)

add_executable(snx-test-ini ini.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/ini.c")
snx_test_target(snx-test-ini 11)
target_compile_options(snx-test-ini PRIVATE -UNDEBUG) # its checks are asserts
add_test(NAME ini COMMAND snx-test-ini)
add_executable(snx-test-ini-lines ini_lines.c "${PROJECT_SOURCE_DIR}/src/common/utilities/ini_lines.c")
snx_test_target(snx-test-ini-lines 90)
target_compile_options(snx-test-ini-lines PRIVATE -UNDEBUG) # its checks are asserts
add_test(NAME ini_lines COMMAND snx-test-ini-lines)

# the controls' atlas and their shaders are generated and checked in: their sources must still make them
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME controls_atlas_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-controls.py" --check)
    add_test(NAME ui_shaders_current
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/build-ui-shaders.py" --check)
    add_test(NAME generators_crlf COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/generators_crlf.py")
    # which GitHub Actions caches the cleanup workflow deletes (scripts/trim-caches.py)
    add_test(NAME trim_caches COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/trim_caches_test.py")
    # the nightly release's title and notes (scripts/nightly-notes.py)
    add_test(NAME nightly_notes COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nightly_notes_test.py")
endif()

# a package patched in the build tree from the cache's copy as released (cmake/patched_copy.cmake)
add_test(NAME patched_copy COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/patched-copy-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/patched_copy_test.cmake")
# the dependency cache's keys and how a build is published to it (cmake/deps_cache.cmake)
add_test(NAME deps_cache COMMAND "${CMAKE_COMMAND}" "-DCC=${CMAKE_C_COMPILER}" "-DSRC=${PROJECT_SOURCE_DIR}"
         "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/deps-cache-test" -P "${CMAKE_CURRENT_SOURCE_DIR}/deps_cache_test.cmake")

add_executable(snx-test-hid-touch hid_touch.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/platform/touch.c"
    "${PROJECT_SOURCE_DIR}/src/nx/hle/services/hid_touch.c")
snx_test_target(snx-test-hid-touch 11)
target_link_libraries(snx-test-hid-touch PRIVATE scarletnx-common)
add_test(NAME hid_touch COMMAND snx-test-hid-touch)

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
    add_executable(snx-test-data-dir data_dir.c)
    snx_test_target(snx-test-data-dir 90)
    target_link_libraries(snx-test-data-dir PRIVATE scarletnx-common)
    add_test(NAME data_dir COMMAND snx-test-data-dir)
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
    target_compile_options(snx-test-ldr-service PRIVATE -Wno-unused-parameter)
    snx_swipc(snx-test-ldr-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
              hle/services/ldr.swipc)
    target_include_directories(snx-test-ldr-service PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(snx-test-ldr-service PRIVATE SNX_RO_INTERP_TEST)
    target_link_libraries(snx-test-ldr-service PRIVATE OpenSSL::Crypto m)
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

add_executable(snx-test-audio-out audio_out.c)
snx_test_target(snx-test-audio-out 11)
# a service's handlers take every argument of their command, used or not
target_compile_options(snx-test-audio-out PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-audio-out ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc hle/services/audio.swipc)
# Isolate audout in this fixture without FFmpeg, also when testing a tools-only build.
# Production NX targets always link FFmpeg and never define SNX_NO_H264.
target_compile_definitions(snx-test-audio-out PRIVATE SNX_NO_H264)
add_test(NAME audio_out COMMAND snx-test-audio-out)
# the same fixture with FFmpeg's Opus decoder, for the hwopus decoders that open. FFmpeg::avcodec is defined after this
# folder (cmake/dependencies.cmake), in every build but a tools-only one
if(NOT SNX_TOOLS_ONLY)
    add_executable(snx-test-audio-opus audio_out.c)
    snx_test_target(snx-test-audio-opus 11)
    target_compile_options(snx-test-audio-opus PRIVATE -Wno-unused-parameter)
    snx_swipc(snx-test-audio-opus ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
              hle/services/audio.swipc)
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
target_compile_options(snx-test-fs-service PRIVATE -Wno-unused-parameter)
snx_swipc(snx-test-fs-service ROOT "${PROJECT_SOURCE_DIR}/src/nx" FILES hle/services/types.swipc
          hle/filesystem/fs.swipc)
add_test(NAME filesystem_raw
    COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> raw)

if(UNIX)
    add_executable(snx-test-memory memory_accounting.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c")
    snx_test_target(snx-test-memory 11)
    add_test(NAME memory_accounting COMMAND snx-test-memory)
    set_tests_properties(memory_accounting PROPERTIES TIMEOUT 60)
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

# Every test of this folder, added above: a sanitized program needs the sanitizer runtime's DLLs on PATH, and the
# others don't mind them
if(SNX_TEST_SANITIZERS AND WIN32)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE snx_clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    get_property(snx_sanitized_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${snx_sanitized_tests}
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${snx_clang_resource}/lib/windows")
endif()
