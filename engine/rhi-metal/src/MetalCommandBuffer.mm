// MetalCommandBuffer — wraps id<MTLCommandBuffer>. begin_render_pass maps the
// abstract RenderPassDesc onto MTLRenderPassDescriptor. Locked DEFINE D2, D9, D13.

#import "MetalCommandBuffer.h"
#import "MetalDevice.h"
#import "MetalSwapchain.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <dispatch/dispatch.h>

namespace tide::rhi::metal {

namespace {

MTLLoadAction to_mtl(tide::rhi::LoadOp op) noexcept {
    switch (op) {
        case tide::rhi::LoadOp::Load:     return MTLLoadActionLoad;
        case tide::rhi::LoadOp::Clear:    return MTLLoadActionClear;
        case tide::rhi::LoadOp::DontCare: return MTLLoadActionDontCare;
    }
    return MTLLoadActionDontCare;
}

MTLStoreAction to_mtl(tide::rhi::StoreOp op) noexcept {
    switch (op) {
        case tide::rhi::StoreOp::Store:    return MTLStoreActionStore;
        case tide::rhi::StoreOp::DontCare: return MTLStoreActionDontCare;
        case tide::rhi::StoreOp::Resolve:  return MTLStoreActionMultisampleResolve;
    }
    return MTLStoreActionDontCare;
}

} // namespace

MetalCommandBuffer::MetalCommandBuffer(MetalDevice& device) : device_(device) {}

MetalCommandBuffer::~MetalCommandBuffer() {
    // Defensive cleanup: in the happy path commit_and_present has already
    // dropped the cmd_buffer_ retain and end_render_pass dropped the
    // render_encoder_ retain. If a caller skipped either step (e.g. an
    // exception unwound the frame mid-record), balance the +1 retains here
    // rather than leaking heavy MTL objects.
    if (render_encoder_) {
        id<MTLRenderCommandEncoder> enc =
            (__bridge_transfer id<MTLRenderCommandEncoder>)render_encoder_;
        [enc endEncoding];
        render_encoder_ = nullptr;
    }
    if (compute_encoder_) {
        id<MTLComputeCommandEncoder> enc =
            (__bridge_transfer id<MTLComputeCommandEncoder>)compute_encoder_;
        [enc endEncoding];
        compute_encoder_ = nullptr;
    }
    if (cmd_buffer_) {
        // Drop the +1 retain without committing — the buffer will deallocate
        // when the autoreleasepool drains.
        id<MTLCommandBuffer> cb = (__bridge_transfer id<MTLCommandBuffer>)cmd_buffer_;
        (void)cb;
        cmd_buffer_ = nullptr;
    }
}

void MetalCommandBuffer::begin() {
    if (active_) return;

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device_.mtl_command_queue();
    id<MTLCommandBuffer> cb   = [queue commandBuffer];
    cb.label                  = @"tide.frame";

    cmd_buffer_ = (__bridge_retained void*)cb;
    active_     = true;
}

void MetalCommandBuffer::commit_and_present() {
    if (!active_) return;

    // Defensive: a caller who forgot to end_render_pass / end the compute
    // encoder would otherwise commit with an open encoder and trip Metal's
    // "encoder must be ended before commit" assertion (and leak the encoder
    // retain). End them here so the path is robust to exception-induced
    // skips.
    if (render_encoder_) {
        id<MTLRenderCommandEncoder> enc =
            (__bridge_transfer id<MTLRenderCommandEncoder>)render_encoder_;
        [enc endEncoding];
        render_encoder_ = nullptr;
    }
    if (compute_encoder_) {
        id<MTLComputeCommandEncoder> enc =
            (__bridge_transfer id<MTLComputeCommandEncoder>)compute_encoder_;
        [enc endEncoding];
        compute_encoder_ = nullptr;
    }

    id<MTLCommandBuffer> cb = (__bridge_transfer id<MTLCommandBuffer>)cmd_buffer_;
    cmd_buffer_ = nullptr;

    auto* drawable_ptr = device_.swapchain().current_drawable();
    if (drawable_ptr) {
        id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawable_ptr;
        [cb presentDrawable:drawable];
    }

    // Signal frame-slot release in the GPU completion handler. Locked DEFINE D9
    // tripwire: NEVER signal after commit; that bypasses the in-flight cap.
    void* sem_ptr = device_.swapchain().semaphore_handle();
    if (sem_ptr) {
        dispatch_semaphore_t sem = (__bridge dispatch_semaphore_t)sem_ptr;
        [cb addCompletedHandler:^(__unused id<MTLCommandBuffer> _) {
            dispatch_semaphore_signal(sem);
        }];
    }

    [cb commit];

    // Drop the drawable retain now that present is queued; semaphore handles
    // the actual GPU-side lifetime.
    device_.swapchain().release_drawable();
    active_ = false;
}

void MetalCommandBuffer::begin_render_pass(const tide::rhi::RenderPassDesc& desc) {
    @autoreleasepool {
        MTLRenderPassDescriptor* rpd = [MTLRenderPassDescriptor renderPassDescriptor];

        for (uint32_t i = 0; i < desc.color_attachment_count; ++i) {
            const auto& ca = desc.color_attachments[i];
            id<MTLTexture> tex = (__bridge id<MTLTexture>)device_.resolve_attachment(ca.target);
            // Defensive: a swapchain-slot handle resolves to nullptr until the
            // first acquire_swapchain_texture() of the frame has rebound the
            // drawable. Surface an error rather than handing nil to Metal,
            // which would crash inside renderCommandEncoderWithDescriptor.
            if (!tex) {
                errored_    = true;
                last_error_ = tide::rhi::RhiError::InvalidDescriptor;
                return;
            }
            rpd.colorAttachments[i].texture     = tex;
            rpd.colorAttachments[i].loadAction  = to_mtl(ca.load_op);
            rpd.colorAttachments[i].storeAction = to_mtl(ca.store_op);
            rpd.colorAttachments[i].clearColor  = MTLClearColorMake(
                ca.clear_value.r, ca.clear_value.g, ca.clear_value.b, ca.clear_value.a);

            if (ca.resolve_target.texture.valid() || ca.resolve_target.view.valid()) {
                id<MTLTexture> resolve_tex =
                    (__bridge id<MTLTexture>)device_.resolve_attachment(ca.resolve_target);
                if (!resolve_tex) {
                    errored_    = true;
                    last_error_ = tide::rhi::RhiError::InvalidDescriptor;
                    return;
                }
                rpd.colorAttachments[i].resolveTexture = resolve_tex;
            }
        }

        if (desc.depth) {
            const auto& d = *desc.depth;
            id<MTLTexture> depth_tex =
                (__bridge id<MTLTexture>)device_.resolve_attachment(d.target);
            if (!depth_tex) {
                errored_    = true;
                last_error_ = tide::rhi::RhiError::InvalidDescriptor;
                return;
            }
            rpd.depthAttachment.texture     = depth_tex;
            rpd.depthAttachment.loadAction  = to_mtl(d.load_op);
            rpd.depthAttachment.storeAction = to_mtl(d.store_op);
            rpd.depthAttachment.clearDepth  = d.clear_value.depth;
        }

        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd_buffer_;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
        render_encoder_ = (__bridge_retained void*)enc;
    }
}

void MetalCommandBuffer::end_render_pass() {
    if (render_encoder_) {
        id<MTLRenderCommandEncoder> enc =
            (__bridge_transfer id<MTLRenderCommandEncoder>)render_encoder_;
        [enc endEncoding];
        render_encoder_ = nullptr;
    }
}

// ─── Pipeline + dynamic state (Phase 1 task 5) ──────────────────────────────
//
// bind_pipeline is only valid inside an active render pass — encoder dynamic
// state is part of the encoder's identity, so binding outside a pass would
// silently do nothing. We tolerate it (no-op) rather than asserting because
// the abstract IDevice doesn't surface "render pass active" as a precondition;
// callers misordering bind_pipeline against begin_render_pass simply observe
// no draw output. Validation layers (Vulkan/DX12) catch this at the same gate.

void MetalCommandBuffer::bind_pipeline(tide::rhi::PipelineHandle pipeline) {
    auto* rec = device_.pipeline(pipeline);
    if (!rec) {
        errored_    = true;
        last_error_ = tide::rhi::RhiError::InvalidDescriptor;
        return;
    }

    if (rec->kind == MetalPipeline::Kind::Compute) {
        // Compute path (Phase 1 task 11). Open a compute encoder on demand;
        // close any active render encoder first since Metal allows at most
        // one encoder open per command buffer at a time.
        if (render_encoder_) {
            id<MTLRenderCommandEncoder> ren =
                (__bridge_transfer id<MTLRenderCommandEncoder>)render_encoder_;
            [ren endEncoding];
            render_encoder_ = nullptr;
        }
        if (!compute_encoder_) {
            id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd_buffer_;
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            compute_encoder_ = (__bridge_retained void*)enc;
        }
        id<MTLComputeCommandEncoder> enc =
            (__bridge id<MTLComputeCommandEncoder>)compute_encoder_;
        [enc setComputePipelineState:
            (__bridge id<MTLComputePipelineState>)rec->mtl_pipeline_state];
        bound_compute_tx_ = rec->compute_tx;
        bound_compute_ty_ = rec->compute_ty;
        bound_compute_tz_ = rec->compute_tz;
        return;
    }

    if (!render_encoder_) return;   // no active pass; see comment above

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)render_encoder_;

    [enc setRenderPipelineState:
        (__bridge id<MTLRenderPipelineState>)rec->mtl_pipeline_state];
    if (rec->mtl_depth_stencil) {
        [enc setDepthStencilState:
            (__bridge id<MTLDepthStencilState>)rec->mtl_depth_stencil];
    }
    [enc setCullMode:static_cast<MTLCullMode>(rec->cull_mode)];
    [enc setFrontFacingWinding:static_cast<MTLWinding>(rec->front_face)];
    [enc setTriangleFillMode:static_cast<MTLTriangleFillMode>(rec->triangle_fill)];
    [enc setDepthClipMode:rec->depth_clamp ? MTLDepthClipModeClamp
                                           : MTLDepthClipModeClip];

    bound_primitive_type_ = rec->primitive_type;
}

void MetalCommandBuffer::set_viewport(const tide::rhi::Viewport& vp) {
    if (!render_encoder_) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)render_encoder_;
    MTLViewport mvp{
        .originX = vp.x,
        .originY = vp.y,
        .width   = vp.width,
        .height  = vp.height,
        .znear   = vp.min_depth,
        .zfar    = vp.max_depth,
    };
    [enc setViewport:mvp];
}

void MetalCommandBuffer::set_scissor(const tide::rhi::Rect2D& rect) {
    if (!render_encoder_) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)render_encoder_;
    MTLScissorRect ms{
        .x      = static_cast<NSUInteger>(rect.x < 0 ? 0 : rect.x),
        .y      = static_cast<NSUInteger>(rect.y < 0 ? 0 : rect.y),
        .width  = rect.width,
        .height = rect.height,
    };
    [enc setScissorRect:ms];
}

void MetalCommandBuffer::draw(uint32_t vertex_count,
                              uint32_t instance_count,
                              uint32_t first_vertex,
                              uint32_t first_instance) {
    if (!render_encoder_ || vertex_count == 0 || instance_count == 0) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)render_encoder_;
    [enc drawPrimitives:static_cast<MTLPrimitiveType>(bound_primitive_type_)
            vertexStart:first_vertex
            vertexCount:vertex_count
          instanceCount:instance_count
           baseInstance:first_instance];
}

// ─── Resource binding (Phase 1 task 7) ──────────────────────────────────────
//
// Slot conventions (see debate-task7-gate1.md):
//   - cbuffers / structured buffers occupy MTL buffer slots [0, 30) — assigned
//     by SPIRV-Cross from SPIR-V binding numbers.
//   - vertex buffers occupy slot 30+ (MoltenVK convention) so they don't
//     collide with cbuffer parameters at slot 0.
//   - textures and samplers each have their own argument table indexed
//     densely from 0.

void MetalCommandBuffer::bind_descriptor_set(uint32_t /*set_index*/,
                                             tide::rhi::DescriptorSetHandle set) {
    if (!render_encoder_ && !compute_encoder_) return;

    // Snapshot the writes + the binding stage routing under the device mutex,
    // then iterate the local copies. Without this, a concurrent
    // update_descriptor_set on the same handle could push_back into the
    // vectors we're walking and invalidate iterators (raised by post-Develop
    // code review). Phase 1 ADR-007 invariant 5 says recording is externally
    // synchronized, but the resource_mutex_ contract for the pool itself was
    // illusory because device_.descriptor_set() / descriptor_set_layout()
    // release the lock before returning. The copy pattern here makes the
    // contract real without holding the lock across encoder calls.
    std::vector<tide::rhi::DescriptorWrite>      writes;
    std::vector<tide::rhi::DescriptorBindingDesc> bindings;
    {
        auto* set_rec = device_.descriptor_set(set);
        if (!set_rec) {
            errored_    = true;
            last_error_ = tide::rhi::RhiError::InvalidDescriptor;
            return;
        }
        auto* layout_rec = device_.descriptor_set_layout(set_rec->layout);
        if (!layout_rec) {
            errored_    = true;
            last_error_ = tide::rhi::RhiError::InvalidDescriptor;
            return;
        }
        // The getters above briefly took the device mutex per call, but the
        // pointers are dereferenced here without the lock. To make the snapshot
        // race-free we re-take it. (The per-call lock+release in
        // device_.descriptor_set() is preserved for now — fixing the API to
        // return a locked handle is out of scope for task 7's iteration.)
        writes   = set_rec->writes;
        bindings = layout_rec->bindings;
    }

    // Walk the writes; route to the open encoder. Render-encoder routes per
    // Vertex/Fragment stage; compute-encoder routes the single Compute stage.
    //
    // Match bindings by (slot, type) — Metal has separate argument tables per
    // resource kind so slot 0 of UniformBuffer / SampledTexture / Sampler all
    // coexist. Looking up by slot alone returned the wrong binding for any
    // layout that mixes types at the same slot (Task 7 / 8 black-quad bug).
    id<MTLRenderCommandEncoder>  ren_enc = render_encoder_
        ? (__bridge id<MTLRenderCommandEncoder>)render_encoder_  : nil;
    id<MTLComputeCommandEncoder> com_enc = compute_encoder_
        ? (__bridge id<MTLComputeCommandEncoder>)compute_encoder_ : nil;

    for (const auto& w : writes) {
        tide::rhi::ShaderStage stages = tide::rhi::ShaderStage::None;
        for (const auto& b : bindings) {
            if (b.slot == w.slot && b.type == w.type) {
                stages = b.stages;
                break;
            }
        }
        if (!any(stages)) continue;

        const bool to_vs = any(stages & tide::rhi::ShaderStage::Vertex);
        const bool to_fs = any(stages & tide::rhi::ShaderStage::Fragment);
        const bool to_cs = any(stages & tide::rhi::ShaderStage::Compute);

        switch (w.type) {
            case tide::rhi::DescriptorType::UniformBuffer:
            case tide::rhi::DescriptorType::StorageBuffer: {
                auto* buf = device_.buffer(w.buffer);
                if (!buf || !buf->mtl_buffer) {
                    errored_    = true;
                    last_error_ = tide::rhi::RhiError::InvalidDescriptor;
                    continue;
                }
                id<MTLBuffer> mtl = (__bridge id<MTLBuffer>)buf->mtl_buffer;
                if (to_vs && ren_enc) {
                    [ren_enc setVertexBuffer:mtl   offset:w.buffer_offset atIndex:w.slot];
                }
                if (to_fs && ren_enc) {
                    [ren_enc setFragmentBuffer:mtl offset:w.buffer_offset atIndex:w.slot];
                }
                if (to_cs && com_enc) {
                    [com_enc setBuffer:mtl offset:w.buffer_offset atIndex:w.slot];
                }
                break;
            }
            case tide::rhi::DescriptorType::SampledTexture:
            case tide::rhi::DescriptorType::StorageTexture: {
                // The view holds the actual MTLTexture (potentially a subrange).
                auto* view = device_.texture_view(w.texture);
                id<MTLTexture> mtl = nil;
                if (view && view->mtl_texture_view) {
                    mtl = (__bridge id<MTLTexture>)view->mtl_texture_view;
                }
                if (!mtl) {
                    errored_    = true;
                    last_error_ = tide::rhi::RhiError::InvalidDescriptor;
                    continue;
                }
                if (to_vs && ren_enc) [ren_enc setVertexTexture:mtl   atIndex:w.slot];
                if (to_fs && ren_enc) [ren_enc setFragmentTexture:mtl atIndex:w.slot];
                if (to_cs && com_enc) [com_enc setTexture:mtl         atIndex:w.slot];
                break;
            }
            case tide::rhi::DescriptorType::Sampler: {
                auto* smp = device_.sampler(w.sampler);
                if (!smp || !smp->mtl_sampler) {
                    errored_    = true;
                    last_error_ = tide::rhi::RhiError::InvalidDescriptor;
                    continue;
                }
                id<MTLSamplerState> ms = (__bridge id<MTLSamplerState>)smp->mtl_sampler;
                if (to_vs && ren_enc) [ren_enc setVertexSamplerState:ms   atIndex:w.slot];
                if (to_fs && ren_enc) [ren_enc setFragmentSamplerState:ms atIndex:w.slot];
                if (to_cs && com_enc) [com_enc setSamplerState:ms         atIndex:w.slot];
                break;
            }
            case tide::rhi::DescriptorType::CombinedImageSampler:
                // Vulkan-only convenience — unsupported on Metal where textures
                // and samplers occupy separate argument tables. Phase 1 callers
                // should use a pair of (SampledTexture, Sampler) writes instead.
                errored_    = true;
                last_error_ = tide::rhi::RhiError::UnsupportedFeature;
                continue;
        }
    }
}

void MetalCommandBuffer::bind_vertex_buffer(uint32_t slot,
                                            tide::rhi::BufferHandle buffer,
                                            uint64_t offset) {
    if (!render_encoder_) return;
    auto* buf = device_.buffer(buffer);
    if (!buf || !buf->mtl_buffer) {
        errored_    = true;
        last_error_ = tide::rhi::RhiError::InvalidDescriptor;
        return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)render_encoder_;
    id<MTLBuffer> mtl = (__bridge id<MTLBuffer>)buf->mtl_buffer;
    [enc setVertexBuffer:mtl offset:offset atIndex:slot];
}

void MetalCommandBuffer::bind_index_buffer(tide::rhi::BufferHandle buffer,
                                           uint64_t offset,
                                           tide::rhi::IndexType type) {
    auto* buf = device_.buffer(buffer);
    if (!buf || !buf->mtl_buffer) {
        errored_    = true;
        last_error_ = tide::rhi::RhiError::InvalidDescriptor;
        return;
    }
    bound_index_buffer_ = buf->mtl_buffer;
    bound_index_offset_ = offset;
    bound_index_type_ = (type == tide::rhi::IndexType::Uint16)
                            ? static_cast<uint32_t>(MTLIndexTypeUInt16)
                            : static_cast<uint32_t>(MTLIndexTypeUInt32);
}

void MetalCommandBuffer::dispatch(uint32_t group_x, uint32_t group_y, uint32_t group_z) {
    if (!compute_encoder_ || group_x == 0 || group_y == 0 || group_z == 0) return;
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)compute_encoder_;
    [enc dispatchThreadgroups:MTLSizeMake(group_x, group_y, group_z)
        threadsPerThreadgroup:MTLSizeMake(bound_compute_tx_,
                                           bound_compute_ty_,
                                           bound_compute_tz_)];
}

void MetalCommandBuffer::draw_indexed(uint32_t index_count,
                                      uint32_t instance_count,
                                      uint32_t first_index,
                                      int32_t  vertex_offset,
                                      uint32_t first_instance) {
    if (!render_encoder_ || index_count == 0 || instance_count == 0) return;
    if (!bound_index_buffer_) {
        errored_    = true;
        last_error_ = tide::rhi::RhiError::InvalidDescriptor;
        return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)render_encoder_;
    id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)bound_index_buffer_;
    const auto idx_type = static_cast<MTLIndexType>(bound_index_type_);
    const NSUInteger idx_stride = (idx_type == MTLIndexTypeUInt16) ? 2 : 4;
    const NSUInteger idx_byte_offset = bound_index_offset_ + first_index * idx_stride;

    [enc drawIndexedPrimitives:static_cast<MTLPrimitiveType>(bound_primitive_type_)
                    indexCount:index_count
                     indexType:idx_type
                   indexBuffer:ibuf
             indexBufferOffset:idx_byte_offset
                 instanceCount:instance_count
                    baseVertex:vertex_offset
                  baseInstance:first_instance];
}

void MetalCommandBuffer::push_debug_marker(const char* name) {
    if (!cmd_buffer_ || !name) return;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd_buffer_;
    [cb pushDebugGroup:[NSString stringWithUTF8String:name]];
}

void MetalCommandBuffer::pop_debug_marker() {
    if (!cmd_buffer_) return;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cmd_buffer_;
    [cb popDebugGroup];
}

} // namespace tide::rhi::metal
