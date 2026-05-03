# tide_add_module(NAME core PUBLIC_DEPS ... PRIVATE_DEPS ... [HEADER_ONLY])
#
# Declares an engine module as a static library (or INTERFACE library if HEADER_ONLY)
# with the standard layout:
#   engine/<NAME>/include/tide/<NAME>/*.h    (public headers)
#   engine/<NAME>/src/*.cpp                  (implementation)
#   engine/<NAME>/src/*.h                    (private headers)
#
# Adds an alias `Tide::<NAME>` for downstream consumers.
#
# See ADR-0001 (engine name and namespace) for the rationale.

function(tide_add_module)
    set(options HEADER_ONLY)
    set(oneValueArgs NAME)
    set(multiValueArgs PUBLIC_DEPS PRIVATE_DEPS SOURCES)
    cmake_parse_arguments(MOD "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT MOD_NAME)
        message(FATAL_ERROR "tide_add_module: NAME is required")
    endif()

    set(_target "tide_${MOD_NAME}")
    set(_alias "Tide::${MOD_NAME}")
    set(_inc_root "${CMAKE_CURRENT_SOURCE_DIR}/include")
    set(_src_root "${CMAKE_CURRENT_SOURCE_DIR}/src")

    if(MOD_HEADER_ONLY)
        add_library(${_target} INTERFACE)
        target_include_directories(${_target} INTERFACE
            $<BUILD_INTERFACE:${_inc_root}>
            $<INSTALL_INTERFACE:include>
        )
        target_link_libraries(${_target} INTERFACE ${MOD_PUBLIC_DEPS})
        target_compile_features(${_target} INTERFACE cxx_std_23)
    else()
        # Auto-discover sources if not provided.
        if(NOT MOD_SOURCES)
            file(GLOB_RECURSE MOD_SOURCES CONFIGURE_DEPENDS "${_src_root}/*.cpp" "${_src_root}/*.mm")
        endif()

        if(NOT MOD_SOURCES)
            # Empty module — declare an interface library so it links cleanly even
            # with no .cpp files yet. Useful in Phase 0 where many modules are stubs.
            add_library(${_target} INTERFACE)
            target_include_directories(${_target} INTERFACE
                $<BUILD_INTERFACE:${_inc_root}>
            )
            target_link_libraries(${_target} INTERFACE ${MOD_PUBLIC_DEPS})
            target_compile_features(${_target} INTERFACE cxx_std_23)
        else()
            add_library(${_target} STATIC ${MOD_SOURCES})
            target_include_directories(${_target}
                PUBLIC
                    $<BUILD_INTERFACE:${_inc_root}>
                PRIVATE
                    ${_src_root}
            )
            target_link_libraries(${_target}
                PUBLIC ${MOD_PUBLIC_DEPS}
                PRIVATE ${MOD_PRIVATE_DEPS}
            )
            target_compile_features(${_target} PUBLIC cxx_std_23)
            set_target_properties(${_target} PROPERTIES
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF
            )

            # Standard warnings — strict but not absurd.
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
                target_compile_options(${_target} PRIVATE
                    -Wall -Wextra -Wpedantic
                    -Wno-unused-parameter
                    -Wno-missing-field-initializers
                )
            elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
                target_compile_options(${_target} PRIVATE /W4 /permissive-)
            endif()
        endif()
    endif()

    add_library(${_alias} ALIAS ${_target})
endfunction()
