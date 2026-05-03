// MetalDevice.mm — IDevice implementation. Phase 1 task 2 scope: device creation,
// swapchain, frame lifecycle, clear-to-color. Resource creation methods stub to
// UnsupportedFeature; they're populated in atomic tasks 4–7.

#import "MetalDevice.h"
#import "MetalCommandBuffer.h"
#import "MetalSwapchain.h"

#import "tide/core/Log.h"
#import "tide/platform/Window.h"

#import <Metal/Metal.h>

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#else
#define ZoneScopedN(x) (void) 0
#endif

#include <cstring>

namespace tide::rhi::metal {

namespace {

constexpr tide::rhi::Format kSwapchainDefaultFormat = tide::rhi::Format::BGRA8_Unorm_sRGB;

} // namespace

// ─── create_device factory ──────────────────────────────────────────────────

tide::expected<std::unique_ptr<tide::rhi::IDevice>, tide::rhi::RhiError>
create_device(tide::platform::Window& window,
              const MetalDeviceOptions& options) {
    @autoreleasepool {
        id<MTLDevice> mtl_device = MTLCreateSystemDefaultDevice();
        if (!mtl_device) {
            return tide::unexpected(tide::rhi::RhiError::DeviceLost);
        }
        if (options.device_name_hint) {
            // Phase 1: hint accepted but ignored; system default chosen.
        }

        id<MTLCommandQueue> queue = [mtl_device newCommandQueue];
        if (!queue) {
            return tide::unexpected(tide::rhi::RhiError::BackendInternal);
        }
        queue.label = @"tide.queue";

        SwapchainConfig sc_cfg{};
        sc_cfg.format         = kSwapchainDefaultFormat;
        sc_cfg.drawable_count = kMaxFramesInFlight;
        sc_cfg.vsync          = true;

        std::unique_ptr<MetalSwapchain> swapchain(
            MetalSwapchain::create((__bridge void*)mtl_device, window, sc_cfg));
        if (!swapchain) {
            return tide::unexpected(tide::rhi::RhiError::BackendInternal);
        }

        tide::rhi::DeviceCapabilities caps{};
        caps.supports_compute              = true;
        caps.uniform_memory_architecture   = true;   // Apple Silicon UMA
        caps.device_name                   = "Metal";
        caps.backend_name                  = "metal";

        // Take +1 retains on device and queue; the MetalDevice owns them and
        // releases in its dtor. Locked DEFINE D24 (ARC) lets us do this safely
        // because every accessor uses __bridge (no ownership transfer).
        void* device_ptr = (__bridge_retained void*)mtl_device;
        void* queue_ptr  = (__bridge_retained void*)queue;

        return std::make_unique<MetalDevice>(device_ptr, queue_ptr,
                                             std::move(swapchain), caps);
    }
}

// ─── MetalDevice ────────────────────────────────────────────────────────────

MetalDevice::MetalDevice(void* mtl_device,
                         void* mtl_queue,
                         std::unique_ptr<MetalSwapchain> swapchain,
                         tide::rhi::DeviceCapabilities caps)
    : mtl_device_(mtl_device),
      mtl_queue_(mtl_queue),
      swapchain_(std::move(swapchain)),
      caps_(caps) {
    // Pre-allocate slot 0 of the texture pool for the swapchain attachment.
    // This avoids the {0,1} sentinel collision flagged by code-review concern
    // #10: the first real call to create_texture() now lands at slot 1+.
    tide::rhi::TextureDesc swapchain_desc{};
    swapchain_desc.dimension       = tide::rhi::TextureDimension::Tex2D;
    swapchain_desc.format          = swapchain_->format();
    swapchain_desc.width           = swapchain_->width();
    swapchain_desc.height          = swapchain_->height();
    swapchain_desc.depth_or_layers = 1;
    swapchain_desc.mip_levels      = 1;
    swapchain_desc.sample_count    = 1;
    swapchain_desc.usage           = tide::rhi::TextureUsage::RenderTarget |
                                     tide::rhi::TextureUsage::Present;
    swapchain_desc.memory          = tide::rhi::MemoryType::DeviceLocal;
    swapchain_desc.debug_name      = "swapchain.drawable";
    swapchain_handle_ = texture_pool_.allocate(
        /*mtl_texture*/nullptr, swapchain_desc, /*owns*/false);
    // Sanity: the first allocation MUST land at slot 0.
    // (TIDE_ASSERT not used here because Phase 1 ctor must not fail-stop on
    // a release-build trip; the static slot-0 contract is documented in
    // MetalDevice.h::kSwapchainSlot.)
}

MetalDevice::~MetalDevice() {
    // Tear down in reverse-construction order. Swapchain references the queue
    // indirectly via in-flight command buffers; release queue last among Metal
    // ObjC objects.
    swapchain_.reset();
    if (mtl_queue_) {
        CFRelease((CFTypeRef)mtl_queue_);
        mtl_queue_ = nullptr;
    }
    if (mtl_device_) {
        CFRelease((CFTypeRef)mtl_device_);
        mtl_device_ = nullptr;
    }
}

void* MetalDevice::mtl_device()        const noexcept { return mtl_device_; }
void* MetalDevice::mtl_command_queue() const noexcept { return mtl_queue_; }

MetalPipeline* MetalDevice::pipeline(tide::rhi::PipelineHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return pipeline_pool_.get(h);
}

MetalBuffer* MetalDevice::buffer(tide::rhi::BufferHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return buffer_pool_.get(h);
}

MetalTexture* MetalDevice::texture(tide::rhi::TextureHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return texture_pool_.get(h);
}

MetalTextureView* MetalDevice::texture_view(tide::rhi::TextureViewHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return texture_view_pool_.get(h);
}

MetalSampler* MetalDevice::sampler(tide::rhi::SamplerHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return sampler_pool_.get(h);
}

MetalDescriptorSet* MetalDevice::descriptor_set(tide::rhi::DescriptorSetHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return descriptor_set_pool_.get(h);
}

MetalDescriptorSetLayout* MetalDevice::descriptor_set_layout(
    tide::rhi::DescriptorSetLayoutHandle h) noexcept {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    return descriptor_layout_pool_.get(h);
}

void* MetalDevice::resolve_attachment(const tide::rhi::AttachmentTarget& target) noexcept {
    if (target.uses_view()) {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        auto* rec = texture_view_pool_.get(target.view);
        return rec ? rec->mtl_texture_view : nullptr;
    }
    if (target.uses_texture()) {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        auto* rec = texture_pool_.get(target.texture);
        if (!rec) return nullptr;
        // Slot 0 is the swapchain — return the per-frame drawable texture.
        if (target.texture.index == kSwapchainSlot) {
            return swapchain_->current_drawable_texture();
        }
        return rec->mtl_texture;
    }
    return nullptr;
}

// ─── Frame lifecycle ────────────────────────────────────────────────────────

tide::expected<void, tide::rhi::RhiError> MetalDevice::begin_frame() {
    swapchain_->wait_for_next_frame_slot();
    return {};
}

tide::expected<void, tide::rhi::RhiError> MetalDevice::end_frame() {
    if (current_cmd_) {
        // Caller may have submit()ed already; if not, commit defensively.
        // (No-op if cmd_buffer is null inside commit_and_present.)
        current_cmd_->commit_and_present();
        current_cmd_.reset();
    } else {
        // No command buffer recorded this frame: release the slot we acquired.
        swapchain_->release_frame_slot();
    }
    return {};
}

tide::expected<tide::rhi::TextureHandle, tide::rhi::RhiError>
MetalDevice::acquire_swapchain_texture() {
    void* drawable = swapchain_->next_drawable();
    if (!drawable) {
        return tide::unexpected(tide::rhi::RhiError::SwapchainOutOfDate);
    }
    std::lock_guard<std::mutex> lock(resource_mutex_);
    // Slot 0 was allocated in the ctor; the captured `swapchain_handle_`
    // carries the live generation. Look up the slot, rebind the per-frame
    // drawable texture so resolve_attachment can find it, and return the
    // captured handle.
    auto* rec = texture_pool_.get(swapchain_handle_);
    if (!rec) {
        return tide::unexpected(tide::rhi::RhiError::BackendInternal);
    }
    rec->rebind_swapchain(swapchain_->current_drawable_texture());
    return swapchain_handle_;
}

tide::rhi::Format MetalDevice::swapchain_format() const noexcept {
    return swapchain_->format();
}

tide::expected<void, tide::rhi::RhiError>
MetalDevice::resize_swapchain(uint32_t width, uint32_t height) {
    swapchain_->resize(width, height);
    return {};
}

// ─── Command buffer pool ────────────────────────────────────────────────────

tide::rhi::ICommandBuffer* MetalDevice::acquire_command_buffer() {
    if (!current_cmd_) {
        current_cmd_ = std::make_unique<MetalCommandBuffer>(*this);
    }
    current_cmd_->begin();
    return current_cmd_.get();
}

void MetalDevice::submit(tide::rhi::ICommandBuffer* cmd) {
    auto* mcb = static_cast<MetalCommandBuffer*>(cmd);
    if (mcb) {
        mcb->commit_and_present();
    }
    // current_cmd_ stays alive until end_frame() resets it; submit is the
    // canonical handoff but end_frame is the lifecycle anchor.
}

// ─── Format / usage / storage mode mapping ─────────────────────────────────

namespace {

MTLStorageMode to_mtl_storage(tide::rhi::MemoryType m) noexcept {
    switch (m) {
        case tide::rhi::MemoryType::DeviceLocal: return MTLStorageModePrivate;
        case tide::rhi::MemoryType::Upload:      return MTLStorageModeShared;
        case tide::rhi::MemoryType::Readback:    return MTLStorageModeShared;
    }
    return MTLStorageModePrivate;
}

MTLPixelFormat to_mtl_pixel_format(tide::rhi::Format f) noexcept {
    switch (f) {
        case tide::rhi::Format::R8_Unorm:           return MTLPixelFormatR8Unorm;
        case tide::rhi::Format::R8_Uint:            return MTLPixelFormatR8Uint;
        case tide::rhi::Format::RG8_Unorm:          return MTLPixelFormatRG8Unorm;
        case tide::rhi::Format::RGBA8_Unorm:        return MTLPixelFormatRGBA8Unorm;
        case tide::rhi::Format::RGBA8_Unorm_sRGB:   return MTLPixelFormatRGBA8Unorm_sRGB;
        case tide::rhi::Format::BGRA8_Unorm:        return MTLPixelFormatBGRA8Unorm;
        case tide::rhi::Format::BGRA8_Unorm_sRGB:   return MTLPixelFormatBGRA8Unorm_sRGB;
        case tide::rhi::Format::R16_Float:          return MTLPixelFormatR16Float;
        case tide::rhi::Format::RG16_Float:         return MTLPixelFormatRG16Float;
        case tide::rhi::Format::RGBA16_Float:       return MTLPixelFormatRGBA16Float;
        case tide::rhi::Format::R32_Uint:           return MTLPixelFormatR32Uint;
        case tide::rhi::Format::R32_Float:          return MTLPixelFormatR32Float;
        case tide::rhi::Format::RG32_Float:         return MTLPixelFormatRG32Float;
        case tide::rhi::Format::RGBA32_Float:       return MTLPixelFormatRGBA32Float;
        case tide::rhi::Format::D16_Unorm:          return MTLPixelFormatDepth16Unorm;
        case tide::rhi::Format::D32_Float:          return MTLPixelFormatDepth32Float;
        case tide::rhi::Format::D24_Unorm_S8_Uint:  return MTLPixelFormatDepth24Unorm_Stencil8;
        case tide::rhi::Format::D32_Float_S8_Uint:  return MTLPixelFormatDepth32Float_Stencil8;
        default:                                    return MTLPixelFormatInvalid;
    }
}

MTLTextureUsage to_mtl_tex_usage(tide::rhi::TextureUsage u) noexcept {
    MTLTextureUsage out = MTLTextureUsageUnknown;
    if (any(u & tide::rhi::TextureUsage::Sampled))      out |= MTLTextureUsageShaderRead;
    if (any(u & tide::rhi::TextureUsage::Storage))      out |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (any(u & tide::rhi::TextureUsage::RenderTarget)) out |= MTLTextureUsageRenderTarget;
    if (any(u & tide::rhi::TextureUsage::DepthStencil)) out |= MTLTextureUsageRenderTarget;
    if (any(u & tide::rhi::TextureUsage::Present))      out |= MTLTextureUsageRenderTarget;
    return out;
}

MTLTextureType to_mtl_tex_type(tide::rhi::TextureDimension d, uint32_t layers) noexcept {
    switch (d) {
        case tide::rhi::TextureDimension::Tex1D:
            return layers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
        case tide::rhi::TextureDimension::Tex2D:
            return layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        case tide::rhi::TextureDimension::Tex3D:
            return MTLTextureType3D;
        case tide::rhi::TextureDimension::TexCube:
            return layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    }
    return MTLTextureType2D;
}

uint32_t bytes_per_pixel(tide::rhi::Format f) noexcept {
    switch (f) {
        case tide::rhi::Format::R8_Unorm:
        case tide::rhi::Format::R8_Uint:           return 1;
        case tide::rhi::Format::RG8_Unorm:
        case tide::rhi::Format::R16_Float:
        case tide::rhi::Format::D16_Unorm:         return 2;
        case tide::rhi::Format::RGBA8_Unorm:
        case tide::rhi::Format::RGBA8_Unorm_sRGB:
        case tide::rhi::Format::BGRA8_Unorm:
        case tide::rhi::Format::BGRA8_Unorm_sRGB:
        case tide::rhi::Format::RG16_Float:
        case tide::rhi::Format::R32_Uint:
        case tide::rhi::Format::R32_Float:
        case tide::rhi::Format::D24_Unorm_S8_Uint:
        case tide::rhi::Format::D32_Float:         return 4;
        case tide::rhi::Format::RGBA16_Float:
        case tide::rhi::Format::RG32_Float:
        case tide::rhi::Format::D32_Float_S8_Uint: return 8;
        case tide::rhi::Format::RGB32_Float:       return 12;
        case tide::rhi::Format::RGBA32_Float:      return 16;
        default:                                   return 0;
    }
}

} // namespace

// ─── Buffers ────────────────────────────────────────────────────────────────

tide::expected<tide::rhi::BufferHandle, tide::rhi::RhiError>
MetalDevice::create_buffer(const tide::rhi::BufferDesc& desc) {
    if (desc.size_bytes == 0) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        MTLResourceOptions opts =
            (to_mtl_storage(desc.memory) == MTLStorageModeShared)
                ? MTLResourceStorageModeShared
                : MTLResourceStorageModePrivate;
        id<MTLBuffer> buf = [device newBufferWithLength:desc.size_bytes options:opts];
        if (!buf) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }
        if (desc.debug_name) {
            buf.label = [NSString stringWithUTF8String:desc.debug_name];
        }
        std::lock_guard<std::mutex> lock(resource_mutex_);
        return buffer_pool_.allocate(
            (__bridge_retained void*)buf, desc.size_bytes, desc.memory, desc.usage);
    }
}

tide::expected<void, tide::rhi::RhiError>
MetalDevice::upload_buffer(tide::rhi::BufferHandle h,
                           const void* src,
                           size_t bytes,
                           size_t offset) {
    ZoneScopedN("BufferUpload");
    if (!src || bytes == 0) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        std::unique_lock<std::mutex> lock(resource_mutex_);
        auto* rec = buffer_pool_.get(h);
        if (!rec) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        if (offset + bytes > rec->size_bytes) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        id<MTLBuffer> dst = (__bridge id<MTLBuffer>)rec->mtl_buffer;
        const tide::rhi::MemoryType mem = rec->memory;
        const uint64_t dst_size = rec->size_bytes;
        lock.unlock();   // release while we may go to GPU

        if (mem == tide::rhi::MemoryType::Upload ||
            mem == tide::rhi::MemoryType::Readback) {
            // Shared-storage: direct memcpy into the mapped contents pointer.
            void* mapped = [dst contents];
            if (!mapped) {
                return tide::unexpected(tide::rhi::RhiError::BackendInternal);
            }
            std::memcpy(static_cast<char*>(mapped) + offset, src, bytes);
            return {};
        }

        // DeviceLocal: blit via a temporary shared staging buffer + blit encoder.
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        id<MTLBuffer> staging = [device newBufferWithBytes:src
                                                    length:bytes
                                                   options:MTLResourceStorageModeShared];
        if (!staging) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_queue_;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        cb.label = @"tide.upload_buffer";
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:staging
                sourceOffset:0
                    toBuffer:dst
           destinationOffset:offset
                        size:bytes];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];   // synchronous upload — Phase 3 makes this async
        (void)dst_size;
        return {};
    }
}

void MetalDevice::destroy_buffer(tide::rhi::BufferHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    buffer_pool_.release(h);   // dtor CFReleases the MTLBuffer
}

tide::expected<void, tide::rhi::RhiError>
MetalDevice::download_buffer(tide::rhi::BufferHandle h,
                             void* dst,
                             size_t bytes,
                             size_t offset) {
    ZoneScopedN("BufferDownload");
    if (!dst || bytes == 0) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        std::unique_lock<std::mutex> lock(resource_mutex_);
        auto* rec = buffer_pool_.get(h);
        if (!rec) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        if (offset + bytes > rec->size_bytes) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        id<MTLBuffer> src = (__bridge id<MTLBuffer>)rec->mtl_buffer;
        const tide::rhi::MemoryType mem = rec->memory;
        lock.unlock();

        if (mem == tide::rhi::MemoryType::Upload ||
            mem == tide::rhi::MemoryType::Readback) {
            // Shared storage — direct memcpy from the mapped contents.
            void* mapped = [src contents];
            if (!mapped) {
                return tide::unexpected(tide::rhi::RhiError::BackendInternal);
            }
            std::memcpy(dst, static_cast<const char*>(mapped) + offset, bytes);
            return {};
        }

        // DeviceLocal — blit to a Shared staging buffer, wait, memcpy out.
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        id<MTLBuffer> staging = [device newBufferWithLength:bytes
                                                    options:MTLResourceStorageModeShared];
        if (!staging) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_queue_;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        cb.label = @"tide.download_buffer";
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:src
                sourceOffset:offset
                    toBuffer:staging
           destinationOffset:0
                        size:bytes];
        [blit synchronizeResource:staging];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        std::memcpy(dst, [staging contents], bytes);
        return {};
    }
}

// ─── Textures ───────────────────────────────────────────────────────────────

tide::expected<tide::rhi::TextureHandle, tide::rhi::RhiError>
MetalDevice::create_texture(const tide::rhi::TextureDesc& desc) {
    MTLPixelFormat px = to_mtl_pixel_format(desc.format);
    if (px == MTLPixelFormatInvalid || desc.width == 0) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;

        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.pixelFormat       = px;
        td.width             = desc.width;
        td.height            = desc.height;
        td.depth             = (desc.dimension == tide::rhi::TextureDimension::Tex3D)
                                ? desc.depth_or_layers : 1;
        td.arrayLength       = (desc.dimension == tide::rhi::TextureDimension::Tex3D)
                                ? 1 : desc.depth_or_layers;
        td.mipmapLevelCount  = desc.mip_levels;
        td.sampleCount       = desc.sample_count;
        td.textureType       = to_mtl_tex_type(desc.dimension, desc.depth_or_layers);
        td.usage             = to_mtl_tex_usage(desc.usage);
        td.storageMode       = to_mtl_storage(desc.memory);

        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        if (!tex) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }
        if (desc.debug_name) {
            tex.label = [NSString stringWithUTF8String:desc.debug_name];
        }
        std::lock_guard<std::mutex> lock(resource_mutex_);
        return texture_pool_.allocate((__bridge_retained void*)tex, desc, /*owns*/true);
    }
}

tide::expected<void, tide::rhi::RhiError>
MetalDevice::upload_texture(tide::rhi::TextureHandle h,
                            const void* src,
                            size_t bytes,
                            uint32_t mip,
                            uint32_t layer) {
    ZoneScopedN("TextureUpload");
    if (!src || bytes == 0) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        std::unique_lock<std::mutex> lock(resource_mutex_);
        auto* rec = texture_pool_.get(h);
        if (!rec || !rec->mtl_texture) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        const auto desc = rec->desc;   // copy under lock
        id<MTLTexture> dst = (__bridge id<MTLTexture>)rec->mtl_texture;
        lock.unlock();

        const uint32_t bpp = bytes_per_pixel(desc.format);
        if (bpp == 0) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        const uint32_t mip_w = std::max(1u, desc.width  >> mip);
        const uint32_t mip_h = std::max(1u, desc.height >> mip);
        const NSUInteger row_bytes = static_cast<NSUInteger>(mip_w) * bpp;
        const NSUInteger expected  = static_cast<NSUInteger>(row_bytes) * mip_h;
        if (bytes < expected) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }

        MTLRegion region = MTLRegionMake2D(0, 0, mip_w, mip_h);
        if (to_mtl_storage(desc.memory) == MTLStorageModeShared) {
            [dst replaceRegion:region
                   mipmapLevel:mip
                         slice:layer
                     withBytes:src
                   bytesPerRow:row_bytes
                 bytesPerImage:expected];
            return {};
        }

        // DeviceLocal: stage + blit.
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        id<MTLBuffer> staging = [device newBufferWithBytes:src
                                                    length:bytes
                                                   options:MTLResourceStorageModeShared];
        if (!staging) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_queue_;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        cb.label = @"tide.upload_texture";
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:staging
                sourceOffset:0
           sourceBytesPerRow:row_bytes
         sourceBytesPerImage:expected
                  sourceSize:MTLSizeMake(mip_w, mip_h, 1)
                   toTexture:dst
            destinationSlice:layer
            destinationLevel:mip
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        return {};
    }
}

void MetalDevice::destroy_texture(tide::rhi::TextureHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    // Slot 0 (swapchain) is never destroyed — it lives for the device lifetime.
    if (h.index == kSwapchainSlot) return;
    texture_pool_.release(h);
}

// ─── download_texture (Phase 1 task 10) ─────────────────────────────────────
// Synchronous: blit texture → shared staging buffer, wait, memcpy out.

tide::expected<void, tide::rhi::RhiError>
MetalDevice::download_texture(tide::rhi::TextureHandle h,
                              void* dst,
                              size_t bytes,
                              uint32_t mip,
                              uint32_t layer) {
    ZoneScopedN("TextureDownload");
    if (!dst || bytes == 0) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        std::unique_lock<std::mutex> lock(resource_mutex_);
        auto* rec = texture_pool_.get(h);
        if (!rec || !rec->mtl_texture) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        const auto desc = rec->desc;
        id<MTLTexture> src = (__bridge id<MTLTexture>)rec->mtl_texture;
        lock.unlock();

        const uint32_t bpp = bytes_per_pixel(desc.format);
        if (bpp == 0) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        const uint32_t mip_w = std::max(1u, desc.width  >> mip);
        const uint32_t mip_h = std::max(1u, desc.height >> mip);
        const NSUInteger row_bytes = static_cast<NSUInteger>(mip_w) * bpp;
        const NSUInteger expected  = static_cast<NSUInteger>(row_bytes) * mip_h;
        if (bytes < expected) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }

        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        id<MTLBuffer> staging = [device newBufferWithLength:expected
                                                    options:MTLResourceStorageModeShared];
        if (!staging) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }

        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)mtl_queue_;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        cb.label = @"tide.download_texture";
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src
                  sourceSlice:layer
                  sourceLevel:mip
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(mip_w, mip_h, 1)
                     toBuffer:staging
            destinationOffset:0
       destinationBytesPerRow:row_bytes
     destinationBytesPerImage:expected];
        // Apple Silicon UMA shared storage doesn't strictly need
        // synchronizeResource:, but the call is a no-op there and required
        // on discrete-GPU paths if/when this code runs on Intel macs.
        [blit synchronizeResource:staging];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        std::memcpy(dst, [staging contents], expected);
        return {};
    }
}

// ─── Texture views ──────────────────────────────────────────────────────────

tide::expected<tide::rhi::TextureViewHandle, tide::rhi::RhiError>
MetalDevice::create_texture_view(const tide::rhi::TextureViewDesc& desc) {
    if (!desc.texture.valid()) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        std::unique_lock<std::mutex> lock(resource_mutex_);
        auto* src = texture_pool_.get(desc.texture);
        if (!src) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        // Swapchain views aren't useful in Phase 1 — the AttachmentTarget API
        // uses TextureHandle directly. Reject for now to avoid dangling views.
        if (desc.texture.index == kSwapchainSlot) {
            return tide::unexpected(tide::rhi::RhiError::UnsupportedFeature);
        }

        id<MTLTexture> base = (__bridge id<MTLTexture>)src->mtl_texture;
        const tide::rhi::Format view_fmt =
            (desc.format == tide::rhi::Format::Undefined) ? src->desc.format : desc.format;
        MTLPixelFormat px = to_mtl_pixel_format(view_fmt);
        if (px == MTLPixelFormatInvalid) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        MTLTextureType view_type = to_mtl_tex_type(desc.dimension, desc.layer_count);
        NSRange levels = NSMakeRange(desc.base_mip,   desc.mip_count);
        NSRange slices = NSMakeRange(desc.base_layer, desc.layer_count);

        id<MTLTexture> view = [base newTextureViewWithPixelFormat:px
                                                       textureType:view_type
                                                            levels:levels
                                                            slices:slices];
        if (!view) {
            return tide::unexpected(tide::rhi::RhiError::BackendInternal);
        }
        if (desc.debug_name) {
            view.label = [NSString stringWithUTF8String:desc.debug_name];
        }
        return texture_view_pool_.allocate(
            (__bridge_retained void*)view, desc.texture, desc);
    }
}

void MetalDevice::destroy_texture_view(tide::rhi::TextureViewHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    texture_view_pool_.release(h);
}

// ─── Shader / Pipeline mapping helpers (Phase 1 task 5) ─────────────────────

namespace {

MTLPrimitiveTopologyClass to_mtl_topology_class(tide::rhi::PrimitiveTopology t) noexcept {
    switch (t) {
        case tide::rhi::PrimitiveTopology::PointList:     return MTLPrimitiveTopologyClassPoint;
        case tide::rhi::PrimitiveTopology::LineList:
        case tide::rhi::PrimitiveTopology::LineStrip:     return MTLPrimitiveTopologyClassLine;
        case tide::rhi::PrimitiveTopology::TriangleList:
        case tide::rhi::PrimitiveTopology::TriangleStrip: return MTLPrimitiveTopologyClassTriangle;
    }
    return MTLPrimitiveTopologyClassUnspecified;
}

MTLPrimitiveType to_mtl_primitive(tide::rhi::PrimitiveTopology t) noexcept {
    switch (t) {
        case tide::rhi::PrimitiveTopology::PointList:     return MTLPrimitiveTypePoint;
        case tide::rhi::PrimitiveTopology::LineList:      return MTLPrimitiveTypeLine;
        case tide::rhi::PrimitiveTopology::LineStrip:     return MTLPrimitiveTypeLineStrip;
        case tide::rhi::PrimitiveTopology::TriangleList:  return MTLPrimitiveTypeTriangle;
        case tide::rhi::PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLCullMode to_mtl_cull(tide::rhi::CullMode m) noexcept {
    switch (m) {
        case tide::rhi::CullMode::None:  return MTLCullModeNone;
        case tide::rhi::CullMode::Front: return MTLCullModeFront;
        case tide::rhi::CullMode::Back:  return MTLCullModeBack;
    }
    return MTLCullModeNone;
}

MTLWinding to_mtl_winding(tide::rhi::FrontFace f) noexcept {
    return f == tide::rhi::FrontFace::CounterClockwise
               ? MTLWindingCounterClockwise
               : MTLWindingClockwise;
}

MTLTriangleFillMode to_mtl_fill(tide::rhi::PolygonMode m) noexcept {
    // Metal exposes only Fill/Lines for polygon mode (no point fill on render
    // encoders); PolygonMode::Point degenerates to Lines as the closest visual
    // approximation (D2 risk note: callers requiring point primitives should
    // pick PrimitiveTopology::PointList instead).
    return m == tide::rhi::PolygonMode::Fill ? MTLTriangleFillModeFill
                                             : MTLTriangleFillModeLines;
}

MTLCompareFunction to_mtl_compare(tide::rhi::DepthCompare c) noexcept {
    switch (c) {
        case tide::rhi::DepthCompare::Never:          return MTLCompareFunctionNever;
        case tide::rhi::DepthCompare::Less:           return MTLCompareFunctionLess;
        case tide::rhi::DepthCompare::Equal:          return MTLCompareFunctionEqual;
        case tide::rhi::DepthCompare::LessOrEqual:    return MTLCompareFunctionLessEqual;
        case tide::rhi::DepthCompare::Greater:        return MTLCompareFunctionGreater;
        case tide::rhi::DepthCompare::NotEqual:       return MTLCompareFunctionNotEqual;
        case tide::rhi::DepthCompare::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
        case tide::rhi::DepthCompare::Always:         return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLBlendFactor to_mtl_blend_factor(tide::rhi::BlendFactor f) noexcept {
    switch (f) {
        case tide::rhi::BlendFactor::Zero:                  return MTLBlendFactorZero;
        case tide::rhi::BlendFactor::One:                   return MTLBlendFactorOne;
        case tide::rhi::BlendFactor::SrcColor:              return MTLBlendFactorSourceColor;
        case tide::rhi::BlendFactor::OneMinusSrcColor:      return MTLBlendFactorOneMinusSourceColor;
        case tide::rhi::BlendFactor::DstColor:              return MTLBlendFactorDestinationColor;
        case tide::rhi::BlendFactor::OneMinusDstColor:      return MTLBlendFactorOneMinusDestinationColor;
        case tide::rhi::BlendFactor::SrcAlpha:              return MTLBlendFactorSourceAlpha;
        case tide::rhi::BlendFactor::OneMinusSrcAlpha:      return MTLBlendFactorOneMinusSourceAlpha;
        case tide::rhi::BlendFactor::DstAlpha:              return MTLBlendFactorDestinationAlpha;
        case tide::rhi::BlendFactor::OneMinusDstAlpha:      return MTLBlendFactorOneMinusDestinationAlpha;
        case tide::rhi::BlendFactor::ConstantColor:         return MTLBlendFactorBlendColor;
        case tide::rhi::BlendFactor::OneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
    }
    return MTLBlendFactorOne;
}

MTLBlendOperation to_mtl_blend_op(tide::rhi::BlendOp op) noexcept {
    switch (op) {
        case tide::rhi::BlendOp::Add:             return MTLBlendOperationAdd;
        case tide::rhi::BlendOp::Subtract:        return MTLBlendOperationSubtract;
        case tide::rhi::BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case tide::rhi::BlendOp::Min:             return MTLBlendOperationMin;
        case tide::rhi::BlendOp::Max:             return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

MTLColorWriteMask to_mtl_write_mask(uint8_t mask) noexcept {
    MTLColorWriteMask out = MTLColorWriteMaskNone;
    if (mask & 0x1) out |= MTLColorWriteMaskRed;
    if (mask & 0x2) out |= MTLColorWriteMaskGreen;
    if (mask & 0x4) out |= MTLColorWriteMaskBlue;
    if (mask & 0x8) out |= MTLColorWriteMaskAlpha;
    return out;
}

MTLVertexFormat to_mtl_vertex_format(tide::rhi::Format f) noexcept {
    // Phase 1 vertex-input set — expand as samples need them. Mirrors the
    // pixel-format table but maps to MTLVertexFormat (the buffer-layout side).
    switch (f) {
        case tide::rhi::Format::R8_Unorm:         return MTLVertexFormatUCharNormalized;
        case tide::rhi::Format::R8_Uint:          return MTLVertexFormatUChar;
        case tide::rhi::Format::RG8_Unorm:        return MTLVertexFormatUChar2Normalized;
        case tide::rhi::Format::RGBA8_Unorm:      return MTLVertexFormatUChar4Normalized;
        case tide::rhi::Format::RGBA8_Unorm_sRGB: return MTLVertexFormatUChar4Normalized;
        case tide::rhi::Format::BGRA8_Unorm:      return MTLVertexFormatUChar4Normalized_BGRA;
        case tide::rhi::Format::R16_Float:        return MTLVertexFormatHalf;
        case tide::rhi::Format::RG16_Float:       return MTLVertexFormatHalf2;
        case tide::rhi::Format::RGBA16_Float:     return MTLVertexFormatHalf4;
        case tide::rhi::Format::R32_Uint:         return MTLVertexFormatUInt;
        case tide::rhi::Format::R32_Float:        return MTLVertexFormatFloat;
        case tide::rhi::Format::RG32_Float:       return MTLVertexFormatFloat2;
        case tide::rhi::Format::RGB32_Float:      return MTLVertexFormatFloat3;
        case tide::rhi::Format::RGBA32_Float:     return MTLVertexFormatFloat4;
        default:                                  return MTLVertexFormatInvalid;
    }
}

MTLVertexStepFunction to_mtl_step(tide::rhi::VertexInputRate r) noexcept {
    return r == tide::rhi::VertexInputRate::Vertex
               ? MTLVertexStepFunctionPerVertex
               : MTLVertexStepFunctionPerInstance;
}

} // namespace

// ─── Shader objects ─────────────────────────────────────────────────────────

tide::expected<tide::rhi::ShaderHandle, tide::rhi::RhiError>
MetalDevice::create_shader(const tide::rhi::ShaderDesc& desc) {
    if (desc.bytecode.empty() || !desc.entry_point) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        // dispatch_data with DEFAULT destructor copies the buffer internally —
        // caller's bytecode pointer does NOT need to outlive this call. See
        // DISPATCH_DATA_DESTRUCTOR_DEFAULT in dispatch/data.h.
        dispatch_data_t data = dispatch_data_create(
            desc.bytecode.data(),
            desc.bytecode.size(),
            dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        if (!data) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }

        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithData:data error:&err];
        if (!lib) {
            TIDE_LOG_ERROR("create_shader: newLibraryWithData failed: {}",
                           err ? err.localizedDescription.UTF8String
                               : "(no NSError)");
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        if (desc.debug_name) {
            lib.label = [NSString stringWithUTF8String:desc.debug_name];
        }

        NSString* entry = [NSString stringWithUTF8String:desc.entry_point];
        id<MTLFunction> fn = [lib newFunctionWithName:entry];
        if (!fn) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
        if (desc.debug_name) {
            fn.label = [NSString stringWithUTF8String:desc.debug_name];
        }

        std::lock_guard<std::mutex> lock(resource_mutex_);
        return shader_pool_.allocate(
            (__bridge_retained void*)lib,
            (__bridge_retained void*)fn,
            desc.stage);
    }
}

void MetalDevice::destroy_shader(tide::rhi::ShaderHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    shader_pool_.release(h);
}

// ─── Graphics pipeline ──────────────────────────────────────────────────────

tide::expected<tide::rhi::PipelineHandle, tide::rhi::RhiError>
MetalDevice::create_graphics_pipeline(const tide::rhi::GraphicsPipelineDesc& desc) {
    if (!desc.vertex_shader.valid()) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    if (desc.color_attachment_count > kMaxColorAttachments) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        // Resolve shader functions under the lock, then drop it before any GPU
        // work — newRenderPipelineStateWithDescriptor can take milliseconds the
        // first time, so we don't want to serialize unrelated resource creation.
        id<MTLFunction> vs_fn = nil;
        id<MTLFunction> fs_fn = nil;
        {
            std::lock_guard<std::mutex> lock(resource_mutex_);
            auto* vs = shader_pool_.get(desc.vertex_shader);
            if (!vs || !vs->mtl_function) {
                return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
            }
            vs_fn = (__bridge id<MTLFunction>)vs->mtl_function;
            if (desc.fragment_shader.valid()) {
                auto* fs = shader_pool_.get(desc.fragment_shader);
                if (!fs || !fs->mtl_function) {
                    return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
                }
                fs_fn = (__bridge id<MTLFunction>)fs->mtl_function;
            }
        }

        MTLRenderPipelineDescriptor* rpd = [[MTLRenderPipelineDescriptor alloc] init];
        rpd.vertexFunction   = vs_fn;
        rpd.fragmentFunction = fs_fn;
        rpd.rasterSampleCount = std::max(1u, desc.sample_count);
        rpd.inputPrimitiveTopology = to_mtl_topology_class(desc.topology);
        if (desc.debug_name) {
            rpd.label = [NSString stringWithUTF8String:desc.debug_name];
        }

        // Color attachments — formats must match the render pass attachment
        // formats at draw time. Phase 1 callers pass swapchain_format() through.
        for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
            MTLPixelFormat px = to_mtl_pixel_format(desc.color_formats[i]);
            if (px == MTLPixelFormatInvalid) {
                return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
            }
            const auto& blend = desc.color_blend[i];
            MTLRenderPipelineColorAttachmentDescriptor* ca = rpd.colorAttachments[i];
            ca.pixelFormat                 = px;
            ca.blendingEnabled             = blend.blend_enable;
            ca.sourceRGBBlendFactor        = to_mtl_blend_factor(blend.src_color);
            ca.destinationRGBBlendFactor   = to_mtl_blend_factor(blend.dst_color);
            ca.rgbBlendOperation           = to_mtl_blend_op(blend.color_op);
            ca.sourceAlphaBlendFactor      = to_mtl_blend_factor(blend.src_alpha);
            ca.destinationAlphaBlendFactor = to_mtl_blend_factor(blend.dst_alpha);
            ca.alphaBlendOperation         = to_mtl_blend_op(blend.alpha_op);
            ca.writeMask                   = to_mtl_write_mask(blend.write_mask);
        }

        if (desc.depth_format != tide::rhi::Format::Undefined) {
            MTLPixelFormat dpx = to_mtl_pixel_format(desc.depth_format);
            if (dpx == MTLPixelFormatInvalid) {
                return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
            }
            rpd.depthAttachmentPixelFormat = dpx;
            // Stencil format is decided implicitly by the depth format choice
            // for the combined-format cases; pure depth formats leave stencil
            // as MTLPixelFormatInvalid (Metal's default).
            if (desc.depth_format == tide::rhi::Format::D24_Unorm_S8_Uint ||
                desc.depth_format == tide::rhi::Format::D32_Float_S8_Uint) {
                rpd.stencilAttachmentPixelFormat = dpx;
            }
        }

        // Vertex input — only build a descriptor when bindings exist. An empty
        // VertexInputState lets shaders that source vertices via SV_VertexID
        // (e.g. fullscreen triangle) work without phony attribute setup.
        if (!desc.vertex_input.bindings.empty() ||
            !desc.vertex_input.attributes.empty()) {
            MTLVertexDescriptor* vdesc = [MTLVertexDescriptor vertexDescriptor];

            // Per Phase 1 DEFINE D14 / D16: HLSL shaders compiled with
            // -fvk-b-shift / -fvk-t-shift / -fvk-u-shift land at high MSL
            // buffer slots (cbuffer at +0, SRV at +16, etc). We reserve the
            // top-end of MTL buffer slots for descriptor-set bindings and place
            // vertex buffers at the bottom. Apple reserves nothing here, so
            // matching the SPIRV-Cross MSL emit (which uses slot 30 for vertex
            // attributes by default) requires offsetting if HLSL set/binding
            // collide. Phase 1 task 7's first textured-quad sample will
            // formalize this slot map; for now we use the Metal-native rule:
            // vertex bindings start at 0 and shaders compiled from triangle.vs
            // use SV_VertexID (no buffer needed) so this path stays untested
            // until task 7. Mapping is straightforward.
            for (const auto& b : desc.vertex_input.bindings) {
                if (b.binding >= 31) {
                    return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
                }
                vdesc.layouts[b.binding].stride       = b.stride;
                vdesc.layouts[b.binding].stepFunction = to_mtl_step(b.input_rate);
                vdesc.layouts[b.binding].stepRate     = 1;
            }
            for (const auto& a : desc.vertex_input.attributes) {
                MTLVertexFormat vf = to_mtl_vertex_format(a.format);
                if (vf == MTLVertexFormatInvalid) {
                    return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
                }
                vdesc.attributes[a.location].format      = vf;
                vdesc.attributes[a.location].offset      = a.offset;
                vdesc.attributes[a.location].bufferIndex = a.binding;
            }
            rpd.vertexDescriptor = vdesc;
        }

        // Compile PSO. Apple recommends async (newRenderPipelineStateWithDescriptor:
        // completionHandler:) for shipping titles; Phase 1 keeps it synchronous
        // for predictable boot — async pipeline cache is Phase 7's problem.
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        NSError* err = nil;
        id<MTLRenderPipelineState> pso =
            [device newRenderPipelineStateWithDescriptor:rpd error:&err];
        if (!pso) {
            TIDE_LOG_ERROR("create_graphics_pipeline: PSO compile failed: {}",
                           err ? err.localizedDescription.UTF8String
                               : "(no NSError)");
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }

        // Build the depth/stencil state. Even when depth_test_enable is false
        // we hand the encoder a default state at bind time so a previous pass's
        // setting doesn't leak through (Metal sticky-state surprise).
        MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
        if (desc.depth_stencil.depth_test_enable) {
            dsd.depthCompareFunction = to_mtl_compare(desc.depth_stencil.depth_compare);
        } else {
            dsd.depthCompareFunction = MTLCompareFunctionAlways;
        }
        dsd.depthWriteEnabled = desc.depth_stencil.depth_write_enable;
        if (desc.debug_name) {
            dsd.label = [NSString stringWithFormat:@"%s.ds",
                                                   desc.debug_name];
        }
        id<MTLDepthStencilState> dss = [device newDepthStencilStateWithDescriptor:dsd];
        if (!dss) {
            return tide::unexpected(tide::rhi::RhiError::BackendInternal);
        }

        std::lock_guard<std::mutex> lock(resource_mutex_);
        return pipeline_pool_.allocate(
            MetalPipeline::Kind::Graphics,
            (__bridge_retained void*)pso,
            (__bridge_retained void*)dss,
            static_cast<uint32_t>(to_mtl_cull(desc.rasterization.cull)),
            static_cast<uint32_t>(to_mtl_winding(desc.rasterization.front_face)),
            static_cast<uint32_t>(to_mtl_fill(desc.rasterization.polygon)),
            static_cast<uint32_t>(to_mtl_primitive(desc.topology)),
            desc.rasterization.depth_clamp_enable);
    }
}

// ─── Compute pipeline ───────────────────────────────────────────────────────

tide::expected<tide::rhi::PipelineHandle, tide::rhi::RhiError>
MetalDevice::create_compute_pipeline(const tide::rhi::ComputePipelineDesc& desc) {
    if (!desc.compute_shader.valid()) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    @autoreleasepool {
        id<MTLFunction> cs_fn = nil;
        {
            std::lock_guard<std::mutex> lock(resource_mutex_);
            auto* cs = shader_pool_.get(desc.compute_shader);
            if (!cs || !cs->mtl_function) {
                return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
            }
            cs_fn = (__bridge id<MTLFunction>)cs->mtl_function;
        }

        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;
        NSError* err = nil;
        id<MTLComputePipelineState> cpso =
            [device newComputePipelineStateWithFunction:cs_fn error:&err];
        if (!cpso) {
            TIDE_LOG_ERROR("create_compute_pipeline: PSO compile failed: {}",
                           err ? err.localizedDescription.UTF8String
                               : "(no NSError)");
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }

        std::lock_guard<std::mutex> lock(resource_mutex_);
        return pipeline_pool_.allocate(
            MetalPipeline::Kind::Compute,
            (__bridge_retained void*)cpso,
            /*ds=*/nullptr,
            /*cull=*/0u, /*winding=*/0u, /*fill=*/0u, /*primitive=*/0u,
            /*depth_clamp=*/false,
            std::max<uint32_t>(1, desc.threads_per_group[0]),
            std::max<uint32_t>(1, desc.threads_per_group[1]),
            std::max<uint32_t>(1, desc.threads_per_group[2]));
    }
}

void MetalDevice::destroy_pipeline(tide::rhi::PipelineHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    pipeline_pool_.release(h);
}

// ─── Sampler (Phase 1 task 7) ───────────────────────────────────────────────

namespace {

MTLSamplerMinMagFilter to_mtl_min_mag_filter(tide::rhi::FilterMode f) noexcept {
    return f == tide::rhi::FilterMode::Linear
               ? MTLSamplerMinMagFilterLinear
               : MTLSamplerMinMagFilterNearest;
}

MTLSamplerMipFilter to_mtl_mip_filter(tide::rhi::MipFilterMode m) noexcept {
    switch (m) {
        case tide::rhi::MipFilterMode::Nearest:      return MTLSamplerMipFilterNearest;
        case tide::rhi::MipFilterMode::Linear:       return MTLSamplerMipFilterLinear;
        case tide::rhi::MipFilterMode::NotMipmapped: return MTLSamplerMipFilterNotMipmapped;
    }
    return MTLSamplerMipFilterNotMipmapped;
}

MTLSamplerAddressMode to_mtl_address(tide::rhi::AddressMode a) noexcept {
    switch (a) {
        case tide::rhi::AddressMode::Repeat:        return MTLSamplerAddressModeRepeat;
        case tide::rhi::AddressMode::MirrorRepeat:  return MTLSamplerAddressModeMirrorRepeat;
        case tide::rhi::AddressMode::ClampToEdge:   return MTLSamplerAddressModeClampToEdge;
        case tide::rhi::AddressMode::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
    }
    return MTLSamplerAddressModeClampToEdge;
}

MTLCompareFunction to_mtl_compare(tide::rhi::CompareOp c) noexcept {
    switch (c) {
        case tide::rhi::CompareOp::Never:          return MTLCompareFunctionNever;
        case tide::rhi::CompareOp::Less:           return MTLCompareFunctionLess;
        case tide::rhi::CompareOp::Equal:          return MTLCompareFunctionEqual;
        case tide::rhi::CompareOp::LessOrEqual:    return MTLCompareFunctionLessEqual;
        case tide::rhi::CompareOp::Greater:        return MTLCompareFunctionGreater;
        case tide::rhi::CompareOp::NotEqual:       return MTLCompareFunctionNotEqual;
        case tide::rhi::CompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
        case tide::rhi::CompareOp::Always:         return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLSamplerBorderColor to_mtl_border(tide::rhi::BorderColor c) noexcept {
    switch (c) {
        case tide::rhi::BorderColor::TransparentBlack: return MTLSamplerBorderColorTransparentBlack;
        case tide::rhi::BorderColor::OpaqueBlack:      return MTLSamplerBorderColorOpaqueBlack;
        case tide::rhi::BorderColor::OpaqueWhite:      return MTLSamplerBorderColorOpaqueWhite;
    }
    return MTLSamplerBorderColorTransparentBlack;
}

} // namespace

tide::expected<tide::rhi::SamplerHandle, tide::rhi::RhiError>
MetalDevice::create_sampler(const tide::rhi::SamplerDesc& desc) {
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mtl_device_;

        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter      = to_mtl_min_mag_filter(desc.min_filter);
        sd.magFilter      = to_mtl_min_mag_filter(desc.mag_filter);
        sd.mipFilter      = to_mtl_mip_filter(desc.mip_filter);
        sd.sAddressMode   = to_mtl_address(desc.address_u);
        sd.tAddressMode   = to_mtl_address(desc.address_v);
        sd.rAddressMode   = to_mtl_address(desc.address_w);
        sd.lodMinClamp    = desc.min_lod;
        sd.lodMaxClamp    = desc.max_lod;
        sd.maxAnisotropy  = std::max<uint32_t>(1, desc.max_anisotropy);
        sd.compareFunction = desc.compare_enable
                                ? to_mtl_compare(desc.compare_op)
                                : MTLCompareFunctionAlways;
        sd.borderColor    = to_mtl_border(desc.border_color);
        if (desc.debug_name) {
            sd.label = [NSString stringWithUTF8String:desc.debug_name];
        }

        id<MTLSamplerState> ss = [device newSamplerStateWithDescriptor:sd];
        if (!ss) {
            return tide::unexpected(tide::rhi::RhiError::OutOfMemory);
        }

        std::lock_guard<std::mutex> lock(resource_mutex_);
        return sampler_pool_.allocate((__bridge_retained void*)ss);
    }
}

void MetalDevice::destroy_sampler(tide::rhi::SamplerHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    sampler_pool_.release(h);
}

// ─── Descriptor set layout / set (Phase 1 task 7) ───────────────────────────
// Metal has no MTLDescriptorSet equivalent. We store the layout binding info
// and the (sparse) writes; bind_descriptor_set in the command buffer walks
// the writes at record time and dispatches to the encoder argument tables.

tide::expected<tide::rhi::DescriptorSetLayoutHandle, tide::rhi::RhiError>
MetalDevice::create_descriptor_set_layout(
    const tide::rhi::DescriptorSetLayoutDesc& desc) {
    MetalDescriptorSetLayout layout;
    layout.bindings.assign(desc.bindings.begin(), desc.bindings.end());

    std::lock_guard<std::mutex> lock(resource_mutex_);
    return descriptor_layout_pool_.allocate(std::move(layout));
}

void MetalDevice::destroy_descriptor_set_layout(
    tide::rhi::DescriptorSetLayoutHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    descriptor_layout_pool_.release(h);
}

tide::expected<tide::rhi::DescriptorSetHandle, tide::rhi::RhiError>
MetalDevice::create_descriptor_set(const tide::rhi::DescriptorSetDesc& desc) {
    if (!desc.layout.valid()) {
        return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
    }
    {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        if (!descriptor_layout_pool_.get(desc.layout)) {
            return tide::unexpected(tide::rhi::RhiError::InvalidDescriptor);
        }
    }
    MetalDescriptorSet set;
    set.layout = desc.layout;
    set.writes.assign(desc.initial_writes.begin(), desc.initial_writes.end());

    std::lock_guard<std::mutex> lock(resource_mutex_);
    return descriptor_set_pool_.allocate(std::move(set));
}

void MetalDevice::destroy_descriptor_set(tide::rhi::DescriptorSetHandle h) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    descriptor_set_pool_.release(h);
}

void MetalDevice::update_descriptor_set(
    tide::rhi::DescriptorSetHandle h,
    std::span<const tide::rhi::DescriptorWrite> writes) {
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto* set = descriptor_set_pool_.get(h);
    if (!set) return;
    // Replace-by-slot semantics: a new write at an existing slot supersedes
    // the previous write (matches Vulkan vkUpdateDescriptorSets behaviour).
    for (const auto& w : writes) {
        bool replaced = false;
        for (auto& cur : set->writes) {
            if (cur.slot == w.slot) { cur = w; replaced = true; break; }
        }
        if (!replaced) set->writes.push_back(w);
    }
}

// ─── Fence: still stubbed (Phase 3+) ────────────────────────────────────────

tide::expected<tide::rhi::FenceHandle, tide::rhi::RhiError>
MetalDevice::create_fence() {
    return tide::unexpected(tide::rhi::RhiError::UnsupportedFeature);
}

} // namespace tide::rhi::metal
