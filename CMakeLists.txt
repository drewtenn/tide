cmake_minimum_required(VERSION 3.27)

if(APPLE)
    # OBJCXX is enabled at the project level so .mm files in engine/rhi-metal/ are
    # auto-recognized without per-file LANGUAGE properties. Per locked DEFINE D23.
    set(_tide_languages CXX OBJCXX)
else()
    set(_tide_languages CXX)
endif()

project(tide
    VERSION 0.0.1
    DESCRIPTION "tide: a competent indie C++ game engine"
    LANGUAGES ${_tide_languages}
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Verify the toolchain actually has the C++23 library bits we depend on.
# `std::expected` (ADR-0039) needs libc++ 17+, libstdc++ 13+, or MSVC 19.36+.
# Failing here gives a clear error instead of a wall of template diagnostics.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles("
    #include <expected>
    #if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
    #  error \"std::expected (P0323R12) is required (ADR-0039)\"
    #endif
    int main() { std::expected<int, int> e = 1; return e.has_value() ? 0 : 1; }
" TIDE_HAVE_STD_EXPECTED)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT TIDE_HAVE_STD_EXPECTED)
    message(FATAL_ERROR
        "Toolchain lacks std::expected. Required: libc++ 17+, libstdc++ 13+, or "
        "MSVC 19.36+. See ADR-0039.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(tide_module)

option(TIDE_BUILD_SAMPLES "Build sample executables" ON)
option(TIDE_BUILD_TESTS "Build unit tests" ON)
option(TIDE_BUILD_TOOLS "Build offline tools (asset cooker, etc.)" ON)
option(TIDE_ENABLE_TRACY "Enable Tracy profiler instrumentation" ON)

# ─── RHI backend selection (locked DEFINE D22) ──────────────────────────────
# Backend modules are gated at add_subdirectory() via if(...). Generator
# expressions don't work here because we'd configure the module on every platform.
if(APPLE)
    option(TIDE_RHI_METAL "Build Metal RHI backend (Phase 1)" ON)
else()
    option(TIDE_RHI_METAL "Build Metal RHI backend (Phase 1)" OFF)
endif()
option(TIDE_RHI_VULKAN "Build Vulkan RHI backend (Phase 2)" OFF)
if(WIN32)
    option(TIDE_RHI_D3D12 "Build DX12 RHI backend (Phase 2.5)" ON)
else()
    option(TIDE_RHI_D3D12 "Build DX12 RHI backend (Phase 2.5)" OFF)
endif()

if(TIDE_RHI_METAL AND NOT APPLE)
    message(FATAL_ERROR "TIDE_RHI_METAL requires APPLE. Disable with -DTIDE_RHI_METAL=OFF.")
endif()
if(TIDE_RHI_D3D12 AND NOT WIN32)
    message(FATAL_ERROR "TIDE_RHI_D3D12 requires WIN32. Disable with -DTIDE_RHI_D3D12=OFF.")
endif()

# ─── Engine modules (declared in dependency order) ──────────────────────────
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/jobs)
add_subdirectory(engine/input)

add_subdirectory(engine/rhi)
if(TIDE_RHI_METAL)
    add_subdirectory(engine/rhi-metal)
endif()
# Vulkan / D3D12 land in P2 / P2.5; the directories don't exist yet.

add_subdirectory(engine/renderer)
add_subdirectory(engine/assets)
add_subdirectory(engine/audio)
add_subdirectory(engine/physics)
add_subdirectory(engine/scene)
add_subdirectory(engine/scripting)
add_subdirectory(engine/gameplay)
add_subdirectory(engine/imgui-integration)

# ─── Tools (offline; not linked into runtime) ───────────────────────────────
if(TIDE_BUILD_TOOLS)
    add_subdirectory(tools/asset-cooker)
endif()

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
message(STATUS "  RHI backends:    Metal=${TIDE_RHI_METAL} Vulkan=${TIDE_RHI_VULKAN} D3D12=${TIDE_RHI_D3D12}")
message(STATUS "  Samples:         ${TIDE_BUILD_SAMPLES}")
message(STATUS "  Tests:           ${TIDE_BUILD_TESTS}")
message(STATUS "  Tools:           ${TIDE_BUILD_TOOLS}")
message(STATUS "")
