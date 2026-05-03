# ADR-0006: Bindless vs descriptor sets — descriptor sets in P1, bindless deferred with explicit migration plan

**Status:** Accepted — lean bindless
**Date:** 2026-05-03
**Phase:** P1 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

Modern explicit GPU APIs all converge toward a "bindless" resource binding model where shaders index into device-wide descriptor heaps rather than referencing per-draw bound resources:

- **DX12:** SM 6.6 dynamic resources (`ResourceDescriptorHeap`).
- **Vulkan:** `VK_EXT_descriptor_indexing` + `update_after_bind` + 1.2/1.3 features.
- **Metal:** Argument buffers (since iOS 11 / macOS 10.13, Tier 2 on M-series for unbounded heaps).

The traditional alternative is **explicit descriptor sets**: per-draw bindings declared at PSO creation, written before draw, bound via `vkCmdBindDescriptorSets` / `setRenderPipelineState` + per-stage encoder slots / `D3D12_ROOT_PARAMETER`.

Phase 1's RHI must pick a model that:

1. Implements end-to-end on Metal *now* (no driver feature gates that the M-series doesn't support).
2. Doesn't preclude bindless when Phase 4+ frame-graph scenes need it (thousands of materials, GPU-driven culling).
3. Doesn't ship a leaky abstraction that exposes per-backend descriptor mechanics to gameplay code.

## Decision

**Phase 1 ships explicit descriptor sets via `DescriptorSetLayout` + `DescriptorSet` + `DescriptorWrite`. Bindless is the explicit Phase 4+ migration target — the descriptor-set API is shaped so the Phase 4 port becomes "swap the backend implementation," not "rewrite every consumer."**

Concrete shape (`engine/rhi/include/tide/rhi/Descriptors.h`):

```cpp
struct DescriptorBindingDesc {
    uint16_t               slot{0};
    uint16_t               array_count{1};
    DescriptorType         type{DescriptorType::UniformBuffer};
    ShaderStage            stages{ShaderStage::All};
    DescriptorBindingFlags flags{DescriptorBindingFlags::None};
};

struct DescriptorSetLayoutDesc {
    std::span<const DescriptorBindingDesc> bindings;
    const char*                            debug_name{nullptr};
    // Bindless promotion is the responsibility of the backend, transparent to
    // call sites — see the lean-bindless commitment below.
};

struct DescriptorWrite {
    uint16_t          slot{0};
    uint16_t          array_index{0};
    DescriptorType    type{DescriptorType::UniformBuffer};
    BufferHandle      buffer;
    TextureViewHandle texture;
    SamplerHandle     sampler;
    uint64_t          buffer_offset{0};
    uint64_t          buffer_range{~0ull};
};
```

A consumer calls `create_descriptor_set_layout(...)` once, `create_descriptor_set(...)` per material, `update_descriptor_set(...)` to rebind, and `bind_descriptor_set(...)` per draw. The layout is shared across pipelines that have the same binding signature.

Slot conventions across pipelines (Phase 1 convention, formalized after the Task 7 black-quad bug):

- **Cbuffers / structured buffers** occupy MSL `[[buffer(N)]]` slots `[0, 30)`.
- **Vertex buffers** occupy slot `30+` (MoltenVK convention, reserved by us).
- **Textures and samplers** each have their own MSL argument tables, indexed densely from 0.
- All `slot` numbers are per-resource-kind: a cbuffer at slot 0 doesn't conflict with a texture at slot 0 because Metal has separate argument tables.

## Alternatives considered

- **Ship bindless from Phase 1.** Rejected:
  - Apple Silicon Tier 2 argument buffers exist but the consumer-facing model is still moving (mostly stable in macOS 14 / 15). Building a Phase 1 sample on it before the Vulkan port is wrong-order.
  - The mental model for descriptor sets matches what every Phase 1 sample needs (a textured quad has 1 cbuffer + 1 texture + 1 sampler — bindless is overkill).
  - Debugging bindless miswires is harder: a wrong index in a heap is a silent garbage read; a wrong descriptor write is a clear validation error.
- **Vulkan-style fully-explicit descriptor sets exposed verbatim.** Rejected: the API would mention Vulkan's `update_after_bind`, `partially_bound`, `variable_count` etc. — leaking pre-bindless mechanics that Metal doesn't have.
- **Per-pipeline `bind_buffer(handle, slot)` / `bind_texture(handle, slot)` API (no descriptor set abstraction).** Rejected: matches Metal but maps awkwardly to Vulkan/DX12 where the binding has to be recorded into a descriptor set / root signature first. Forces every backend-port to write the descriptor set inside `bind_*` — moving the cost without removing it.

## Consequences

**Positive.**
- Maps cleanly to all three Phase 2+ backends.
- The DescriptorWrite-by-value pattern means the descriptor set is just data — easy to test, easy to update from any thread (Metal backend's `update_descriptor_set` is mutex-protected).
- Per-slot-and-type matching is unambiguous (Phase 1 task 7's black-quad bug taught us to match by `(slot, type)`, not slot alone).
- The slot conventions in samples are reusable: `samples/03_textured_quad` and `samples/04_imgui_overlay` share the same b0/t0/s0 + vbuf-at-30 layout.

**Negative / accepted costs.**
- Per-frame `update_descriptor_set` is the canonical pattern for ring-buffered cbuffers — suboptimal vs Vulkan's "dynamic offsets" / "push descriptors" / inline uniform blocks. Phase 1 accepts the cost; the bindless migration in Phase 4 collapses this anyway.
- The slot map (`b0/t0/s0` + vbuf at 30) is an unenforced convention. A future shader that uses `register(b1)` would land at MSL `[[buffer(1)]]` and the consumer-side descriptor write would need to match. Phase 1 doesn't surface a slot-map authoring tool; the convention lives in shader comments + the Phase 4 reflection-driven generator (TODO).
- `DescriptorBindingDesc::array_count` is plumbed but only `1` is exercised. Variable-count bindings (the bindless prerequisite) won't be tested until they're consumed.
- Three resource kinds at the same numeric slot (cbuffer 0 / texture 0 / sampler 0) — clear once you know it, mildly confusing for newcomers. Documented in `MetalCommandBuffer.mm`'s bind_descriptor_set comment.

**Reversibility.** Multi-month retrofit *if done wrong*; **same-day backend swap** if the forward-design hooks below are honoured. The whole point of this ADR is to keep the migration cheap.

## Forward-design hooks

These five must hold from Phase 1 onward so the Phase 4 bindless port is local to the backend, not a cross-cutting refactor:

1. **The plan tripwire: no virtual methods on `IDevice` or `ICommandBuffer` for binding resources.** ImGui rendering goes through the same `bind_pipeline + bind_descriptor_set + bind_vertex_buffer + bind_index_buffer + draw_indexed` surface as the rest of the engine. Verified in Phase 1 task 8: `git diff HEAD -- engine/rhi/include/tide/rhi/IDevice.h` is empty for the ImGui module.
2. **No `bindless_layout` flag on `DescriptorSetLayoutDesc`.** Bindless promotion is the backend's responsibility, transparent to call sites. (Comment in the struct preserves this commitment.)
3. **`DescriptorSet` is opaque from the consumer's side.** Consumers build a layout, allocate a set, write to it. They never see how the backend implements it. Phase 4's bindless backend can replace `MetalDescriptorSet { vector<DescriptorWrite> }` with `MetalDescriptorSet { uint32_t heap_index; vector<DescriptorWrite> last_writes; }` without touching a single consumer.
4. **Slot numbers are stable per shader.** A shader's `register(b0)` always means "this pipeline's cbuffer 0". A frame graph in Phase 4 may rewrite shaders to fetch from a global descriptor heap, but the *consumer-side* slot identity stays.
5. **`update_descriptor_set` accepts a span of writes — never a single write.** Phase 1 callers happen to pass spans of size 1 sometimes, but the API is shaped for batched updates. Phase 4 bindless will batch heap-index writes the same way.

## Related ADRs

- ADR-0003: RHI handle strategy — descriptor writes carry handles.
- ADR-0004: Shader pipeline — DXC `-fvk-{b,t,u,s}-shift` flags determine the SPIR-V binding numbers SPIRV-Cross packs into MSL.
- ADR-0005: Memory model — `Upload` cbuffers are the typical descriptor-write target.
- ADR-0007: Threading model — descriptor set updates are mutex-protected, callable from any thread.
