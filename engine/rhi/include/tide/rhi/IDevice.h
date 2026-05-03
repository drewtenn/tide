#pragma once

// IDevice / ICommandBuffer / IFence — the only virtual interfaces in tide's RHI.
// Per ADR-003, resources are opaque integer handles (no virtual on resources). Per
// ADR-007, all IDevice resource-creation methods must be callable from any thread;
// the backend serializes via an internal mutex.
//
// The vtable for these classes is anchored in src/IDevice.cpp; see Herb Sutter
// GotW #31 for why.

#include "tide/core/Expected.h"
#include "tide/rhi/Descriptors.h"
#include "tide/rhi/Handles.h"

#include <cstddef>
#include <cstdint>

namespace tide::rhi {

// ─── Errors ─────────────────────────────────────────────────────────────────

enum class RhiError : uint32_t {
    OutOfMemory,
    InvalidDescriptor,
    DeviceLost,
    SwapchainOutOfDate,
    BackendInternal,
    UnsupportedFeature,
};

// ─── Forward declarations ───────────────────────────────────────────────────

class ICommandBuffer;
class IFence;

// ─── ICommandBuffer ─────────────────────────────────────────────────────────
// Recording-stage interface. Per the DEFINE document, recording calls are void
// and accumulate errors internally; the error flag is checked at IDevice::submit().
//
// Command buffers are externally synchronized (ADR-007 invariant 5): a single
// thread records into a given ICommandBuffer; ownership does not cross thread
// boundaries silently. In Phase 1 the main thread does all recording and submit.

class ICommandBuffer {
public:
    virtual ~ICommandBuffer();

    // Resource state transitions. On Metal these are no-ops (automatic hazard
    // tracking). On Vulkan/DX12 they emit pipeline barriers.
    //
    // Texture overload accepts a subresource range with sentinel ~0u defaults
    // meaning "all subresources" (Vulkan-required, see DEFINE D3).
    virtual void transition(BufferHandle buf,
                            ResourceState old_state,
                            ResourceState new_state) = 0;

    virtual void transition(TextureHandle tex,
                            ResourceState old_state,
                            ResourceState new_state,
                            uint32_t base_mip = 0,
                            uint32_t mip_count = ~0u,
                            uint32_t base_layer = 0,
                            uint32_t layer_count = ~0u) = 0;

    // Render pass — dynamic-rendering style. RenderPassDesc is POD.
    virtual void begin_render_pass(const RenderPassDesc& desc) = 0;
    virtual void end_render_pass() = 0;

    // Pipeline + binding
    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    virtual void bind_descriptor_set(uint32_t set_index,
                                     DescriptorSetHandle set) = 0;
    virtual void bind_vertex_buffer(uint32_t slot,
                                    BufferHandle buffer,
                                    uint64_t offset = 0) = 0;
    virtual void bind_index_buffer(BufferHandle buffer,
                                   uint64_t offset = 0,
                                   IndexType type = IndexType::Uint32) = 0;

    // Dynamic state
    virtual void set_viewport(const Viewport& vp) = 0;
    virtual void set_scissor(const Rect2D& rect) = 0;
    virtual void set_push_constants(uint32_t offset,
                                    uint32_t size,
                                    const void* data) = 0;

    // Draws + dispatch
    virtual void draw(uint32_t vertex_count,
                      uint32_t instance_count = 1,
                      uint32_t first_vertex = 0,
                      uint32_t first_instance = 0) = 0;
    virtual void draw_indexed(uint32_t index_count,
                              uint32_t instance_count = 1,
                              uint32_t first_index = 0,
                              int32_t  vertex_offset = 0,
                              uint32_t first_instance = 0) = 0;
    virtual void dispatch(uint32_t group_x,
                          uint32_t group_y,
                          uint32_t group_z) = 0;

    // Copy
    virtual void copy_buffer(BufferHandle src,
                             BufferHandle dst,
                             uint64_t src_offset,
                             uint64_t dst_offset,
                             uint64_t size) = 0;

    // Debug markers (no-op in release)
    virtual void push_debug_marker(const char* name) = 0;
    virtual void pop_debug_marker() = 0;
};

// ─── IFence ─────────────────────────────────────────────────────────────────

class IFence {
public:
    virtual ~IFence();

    virtual void wait(uint64_t timeout_ns = ~0ull) = 0;
    [[nodiscard]] virtual bool is_signaled() const = 0;
};

// ─── IDevice ────────────────────────────────────────────────────────────────

class IDevice {
public:
    virtual ~IDevice();

    // ─── Resource creation (callable from any thread; ADR-007 invariant 1) ──
    [[nodiscard]] virtual tide::expected<BufferHandle, RhiError>
        create_buffer(const BufferDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<TextureHandle, RhiError>
        create_texture(const TextureDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<TextureViewHandle, RhiError>
        create_texture_view(const TextureViewDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<SamplerHandle, RhiError>
        create_sampler(const SamplerDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<ShaderHandle, RhiError>
        create_shader(const ShaderDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<PipelineHandle, RhiError>
        create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<PipelineHandle, RhiError>
        create_compute_pipeline(const ComputePipelineDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<DescriptorSetLayoutHandle, RhiError>
        create_descriptor_set_layout(const DescriptorSetLayoutDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<DescriptorSetHandle, RhiError>
        create_descriptor_set(const DescriptorSetDesc& desc) = 0;

    [[nodiscard]] virtual tide::expected<FenceHandle, RhiError>
        create_fence() = 0;

    // Symmetric destruction. Idempotent on null handles. Backend ensures the
    // resource is not in flight before actually freeing.
    virtual void destroy_buffer(BufferHandle h) = 0;
    virtual void destroy_texture(TextureHandle h) = 0;
    virtual void destroy_texture_view(TextureViewHandle h) = 0;
    virtual void destroy_sampler(SamplerHandle h) = 0;
    virtual void destroy_shader(ShaderHandle h) = 0;
    virtual void destroy_pipeline(PipelineHandle h) = 0;
    virtual void destroy_descriptor_set_layout(DescriptorSetLayoutHandle h) = 0;
    virtual void destroy_descriptor_set(DescriptorSetHandle h) = 0;
    virtual void destroy_fence(FenceHandle h) = 0;

    // Update an existing descriptor set. Cheaper than recreate.
    virtual void update_descriptor_set(
        DescriptorSetHandle set,
        std::span<const DescriptorWrite> writes) = 0;

    // ─── Frame lifecycle (main thread only; ADR-007) ────────────────────────
    [[nodiscard]] virtual tide::expected<void, RhiError> begin_frame() = 0;
    [[nodiscard]] virtual tide::expected<void, RhiError> end_frame() = 0;

    // Acquire the swapchain backbuffer for this frame. Returned handle is
    // valid only until the matching end_frame(). Returns SwapchainOutOfDate
    // when a resize is pending; caller should resize and skip the frame.
    [[nodiscard]] virtual tide::expected<TextureHandle, RhiError>
        acquire_swapchain_texture() = 0;

    [[nodiscard]] virtual Format swapchain_format() const noexcept = 0;

    [[nodiscard]] virtual tide::expected<void, RhiError>
        resize_swapchain(uint32_t width, uint32_t height) = 0;

    // ─── Command buffer pool ────────────────────────────────────────────────
    // Acquire a fresh command buffer from the per-frame pool. Lifetime is
    // bounded by end_frame(); do not retain across frames.
    [[nodiscard]] virtual ICommandBuffer* acquire_command_buffer() = 0;

    // Submit a recorded command buffer. Returns void since submission errors
    // surface as DeviceLost on the next begin_frame() (matches DX12's
    // GetDeviceRemovedReason pattern; see DEFINE D7).
    virtual void submit(ICommandBuffer* cmd) = 0;

    // ─── Synchronous upload helpers (Phase 1) ───────────────────────────────
    // Phase 3 will add async variants returning a fence.
    [[nodiscard]] virtual tide::expected<void, RhiError>
        upload_buffer(BufferHandle dst,
                      const void* src,
                      size_t bytes,
                      size_t offset = 0) = 0;

    [[nodiscard]] virtual tide::expected<void, RhiError>
        upload_texture(TextureHandle dst,
                       const void* src,
                       size_t bytes,
                       uint32_t mip = 0,
                       uint32_t layer = 0) = 0;

    // Synchronous mirror of upload_buffer. Reads `src`'s contents back into
    // the caller's buffer. Used by Phase 1 task 11's compute round-trip
    // (UAV-buffer readback). Phase 3+ adds async variants.
    [[nodiscard]] virtual tide::expected<void, RhiError>
        download_buffer(BufferHandle src,
                        void* dst,
                        size_t bytes,
                        size_t offset = 0) = 0;

    // Synchronous mirror of upload_texture. Reads the contents of `src` (the
    // given mip/layer) back into the caller's buffer. Used by Phase 1 task 10
    // for offscreen-readback golden-hash CI; future async variants land in
    // Phase 3 alongside the asset pipeline. The caller's buffer must be
    // sized to the natural row stride of the format (no padding).
    [[nodiscard]] virtual tide::expected<void, RhiError>
        download_texture(TextureHandle src,
                         void* dst,
                         size_t bytes,
                         uint32_t mip = 0,
                         uint32_t layer = 0) = 0;

    // ─── Capabilities ───────────────────────────────────────────────────────
    [[nodiscard]] virtual const DeviceCapabilities& capabilities() const noexcept = 0;
};

} // namespace tide::rhi
