#pragma once

// Public factory for the Metal RHI backend. Hides every Apple-only header behind
// the implementation file (.mm). Callers see only the abstract IDevice contract.
//
// Phase 1 task 2 scope: device + swapchain + present + clear-to-color. Resource
// creation (buffers, textures, pipelines) lands in atomic tasks 4–7.

#include "tide/core/Expected.h"
#include "tide/rhi/IDevice.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace tide::platform {
class Window;
} // namespace tide::platform

namespace tide::rhi::metal {

struct MetalDeviceOptions {
    bool        enable_validation{false};   // Metal validation layer (Xcode)
    const char* device_name_hint{nullptr};  // null = system default
};

[[nodiscard]] tide::expected<std::unique_ptr<tide::rhi::IDevice>, RhiError>
create_device(tide::platform::Window& window,
              const MetalDeviceOptions& options = {});

// Capture API (Phase 1 task 12 hook; declared now so call sites compile).
// Per DEFINE: not on IDevice (avoids the "no virtual on IDevice for ImGui"
// tripwire). Returns false if MTLCaptureManager is unavailable or capture not
// allowed in current launch environment.
bool begin_frame_capture(tide::rhi::IDevice& device, std::string_view label);
void end_frame_capture();

} // namespace tide::rhi::metal
