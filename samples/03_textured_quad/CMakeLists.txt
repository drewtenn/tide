# samples/03_textured_quad — Phase 1 task 7: rotating textured quad on Metal.
#
# First sample to exercise the production HLSL → SPIR-V → MSL pipeline (the
# DXC toolchain fix from this same session unblocks it). Uses tide_compile_shader
# instead of tide_compile_metal_source.

if(NOT TIDE_RHI_METAL)
    return()
endif()

include(CompileShader)

add_executable(03_textured_quad main.cpp)

target_link_libraries(03_textured_quad PRIVATE
    Tide::core
    Tide::platform
    Tide::input
    Tide::rhi
    Tide::rhi-metal
)

# Sample needs the cbuffer layout struct from engine/shaders/PerFrame.h.
# Narrow include — only the shaders dir, not all of engine/.
target_include_directories(03_textured_quad PRIVATE "${CMAKE_SOURCE_DIR}/engine/shaders")

set_target_properties(03_textured_quad PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/03_textured_quad"
)

set(_shader_dir "${CMAKE_SOURCE_DIR}/engine/shaders")

tide_compile_shader(
    TARGET 03_textured_quad
    SOURCE "${_shader_dir}/textured_quad.vs.hlsl"
    STAGE  vs
)
tide_compile_shader(
    TARGET 03_textured_quad
    SOURCE "${_shader_dir}/textured_quad.ps.hlsl"
    STAGE  ps
)
