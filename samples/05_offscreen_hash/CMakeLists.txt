# samples/05_offscreen_hash — Phase 1 task 10: offscreen-readback golden hash.
#
# Renders a deterministic frame to an offscreen MTLTexture, downloads the
# pixels, and prints the xxh3 64-bit hash. With --check <hex>, asserts the
# hash matches and exits 0 / 1. The accompanying ctest runs the binary with
# --check against a hardcoded golden so any future regression that flips a
# pixel (the slot-0 binding bug, the bind_descriptor_set type mismatch,
# etc.) trips automatically.

if(NOT TIDE_RHI_METAL)
    return()
endif()

find_package(xxHash CONFIG REQUIRED)
include(CompileShader)

add_executable(05_offscreen_hash main.cpp)

target_link_libraries(05_offscreen_hash PRIVATE
    Tide::core
    Tide::platform
    Tide::rhi
    Tide::rhi-metal
    xxHash::xxhash
)

set_target_properties(05_offscreen_hash PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/05_offscreen_hash"
)

set(_shader_dir "${CMAKE_SOURCE_DIR}/engine/shaders")

# Reuse the triangle.{vs,ps} canary HLSL. tide_compile_shader writes its
# output as `<name>.<stage>.hlsl.metallib` so it cannot collide with the
# hand-written `<name>.<stage>.metallib` produced by tide_compile_metal_source
# (sample 02's path).
tide_compile_shader(
    TARGET 05_offscreen_hash
    SOURCE "${_shader_dir}/triangle.vs.hlsl"
    STAGE  vs
)
tide_compile_shader(
    TARGET 05_offscreen_hash
    SOURCE "${_shader_dir}/triangle.ps.hlsl"
    STAGE  ps
)
