#pragma once

// tide::imgui::Context — Dear ImGui rendered through the engine's RHI.
//
// Per the Phase 1 task 8 deliverable: this module does NOT use the vendor
// imgui_impl_metal / imgui_impl_glfw backends. All draws go through the
// existing ICommandBuffer surface (bind_pipeline + bind_descriptor_set +
// bind_vertex_buffer + bind_index_buffer + draw_indexed). One IDevice + one
// Window per Context.
//
// Lifecycle (see samples/04_imgui_overlay for the canonical caller):
//   ImGui::CreateContext()           // implicitly via ctor
//   ctx.set_metallib_dir(path)       // optional; default = exe dir / ../../shaders
//   ctx.create_resources()           // build font atlas, shaders, PSO
//   ...
//   while (!quit) {
//       ctx.begin_frame()
//       ImGui::ShowDemoWindow() or whatever
//       cmd->begin_render_pass(...)
//       cmd->bind_pipeline(scene_pso)
//       ... scene draws ...
//       ctx.render(*cmd, fb_width, fb_height);
//       cmd->end_render_pass()
//   }

#include "tide/core/Expected.h"
#include "tide/rhi/Descriptors.h"
#include "tide/rhi/Handles.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace tide::rhi { class IDevice; class ICommandBuffer; }
namespace tide::platform { class Window; }
namespace tide::input { class InputSystem; }

namespace tide::imgui {

enum class ContextError : uint32_t {
    AlreadyCreated,
    ImGuiCreateFailed,
    ShaderLoadFailed,
    PipelineCreateFailed,
    ResourceCreateFailed,
};

// Number of in-flight frames the per-frame ring buffers serve. Must be ≥
// MetalSwapchain::kMaxFramesInFlight (3) — otherwise the GPU still reads
// from the slot we're about to overwrite. Phase 1 hardcodes this; future
// work should expose it via DeviceCapabilities so consumers don't duplicate.
inline constexpr uint32_t kFramesInFlight = 3;

// Per-frame vbuf/ibuf cap. 64k verts × 20 B = 1.28 MB; 64k indices × 2 B =
// 128 KB. Times 3 ring slots = ~4.2 MB host-visible total. Default debug UI
// fits comfortably; if a frame exceeds the cap the renderer logs a warning
// and skips ImGui draws for that frame (no truncation).
inline constexpr uint32_t kMaxVerts   = 64 * 1024;
inline constexpr uint32_t kMaxIndices = 64 * 1024;

class Context {
public:
    // Construct with a device and window. Optionally pass an InputSystem to
    // receive raw mouse/keyboard/scroll/char events — without it, ImGui
    // renders but cannot capture clicks. Caller owns input lifetime; the
    // observer is attached for the Context's lifetime and detached in dtor.
    Context(rhi::IDevice& device,
            platform::Window& window,
            input::InputSystem* input = nullptr);
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Override where the compiled metallibs live. Defaults to the binary's
    // ../../shaders/ following the same convention as samples/02 and 03.
    void set_metallib_dir(std::filesystem::path dir) noexcept;

    // Builds the font atlas, shaders, descriptor sets, pipeline, and ring
    // buffers. Must be called once before begin_frame(). Idempotent under
    // a flush_atlas() request (rebuilds the atlas in place).
    [[nodiscard]] tide::expected<void, ContextError>
        create_resources(rhi::Format color_format);

    // Per-frame: pumps DisplaySize / DisplayFramebufferScale / cursor pos
    // / dt into ImGuiIO and calls ImGui::NewFrame().
    void begin_frame(double frame_dt_seconds);

    // Records ImGui draws into the currently-open render pass on `cmd`.
    // Caller owns begin_render_pass / end_render_pass; this is an in-pass
    // continuation. fb_width and fb_height are framebuffer pixels.
    void render(rhi::ICommandBuffer& cmd, uint32_t fb_width, uint32_t fb_height);

    // Marks the font atlas dirty; the next begin_frame rebuilds and
    // re-uploads. Use after AddFont() / DPI change.
    void mark_atlas_dirty() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tide::imgui
