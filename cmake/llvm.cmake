# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's C API is required by the recompiler on every platform. SNX_LLVM_CONFIG selects an llvm-config that runs
# on the build machine and describes the LLVM development files for the target, including when cross-compiling.
if(ANDROID OR IOS OR (APPLE AND LLVM_DIR AND NOT SNX_LLVM_CONFIG))
    # Android's and iOS's LLVM (scripts/android-llvm.sh, scripts/apple-llvm.sh) have an llvm-config that only runs on the
    # device, and the Mac's from scripts/apple-llvm.sh none: their CMake package instead, named by LLVM_DIR
    find_package(LLVM REQUIRED CONFIG)
    llvm_map_components_to_libnames(SNX_LLVM_LIBS core native passes target analysis mcjit)
    set(SNX_LLVM_VERSION "${LLVM_PACKAGE_VERSION}")
    set(SNX_LLVM_INCLUDE_DIR "${LLVM_INCLUDE_DIRS}")
    set(SNX_LLVM_LIBRARY_DIR "${LLVM_LIBRARY_DIRS}")
    set(SNX_LLVM_SYSTEM_LIBS "")
    message(STATUS "LLVM backend: LLVM ${SNX_LLVM_VERSION} (${LLVM_DIR})")
    return()
endif()
set(snx_llvm_names llvm-config)
foreach(version RANGE 30 17 -1)
    list(APPEND snx_llvm_names llvm-config-${version} llvm-config${version})
endforeach()
find_program(SNX_LLVM_CONFIG NAMES ${snx_llvm_names} DOC "llvm-config of the LLVM the LLVM backend links")
if(NOT SNX_LLVM_CONFIG)
    message(FATAL_ERROR "LLVM's development files are required by the recompiler on every platform. Install them "
                        "or point SNX_LLVM_CONFIG at an llvm-config for the target LLVM installation.")
endif()

function(snx_llvm_query output)
    execute_process(COMMAND "${SNX_LLVM_CONFIG}" ${ARGN} OUTPUT_VARIABLE value
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM is required, but ${SNX_LLVM_CONFIG} ${ARGN} failed (${result}): ${error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

snx_llvm_query(SNX_LLVM_VERSION --version)
snx_llvm_query(SNX_LLVM_INCLUDE_DIR --includedir)
snx_llvm_query(SNX_LLVM_LIBRARY_DIR --libdir)
snx_llvm_query(SNX_LLVM_LIBS --libs core native passes target analysis mcjit)
snx_llvm_query(SNX_LLVM_SYSTEM_LIBS --system-libs core native passes target analysis mcjit)
if(NOT SNX_LLVM_VERSION OR NOT EXISTS "${SNX_LLVM_INCLUDE_DIR}/llvm-c/Core.h" OR
   NOT IS_DIRECTORY "${SNX_LLVM_LIBRARY_DIR}" OR NOT SNX_LLVM_LIBS)
    message(FATAL_ERROR "LLVM's development files are required, but ${SNX_LLVM_CONFIG} reports an incomplete "
                        "installation. Check its headers and libraries or select another SNX_LLVM_CONFIG.")
endif()
separate_arguments(SNX_LLVM_LIBS UNIX_COMMAND "${SNX_LLVM_LIBS}")
separate_arguments(SNX_LLVM_SYSTEM_LIBS UNIX_COMMAND "${SNX_LLVM_SYSTEM_LIBS}")

function(snx_llvm_windows_libraries output)
    # With -static, MinGW's linker will not find libLLVM-23.dll.a through
    # -lLLVM-23. Pass the exact import library instead, without changing the
    # linkage of the toolchain or SDL. llvm-config --libfiles is not a substitute:
    # on Windows it reports the runtime DLL in bin/, not its import library.
    set(CMAKE_FIND_LIBRARY_PREFIXES "lib" "")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".dll.a" ".a" ".lib")
    set(libraries "")
    foreach(library IN LISTS ARGN)
        if(library MATCHES "^-l(.+)$")
            set(name "${CMAKE_MATCH_1}")
            set(resolved "resolved-NOTFOUND")
            find_library(resolved NAMES "${name}" PATHS "${SNX_LLVM_LIBRARY_DIR}"
                         NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE)
            if(NOT resolved)
                message(FATAL_ERROR "LLVM is required, but ${library} from ${SNX_LLVM_CONFIG} has no library in "
                                    "${SNX_LLVM_LIBRARY_DIR}. Install the target LLVM development libraries.")
            endif()
            list(APPEND libraries "${resolved}")
        else()
            list(APPEND libraries "${library}")
        endif()
    endforeach()
    set(${output} "${libraries}" PARENT_SCOPE)
endfunction()
if(WIN32)
    snx_llvm_windows_libraries(SNX_LLVM_LIBS ${SNX_LLVM_LIBS})
endif()
message(STATUS "LLVM backend: LLVM ${SNX_LLVM_VERSION} (${SNX_LLVM_CONFIG})")
