#pragma once

// MetalCommandBuffer — wraps id<MTLCommandBuffer> + id<MTLRenderCommandEncoder>.
// Per locked DEFINE D2: transition() is a no-op (Metal automatic hazard tracking).

#include "tide/rhi/IDevice.h"

namespace tide::rhi::metal {

class MetalDevice;

class MetalCommandBuffer final : public tide::rhi::ICommandBuffer {
public:
    explicit MetalCommandBuffer(MetalDevice& device);
    ~MetalCommandBuffer() override;

    MetalCommandBuffer(const MetalCommandBuffer&)            = delete;
    MetalCommandBuffer& operator=(const MetalCommandBuffer&) = delete;

    // ─── Begin / end (called by MetalDevice::begin_frame / end_frame) ───────
    void begin();
    void commit_and_present();

    // ─── State transitions — Metal no-ops (DEFINE D2) ───────────────────────
    void transition(tide::rhi::BufferHandle,
                    tide::rhi::ResourceState,
                    tide::rhi::ResourceState) override {}
    void transition(tide::rhi::TextureHandle,
                    tide::rhi::ResourceState,
                    tide::rhi::ResourceState,
                    uint32_t, uint32_t, uint32_t, uint32_t) override {}

    // ─── Render pass ────────────────────────────────────────────────────────
    void begin_render_pass(const tide::rhi::RenderPassDesc& desc) override;
    void end_render_pass() override;

    // ─── Pipeline + binding ─────────────────────────────────────────────────
    // bind_pipeline (task 5) records the Metal-native primitive type for the
    // active PSO so subsequent draw() calls inside the same render pass don't
    // need a fresh handle lookup.
    void bind_pipeline(tide::rhi::PipelineHandle pipeline) override;
    void bind_descriptor_set(uint32_t set_index,
                             tide::rhi::DescriptorSetHandle set) override;
    void bind_vertex_buffer(uint32_t slot,
                            tide::rhi::BufferHandle buffer,
                            uint64_t offset) override;
    void bind_index_buffer(tide::rhi::BufferHandle buffer,
                           uint64_t offset,
                           tide::rhi::IndexType type) override;

    void set_viewport(const tide::rhi::Viewport& vp) override;
    void set_scissor(const tide::rhi::Rect2D& rect) override;
    void set_push_constants(uint32_t, uint32_t, const void*) override {}

    void draw(uint32_t vertex_count,
              uint32_t instance_count,
              uint32_t first_vertex,
              uint32_t first_instance) override;
    void draw_indexed(uint32_t index_count,
                      uint32_t instance_count,
                      uint32_t first_index,
                      int32_t  vertex_offset,
                      uint32_t first_instance) override;
    void dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z) override;

    void copy_buffer(tide::rhi::BufferHandle, tide::rhi::BufferHandle,
                     uint64_t, uint64_t, uint64_t) override {}

    void push_debug_marker(const char* name) override;
    void pop_debug_marker() override;

    // Error accumulator (DEFINE D7). Recording calls return void; if any
    // surfaces an error it's set here and surfaced at submit() time. Phase 1
    // task 2 has no error sources yet (transition() and most stubs are
    // no-ops), but the path is in place so task 4+ doesn't have to retrofit.
    [[nodiscard]] bool has_error() const noexcept { return errored_; }
    [[nodiscard]] tide::rhi::RhiError last_error() const noexcept { return last_error_; }
    void clear_error() noexcept { errored_ = false; last_error_ = tide::rhi::RhiError::BackendInternal; }

private:
    MetalDevice& device_;
    void*        cmd_buffer_{nullptr};       // id<MTLCommandBuffer>
    void*        render_encoder_{nullptr};   // id<MTLRenderCommandEncoder>
    void*        compute_encoder_{nullptr};  // id<MTLComputeCommandEncoder> — at most one encoder open at a time
    bool         active_{false};
    bool         errored_{false};
    tide::rhi::RhiError last_error_{tide::rhi::RhiError::BackendInternal};

    // Cached at bind_pipeline() so draw() can issue drawPrimitives without
    // a second handle lookup. uint32_t holds an MTLPrimitiveType — matches
    // the encoding stored in MetalPipeline::primitive_type.
    uint32_t     bound_primitive_type_{0};

    // Cached compute threadgroup size from the bound compute PSO (matches
    // the kernel's [numthreads] / OpExecutionMode LocalSize). dispatch()
    // passes these to dispatchThreadgroups:threadsPerThreadgroup:.
    uint32_t     bound_compute_tx_{1};
    uint32_t     bound_compute_ty_{1};
    uint32_t     bound_compute_tz_{1};

    // Index-buffer binding for draw_indexed (Phase 1 task 7). Held without an
    // extra retain — the underlying MTLBuffer's lifetime is owned by the
    // MetalBuffer record in the device pool, which outlives the command-buffer
    // recording window per ADR-007 invariant 5.
    void*    bound_index_buffer_{nullptr};   // id<MTLBuffer>, NOT retained
    uint64_t bound_index_offset_{0};
    uint32_t bound_index_type_{0};           // MTLIndexType
};

} // namespace tide::rhi::metal
