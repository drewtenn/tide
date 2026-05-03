find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

tide_add_module(
    NAME core
    PUBLIC_DEPS
        glm::glm
        spdlog::spdlog
        fmt::fmt
)

# tide::expected is std::expected (C++23). Requires GCC 13+, Apple Clang 16+,
# MSVC 19.36+. See ADR-0039 — Linux CI runner amended to ubuntu-24.04.

if(TIDE_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(tide_core INTERFACE Tracy::TracyClient)
    target_compile_definitions(tide_core INTERFACE TRACY_ENABLE)
endif()
