# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the SwitchRecomp fork contributors
# hactool (target hactool), which scripts/extract.py uses to unpack game dumps, with the mbedtls copy it bundles. Its
# Makefile builds mbedtls with mbedtls's own Makefile, whose CMake script is too old for CMake 4, so both are built
# from their sources here. hactool only uses mbedtls's crypto, and the linker leaves the rest of the library out.
set(src "${hactool_SOURCE_DIR}")
file(GLOB mbedtls_sources CONFIGURE_DEPENDS "${src}/mbedtls/library/*.c")
add_library(hactool_mbedtls STATIC ${mbedtls_sources})
# A plain -I, not -isystem: the compiler searches every -I folder before any -isystem one, so with -isystem an
# -I/usr/local/include in CFLAGS (usual on FreeBSD) put a system mbedtls 3 ahead of the bundled 2.x headers
target_include_directories(hactool_mbedtls BEFORE PUBLIC "${src}/mbedtls/include")
target_compile_options(hactool_mbedtls PRIVATE -w)
set_target_properties(hactool_mbedtls PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

set(hactool_sources save sha aes extkeys rsa npdm bktr kip packages pki pfs0 hfs0 nca0_romfs romfs utils nax0 nso lz4
                    nca xci main filepath ConvertUTF cJSON)
list(TRANSFORM hactool_sources PREPEND "${src}/")
list(TRANSFORM hactool_sources APPEND .c)
add_executable(hactool ${hactool_sources})
target_link_libraries(hactool PRIVATE hactool_mbedtls)
target_compile_definitions(hactool PRIVATE _BSD_SOURCE _POSIX_SOURCE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE
                                           __USE_MINGW_ANSI_STDIO=1 _FILE_OFFSET_BITS=64)
target_compile_options(hactool PRIVATE -w)
set_target_properties(hactool PROPERTIES C_STANDARD 11 C_EXTENSIONS ON RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    target_link_options(hactool PRIVATE -static)
endif()
