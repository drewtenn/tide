// samples/01_clear — Phase 1 task 2 runtime smoke.
//
// Opens a window, brings up the Metal RHI, runs a frame loop that clears the
// drawable to a slowly-shifting color via the abstract IDevice / ICommandBuffer
// surface, and exits on Escape or window close. Validates the entire Phase 1
// foundation (IDevice + RenderPassDesc + AttachmentTarget + clear-to-color +
// triple-buffer pacing) end-to-end.

#include "tide/core/Assert.h"
#include "tide/core/Log.h"
#include "tide/input/Actions.h"
#include "tide/input/InputContext.h"
#include "tide/platform/Time.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#else
#define ZoneScopedN(x) (void) 0
#define FrameMark (void) 0
namespace tracy {
inline void SetThreadName(const char*) {}
} // namespace tracy
#endif

#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

tide::rhi::ClearColorValue color_for_frame(uint64_t frame) {
    // Slow pulse so the user can see the clear is actually running each frame.
    const float t = static_cast<float>(frame) * 0.01f;
    return {
        .r = 0.5f + 0.5f * std::sin(t),
        .g = 0.5f + 0.5f * std::sin(t + 2.094f),
        .b = 0.5f + 0.5f * std::sin(t + 4.188f),
        .a = 1.0f,
    };
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    tide::log::init();
    tracy::SetThreadName("main");

    TIDE_LOG_INFO("tide samples/01_clear — Metal RHI clear-to-color smoke");
    TIDE_LOG_INFO("Press Escape or close the window to exit.");

    auto window_result = tide::platform::Window::create({
        .width = 1280,
        .height = 720,
        .title = "tide — 01_clear",
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
        TIDE_LOG_ERROR("MetalDevice::create_device failed: {}",
                       static_cast<int>(device_result.error()));
        return 2;
    }
    auto device = std::move(*device_result);

    const auto& caps = device->capabilities();
    TIDE_LOG_INFO("RHI ready: backend={} device={} UMA={}",
                  caps.backend_name, caps.device_name,
                  caps.uniform_memory_architecture ? "yes" : "no");

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
            // SwapchainOutOfDate — skip the frame. Resize handling lands later.
            (void)device->end_frame();
            continue;
        }

        auto* cmd = device->acquire_command_buffer();

        cmd->transition(*sw, tide::rhi::ResourceState::Undefined,
                              tide::rhi::ResourceState::RenderTarget);

        tide::rhi::RenderPassDesc rp{};
        rp.color_attachment_count = 1;
        rp.color_attachments[0].target.texture = *sw;
        rp.color_attachments[0].load_op        = tide::rhi::LoadOp::Clear;
        rp.color_attachments[0].store_op       = tide::rhi::StoreOp::Store;
        rp.color_attachments[0].clear_value    = color_for_frame(frame_index);
        rp.render_area = {0, 0, static_cast<uint32_t>(window.framebuffer_size().width),
                                static_cast<uint32_t>(window.framebuffer_size().height)};

        cmd->begin_render_pass(rp);
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

    input.pop_context(gameplay_handle);
    TIDE_LOG_INFO("Clean shutdown after {} frames.", frame_index);
    return 0;
}
