# VerifyShaderArtifacts.cmake — Phase 1 task 3 exit-criterion test.
#
# Asserts every canary shader produced non-empty .spv (and .metallib on Apple).
# Invoked via `cmake -P` from a ctest.

if(NOT DEFINED SHADERS_DIR)
    message(FATAL_ERROR "SHADERS_DIR not provided")
endif()

set(_canaries
    "triangle.vs"
    "triangle.ps"
    "compute_canary.cs"
    "compute_uav_tex.cs"
)

set(_failed 0)

foreach(c ${_canaries})
    set(_spv "${SHADERS_DIR}/${c}.hlsl.spv")
    if(NOT EXISTS "${_spv}")
        message(SEND_ERROR "MISSING: ${_spv}")
        math(EXPR _failed "${_failed}+1")
        continue()
    endif()
    file(SIZE "${_spv}" _spv_size)
    if(_spv_size LESS 100)
        message(SEND_ERROR "TOO SMALL: ${_spv} (${_spv_size} bytes)")
        math(EXPR _failed "${_failed}+1")
        continue()
    endif()
    message(STATUS "  ${c}.spv  OK (${_spv_size} bytes)")

    set(_msl "${SHADERS_DIR}/${c}.hlsl.metal")
    if(NOT EXISTS "${_msl}")
        message(SEND_ERROR "MISSING: ${_msl}")
        math(EXPR _failed "${_failed}+1")
        continue()
    endif()
    file(SIZE "${_msl}" _msl_size)
    if(_msl_size LESS 50)
        message(SEND_ERROR "TOO SMALL: ${_msl} (${_msl_size} bytes)")
        math(EXPR _failed "${_failed}+1")
        continue()
    endif()
    message(STATUS "  ${c}.metal OK (${_msl_size} bytes)")

    if(APPLE_BUILD)
        set(_lib "${SHADERS_DIR}/${c}.hlsl.metallib")
        if(NOT EXISTS "${_lib}")
            message(SEND_ERROR "MISSING: ${_lib}")
            math(EXPR _failed "${_failed}+1")
            continue()
        endif()
        file(SIZE "${_lib}" _lib_size)
        if(_lib_size LESS 64)
            message(SEND_ERROR "TOO SMALL: ${_lib} (${_lib_size} bytes)")
            math(EXPR _failed "${_failed}+1")
            continue()
        endif()
        message(STATUS "  ${c}.metallib OK (${_lib_size} bytes)")
    endif()
endforeach()

if(_failed GREATER 0)
    message(FATAL_ERROR "Shader smoke: ${_failed} canary(s) failed")
endif()

message(STATUS "Shader smoke: all ${_canaries} canaries OK")
