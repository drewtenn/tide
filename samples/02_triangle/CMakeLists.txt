# samples/02_triangle — Phase 1 task 6: first colored triangle on Metal.
#
# Apple-only (Metal RHI). Compiles hand-written MSL canaries to .metallib via
# tide_compile_metal_source so the sample runs on a dev machine without the
# DXC toolchain. Once DXC is installed the production HLSL path is preferred
# (see cmake/CompileShader.cmake).

if(NOT TIDE_RHI_METAL)
    return()
endif()

include(CompileShader)

add_executable(02_triangle main.cpp)

target_link_libraries(02_triangle PRIVATE
    Tide::core
    Tide::platform
    Tide::input
    Tide::rhi
    Tide::rhi-metal
)

set_target_properties(02_triangle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/02_triangle"
)

set(_shader_dir "${CMAKE_SOURCE_DIR}/engine/shaders")

tide_compile_metal_source(
    TARGET 02_triangle
    SOURCE "${_shader_dir}/triangle.vs.metal"
    STAGE  vs
)
tide_compile_metal_source(
    TARGET 02_triangle
    SOURCE "${_shader_dir}/triangle.ps.metal"
    STAGE  ps
)
