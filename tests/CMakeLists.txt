# Unit tests are doctest-based. Each module gets a directory under tests/unit
# and contributes its own .cpp files to a single test executable.

find_package(doctest CONFIG REQUIRED)
find_package(xxHash  CONFIG REQUIRED)

# ─── Mesh fixture: cooked at build time, used by test_mesh_runtime ──────────
# A pre-cooked triangle.tide artifact. Built only when both TIDE_BUILD_TOOLS
# is on (so the cooker exists) and TIDE_BUILD_TESTS is on. The path is baked
# into the generated test_mesh_runtime.cpp via configure_file().
if(TIDE_BUILD_TOOLS)
    set(_mesh_fixture_in   "${CMAKE_SOURCE_DIR}/tests/fixtures/cooker/triangle.gltf")
    set(_mesh_fixture_meta "${CMAKE_SOURCE_DIR}/tests/fixtures/cooker/triangle.gltf.meta")
    set(_mesh_fixture_out  "${CMAKE_BINARY_DIR}/tests/fixtures/cooker-out/triangle.tide")

    add_custom_command(
        OUTPUT  "${_mesh_fixture_out}"
        COMMAND $<TARGET_FILE:tide_cooker>
                --in    "${_mesh_fixture_in}"
                --out   "${_mesh_fixture_out}"
                --kind  mesh
                --hints "${_mesh_fixture_meta}"
        DEPENDS tide_cooker "${_mesh_fixture_in}" "${_mesh_fixture_meta}"
        COMMENT "Cooking test mesh fixture: triangle.gltf -> triangle.tide"
        VERBATIM
    )
    add_custom_target(tide_test_mesh_fixture DEPENDS "${_mesh_fixture_out}")

    set(TIDE_TEST_MESH_FIXTURE "${_mesh_fixture_out}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/unit/assets/test_mesh_runtime.cpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/unit/assets/test_mesh_runtime.cpp"
        @ONLY
    )
    set(_tide_mesh_runtime_test "${CMAKE_CURRENT_BINARY_DIR}/unit/assets/test_mesh_runtime.cpp")
endif()

add_executable(tide_tests
    unit/main.cpp
    unit/core/test_handle.cpp
    unit/jobs/test_inline_jobs.cpp
    unit/jobs/test_work_stealing.cpp
    unit/input/test_action_compile_time.cpp
    unit/assets/test_uuid.cpp
    unit/assets/test_asset_db.cpp
    unit/assets/test_asset_db_async.cpp
    unit/assets/test_mmap_file.cpp
    unit/assets/test_texture_loader.cpp
    unit/assets/test_shader_loader.cpp
    ${_tide_mesh_runtime_test}
)

if(TIDE_BUILD_TOOLS)
    add_dependencies(tide_tests tide_test_mesh_fixture)
    target_include_directories(tide_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/asset-cooker/include"
    )
endif()

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
    Tide::assets
    # xxhash is needed by test_texture_loader / test_shader_loader to
    # compute the content_hash of synthesized cooked-artifact byte
    # buffers (the loaders verify it on load). The loader implementations
    # also use xxhash but link it PRIVATE on Tide::assets, so consumers
    # have to bring it in independently.
    xxHash::xxhash
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

# ─── Cooker scaffold smoke (Phase 3 task 4) ─────────────────────────────────
# Header-shape smoke: cook a `manifest`-kind input (which still uses the
# placeholder emitter — no source-format parsing required) and assert the
# RuntimeHeader bytes match ADR-0017's wire format. The mesh path is covered
# end-to-end by the doctest in test_mesh_runtime.cpp.
if(TIDE_BUILD_TOOLS)
    set(_cooker_fixture_in   "${CMAKE_SOURCE_DIR}/tests/fixtures/cooker/floor.gltf")
    set(_cooker_fixture_meta "${CMAKE_SOURCE_DIR}/tests/fixtures/cooker/floor.gltf.meta")
    set(_cooker_fixture_out  "${CMAKE_BINARY_DIR}/tests/fixtures/cooker-out/floor.tide")

    add_test(
        NAME tide_cooker_smoke_cook
        COMMAND $<TARGET_FILE:tide_cooker>
            --in    "${_cooker_fixture_in}"
            --out   "${_cooker_fixture_out}"
            --kind  manifest
    )
    set_tests_properties(tide_cooker_smoke_cook PROPERTIES
        FIXTURES_SETUP tide_cooker_smoke_artifact
    )

    add_test(
        NAME tide_cooker_smoke_verify
        COMMAND ${CMAKE_COMMAND}
            -DARTIFACT=${_cooker_fixture_out}
            -DEXPECTED_MAGIC=54494445
            -DEXPECTED_KIND=01000000
            -DEXPECTED_UUID_HEX=00000000000000000000000000000000
            -P ${CMAKE_SOURCE_DIR}/cmake/VerifyCookedArtifact.cmake
    )
    set_tests_properties(tide_cooker_smoke_verify PROPERTIES
        FIXTURES_REQUIRED tide_cooker_smoke_artifact
    )
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
