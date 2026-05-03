// samples/02_triangle — Phase 1 task 6 deliverable.
//
// First colored triangle on Metal via the engine's RHI. Loads a precompiled
// .metallib pair, builds a graphics pipeline matched to the swapchain format,
// and issues a 3-vertex draw call inside the per-frame render pass. No vertex
// buffers, no uniforms — the VS sources its positions/colors from vertex_id
// (matches engine/shaders/triangle.vs.metal). Promotion to textured quad with
// rotating uniform is task 7.

#include "tide/core/Assert.h"
#include "tide/core/Log.h"
#include "tide/input/Actions.h"
#include "tide/input/InputContext.h"
#include "tide/platform/Path.h"
#include "tide/platform/Time.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#else
#define ZoneScopedN(x) (void) 0
#define FrameMark      (void) 0
namespace tracy {
inline void SetThreadName(const char*) {}
} // namespace tracy
#endif

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

// Reads a whole file into a byte buffer. Returns empty on any failure; the
// caller treats empty as fatal — there's no recovery path from "shader
// missing" in this sample.
std::vector<std::byte> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto end = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes;
    bytes.resize(static_cast<size_t>(end));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), end)) return {};
    return bytes;
}

// Anchors the shaders directory to the running binary's path rather than
// CWD. CMake stages outputs at ${CMAKE_BINARY_DIR}/shaders/ and the sample
// binary at ${CMAKE_BINARY_DIR}/samples/02_triangle/<exe>, so
// `<exe-dir>/../../shaders` reaches the artefacts regardless of how the
// binary was launched.
std::string metallib_path(const char* basename) {
    static const auto base = tide::platform::executable_dir() / ".." / ".." / "shaders";
    return (base / basename).string();
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    tide::log::init();
    tracy::SetThreadName("main");

    TIDE_LOG_INFO("tide samples/02_triangle — first colored triangle on Metal RHI");
    TIDE_LOG_INFO("Press Escape or close the window to exit.");

    auto window_result = tide::platform::Window::create({
        .width  = 1280,
        .height = 720,
        .title  = "tide — 02_triangle",
    });
    if (!window_result) {
        TIDE_LOG_ERROR("Window::create failed: {}",
                       static_cast<int>(window_result.error()));
        return 1;
    }
    auto window = std::move(*window_result);

    tide::input::InputSystem input(window);
    input.bind(tide::input::KeyBinding{
        .action = tide::input::Actions::Quit,
        .key    = tide::input::Key::Escape,
    });
    tide::input::GameplayContext gameplay_ctx;
    const uint32_t gameplay_handle = input.push_context(&gameplay_ctx);

    auto device_result = tide::rhi::metal::create_device(window);
    if (!device_result) {
        TIDE_LOG_ERROR("create_device failed: {}",
                       static_cast<int>(device_result.error()));
        return 2;
    }
    auto device = std::move(*device_result);

    const auto& caps = device->capabilities();
    TIDE_LOG_INFO("RHI ready: backend={} device={} UMA={}",
                  caps.backend_name, caps.device_name,
                  caps.uniform_memory_architecture ? "yes" : "no");

    // ─── Shader load ─────────────────────────────────────────────────────────
    auto vs_bytes = read_file_bytes(metallib_path("triangle.vs.metallib"));
    auto fs_bytes = read_file_bytes(metallib_path("triangle.ps.metallib"));
    if (vs_bytes.empty() || fs_bytes.empty()) {
        TIDE_LOG_ERROR("Failed to read triangle metallibs from {}",
                       metallib_path(""));
        return 3;
    }

    tide::rhi::ShaderDesc vs_desc{};
    vs_desc.stage       = tide::rhi::ShaderStage::Vertex;
    vs_desc.bytecode    = std::span<const std::byte>(vs_bytes.data(), vs_bytes.size());
    vs_desc.entry_point = "main0";
    vs_desc.debug_name  = "triangle.vs";

    tide::rhi::ShaderDesc fs_desc{};
    fs_desc.stage       = tide::rhi::ShaderStage::Fragment;
    fs_desc.bytecode    = std::span<const std::byte>(fs_bytes.data(), fs_bytes.size());
    fs_desc.entry_point = "main0";
    fs_desc.debug_name  = "triangle.ps";

    auto vs = device->create_shader(vs_desc);
    auto fs = device->create_shader(fs_desc);
    if (!vs || !fs) {
        TIDE_LOG_ERROR("create_shader failed (vs={}, fs={})",
                       vs ? 0 : static_cast<int>(vs.error()),
                       fs ? 0 : static_cast<int>(fs.error()));
        return 4;
    }

    // ─── Pipeline ────────────────────────────────────────────────────────────
    tide::rhi::GraphicsPipelineDesc pso_desc{};
    pso_desc.vertex_shader              = *vs;
    pso_desc.fragment_shader            = *fs;
    pso_desc.topology                   = tide::rhi::PrimitiveTopology::TriangleList;
    pso_desc.rasterization.cull         = tide::rhi::CullMode::None;
    pso_desc.rasterization.front_face   = tide::rhi::FrontFace::CounterClockwise;
    pso_desc.color_attachment_count     = 1;
    pso_desc.color_formats[0]           = device->swapchain_format();
    pso_desc.color_blend[0].write_mask  = 0xF;
    pso_desc.sample_count               = 1;
    pso_desc.debug_name                 = "triangle.pso";

    auto pso = device->create_graphics_pipeline(pso_desc);
    if (!pso) {
        TIDE_LOG_ERROR("create_graphics_pipeline failed: {}",
                       static_cast<int>(pso.error()));
        return 5;
    }
    TIDE_LOG_INFO("Pipeline built. Entering frame loop.");

    // ─── Frame loop ──────────────────────────────────────────────────────────
    tide::platform::FrameTimer frame_timer;
    uint64_t frame_index = 0;

    while (!window.should_close()) {
        ZoneScopedN("Main");

        input.begin_frame();
        tide::platform::Window::poll_events();

        if (input.is_just_pressed(tide::input::Actions::Quit)) {
            TIDE_LOG_INFO("Quit action triggered — closing window.");
            window.request_close();
        }

        if (auto begin = device->begin_frame(); !begin) {
            TIDE_LOG_ERROR("begin_frame failed: {}",
                           static_cast<int>(begin.error()));
            break;
        }

        auto sw = device->acquire_swapchain_texture();
        if (!sw) {
            (void)device->end_frame();
            continue;
        }

        auto* cmd = device->acquire_command_buffer();
        cmd->transition(*sw, tide::rhi::ResourceState::Undefined,
                              tide::rhi::ResourceState::RenderTarget);

        const auto fb = window.framebuffer_size();
        tide::rhi::RenderPassDesc rp{};
        rp.color_attachment_count                = 1;
        rp.color_attachments[0].target.texture   = *sw;
        rp.color_attachments[0].load_op          = tide::rhi::LoadOp::Clear;
        rp.color_attachments[0].store_op         = tide::rhi::StoreOp::Store;
        rp.color_attachments[0].clear_value      = {0.05f, 0.07f, 0.10f, 1.0f};
        rp.render_area = {0, 0, static_cast<uint32_t>(fb.width),
                                static_cast<uint32_t>(fb.height)};

        cmd->begin_render_pass(rp);

        tide::rhi::Viewport vp{
            .x         = 0.0f,
            .y         = 0.0f,
            .width     = static_cast<float>(fb.width),
            .height    = static_cast<float>(fb.height),
            .min_depth = 0.0f,
            .max_depth = 1.0f,
        };
        cmd->set_viewport(vp);
        cmd->set_scissor({0, 0, static_cast<uint32_t>(fb.width),
                                static_cast<uint32_t>(fb.height)});

        cmd->bind_pipeline(*pso);
        cmd->draw(/*vertex_count=*/3, /*instance_count=*/1,
                  /*first_vertex=*/0, /*first_instance=*/0);

        cmd->end_render_pass();
        cmd->transition(*sw, tide::rhi::ResourceState::RenderTarget,
                              tide::rhi::ResourceState::Present);

        device->submit(cmd);
        (void)device->end_frame();

        const auto dt = frame_timer.tick();
        if (++frame_index % 120 == 0) {
            TIDE_LOG_DEBUG("frame {} dt={:.3f}ms",
                           frame_index, tide::platform::milliseconds(dt));
        }
        FrameMark;
    }

    // Tear down before the device — destroy_pipeline must run while shaders
    // and the device are still alive (the PSO holds references to MTLFunction).
    device->destroy_pipeline(*pso);
    device->destroy_shader(*vs);
    device->destroy_shader(*fs);

    input.pop_context(gameplay_handle);
    TIDE_LOG_INFO("Clean shutdown after {} frames.", frame_index);
    return 0;
}
