# tools/asset-cooker — offline CLI that cooks source assets into the
# runtime binary format defined by ADR-0017. Standalone executable per
# ADR-0018; the runtime never links against this target's source-format
# parsers (currently just nlohmann-json; cgltf / stb_image land in P3
# atomic tasks 3 / 4).

find_package(nlohmann_json CONFIG REQUIRED)
find_package(xxHash        CONFIG REQUIRED)
# cgltf has no CMake config (single-header C library). Vcpkg places the
# header on the toolchain include path; we include it as `<cgltf.h>` and
# define `CGLTF_IMPLEMENTATION` in exactly one TU (MeshCooker.cpp).

add_executable(tide_cooker
    src/main.cpp
    src/Args.cpp
    src/MetaFile.cpp
    src/MeshCooker.cpp
)

target_include_directories(tide_cooker PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(tide_cooker PRIVATE
    Tide::core            # tide::expected (ADR-0039)
    Tide::assets          # RuntimeHeader, Uuid, AssetKind, MeshAsset
    nlohmann_json::nlohmann_json
    xxHash::xxhash
)

target_compile_features(tide_cooker PRIVATE cxx_std_23)
set_target_properties(tide_cooker PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
    OUTPUT_NAME           tide-cooker
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/asset-cooker"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(tide_cooker PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -Wno-missing-field-initializers
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(tide_cooker PRIVATE /W4 /permissive-)
endif()

# Convenience alias so other CMake code (custom commands in P3+) can write
# `$<TARGET_FILE:Tide::cooker>` rather than the underscore form.
add_executable(Tide::cooker ALIAS tide_cooker)
