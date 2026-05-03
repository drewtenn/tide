# engine/imgui-integration — Phase 1 task 8.
#
# Renders Dear ImGui through the engine's RHI surface (no vendor backends).
# Per the plan tripwire: "ImGui rendering goes through ICommandBuffer::draw()
# like everything else." Public surface is `tide::imgui::Context`; callers
# construct one per Window+IDevice pair, call begin_frame() each frame, build
# their UI, then call render() inside their open render pass.

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

tide_add_module(
    NAME imgui-integration
    PUBLIC_DEPS
        Tide::core
        Tide::rhi
        Tide::input
        imgui::imgui
    PRIVATE_DEPS
        Tide::platform
        glfw
)
