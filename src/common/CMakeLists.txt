# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(scarletnx-common STATIC
    utilities/common.c utilities/process.c utilities/font.c utilities/ini_lines.c utilities/pad_map.c
    utilities/pad_screen.c
    cpu/aarch64/a64.c cpu/recompiler/codegen.c cpu/recompiler/backend.c cpu/execution/execution.c)
target_compile_definitions(scarletnx-common PRIVATE _CRT_SECURE_NO_WARNINGS
    SNX_HOST_COMPILER="${CMAKE_C_COMPILER}"
    SNX_CPU_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/cpu/aarch64"
    SNX_SOURCE_INCLUDE="${PROJECT_SOURCE_DIR}/src")
target_compile_options(scarletnx-common PRIVATE -fno-strict-aliasing -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-common PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(scarletnx-common PUBLIC advapi32)
else()
    target_link_libraries(scarletnx-common PUBLIC m)
endif()

# The same indexed host filesystem and shared immutable file views serve NX
# and CTR. No guest ABI or runtime/platform dependency enters this target.
add_library(scarletnx-filesystem STATIC filesystem/tree.c filesystem/host.c)
set_target_properties(scarletnx-filesystem PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(scarletnx-filesystem PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
target_compile_options(scarletnx-filesystem PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-filesystem PUBLIC Threads::Threads)

# The emulated network stack (network/net.h), shared by nx's bsd and sfdnsres and the GNU ABI's socket syscalls. A
# library of its own so the C90 tools don't link Winsock
add_library(scarletnx-net STATIC network/net.c network/net_icmp.c network/net_event.c network/net_dns.c
    network/net_iface.c)
target_compile_options(scarletnx-net PRIVATE -fno-strict-aliasing -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-net PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(scarletnx-net PUBLIC ws2_32 iphlpapi)
elseif(APPLE) # the system resolver's DNS servers (network/net_iface.c)
    target_link_libraries(scarletnx-net PUBLIC resolv)
endif()
