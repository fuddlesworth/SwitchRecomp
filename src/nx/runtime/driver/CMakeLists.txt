# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# what the backends share: function discovery over NX images, chunk labels, progress lines, the entry table
add_library(snx_nx_module STATIC ../../loader/module.c ../../loader/codegen_view.c)
set_target_properties(snx_nx_module PROPERTIES C_STANDARD ${SNX_C90_STANDARD} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_nx_module PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_nx_module PUBLIC scarletnx-common)
add_library(snx_driver STATIC driver.c extract.c game_name.c recompile.c toolchain.c ../../loader/loader.c)
set_target_properties(snx_driver PROPERTIES C_STANDARD ${SNX_C90_STANDARD} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(snx_driver PRIVATE
    SNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}" SNX_BUILD_DIR="${CMAKE_BINARY_DIR}" _CRT_SECURE_NO_WARNINGS
    SNX_NX_ABI_INCLUDE="${CMAKE_SOURCE_DIR}/src/nx/loader"
    SNX_COMMON_INCLUDE="${CMAKE_SOURCE_DIR}/src/common")
target_compile_options(snx_driver PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_driver PUBLIC scarletnx-common snx_nx_module snx_lz4 OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(snx_driver PUBLIC advapi32)
endif()
if(NOT SNX_TOOLS_ONLY)
    target_compile_definitions(snx_driver PRIVATE SNX_WITH_RUNTIME)
endif()
include("${CMAKE_SOURCE_DIR}/cmake/llvm.cmake")
add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/recompiler/llvm" llvm-backend)
# the NX game module through the LLVM backend; C11 for the LLVM-C headers, the driver itself stays C90
add_library(snx_driver_llvm STATIC ../../loader/module_llvm.c ../../loader/module_arm.c)
set_target_properties(snx_driver_llvm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_compile_options(snx_driver_llvm PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_driver_llvm PUBLIC snx_llvm snx_arm_llvm snx_nx_module)
target_link_libraries(snx_driver PUBLIC snx_driver_llvm)
include("${CMAKE_SOURCE_DIR}/cmake/gccjit.cmake")
if(SNX_GCCJIT_FOUND)
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/recompiler/gccjit" gccjit-backend)
    # module_gccjit.c with libgccjit: C11 for its header, linked with the LLVM backend's runtime library
    add_library(snx_driver_gccjit STATIC ../../loader/module_gccjit.c)
    set_target_properties(snx_driver_gccjit PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
    target_compile_definitions(snx_driver_gccjit PRIVATE SNX_WITH_GCCJIT)
    target_compile_options(snx_driver_gccjit PRIVATE -Wall -Wextra -Werror=missing-prototypes)
    target_link_libraries(snx_driver_gccjit PUBLIC snx_jit snx_llvm snx_nx_module)
    target_link_libraries(snx_driver PUBLIC snx_driver_gccjit)
else()
    target_sources(snx_driver PRIVATE ../../loader/module_gccjit.c) # the stub: built without libgccjit
endif()
# the recompiler as a library (snx_recompiler.h), over the backends configured above
add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/recompiler" recompiler-library)
if(SNX_TOOLS_ONLY)
    add_executable(scarletnx main.c)
    set_target_properties(scarletnx PROPERTIES C_STANDARD ${SNX_C90_STANDARD} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(scarletnx PRIVATE snx_driver)
    if(WIN32)
        if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
            target_link_options(scarletnx PRIVATE "LINKER:/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/src/nx/runtime/platform/runtime.manifest")
        else()
            enable_language(RC)
            target_sources(scarletnx PRIVATE "${CMAKE_SOURCE_DIR}/src/nx/runtime/platform/runtime.rc")
            # where the resource compiler finds the manifest and the icon; C includes from src/ alone
            target_include_directories(scarletnx PRIVATE
                                       "$<$<COMPILE_LANGUAGE:RC>:${CMAKE_SOURCE_DIR}/src/nx/runtime/platform>")
        endif()
    endif()
endif()
