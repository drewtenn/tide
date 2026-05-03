#pragma once

// MetalSwapchain — owns the CAMetalLayer attached to an NSWindow's contentView
// plus the dispatch_semaphore used for triple-buffer pacing. Locked DEFINE D9–D11.

#include "tide/rhi/Descriptors.h"
#include "tide/rhi/Handles.h"

#include <cstdint>

namespace tide::platform {
class Window;
} // namespace tide::platform

namespace tide::rhi::metal {

constexpr uint32_t kMaxFramesInFlight = 3;

struct SwapchainConfig {
    tide::rhi::Format format{tide::rhi::Format::BGRA8_Unorm_sRGB};
    uint32_t          drawable_count{kMaxFramesInFlight};
    bool              vsync{true};
};

class MetalSwapchain {
public:
    // Constructed via MetalSwapchain::create() — uses Apple types so impl is .mm.
    MetalSwapchain();
    ~MetalSwapchain();

    MetalSwapchain(const MetalSwapchain&)            = delete;
    MetalSwapchain& operator=(const MetalSwapchain&) = delete;

    static MetalSwapchain* create(void* mtl_device,
                                  tide::platform::Window& window,
                                  const SwapchainConfig& cfg);

    // Frame lifecycle.
    void wait_for_next_frame_slot();   // blocks if N frames already in flight
    void release_frame_slot();         // signal helper (used by completion)

    // Acquire the per-frame drawable. Returns nullptr (id<CAMetalDrawable>) if
    // the layer timed out. Skip the frame if so.
    [[nodiscard]] void* next_drawable() noexcept;

    // Drop the drawable reference once present has been issued.
    void release_drawable() noexcept;

    void resize(uint32_t width, uint32_t height) noexcept;

    [[nodiscard]] tide::rhi::Format format() const noexcept { return cfg_.format; }
    [[nodiscard]] uint32_t          width() const noexcept;
    [[nodiscard]] uint32_t          height() const noexcept;

    // Internal: the dispatch_semaphore handle, used by command-buffer completion
    // handlers to signal slot release.
    [[nodiscard]] void* semaphore_handle() const noexcept;

    // Internal: the CAMetalLayer pointer for tests / capture.
    [[nodiscard]] void* metal_layer() const noexcept;

    // Internal: id<MTLTexture> for the most recently-acquired drawable, valid
    // only between next_drawable() and release_drawable().
    [[nodiscard]] void* current_drawable_texture() const noexcept;

    // Internal: current drawable for present.
    [[nodiscard]] void* current_drawable() const noexcept;

private:
    SwapchainConfig cfg_{};
    // Opaque pointers held as (__bridge_retained)/raw void* with manual lifetime
    // management via the MetalSwapchain.mm RAII pattern. Implementation file
    // documents the ownership semantics.
    void* layer_{nullptr};
    void* semaphore_{nullptr};
    void* current_drawable_{nullptr};
    void* current_drawable_texture_{nullptr};
};

} // namespace tide::rhi::metal
