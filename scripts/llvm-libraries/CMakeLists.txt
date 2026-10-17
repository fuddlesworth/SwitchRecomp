# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The LLVM libraries the recompiler links (cmake/llvm_components.cmake) and every one they link, from a configured LLVM
# build tree, written to OUTPUT as a CMake list: scripts/android-llvm.sh and scripts/apple-llvm.sh build only those,
# as LLVM_DISTRIBUTION_COMPONENTS, which also leaves the others out of the package they install. Reading LLVM's own
# package keeps it right for each LLVM version, where a list written here would go stale
#
#   cmake -S scripts/llvm-libraries -B DIR -DLLVM_BUILD=TREE -DOUTPUT=FILE
cmake_minimum_required(VERSION 3.25)
project(snx_llvm_libraries LANGUAGES C)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/llvm_components.cmake")
find_package(LLVM REQUIRED CONFIG PATHS "${LLVM_BUILD}/lib/cmake/llvm" NO_DEFAULT_PATH)
llvm_map_components_to_libnames(snx_wanted ${SNX_LLVM_COMPONENTS})
set(snx_libraries "")
set(snx_todo ${snx_wanted})
while(snx_todo)
    list(POP_FRONT snx_todo snx_library)
    if(snx_library IN_LIST snx_libraries OR NOT TARGET ${snx_library})
        continue()
    endif()
    list(APPEND snx_libraries ${snx_library})
    get_target_property(snx_links ${snx_library} INTERFACE_LINK_LIBRARIES)
    if(NOT snx_links)
        continue()
    endif()
    # a static library's own links are exported as $<LINK_ONLY:...>; the system's are no LLVM targets (m, pthread, or
    # ZLIB::ZLIB from a package LLVM found)
    foreach(snx_link IN LISTS snx_links)
        string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" snx_link "${snx_link}")
        if(TARGET ${snx_link} AND snx_link MATCHES "^LLVM")
            list(APPEND snx_todo ${snx_link})
        endif()
    endforeach()
endwhile()
list(SORT snx_libraries)
list(LENGTH snx_libraries snx_count)
message(STATUS "LLVM libraries for ${SNX_LLVM_COMPONENTS}: ${snx_count}")
file(WRITE "${OUTPUT}" "${snx_libraries}")
