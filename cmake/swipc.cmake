# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# swipc (tools/swipc/swipc.c) generates the IPC dispatchers from the interface definitions (.swipc, docs/IPC.md). It
# runs during the build, so a cross build also builds it for the machine doing the build, with that machine's C
# compiler (SNX_HOST_CC, which FFmpeg's cross build uses too)
include_guard(GLOBAL)
if(CMAKE_CROSSCOMPILING)
    include("${CMAKE_CURRENT_LIST_DIR}/host_cc.cmake")
    include(ExternalProject)
    set(snx_swipc_dir "${CMAKE_BINARY_DIR}/swipc-host")
    set(snx_swipc_exe "${snx_swipc_dir}/bin/swipc")
    if(CMAKE_HOST_WIN32)
        string(APPEND snx_swipc_exe ".exe")
    endif()
    # the output folder is named for every configuration, so a multi-config generator puts it there too
    ExternalProject_Add(snx_swipc_host
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/tools/swipc"
        BINARY_DIR "${snx_swipc_dir}"
        # no -Werror: the host compiler may not be clang (SNX_HOST_CC), and its warnings shouldn't stop a cross build
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=${SNX_HOST_CC}"
                   "-DCMAKE_C_FLAGS=-pedantic-errors -Wall -Wextra"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${snx_swipc_dir}/bin"
                   "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=${snx_swipc_dir}/bin"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        # a local source has no download step to tell the build that swipc.c changed: its own build decides
        BUILD_ALWAYS 1
        BUILD_BYPRODUCTS "${snx_swipc_exe}"
        INSTALL_COMMAND "")
    # GLOBAL: tests/ includes this file before src/nx does, and a plain imported target is only seen below the
    # folder that made it
    add_executable(snx-swipc IMPORTED GLOBAL)
    set_target_properties(snx-swipc PROPERTIES IMPORTED_LOCATION "${snx_swipc_exe}")
    add_dependencies(snx-swipc snx_swipc_host)
else()
    add_executable(snx-swipc "${PROJECT_SOURCE_DIR}/tools/swipc/swipc.c")
    set_target_properties(snx-swipc PROPERTIES C_STANDARD ${SNX_C90_STANDARD} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(snx-swipc PRIVATE -pedantic-errors -Wall -Wextra -Werror)
endif()

# snx_swipc(<target> ROOT <dir> FILES <file.swipc>...): generates <file.swipc>.h and .inc in <target>-swipc in the
# build folder for each file (relative to ROOT, types before their first use) and gives <target> the headers' include
# root. The C file that defines an interface's handlers includes its "<file.swipc>.inc", other files the .h. A test
# that compiles a service's C file generates that service's files for itself the same way. swipc only rewrites an
# output that changed, which the Ninja generators see (restat)
function(snx_swipc target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "CTR" "ROOT" "FILES")
    set(mode)
    if(arg_CTR)
        set(mode --ctr)
    endif()
    set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}-swipc")
    set(inputs)
    set(outputs)
    set(dirs)
    foreach(f IN LISTS arg_FILES)
        get_filename_component(dir "${out}/${f}" DIRECTORY)
        list(APPEND dirs "${dir}")
        list(APPEND inputs "${arg_ROOT}/${f}")
        list(APPEND outputs "${out}/${f}.h" "${out}/${f}.inc")
    endforeach()
    list(REMOVE_DUPLICATES dirs)
    add_custom_command(OUTPUT ${outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory ${dirs}
        COMMAND snx-swipc ${mode} --root "${arg_ROOT}" --out "${out}" ${arg_FILES}
        DEPENDS snx-swipc ${inputs}
        COMMENT "Generating the IPC dispatchers of ${target}" VERBATIM)
    # listed as sources so they are generated before anything of the target compiles
    target_sources(${target} PRIVATE ${outputs})
    target_include_directories(${target} PUBLIC "${out}")
endfunction()
