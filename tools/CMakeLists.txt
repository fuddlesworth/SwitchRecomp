# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# snx-shaderc: translates a shader corpus (SNX_SHADER_CORPUS) or a game's NVN shader files offline and compiles them
# to SPIR-V, with the runtime's shader and spirv components
add_executable(snx-shaderc snx_shaderc.c)
target_link_libraries(snx-shaderc PRIVATE snx_shader snx_spirv)
if(NOT WIN32)
    target_link_libraries(snx-shaderc PRIVATE Threads::Threads m)
endif()
set_target_properties(snx-shaderc PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
