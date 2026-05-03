# samples/06_compute_dispatch — Phase 1 task 11: compute round-trip canary.
#
# Validates the HLSL → SPIR-V → MSL compute path AND the runtime compute
# encoder by running engine/shaders/compute_canary.cs.hlsl on a 64-element
# storage buffer initialized to zero. Each thread does:
#     buf[tid] = (buf[tid] + 1) * 2  (with InterlockedAdd + threadgroup barrier)
# Expected post-dispatch: every element == 2. Sample asserts and exits 0/1.

if(NOT TIDE_RHI_METAL)
    return()
endif()

include(CompileShader)

add_executable(06_compute_dispatch main.cpp)

target_link_libraries(06_compute_dispatch PRIVATE
    Tide::core
    Tide::platform
    Tide::rhi
    Tide::rhi-metal
)

set_target_properties(06_compute_dispatch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/06_compute_dispatch"
)

# The canary shader is also compiled by tests/tide_shader_smoke; tide_compile_shader
# is idempotent on the same source so listing it again here is safe and makes
# the sample's dependency explicit.
tide_compile_shader(
    TARGET 06_compute_dispatch
    SOURCE "${CMAKE_SOURCE_DIR}/engine/shaders/compute_canary.cs.hlsl"
    STAGE  cs
)
