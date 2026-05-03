// MetalSwapchain — CAMetalLayer setup + drawable acquisition + dispatch_semaphore
// pacing. Locked DEFINE D9, D10, D11.

#import "MetalSwapchain.h"

#import "tide/platform/Window.h"

#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

#include <cstdio>

namespace tide::rhi::metal {

namespace {

// Returns MTLPixelFormatInvalid for unsupported / depth / Undefined inputs so
// the caller can fail fast rather than silently producing a wrong-format layer.
MTLPixelFormat to_mtl_color_format(tide::rhi::Format f) noexcept {
    switch (f) {
        case tide::rhi::Format::BGRA8_Unorm:       return MTLPixelFormatBGRA8Unorm;
        case tide::rhi::Format::BGRA8_Unorm_sRGB:  return MTLPixelFormatBGRA8Unorm_sRGB;
        case tide::rhi::Format::RGBA8_Unorm:       return MTLPixelFormatRGBA8Unorm;
        case tide::rhi::Format::RGBA8_Unorm_sRGB:  return MTLPixelFormatRGBA8Unorm_sRGB;
        case tide::rhi::Format::RGBA16_Float:      return MTLPixelFormatRGBA16Float;
        default:                                   return MTLPixelFormatInvalid;
    }
}

} // namespace

MetalSwapchain::MetalSwapchain()  = default;
MetalSwapchain::~MetalSwapchain() {
    if (current_drawable_) {
        CFRelease((CFTypeRef)current_drawable_);
        current_drawable_ = nullptr;
    }
    if (semaphore_) {
        CFRelease((CFTypeRef)semaphore_);
        semaphore_ = nullptr;
    }
    if (layer_) {
        CFRelease((CFTypeRef)layer_);
        layer_ = nullptr;
    }
}

MetalSwapchain* MetalSwapchain::create(void* mtl_device,
                                       tide::platform::Window& window,
                                       const SwapchainConfig& cfg) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device;
    if (!device) return nullptr;

    // Validate format up front so a typo or unsupported format fails fast
    // rather than silently configuring the layer as BGRA8_Unorm_sRGB.
    MTLPixelFormat px = to_mtl_color_format(cfg.format);
    if (px == MTLPixelFormatInvalid) {
        std::fprintf(stderr, "tide.metal: unsupported swapchain format %d\n",
                     static_cast<int>(cfg.format));
        return nullptr;
    }

    // Window must have an NSWindow on macOS — without one we cannot attach the
    // layer; fail fast rather than returning a half-built swapchain.
    auto* ns_window = (__bridge NSWindow*)window.cocoa_window();
    if (!ns_window || !ns_window.contentView) {
        std::fprintf(stderr, "tide.metal: cocoa_window/contentView is null\n");
        return nullptr;
    }

    // Compute the effective drawable count once so the layer cap and the
    // semaphore stay in lock-step. Locked DEFINE D10 caps at 3.
    uint32_t effective = cfg.drawable_count;
    if (effective < 2)               effective = kMaxFramesInFlight;
    else if (effective > 3)          effective = 3;

    auto* sc = new MetalSwapchain();
    sc->cfg_                = cfg;
    sc->cfg_.drawable_count = effective;

    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device                  = device;
    layer.pixelFormat             = px;
    layer.framebufferOnly         = YES;
    layer.displaySyncEnabled      = cfg.vsync ? YES : NO;
    layer.maximumDrawableCount    = effective;
    layer.presentsWithTransaction = NO;

    NSView* view = ns_window.contentView;
    view.wantsLayer = YES;
    view.layer      = layer;

    // Initial drawable size in pixels (Retina-aware).
    const auto fb = window.framebuffer_size();
    layer.drawableSize = CGSizeMake(static_cast<CGFloat>(fb.width),
                                    static_cast<CGFloat>(fb.height));

    sc->layer_ = (void*)CFBridgingRetain(layer);
    // Bridge-retain the semaphore so its lifetime matches the swapchain. We
    // cast back via __bridge in wait/signal helpers (no ownership transfer).
    // Initial count must match the layer's maximum drawable count.
    sc->semaphore_ = (void*)CFBridgingRetain(
        dispatch_semaphore_create(static_cast<long>(effective)));

    return sc;
}

void MetalSwapchain::wait_for_next_frame_slot() {
    dispatch_semaphore_t sem = (__bridge dispatch_semaphore_t)semaphore_;
    if (sem) {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    }
}

void MetalSwapchain::release_frame_slot() {
    dispatch_semaphore_t sem = (__bridge dispatch_semaphore_t)semaphore_;
    if (sem) {
        dispatch_semaphore_signal(sem);
    }
}

void* MetalSwapchain::next_drawable() noexcept {
    auto* layer = (__bridge CAMetalLayer*)layer_;
    if (!layer) return nullptr;

    // DEFINE D11: late acquisition inside @autoreleasepool. Without the pool,
    // any autoreleased Foundation objects produced during nextDrawable's
    // internal work would accumulate until the runloop drains, potentially
    // pinning drawable lifetime longer than expected.
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) {
            return nullptr;  // timed out — caller skips frame
        }
        current_drawable_         = (__bridge_retained void*)drawable;
        current_drawable_texture_ = (__bridge void*)drawable.texture;
    }
    return current_drawable_;
}

void MetalSwapchain::release_drawable() noexcept {
    if (current_drawable_) {
        CFRelease((CFTypeRef)current_drawable_);
        current_drawable_         = nullptr;
        current_drawable_texture_ = nullptr;
    }
}

void MetalSwapchain::resize(uint32_t width, uint32_t height) noexcept {
    auto* layer = (__bridge CAMetalLayer*)layer_;
    if (!layer) return;
    layer.drawableSize = CGSizeMake(static_cast<CGFloat>(width),
                                    static_cast<CGFloat>(height));
}

uint32_t MetalSwapchain::width() const noexcept {
    auto* layer = (__bridge CAMetalLayer*)layer_;
    return layer ? static_cast<uint32_t>(layer.drawableSize.width) : 0;
}

uint32_t MetalSwapchain::height() const noexcept {
    auto* layer = (__bridge CAMetalLayer*)layer_;
    return layer ? static_cast<uint32_t>(layer.drawableSize.height) : 0;
}

void* MetalSwapchain::semaphore_handle() const noexcept { return semaphore_; }
void* MetalSwapchain::metal_layer()      const noexcept { return layer_; }
void* MetalSwapchain::current_drawable_texture() const noexcept {
    return current_drawable_texture_;
}
void* MetalSwapchain::current_drawable() const noexcept {
    return current_drawable_;
}

} // namespace tide::rhi::metal
