#pragma once

#include <chrono>
#include <cstdint>

namespace tide::platform {

// Monotonic clock measured in nanoseconds since an arbitrary engine epoch.
// Use for frame timing, profiling, and animation. Never use std::chrono::system_clock
// for these — system clock can jump backward on NTP corrections.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;

[[nodiscard]] inline TimePoint now() noexcept {
    return Clock::now();
}

[[nodiscard]] inline double seconds(Duration d) noexcept {
    return std::chrono::duration<double>(d).count();
}

[[nodiscard]] inline double milliseconds(Duration d) noexcept {
    return std::chrono::duration<double, std::milli>(d).count();
}

[[nodiscard]] inline int64_t nanoseconds(Duration d) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
}

// Frame-time accumulator. Owns a TimePoint of the previous tick; tick()
// returns the elapsed Duration and updates internal state.
class FrameTimer {
public:
    FrameTimer() : last_(now()) {}

    Duration tick() noexcept {
        const TimePoint t = now();
        const Duration dt = t - last_;
        last_ = t;
        return dt;
    }

    [[nodiscard]] TimePoint last() const noexcept { return last_; }

private:
    TimePoint last_;
};

} // namespace tide::platform
