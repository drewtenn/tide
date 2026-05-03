// samples/00_window — Phase 0 deliverable demo.
//
// Opens an empty native window, polls events at ~60Hz, and exits cleanly when
// the OS asks (Cmd-Q / Alt-F4 / WM close) or when the user presses Escape via
// the action map. Emits Tracy FrameMark per frame so a connected tracy-profiler
// shows a heartbeat.

#include "tide/core/Assert.h"
#include "tide/core/Log.h"
#include "tide/input/Actions.h"
#include "tide/input/InputContext.h"
#include "tide/jobs/IJobSystem.h"
#include "tide/platform/Time.h"
#include "tide/platform/Window.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#else
#define ZoneScopedN(x) (void) 0
#define FrameMark (void) 0

namespace tracy {
inline void SetThreadName(const char*) {}
} // namespace tracy
#endif

#include <chrono>
#include <thread>

namespace {

constexpr int kTargetFps = 60;
constexpr double kTargetFrameSec = 1.0 / static_cast<double>(kTargetFps);

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    tide::log::init();
    tracy::SetThreadName("main");

    TIDE_LOG_INFO("tide samples/00_window — Phase 0 deliverable");
    TIDE_LOG_INFO("Press Escape or close the window to exit.");

    auto window_result = tide::platform::Window::create({
        .width = 1280,
        .height = 720,
        .title = "tide — 00_window",
    });

    if (!window_result) {
        TIDE_LOG_ERROR("Window::create failed: {}", static_cast<int>(window_result.error()));
        return 1;
    }
    auto window = std::move(*window_result);

    tide::input::InputSystem input(window);
    input.bind(
        tide::input::KeyBinding{
            .action = tide::input::Actions::Quit,
            .key = tide::input::Key::Escape,
        }
    );

    tide::input::GameplayContext gameplay_ctx;
    const uint32_t gameplay_handle = input.push_context(&gameplay_ctx);

    // Confirm jobs API is wired even though Phase 0 runs inline.
    auto& jobs = tide::jobs::default_job_system();
    TIDE_LOG_DEBUG("Job system worker_count = {}", jobs.worker_count());

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

        // Phase 0 has no renderer; the loop is just timing + event pumping.
        // A trivial inline-job submission proves the jobs system is wired.
        tide::jobs::submit(jobs, [] { ZoneScopedN("Tick"); }, "Tick");

        const auto dt = frame_timer.tick();
        if (++frame_index % 120 == 0) {
            TIDE_LOG_DEBUG("frame {} dt={:.3f}ms", frame_index, tide::platform::milliseconds(dt));
        }

        FrameMark;

        // Crude frame cap. Real frame pacing arrives in Phase 4.
        const double frame_sec = tide::platform::seconds(dt);
        if (frame_sec < kTargetFrameSec) {
            std::this_thread::sleep_for(std::chrono::duration<double>(kTargetFrameSec - frame_sec));
        }
    }

    input.pop_context(gameplay_handle);
    TIDE_LOG_INFO("Clean shutdown.");
    return 0;
}
