# swr-shaderc: translates a shader corpus (SWR_SHADER_CORPUS) or a game's NVN shader files offline and compiles them
# to SPIR-V, with the runtime's shader and spirv components
add_executable(swr-shaderc swr_shaderc.c)
target_link_libraries(swr-shaderc PRIVATE swr_shader swr_spirv "${DEPS}/lib/libglslang-all.a")
if(NOT WIN32)
    target_link_libraries(swr-shaderc PRIVATE pthread m)
endif()
set_target_properties(swr-shaderc PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
