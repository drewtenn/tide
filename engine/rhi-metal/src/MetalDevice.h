#pragma once

// Internal Metal IDevice implementation header. Lives under src/, not include/,
// because it depends on Apple-only types via opaque pointers. The .mm
// implementation is the only place where Metal types are visible. This header
// is OBJCXX-okay (only included from .mm files in engine/rhi-metal/src/).

#include "tide/core/Handle.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace tide::rhi::metal {

class MetalCommandBuffer;
class MetalSwapchain;

// ─── Resource records (Phase 1 task 4) ──────────────────────────────────────
// Each record holds a CFBridgingRetain'd id<MTL...> as void*. Move semantics
// transfer the retain; destructor balances. Records live in HandlePools owned
// by MetalDevice. ARC under -fobjc-arc handles the autorelease side cleanly.

struct MetalBuffer {
    void*                 mtl_buffer{nullptr};   // id<MTLBuffer>, CF-retained
    uint64_t              size_bytes{0};
    tide::rhi::MemoryType memory{tide::rhi::MemoryType::DeviceLocal};
    tide::rhi::BufferUsage usage{tide::rhi::BufferUsage::None};

    MetalBuffer() = default;
    MetalBuffer(void* b, uint64_t s, tide::rhi::MemoryType m, tide::rhi::BufferUsage u) noexcept
        : mtl_buffer(b), size_bytes(s), memory(m), usage(u) {}
    MetalBuffer(MetalBuffer&& o) noexcept
        : mtl_buffer(o.mtl_buffer), size_bytes(o.size_bytes), memory(o.memory), usage(o.usage) {
        o.mtl_buffer = nullptr;
    }
    MetalBuffer& operator=(MetalBuffer&& o) noexcept {
        if (this != &o) {
            if (mtl_buffer) CFRelease(static_cast<CFTypeRef>(mtl_buffer));
            mtl_buffer = o.mtl_buffer; size_bytes = o.size_bytes;
            memory = o.memory; usage = o.usage;
            o.mtl_buffer = nullptr;
        }
        return *this;
    }
    MetalBuffer(const MetalBuffer&) = delete;
    MetalBuffer& operator=(const MetalBuffer&) = delete;
    ~MetalBuffer() {
        if (mtl_buffer) CFRelease(static_cast<CFTypeRef>(mtl_buffer));
    }
};

struct MetalTexture {
    void*                  mtl_texture{nullptr};   // id<MTLTexture>, CF-retained when owned
    tide::rhi::TextureDesc desc{};
    // Slot 0 of the texture pool is reserved for the swapchain drawable; in
    // that case mtl_texture is rebound each frame and is owned by the drawable
    // (no CFRelease at our destruction). owns_texture distinguishes the cases.
    bool                   owns_texture{true};

    MetalTexture() = default;
    MetalTexture(void* t, const tide::rhi::TextureDesc& d, bool owns) noexcept
        : mtl_texture(t), desc(d), owns_texture(owns) {}
    MetalTexture(MetalTexture&& o) noexcept
        : mtl_texture(o.mtl_texture), desc(o.desc), owns_texture(o.owns_texture) {
        o.mtl_texture = nullptr;
    }
    MetalTexture& operator=(MetalTexture&& o) noexcept {
        if (this != &o) {
            if (owns_texture && mtl_texture) CFRelease(static_cast<CFTypeRef>(mtl_texture));
            mtl_texture = o.mtl_texture; desc = o.desc; owns_texture = o.owns_texture;
            o.mtl_texture = nullptr;
        }
        return *this;
    }
    MetalTexture(const MetalTexture&) = delete;
    MetalTexture& operator=(const MetalTexture&) = delete;
    ~MetalTexture() {
        if (owns_texture && mtl_texture) CFRelease(static_cast<CFTypeRef>(mtl_texture));
    }

    // Bind/unbind the per-frame swapchain drawable texture. Slot-0 only.
    void rebind_swapchain(void* drawable_texture) noexcept {
        // owns_texture must be false for the swapchain slot; assert via no-op.
        mtl_texture = drawable_texture;
    }
};

struct MetalTextureView {
    void*                       mtl_texture_view{nullptr}; // id<MTLTexture>, CF-retained
    tide::rhi::TextureHandle    source{};
    tide::rhi::TextureViewDesc  desc{};

    MetalTextureView() = default;
    MetalTextureView(void* v, tide::rhi::TextureHandle src,
                     const tide::rhi::TextureViewDesc& d) noexcept
        : mtl_texture_view(v), source(src), desc(d) {}
    MetalTextureView(MetalTextureView&& o) noexcept
        : mtl_texture_view(o.mtl_texture_view), source(o.source), desc(o.desc) {
        o.mtl_texture_view = nullptr;
    }
    MetalTextureView& operator=(MetalTextureView&& o) noexcept {
        if (this != &o) {
            if (mtl_texture_view) CFRelease(static_cast<CFTypeRef>(mtl_texture_view));
            mtl_texture_view = o.mtl_texture_view; source = o.source; desc = o.desc;
            o.mtl_texture_view = nullptr;
        }
        return *this;
    }
    MetalTextureView(const MetalTextureView&) = delete;
    MetalTextureView& operator=(const MetalTextureView&) = delete;
    ~MetalTextureView() {
        if (mtl_texture_view) CFRelease(static_cast<CFTypeRef>(mtl_texture_view));
    }
};

// ─── Shader / Pipeline records (Phase 1 task 5) ─────────────────────────────
// Shaders own a +1 retain on both the MTLLibrary they were loaded from and the
// MTLFunction extracted by entry-point name. The library is kept alive because
// MTLFunction does not retain its parent (Apple's metal-cpp samples confirm),
// and a future create_shader call may want to reuse the library — though the
// Phase 1 IRhi loads one shader per metallib, so cache reuse comes in P3+.
struct MetalShader {
    void*                   mtl_library{nullptr};   // id<MTLLibrary>, CF-retained
    void*                   mtl_function{nullptr};  // id<MTLFunction>, CF-retained
    tide::rhi::ShaderStage  stage{tide::rhi::ShaderStage::None};

    MetalShader() = default;
    MetalShader(void* lib, void* fn, tide::rhi::ShaderStage s) noexcept
        : mtl_library(lib), mtl_function(fn), stage(s) {}
    MetalShader(MetalShader&& o) noexcept
        : mtl_library(o.mtl_library), mtl_function(o.mtl_function), stage(o.stage) {
        o.mtl_library = nullptr; o.mtl_function = nullptr;
    }
    MetalShader& operator=(MetalShader&& o) noexcept {
        if (this != &o) {
            if (mtl_function) CFRelease(static_cast<CFTypeRef>(mtl_function));
            if (mtl_library)  CFRelease(static_cast<CFTypeRef>(mtl_library));
            mtl_library = o.mtl_library; mtl_function = o.mtl_function; stage = o.stage;
            o.mtl_library = nullptr; o.mtl_function = nullptr;
        }
        return *this;
    }
    MetalShader(const MetalShader&) = delete;
    MetalShader& operator=(const MetalShader&) = delete;
    ~MetalShader() {
        if (mtl_function) CFRelease(static_cast<CFTypeRef>(mtl_function));
        if (mtl_library)  CFRelease(static_cast<CFTypeRef>(mtl_library));
    }
};

// MetalPipeline is the unified record for both graphics and compute PSOs. They
// share the PipelineHandle namespace at the IDevice surface (single tag), so a
// destroy_pipeline call resolves to one slot regardless of kind. Encoder dynamic
// state (cull, fill, winding, primitive type, depth clamp) is *not* part of the
// MTLRenderPipelineState — it must be applied on the encoder at bind time, so
// it's cached here in raw Metal-enum values to avoid re-translating each frame.
struct MetalPipeline {
    enum class Kind : uint8_t { Graphics, Compute };

    Kind     kind{Kind::Graphics};
    void*    mtl_pipeline_state{nullptr};   // id<MTLRenderPipelineState> or id<MTLComputePipelineState>
    void*    mtl_depth_stencil{nullptr};    // id<MTLDepthStencilState>, graphics only

    uint32_t cull_mode{0};        // MTLCullMode
    uint32_t front_face{0};       // MTLWinding
    uint32_t triangle_fill{0};    // MTLTriangleFillMode
    uint32_t primitive_type{0};   // MTLPrimitiveType (consumed by draw/draw_indexed)
    bool     depth_clamp{false};

    // Compute-only: threads-per-threadgroup, copied from the
    // ComputePipelineDesc so dispatchThreadgroups can supply it.
    uint32_t compute_tx{1}, compute_ty{1}, compute_tz{1};

    MetalPipeline() = default;
    MetalPipeline(Kind k, void* pso, void* ds,
                  uint32_t cull, uint32_t face, uint32_t fill, uint32_t prim,
                  bool clamp,
                  uint32_t tx = 1, uint32_t ty = 1, uint32_t tz = 1) noexcept
        : kind(k), mtl_pipeline_state(pso), mtl_depth_stencil(ds),
          cull_mode(cull), front_face(face), triangle_fill(fill),
          primitive_type(prim), depth_clamp(clamp),
          compute_tx(tx), compute_ty(ty), compute_tz(tz) {}
    MetalPipeline(MetalPipeline&& o) noexcept
        : kind(o.kind), mtl_pipeline_state(o.mtl_pipeline_state),
          mtl_depth_stencil(o.mtl_depth_stencil),
          cull_mode(o.cull_mode), front_face(o.front_face),
          triangle_fill(o.triangle_fill), primitive_type(o.primitive_type),
          depth_clamp(o.depth_clamp),
          compute_tx(o.compute_tx), compute_ty(o.compute_ty), compute_tz(o.compute_tz) {
        o.mtl_pipeline_state = nullptr;
        o.mtl_depth_stencil  = nullptr;
    }
    MetalPipeline& operator=(MetalPipeline&& o) noexcept {
        if (this != &o) {
            if (mtl_depth_stencil)   CFRelease(static_cast<CFTypeRef>(mtl_depth_stencil));
            if (mtl_pipeline_state)  CFRelease(static_cast<CFTypeRef>(mtl_pipeline_state));
            kind = o.kind;
            mtl_pipeline_state = o.mtl_pipeline_state;
            mtl_depth_stencil  = o.mtl_depth_stencil;
            cull_mode = o.cull_mode; front_face = o.front_face;
            triangle_fill = o.triangle_fill; primitive_type = o.primitive_type;
            depth_clamp = o.depth_clamp;
            compute_tx = o.compute_tx; compute_ty = o.compute_ty; compute_tz = o.compute_tz;
            o.mtl_pipeline_state = nullptr;
            o.mtl_depth_stencil  = nullptr;
        }
        return *this;
    }
    MetalPipeline(const MetalPipeline&) = delete;
    MetalPipeline& operator=(const MetalPipeline&) = delete;
    ~MetalPipeline() {
        if (mtl_depth_stencil)   CFRelease(static_cast<CFTypeRef>(mtl_depth_stencil));
        if (mtl_pipeline_state)  CFRelease(static_cast<CFTypeRef>(mtl_pipeline_state));
    }
};

// Slot 0 of the texture pool is reserved for the swapchain attachment so the
// sentinel handle returned by acquire_swapchain_texture() doesn't collide with
// real allocations (code-review concern #10). `kSwapchainSlot` = 0.
inline constexpr tide::Handle<tide::rhi::TextureTag>::IndexType kSwapchainSlot = 0;

// ─── Sampler / DescriptorSet records (Phase 1 task 7) ───────────────────────
// Sampler records hold a +1 retain on id<MTLSamplerState>. Descriptor sets are
// pure POD on Metal — there is no native MTLDescriptorSet equivalent. They store
// the layout handle plus the (sparse) writes; bind_descriptor_set walks the
// writes at record time and dispatches to the encoder. This matches what
// VK_KHR_push_descriptor would do on Vulkan, keeping the surface portable.

struct MetalSampler {
    void* mtl_sampler{nullptr};   // id<MTLSamplerState>, CF-retained

    MetalSampler() = default;
    explicit MetalSampler(void* s) noexcept : mtl_sampler(s) {}
    MetalSampler(MetalSampler&& o) noexcept : mtl_sampler(o.mtl_sampler) {
        o.mtl_sampler = nullptr;
    }
    MetalSampler& operator=(MetalSampler&& o) noexcept {
        if (this != &o) {
            if (mtl_sampler) CFRelease(static_cast<CFTypeRef>(mtl_sampler));
            mtl_sampler = o.mtl_sampler; o.mtl_sampler = nullptr;
        }
        return *this;
    }
    MetalSampler(const MetalSampler&) = delete;
    MetalSampler& operator=(const MetalSampler&) = delete;
    ~MetalSampler() {
        if (mtl_sampler) CFRelease(static_cast<CFTypeRef>(mtl_sampler));
    }
};

struct MetalDescriptorSetLayout {
    std::vector<tide::rhi::DescriptorBindingDesc> bindings;
};

struct MetalDescriptorSet {
    tide::rhi::DescriptorSetLayoutHandle layout{};
    std::vector<tide::rhi::DescriptorWrite> writes;
};

class MetalDevice final : public tide::rhi::IDevice {
public:
    // Constructed via tide::rhi::metal::create_device(); see MetalDevice.mm.
    // Takes ownership of the (already CFBridgingRetain'd) device + queue
    // pointers; releases them in the destructor.
    explicit MetalDevice(void* mtl_device,
                         void* mtl_queue,
                         std::unique_ptr<MetalSwapchain> swapchain,
                         tide::rhi::DeviceCapabilities caps);
    ~MetalDevice() override;

    MetalDevice(const MetalDevice&)            = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

    // ─── Resource creation (Phase 1 task 2: stubs returning UnsupportedFeature) ─
    tide::expected<tide::rhi::BufferHandle, tide::rhi::RhiError>
        create_buffer(const tide::rhi::BufferDesc&) override;
    tide::expected<tide::rhi::TextureHandle, tide::rhi::RhiError>
        create_texture(const tide::rhi::TextureDesc&) override;
    tide::expected<tide::rhi::TextureViewHandle, tide::rhi::RhiError>
        create_texture_view(const tide::rhi::TextureViewDesc&) override;
    tide::expected<tide::rhi::SamplerHandle, tide::rhi::RhiError>
        create_sampler(const tide::rhi::SamplerDesc&) override;
    tide::expected<tide::rhi::ShaderHandle, tide::rhi::RhiError>
        create_shader(const tide::rhi::ShaderDesc&) override;
    tide::expected<tide::rhi::PipelineHandle, tide::rhi::RhiError>
        create_graphics_pipeline(const tide::rhi::GraphicsPipelineDesc&) override;
    tide::expected<tide::rhi::PipelineHandle, tide::rhi::RhiError>
        create_compute_pipeline(const tide::rhi::ComputePipelineDesc&) override;
    tide::expected<tide::rhi::DescriptorSetLayoutHandle, tide::rhi::RhiError>
        create_descriptor_set_layout(const tide::rhi::DescriptorSetLayoutDesc&) override;
    tide::expected<tide::rhi::DescriptorSetHandle, tide::rhi::RhiError>
        create_descriptor_set(const tide::rhi::DescriptorSetDesc&) override;
    tide::expected<tide::rhi::FenceHandle, tide::rhi::RhiError> create_fence() override;

    void destroy_buffer(tide::rhi::BufferHandle) override;
    void destroy_texture(tide::rhi::TextureHandle) override;
    void destroy_texture_view(tide::rhi::TextureViewHandle) override;
    void destroy_sampler(tide::rhi::SamplerHandle) override;
    void destroy_shader(tide::rhi::ShaderHandle) override;
    void destroy_pipeline(tide::rhi::PipelineHandle) override;
    void destroy_descriptor_set_layout(tide::rhi::DescriptorSetLayoutHandle) override;
    void destroy_descriptor_set(tide::rhi::DescriptorSetHandle) override;
    void destroy_fence(tide::rhi::FenceHandle) override                         {}

    void update_descriptor_set(
        tide::rhi::DescriptorSetHandle set,
        std::span<const tide::rhi::DescriptorWrite> writes) override;

    // ─── Frame lifecycle ────────────────────────────────────────────────────
    tide::expected<void, tide::rhi::RhiError> begin_frame() override;
    tide::expected<void, tide::rhi::RhiError> end_frame() override;

    tide::expected<tide::rhi::TextureHandle, tide::rhi::RhiError>
        acquire_swapchain_texture() override;

    [[nodiscard]] tide::rhi::Format swapchain_format() const noexcept override;

    tide::expected<void, tide::rhi::RhiError>
        resize_swapchain(uint32_t width, uint32_t height) override;

    // ─── Command buffer pool ────────────────────────────────────────────────
    tide::rhi::ICommandBuffer* acquire_command_buffer() override;
    void                       submit(tide::rhi::ICommandBuffer* cmd) override;

    // ─── Upload helpers (stubs in Phase 1 task 2) ───────────────────────────
    tide::expected<void, tide::rhi::RhiError> upload_buffer(
        tide::rhi::BufferHandle, const void*, size_t, size_t) override;
    tide::expected<void, tide::rhi::RhiError> upload_texture(
        tide::rhi::TextureHandle, const void*, size_t, uint32_t, uint32_t) override;
    tide::expected<void, tide::rhi::RhiError> download_texture(
        tide::rhi::TextureHandle, void*, size_t, uint32_t, uint32_t) override;
    tide::expected<void, tide::rhi::RhiError> download_buffer(
        tide::rhi::BufferHandle, void*, size_t, size_t) override;

    // ─── Capabilities ───────────────────────────────────────────────────────
    [[nodiscard]] const tide::rhi::DeviceCapabilities& capabilities() const noexcept override {
        return caps_;
    }

    // ─── Internal accessors used by MetalCommandBuffer ──────────────────────
    [[nodiscard]] void* mtl_device() const noexcept;          // id<MTLDevice>
    [[nodiscard]] void* mtl_command_queue() const noexcept;   // id<MTLCommandQueue>
    [[nodiscard]] MetalSwapchain& swapchain() noexcept { return *swapchain_; }

    // Resolves the AttachmentTarget set on a ColorAttachmentDesc into an
    // id<MTLTexture>. Must be called inside @autoreleasepool.
    [[nodiscard]] void* resolve_attachment(
        const tide::rhi::AttachmentTarget& target) noexcept;

    // Pipeline lookup for command-buffer recording. Returns nullptr on stale
    // handles. The mutex serializes the lookup itself; the returned pointer
    // is dereferenced *outside* the mutex by the caller. Callers must observe
    // ADR-007 invariant 5 (recording is externally synchronized) AND must not
    // call destroy_pipeline while another thread is recording with the same
    // handle. Phase 3+ deferred-deletion will enforce this; Phase 1 trusts it.
    [[nodiscard]] MetalPipeline* pipeline(tide::rhi::PipelineHandle h) noexcept;

    // Lookups for command-buffer binding. Same threading rules as pipeline().
    [[nodiscard]] MetalBuffer*            buffer(tide::rhi::BufferHandle h) noexcept;
    [[nodiscard]] MetalTexture*           texture(tide::rhi::TextureHandle h) noexcept;
    [[nodiscard]] MetalTextureView*       texture_view(tide::rhi::TextureViewHandle h) noexcept;
    [[nodiscard]] MetalSampler*           sampler(tide::rhi::SamplerHandle h) noexcept;
    [[nodiscard]] MetalDescriptorSet*     descriptor_set(tide::rhi::DescriptorSetHandle h) noexcept;
    [[nodiscard]] MetalDescriptorSetLayout* descriptor_set_layout(tide::rhi::DescriptorSetLayoutHandle h) noexcept;

private:
    void*                            mtl_device_{nullptr};   // id<MTLDevice>, owned (CF-retained)
    void*                            mtl_queue_{nullptr};    // id<MTLCommandQueue>, owned (CF-retained)
    std::unique_ptr<MetalSwapchain>  swapchain_;
    tide::rhi::DeviceCapabilities    caps_;

    // Resource pools. ADR-007 invariant 1 (callable from any thread) is honored
    // via resource_mutex_ in the create_*/destroy_* methods.
    tide::HandlePool<MetalBuffer,              tide::rhi::BufferTag>              buffer_pool_;
    tide::HandlePool<MetalTexture,             tide::rhi::TextureTag>             texture_pool_;
    tide::HandlePool<MetalTextureView,         tide::rhi::TextureViewTag>         texture_view_pool_;
    tide::HandlePool<MetalShader,              tide::rhi::ShaderTag>              shader_pool_;
    tide::HandlePool<MetalPipeline,            tide::rhi::PipelineTag>            pipeline_pool_;
    tide::HandlePool<MetalSampler,             tide::rhi::SamplerTag>             sampler_pool_;
    tide::HandlePool<MetalDescriptorSetLayout, tide::rhi::DescriptorSetLayoutTag> descriptor_layout_pool_;
    tide::HandlePool<MetalDescriptorSet,       tide::rhi::DescriptorSetTag>       descriptor_set_pool_;

    // Per-frame command buffer pool. One in-flight CB at a time in Phase 1
    // task 2 (no parallel recording yet); the pool exists so the per-frame CB
    // outlives the call to acquire/submit.
    std::unique_ptr<MetalCommandBuffer> current_cmd_;

    // Captured at ctor time from the texture-pool slot-0 allocation. Used
    // verbatim by acquire_swapchain_texture() so the returned handle's
    // generation matches the pool's view of slot 0 — replaces the previous
    // hardcoded `{kSwapchainSlot, 1}` literal which would have desynced if
    // slot 0 were ever release()d and re-allocated.
    tide::rhi::TextureHandle swapchain_handle_{};

    // ADR-007: resource creation must be callable from any thread. Phase 1
    // task 2 doesn't yet exercise resource creation, but the mutex is in place
    // so future tasks (4+) inherit the invariant.
    std::mutex resource_mutex_;
};

} // namespace tide::rhi::metal
