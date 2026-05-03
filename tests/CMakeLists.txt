# Unit tests are doctest-based. Each module gets a directory under tests/unit
# and contributes its own .cpp files to a single test executable.

find_package(doctest CONFIG REQUIRED)

add_executable(tide_tests
    unit/main.cpp
    unit/core/test_handle.cpp
    unit/jobs/test_inline_jobs.cpp
    unit/jobs/test_work_stealing.cpp
    unit/input/test_action_compile_time.cpp
)

# test_work_stealing.cpp pulls in the WorkStealingJobSystem private header
# directly (engine/jobs/src/) — same pattern other tests would use to reach
# implementation-internal types for white-box testing.
target_include_directories(tide_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/engine/jobs/src"
)

target_link_libraries(tide_tests PRIVATE
    doctest::doctest
    Tide::core
    Tide::jobs
    Tide::input
)

# Register every TEST_CASE as a separate ctest test for granular reporting.
include(doctest)
doctest_discover_tests(tide_tests)

# ─── Shader compile pipeline smoke (Phase 1 task 3 exit criterion) ──────────
# Custom target that produces .spv (+ .metal / .metallib on Apple) for every
# canary shader. Asserts the toolchain wires up and emits non-empty artifacts.
# Skipped cleanly when DXC is unavailable (locked DEFINE D21 contingency).
include(CompileShader)

if(TIDE_SHADER_TOOLCHAIN_AVAILABLE)
    add_custom_target(tide_shader_smoke ALL
        COMMENT "Phase 1 shader-toolchain smoke: HLSL -> SPIR-V (-> MSL -> metallib)"
    )

    set(_canary_dir "${CMAKE_SOURCE_DIR}/engine/shaders")
    tide_compile_shader(
        TARGET tide_shader_smoke
        SOURCE "${_canary_dir}/triangle.vs.hlsl"
        STAGE  vs
    )
    tide_compile_shader(
        TARGET tide_shader_smoke
        SOURCE "${_canary_dir}/triangle.ps.hlsl"
        STAGE  ps
    )
    tide_compile_shader(
        TARGET tide_shader_smoke
        SOURCE "${_canary_dir}/compute_canary.cs.hlsl"
        STAGE  cs
    )
    tide_compile_shader(
        TARGET tide_shader_smoke
        SOURCE "${_canary_dir}/compute_uav_tex.cs.hlsl"
        STAGE  cs
    )

    add_test(
        NAME tide_shader_smoke
        COMMAND ${CMAKE_COMMAND}
            -DSHADERS_DIR=${CMAKE_BINARY_DIR}/shaders
            -DAPPLE_BUILD=$<BOOL:${APPLE}>
            -P ${CMAKE_SOURCE_DIR}/cmake/VerifyShaderArtifacts.cmake
    )
else()
    message(STATUS "Shader smoke test SKIPPED — DXC not found.")
endif()

# ─── Offscreen-hash golden test (Phase 1 task 10) ───────────────────────────
# Runs samples/05_offscreen_hash with --check against the known-good xxh3
# hash of the deterministic colored-triangle frame. Catches "no Metal
# validation errors but pixels are wrong" regressions like the recent
# bind_descriptor_set type-matching bug. Apple-only because TIDE_RHI_METAL
# is the only backend Phase 1 supports.
if(TIDE_RHI_METAL)
    add_test(
        NAME tide_golden_05_offscreen_hash
        COMMAND $<TARGET_FILE:05_offscreen_hash> --check df6143812ec262e6
    )

    # Phase 1 task 11: compute round-trip canary. The shader does
    # `buf[tid] = (buf[tid] + 1) * 2` with InterlockedAdd + threadgroup
    # barrier; with initial buf == 0 every element should end up at 2.
    # Asserts the SPIRV-Cross compute path AND the runtime compute encoder
    # are both wired correctly.
    add_test(
        NAME tide_compute_canary
        COMMAND $<TARGET_FILE:06_compute_dispatch>
    )
endif()
