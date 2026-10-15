# SPDX-License-Identifier: MPL-2.0
# Copyright 2026 the mGBA contributors
# Copyright 2026 the ScarletNX contributors
# The AGB (Game Boy Advance) board runtime: the board emulation scarletnx-agb runs GBA ROMs on. The board
# library is independent of the NX and GNU ABI runtimes; the executable is its system type entry point.
#
#   scarletnx-agb-runtime   the board: cartridge, memory map, devices, and the AOT recompiler
#   scarletnx-agb-run       the system type's executable, named scarletnx-agb

# The ARM and Thumb recompiler of src/common/cpu/arm, the same one the other system types use.
if(NOT TARGET snx_arm_llvm)
    include("${CMAKE_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/arm" agb-arm-backend)
endif()

add_library(scarletnx-agb STATIC board/board.c board/memory.c board/dma.c board/timers.c board/save.c
                                 cartridge/cartridge.c bios/bios.c ppu/ppu.c apu/apu.c cpu/memmap.c cpu/recomp.c
                                 cpu/cpu.c)
set_target_properties(scarletnx-agb PROPERTIES OUTPUT_NAME scarletnx-agb-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_include_directories(scarletnx-agb PUBLIC "${PROJECT_SOURCE_DIR}/src/agb" "${PROJECT_SOURCE_DIR}/src/common")
target_link_libraries(scarletnx-agb PUBLIC scarletnx-common snx_arm_llvm)
target_compile_definitions(scarletnx-agb PRIVATE SNX_AGB_CC="${CMAKE_C_COMPILER}")
target_compile_options(scarletnx-agb PRIVATE -Wall -Wextra -Werror)

add_executable(scarletnx-agb-run runtime/main.c)
target_link_libraries(scarletnx-agb-run PRIVATE scarletnx-agb)
set_target_properties(scarletnx-agb-run PROPERTIES OUTPUT_NAME scarletnx-agb
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" C_STANDARD 23 C_STANDARD_REQUIRED ON)

if(BUILD_TESTING)
    add_executable(snx-test-agb-board "${PROJECT_SOURCE_DIR}/tests/agb_board.c")
    target_link_libraries(snx-test-agb-board PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-board PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-board PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_board COMMAND snx-test-agb-board)
    add_executable(snx-test-agb-bios "${PROJECT_SOURCE_DIR}/tests/agb_bios.c")
    target_link_libraries(snx-test-agb-bios PRIVATE scarletnx-agb m)
    target_compile_options(snx-test-agb-bios PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-bios PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_bios COMMAND snx-test-agb-bios)
    add_executable(snx-test-agb-ppu "${PROJECT_SOURCE_DIR}/tests/agb_ppu.c")
    target_link_libraries(snx-test-agb-ppu PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-ppu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-ppu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_ppu COMMAND snx-test-agb-ppu)
    add_executable(snx-test-agb-apu "${PROJECT_SOURCE_DIR}/tests/agb_apu.c")
    target_link_libraries(snx-test-agb-apu PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-apu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-apu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_apu COMMAND snx-test-agb-apu)
    add_executable(snx-test-agb-cpu "${PROJECT_SOURCE_DIR}/tests/agb_cpu.c")
    target_link_libraries(snx-test-agb-cpu PRIVATE scarletnx-agb)
    target_compile_definitions(snx-test-agb-cpu PRIVATE SNX_AGB_TEST_CC="${CMAKE_C_COMPILER}")
    target_compile_options(snx-test-agb-cpu PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-cpu PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_cpu COMMAND snx-test-agb-cpu)
endif()

include(GNUInstallDirs)
install(TARGETS scarletnx-agb-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
