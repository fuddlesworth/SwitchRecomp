# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME runtime_architecture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/architecture.py"
            --repo "${PROJECT_SOURCE_DIR}" --build "${PROJECT_BINARY_DIR}" --cmake "${CMAKE_COMMAND}")
    # Graph generation reconfigures the existing build tree, so it cannot overlap
    # a native regression test that invokes the project's compiler or driver.
    set_tests_properties(runtime_architecture PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
    add_test(NAME architecture_checker COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/architecture_test.py")
    add_test(NAME gnuabi_differential_harness COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/gnuabi_diff_test.py")
    if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND SNX_TEST_VULKAN)
        add_test(NAME nx_runtime_homebrew
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/nx_runtime_smoke.py"
                --runtime $<TARGET_FILE:scarletnx> --repo "${PROJECT_SOURCE_DIR}"
                --artifact-dir "${PROJECT_BINARY_DIR}/nx-smoke-failures")
        set_tests_properties(nx_runtime_homebrew PROPERTIES TIMEOUT 90)
    endif()
    if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND UNIX AND NOT APPLE)
        add_test(NAME xdg_layout
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/xdg_layout.py" --driver $<TARGET_FILE:scarletnx>)
        set_tests_properties(xdg_layout PROPERTIES TIMEOUT 240)
    endif()
endif()
