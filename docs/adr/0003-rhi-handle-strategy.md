# ADR-0003: RHI handle strategy — opaque integer (generation+index), no virtual dispatch on resources

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P1 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

The RHI must hand out references to GPU-side resources (buffers, textures, pipelines, shaders, samplers, descriptor sets, fences). The interface choice for these references determines:

- Cross-backend cost: Metal, Vulkan, and DX12 all expose handles as opaque pointers or COM/`id<>` references. The engine wraps them in something portable.
- ABA-safety: a freed slot reused by a later allocation must not be confused with the prior owner's stale reference.
- Threading: handles are passed across threads (asset loading on workers, RHI submission on the main thread until P4).
- Cost per draw: a textured-quad sample issues O(10) handle look-ups per frame; a real scene issues O(thousands).

Three viable shapes:

1. **`std::shared_ptr<IResource>`** with virtual dispatch on every operation.
2. **Raw native pointers** (`id<MTLBuffer>`, `ID3D12Resource*`) wrapped in a thin C++ class.
3. **Opaque integer handle** (index + generation) that indexes a backend-owned pool.

## Decision

**Opaque integer handles, 32-bit index + 32-bit generation, with strongly-typed tags so different resource kinds cannot be substituted by accident.** Resource records live in per-backend `HandlePool<T, Tag>` containers; lookups are O(1) array access guarded by a generation check. There is **no virtual dispatch on resources** — only `IDevice`, `ICommandBuffer`, and `IFence` are abstract; everything they create is a typed `Handle<Tag>`.

Concrete shape (`engine/core/include/tide/core/Handle.h`):

```cpp
template <class Tag> struct Handle {
    using IndexType      = uint32_t;
    using GenerationType = uint32_t;

    static constexpr IndexType kInvalidIndex = std::numeric_limits<IndexType>::max();

    IndexType      index{kInvalidIndex};
    GenerationType generation{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return index != kInvalidIndex; }
    [[nodiscard]] constexpr uint64_t bits() const noexcept;   // for ImTextureID, debug print
    friend constexpr bool operator==(Handle, Handle) noexcept;
};
```

RHI-side aliases (`engine/rhi/include/tide/rhi/Handles.h`):

```cpp
struct BufferTag {};
struct TextureTag {};
struct TextureViewTag {};
struct SamplerTag {};
struct ShaderTag {};
struct PipelineTag {};
struct DescriptorSetLayoutTag {};
struct DescriptorSetTag {};
struct FenceTag {};

using BufferHandle              = Handle<BufferTag>;
using TextureHandle             = Handle<TextureTag>;
// ... and so on
```

Tags are empty structs solely for compile-time type discrimination — `BufferHandle` and `TextureHandle` cannot be implicitly converted to one another.

## Alternatives considered

- **`std::shared_ptr<IResource>` + virtual dispatch.** Rejected:
  - Cache cost: every draw indirects through a vtable for `bind_pipeline`/`draw`/`copy`. On hot paths with thousands of bindings per frame, the indirection costs measurable cycles even before the cache miss penalty.
  - Lifetime ambiguity: who owns a `shared_ptr` returned from `create_buffer`? Reference counting across the GPU/CPU boundary is hard to reason about. Two-phase commit (CPU-released + GPU-still-using) is the actual contract; `shared_ptr` doesn't model it.
  - Backend pollution: the abstract base would need to declare every operation each backend supports, leaking detail across the boundary.
- **Raw native pointers wrapped (e.g. `MetalBuffer{id<MTLBuffer>}`).** Rejected:
  - Doesn't survive Vulkan port: Vulkan resources are `VkBuffer` (a 64-bit handle on most platforms, but conceptually opaque), not pointers — wrapping them as "pointers" is a Metal-ism.
  - ABA-unsafe: if a backend reuses a `MTLBuffer` slot after free, the wrapper has no way to detect it.
  - Manual lifetime: the consumer has to remember which backend's release semantics apply.
- **`std::variant<MetalBuffer, VulkanBuffer, D3D12Buffer>`.** Rejected:
  - Sized to the largest variant on every site, regardless of which backend is built.
  - Loses the type-tag advantage — every operation has to `std::visit`.
  - Compile-time bloat from instantiating the variant + visitor for every API call.

## Consequences

**Positive.**
- O(1) handle resolution: `pool.get(handle)` is an array index plus a generation check. No cache miss on the metadata path; the actual GPU resource lookup happens at submit time.
- ABA-safe: `HandlePool::release(h)` increments the slot's generation; a stale handle with the prior generation fails `pool.owns(h)`. Verified by `tests/unit/core/test_handle.cpp` ("ABA-safe reuse: stale handle is rejected after generation bump").
- Type-safe: `BufferHandle` and `TextureHandle` are not interchangeable at the type level. Mixing them at a call site is a compile error, not a runtime crash.
- Trivially copyable, trivially comparable: passes by value, hashable, fits in 64 bits — fine to store in vectors, unordered_maps, descriptor writes (`DescriptorWrite` carries handles by value).
- Backend-agnostic: the same handle types work for Metal, Vulkan, and DX12. Each backend's pool stores its own native record (`MetalBuffer`, future `VulkanBuffer`, etc.).

**Negative / accepted costs.**
- An extra layer of indirection vs. raw native pointers: every draw-time lookup goes through the per-backend pool. Mitigated by keeping resource records in a contiguous `std::vector` (the HandlePool's storage) so the cache behaviour is friendly.
- HandlePool's `std::vector<Slot>` reallocates on growth, which invalidates pointers held by other threads concurrently doing `get(h)`. Caught the hard way during Phase 1 task 9 stress testing — the fix is to call `HandlePool::reserve(N)` up-front (added in the same task). The cap-and-reserve pattern is now load-bearing for any pool serving cross-thread lookups.
- Generation-rollover: `uint32_t generation` allows ~4B reuses of one slot before wrapping. An adversarial workload that releases-and-allocates a single slot once per microsecond rolls over in ~70 minutes; in practice nothing in the engine churns slots that fast. Documented; not mitigated in P1.

**Reversibility.** Multi-month retrofit. Every call site that touches a resource takes a `Handle<Tag>` by value; switching to `shared_ptr<IResource>` would mean rewriting every backend, every consumer, and every test.

## Forward-design hooks

- **No virtual on resources, ever.** If a future system needs polymorphic behaviour on a per-resource basis (e.g. a "resource view" abstraction), it MUST be implemented at the `IDevice`/`ICommandBuffer` level by routing on the handle's tag, not by making the resource record polymorphic. Adding a virtual to `MetalBuffer` is the same architectural mistake as adding a virtual to `IDevice` for ImGui — see ADR-0006's tripwire.
- **Per-backend pools, not per-resource pools.** Each backend owns one `HandlePool` per resource kind. If we add a Vulkan backend in Phase 2, it gets its own `HandlePool<VulkanBuffer, BufferTag>` — same handle type, different storage. Consumers don't notice.
- **Cap-and-reserve, not unbounded grow.** Pool sizes are declared up-front as `kMaxJobsInFlight` / `kMaxFramesInFlight` style constants. Growth-via-realloc is a thread-safety footgun; if a future workload needs more capacity than the cap, raise the cap, don't make the pool dynamic.
- **Slot-zero special cases must be `inline constexpr` constants in the backend header**, not magic literals. The Metal backend reserves slot 0 of the texture pool for the swapchain (`kSwapchainSlot`). New reserved slots should follow the same pattern.
- **`Handle::bits()` is the only legal way to pack a handle into an opaque uint64** (e.g. ImGui's `ImTextureID`). Decoders must use the symmetric helper, not bit-shift directly.

## Related ADRs

- ADR-0007: Threading model — explains why pools are externally synchronizable (resource creation callable from any thread).
- ADR-0006: Bindless vs descriptor sets — the descriptor-set design is built on these handles.
- ADR-0011: Jobs system — `JobHandle` follows the same `Handle<Tag>` shape (different namespace, same idea).
- ADR-0012: Error handling — `tide::expected<TextureHandle, RhiError>` is the canonical creation-result type.
