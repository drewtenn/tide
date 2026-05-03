# tide_compile_shader(
#     TARGET <consumer-target>
#     SOURCE <name.hlsl>
#     STAGE <vs|ps|cs>
#     [ENTRY <name>]                # default: main
#     [DXC_FLAGS <extra-flags...>]
# )
#
# Outputs (under ${CMAKE_BINARY_DIR}/shaders/):
#   <name>.<stage>.spv          (always)
#   <name>.<stage>.metal        (always)
#   <name>.<stage>.air          (Apple only)
#   <name>.<stage>.metallib     (Apple only)
#
# Locked DEFINE D14 (DXC core flags), D15 (no scalar layout), D16 (wide register
# shifts), D18 (build-time reflection), D21 (find_program with FetchContent
# fallback per amendment A3).

include_guard(GLOBAL)
include(FetchContent)

# ─── Tool discovery ─────────────────────────────────────────────────────────
# Two-stage search per locked DEFINE D21 + amendment A3:
#   1. find_program first (custom toolchains, Vulkan SDK, Homebrew)
#   2. FetchContent of pinned tarball as fallback (default fresh-clone path)
#
# Set TIDE_NO_FETCHCONTENT_DXC=ON to disable the FetchContent fallback.

option(TIDE_NO_FETCHCONTENT_DXC "Don't auto-download DXC; require system install" OFF)

# tide_compile_metal_source(
#     TARGET <consumer-target>
#     SOURCE <name.metal>
#     STAGE <vs|ps|cs>
# )
#
# Hand-written MSL → .metallib via xcrun metal + xcrun metallib. Used for the
# Phase 1 task 6 colored-triangle sample so the engine's RHI can be validated
# end-to-end without requiring DXC (which is not consistently shipped on
# macOS — see locked DEFINE D20 / D21).
#
# Defined here, BEFORE the DXC discovery block, because that block can
# return() early when DXC is missing and skip everything below it. The MSL
# path only depends on xcrun (always present on macOS dev machines), so it
# stays usable even when the HLSL toolchain is unavailable.
#
# Output (under ${CMAKE_BINARY_DIR}/shaders/):
#   <name>.<stage>.air         (compiled object)
#   <name>.<stage>.metallib    (linked library — what create_shader consumes)
#
# The HLSL path (tide_compile_shader) below is the production cross-backend
# pipeline; this helper exists only to unblock samples that need a baked
# .metallib while the HLSL toolchain is unavailable. Once DXC is installed
# the HLSL path is preferred (the SPIR-V intermediate is the Vulkan/DX12
# source-of-truth).
function(tide_compile_metal_source)
    if(NOT APPLE)
        message(FATAL_ERROR "tide_compile_metal_source: Apple-only helper")
    endif()

    set(_options "")
    set(_oneval  TARGET SOURCE STAGE)
    set(_multi   "")
    cmake_parse_arguments(ARG "${_options}" "${_oneval}" "${_multi}" ${ARGN})

    if(NOT ARG_TARGET OR NOT ARG_SOURCE OR NOT ARG_STAGE)
        message(FATAL_ERROR "tide_compile_metal_source: TARGET, SOURCE, STAGE required")
    endif()
    if(NOT ARG_STAGE MATCHES "^(vs|ps|cs)$")
        message(FATAL_ERROR "tide_compile_metal_source: unknown STAGE '${ARG_STAGE}'")
    endif()

    find_program(XCRUN_EXECUTABLE xcrun REQUIRED
        DOC "Xcode command-line tools — needed for metal/metallib")

    if(IS_ABSOLUTE "${ARG_SOURCE}")
        set(_source_abs "${ARG_SOURCE}")
    else()
        set(_source_abs "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}")
    endif()

    get_filename_component(_name "${ARG_SOURCE}" NAME_WE)

    set(_out_dir "${CMAKE_BINARY_DIR}/shaders")
    set(_air     "${_out_dir}/${_name}.${ARG_STAGE}.air")
    set(_mlib    "${_out_dir}/${_name}.${ARG_STAGE}.metallib")

    add_custom_command(
        OUTPUT  "${_air}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
        COMMAND "${XCRUN_EXECUTABLE}" metal
            -c "${_source_abs}"
            -o "${_air}"
            -gline-tables-only
        DEPENDS "${_source_abs}"
        COMMENT "[xcrun metal] ${_name}.${ARG_STAGE}.metal -> AIR"
        VERBATIM
    )

    add_custom_command(
        OUTPUT  "${_mlib}"
        COMMAND "${XCRUN_EXECUTABLE}" metallib
            "${_air}"
            -o "${_mlib}"
        DEPENDS "${_air}"
        COMMENT "[xcrun metallib] ${_name}.${ARG_STAGE}.air -> metallib"
        VERBATIM
    )

    set(_rule "metal_${_name}_${ARG_STAGE}")
    if(NOT TARGET "${_rule}")
        add_custom_target("${_rule}" DEPENDS "${_mlib}")
    endif()
    add_dependencies("${ARG_TARGET}" "${_rule}")
endfunction()

if(NOT DEFINED TIDE_DXC_VERSION)
    # Pin to a known-good DXC release. Bump deliberately, not opportunistically.
    set(TIDE_DXC_VERSION "v1.8.2407")
endif()

if(NOT DEFINED TIDE_SPIRV_CROSS_TAG)
    set(TIDE_SPIRV_CROSS_TAG "vulkan-sdk-1.3.296.0")
endif()

# Step 1: try the system / Vulkan SDK / Homebrew.
find_program(DXC_EXECUTABLE dxc
    HINTS
        "$ENV{VULKAN_SDK}/macOS/bin"
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/local/bin"
        "/opt/homebrew/bin"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/directx-dxc"
    DOC "DirectX Shader Compiler (dxc) — Vulkan SDK, Homebrew, or vcpkg port"
)

find_program(SPIRV_CROSS_EXECUTABLE spirv-cross
    HINTS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/spirv-cross"
        "$ENV{VULKAN_SDK}/macOS/bin"
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/local/bin"
        "/opt/homebrew/bin"
    DOC "SPIRV-Cross — Vulkan SDK, Homebrew, or vcpkg port"
)

# Step 2: FetchContent fallback if the tools weren't found and fallback is enabled.
# We download pre-built binaries; we don't compile them. SHA pinning would be
# stronger but requires per-platform hashes — Phase 1 stays HTTPS + URL_HASH-free
# and tightens this in a follow-up. (Risk register R2 covers DXC drift.)
if(NOT DXC_EXECUTABLE AND NOT TIDE_NO_FETCHCONTENT_DXC)
    if(APPLE)
        # Microsoft does not consistently publish macOS DXC release artifacts.
        # Phase 1 task 3 documents the prereq: install Vulkan SDK from LunarG
        # (https://vulkan.lunarg.com), which ships dxc, or build DXC from source.
        # Until DXC is found, downstream `tide_compile_shader()` is a no-op so
        # the rest of the engine still builds.
        message(WARNING
            "DXC not found on macOS. Shader compilation is disabled.\n"
            "Install Vulkan SDK from https://vulkan.lunarg.com or set "
            "DXC_EXECUTABLE explicitly to enable.\n"
            "Phase 1 task 3 exit criterion ('verify HLSL→SPIR-V→MSL') requires DXC.")
        return()
    elseif(UNIX)
        FetchContent_Declare(tide_dxc
            URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${TIDE_DXC_VERSION}/linux_dxc_2024_07_31.x86_64.tar.gz"
        )
        FetchContent_MakeAvailable(tide_dxc)
        find_program(DXC_EXECUTABLE dxc HINTS "${tide_dxc_SOURCE_DIR}/bin" REQUIRED)
    elseif(WIN32)
        FetchContent_Declare(tide_dxc
            URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${TIDE_DXC_VERSION}/dxc_2024_07_31.zip"
        )
        FetchContent_MakeAvailable(tide_dxc)
        find_program(DXC_EXECUTABLE dxc HINTS "${tide_dxc_SOURCE_DIR}/bin/x64" REQUIRED)
    endif()
endif()

if(NOT DXC_EXECUTABLE)
    if(TIDE_NO_FETCHCONTENT_DXC)
        message(WARNING
            "DXC not found and TIDE_NO_FETCHCONTENT_DXC=ON. Shader compilation disabled.")
    endif()
    set(TIDE_SHADER_TOOLCHAIN_AVAILABLE FALSE CACHE INTERNAL "")
    return()
endif()

if(NOT SPIRV_CROSS_EXECUTABLE)
    message(WARNING
        "spirv-cross not found despite vcpkg manifest entry — installation likely "
        "failed. Shader compilation disabled.")
    set(TIDE_SHADER_TOOLCHAIN_AVAILABLE FALSE CACHE INTERNAL "")
    return()
endif()

set(TIDE_SHADER_TOOLCHAIN_AVAILABLE TRUE CACHE INTERNAL "")

if(APPLE)
    find_program(XCRUN_EXECUTABLE xcrun REQUIRED
        DOC "Xcode command-line tools — needed for metal/metallib")
endif()

# Detect DXC's entry-point-naming flag style. Upstream Microsoft DXC accepts
# `-fvk-use-entrypoint-name` (no value); the dxc-3.7 build that ships with the
# LunarG Vulkan SDK uses `-fspv-entrypoint-name=<value>` instead. Both achieve
# the same effect: preserve the HLSL entry-point name in the SPIR-V module.
if(NOT DEFINED TIDE_DXC_ENTRYPOINT_STYLE)
    execute_process(
        COMMAND "${DXC_EXECUTABLE}" --help
        OUTPUT_VARIABLE _dxc_help
        ERROR_VARIABLE  _dxc_help
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_dxc_help MATCHES "-fvk-use-entrypoint-name")
        set(_style "fvk")
    elseif(_dxc_help MATCHES "-fspv-entrypoint-name")
        set(_style "fspv")
    else()
        set(_style "none")
    endif()
    set(TIDE_DXC_ENTRYPOINT_STYLE "${_style}" CACHE INTERNAL "")
    unset(_dxc_help)
endif()

message(STATUS "Shader toolchain:")
message(STATUS "  DXC:         ${DXC_EXECUTABLE}")
message(STATUS "  SPIRV-Cross: ${SPIRV_CROSS_EXECUTABLE}")
message(STATUS "  DXC entry-point flag style: ${TIDE_DXC_ENTRYPOINT_STYLE}")
if(APPLE)
    message(STATUS "  xcrun:       ${XCRUN_EXECUTABLE}")
endif()

# ─── tide_compile_shader(...) ───────────────────────────────────────────────

function(tide_compile_shader)
    if(NOT TIDE_SHADER_TOOLCHAIN_AVAILABLE)
        # Toolchain not available; tide_compile_shader is a no-op so the engine
        # still builds. Locked DEFINE D21 contingency.
        return()
    endif()

    set(_options "")
    set(_oneval  TARGET SOURCE STAGE ENTRY)
    set(_multi   DXC_FLAGS)
    cmake_parse_arguments(ARG "${_options}" "${_oneval}" "${_multi}" ${ARGN})

    if(NOT ARG_TARGET OR NOT ARG_SOURCE OR NOT ARG_STAGE)
        message(FATAL_ERROR "tide_compile_shader: TARGET, SOURCE, STAGE required")
    endif()
    if(NOT ARG_ENTRY)
        set(ARG_ENTRY "main")
    endif()

    if(ARG_STAGE STREQUAL "vs")
        set(_profile "vs_6_7")
    elseif(ARG_STAGE STREQUAL "ps")
        set(_profile "ps_6_7")
    elseif(ARG_STAGE STREQUAL "cs")
        set(_profile "cs_6_7")
    else()
        message(FATAL_ERROR "tide_compile_shader: unknown STAGE '${ARG_STAGE}'")
    endif()

    if(IS_ABSOLUTE "${ARG_SOURCE}")
        set(_source_abs "${ARG_SOURCE}")
    else()
        set(_source_abs "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}")
    endif()

    get_filename_component(_name "${ARG_SOURCE}" NAME_WE)

    # All HLSL-derived artefacts use a `.hlsl.` infix so they cannot collide
    # with hand-written-MSL outputs from tide_compile_metal_source. The
    # collision was hit during Phase 1 task 10 — both rules wrote
    # `triangle.vs.metallib` and the build-graph order silently determined
    # which one downstream samples loaded. The infix is the cheap structural
    # fix.
    set(_out_dir "${CMAKE_BINARY_DIR}/shaders")
    set(_spv     "${_out_dir}/${_name}.${ARG_STAGE}.hlsl.spv")
    set(_msl     "${_out_dir}/${_name}.${ARG_STAGE}.hlsl.metal")
    set(_air     "${_out_dir}/${_name}.${ARG_STAGE}.hlsl.air")
    set(_mlib    "${_out_dir}/${_name}.${ARG_STAGE}.hlsl.metallib")

    # Translate the configure-time-detected entry-point style into the right
    # flag for this DXC build (see TIDE_DXC_ENTRYPOINT_STYLE above).
    if(TIDE_DXC_ENTRYPOINT_STYLE STREQUAL "fvk")
        set(_entrypoint_flags -fvk-use-entrypoint-name)
    elseif(TIDE_DXC_ENTRYPOINT_STYLE STREQUAL "fspv")
        set(_entrypoint_flags "-fspv-entrypoint-name=${ARG_ENTRY}")
    else()
        set(_entrypoint_flags "")
    endif()

    # Step 1: HLSL -> SPIR-V via DXC.
    # Locked DEFINE D14 (core flags), D15 (no scalar layout), D16 (wide shifts).
    add_custom_command(
        OUTPUT  "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_out_dir}"
        COMMAND "${DXC_EXECUTABLE}"
            -spirv
            -T  ${_profile}
            -E  ${ARG_ENTRY}
            -HV 2021
            -fspv-target-env=vulkan1.3
            -fvk-use-dx-layout
            ${_entrypoint_flags}
            -fvk-b-shift  0 all
            -fvk-t-shift 16 all
            -fvk-u-shift 32 all
            -fvk-s-shift 48 all
            ${ARG_DXC_FLAGS}
            -Fo "${_spv}"
            "${_source_abs}"
        DEPENDS "${_source_abs}"
        COMMENT "[DXC] ${_name}.${ARG_STAGE}.hlsl -> SPIR-V"
        VERBATIM
    )

    # Step 2: SPIR-V -> MSL via SPIRV-Cross. Locked DEFINE D17 (no argument
    # buffers in P1) — that's the default; no flag needed to opt out.
    # spirv-cross targets macOS by default; --msl-ios would opt into iOS.
    add_custom_command(
        OUTPUT  "${_msl}"
        COMMAND "${SPIRV_CROSS_EXECUTABLE}"
            --msl
            --msl-version 30000
            --output "${_msl}"
            "${_spv}"
        DEPENDS "${_spv}"
        COMMENT "[SPIRV-Cross] ${_name}.${ARG_STAGE}.spv -> MSL"
        VERBATIM
    )

    # Step 2b: SPIR-V reflection JSON. Locked DEFINE D18 — build-time bake. The
    # JSON is consumed by a future generator (task 5/6) that emits per-shader
    # binding-name constant headers for application code to #include.
    set(_refl "${_out_dir}/${_name}.${ARG_STAGE}.hlsl.refl.json")
    add_custom_command(
        OUTPUT  "${_refl}"
        COMMAND "${SPIRV_CROSS_EXECUTABLE}"
            --reflect
            --output "${_refl}"
            "${_spv}"
        DEPENDS "${_spv}"
        COMMENT "[SPIRV-Cross --reflect] ${_name}.${ARG_STAGE}.spv -> reflection JSON"
        VERBATIM
    )

    if(APPLE)
        # Step 3: MSL -> AIR
        add_custom_command(
            OUTPUT  "${_air}"
            COMMAND "${XCRUN_EXECUTABLE}" metal
                -c "${_msl}"
                -o "${_air}"
                -gline-tables-only
            DEPENDS "${_msl}"
            COMMENT "[xcrun metal] ${_name}.${ARG_STAGE}.metal -> AIR"
            VERBATIM
        )

        # Step 4: AIR -> .metallib
        add_custom_command(
            OUTPUT  "${_mlib}"
            COMMAND "${XCRUN_EXECUTABLE}" metallib
                "${_air}"
                -o "${_mlib}"
            DEPENDS "${_air}"
            COMMENT "[xcrun metallib] ${_name}.${ARG_STAGE}.air -> metallib"
            VERBATIM
        )
        set(_final_artifacts "${_mlib}" "${_refl}")
    else()
        set(_final_artifacts "${_spv}" "${_refl}")
    endif()

    # Custom target so `cmake --build . --target shader_<name>_<stage>` works,
    # and so multiple consumer targets can share a single shader artifact.
    set(_rule "shader_${_name}_${ARG_STAGE}")
    if(NOT TARGET "${_rule}")
        add_custom_target("${_rule}" DEPENDS ${_final_artifacts})
    endif()
    add_dependencies("${ARG_TARGET}" "${_rule}")
endfunction()
