# samples/04_imgui_overlay — Phase 1 task 8: ImGui via the engine's RHI.
#
# Apple-only (Metal RHI). Uses the production HLSL → SPIR-V → MSL pipeline for
# both the textured-quad shaders (carried from sample 03) and the new ImGui
# shaders. ImGui rendering goes through ICommandBuffer::draw_indexed — no
# vendor backend.

if(NOT TIDE_RHI_METAL)
    return()
endif()

include(CompileShader)

add_executable(04_imgui_overlay main.cpp)

target_link_libraries(04_imgui_overlay PRIVATE
    Tide::core
    Tide::platform
    Tide::input
    Tide::rhi
    Tide::rhi-metal
    Tide::imgui-integration
)

# Sample needs the cbuffer layout struct from engine/shaders/PerFrame.h.
target_include_directories(04_imgui_overlay PRIVATE "${CMAKE_SOURCE_DIR}/engine/shaders")

set_target_properties(04_imgui_overlay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/04_imgui_overlay"
)

set(_shader_dir "${CMAKE_SOURCE_DIR}/engine/shaders")

# Reuse the textured-quad shaders for the scene, plus the new imgui shaders.
tide_compile_shader(
    TARGET 04_imgui_overlay
    SOURCE "${_shader_dir}/textured_quad.vs.hlsl"
    STAGE  vs
)
tide_compile_shader(
    TARGET 04_imgui_overlay
    SOURCE "${_shader_dir}/textured_quad.ps.hlsl"
    STAGE  ps
)
tide_compile_shader(
    TARGET 04_imgui_overlay
    SOURCE "${_shader_dir}/imgui.vs.hlsl"
    STAGE  vs
)
tide_compile_shader(
    TARGET 04_imgui_overlay
    SOURCE "${_shader_dir}/imgui.ps.hlsl"
    STAGE  ps
)
