# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx_build_info: the line the window titles carry, "ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)". The branch
# and commit are read at every build (cmake/commit.cmake), the rest is the compiler's own macros
include_guard(GLOBAL)
set(snx_commit_header "${CMAKE_BINARY_DIR}/generated/snx_commit.h")
add_custom_target(snx_commit
    COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}" "-DOUT=${snx_commit_header}"
            -P "${PROJECT_SOURCE_DIR}/cmake/commit.cmake"
    BYPRODUCTS "${snx_commit_header}" VERBATIM)
add_library(snx_build_info STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/build_info.c")
add_dependencies(snx_build_info snx_commit)
target_include_directories(snx_build_info PRIVATE "${CMAKE_BINARY_DIR}/generated")
set_target_properties(snx_build_info PROPERTIES C_STANDARD ${SNX_C90_STANDARD} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_options(snx_build_info PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
if(BUILD_TESTING)
    add_executable(snx-test-build-info "${PROJECT_SOURCE_DIR}/tests/build_info.c")
    target_link_libraries(snx-test-build-info PRIVATE snx_build_info)
    target_include_directories(snx-test-build-info PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_compile_options(snx-test-build-info PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME build_info COMMAND snx-test-build-info)
    add_test(NAME build_info_commit COMMAND "${CMAKE_COMMAND}" "-DSRC=${PROJECT_SOURCE_DIR}"
             "-DOUT=${snx_commit_header}" -DCHECK=1 -P "${PROJECT_SOURCE_DIR}/cmake/commit.cmake")
endif()
