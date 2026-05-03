cmake_minimum_required(VERSION 3.27)

project(tide
    VERSION 0.0.1
    DESCRIPTION "tide: a competent indie C++ game engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(tide_module)

option(TIDE_BUILD_SAMPLES "Build sample executables" ON)
option(TIDE_BUILD_TESTS "Build unit tests" ON)
option(TIDE_ENABLE_TRACY "Enable Tracy profiler instrumentation" ON)

# ─── Engine modules (declared in dependency order) ──────────────────────────
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/jobs)
add_subdirectory(engine/input)

add_subdirectory(engine/rhi)
# RHI backends are conditionally added per platform in their own CMakeLists.
# Phase 0 stops here; rhi-metal/rhi-vulkan/rhi-d3d12 land in P1/P2/P2.5.

add_subdirectory(engine/renderer)
add_subdirectory(engine/assets)
add_subdirectory(engine/audio)
add_subdirectory(engine/physics)
add_subdirectory(engine/scene)
add_subdirectory(engine/scripting)
add_subdirectory(engine/gameplay)
add_subdirectory(engine/imgui-integration)

# ─── Samples & tests ────────────────────────────────────────────────────────
if(TIDE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(TIDE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Build summary ──────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "tide ${PROJECT_VERSION}")
message(STATUS "  C++ standard:    ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:        ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Toolchain:       ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "  Triplet:         ${VCPKG_TARGET_TRIPLET}")
message(STATUS "  Tracy:           ${TIDE_ENABLE_TRACY}")
message(STATUS "  Samples:         ${TIDE_BUILD_SAMPLES}")
message(STATUS "  Tests:           ${TIDE_BUILD_TESTS}")
message(STATUS "")
