# ADR-0005: Memory model — `BufferDesc::memory_type` enum (DeviceLocal / Upload / Readback)

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P1 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

Every modern explicit GPU API exposes some form of memory tier:

- **Metal:** `MTLStorageMode` ∈ {`Private`, `Shared`, `Managed`, `Memoryless`}.
- **Vulkan:** `VkMemoryPropertyFlags` — `DEVICE_LOCAL`, `HOST_VISIBLE`, `HOST_COHERENT`, `HOST_CACHED`, lazily-allocated, etc.
- **DX12:** `D3D12_HEAP_TYPE` ∈ {`DEFAULT`, `UPLOAD`, `READBACK`, `CUSTOM`}.

The RHI must pick an abstraction that:

1. Maps cleanly to all three backends (no leaky `void* metalSpecificFlag`).
2. Captures the consumer's *intent* (where the data flows) rather than backend-specific properties.
3. Doesn't constrain Apple Silicon's UMA advantage — `Shared` storage on UMA is effectively zero-copy.
4. Is small enough that a samples/test author can pick one without reading two pages of docs.

## Decision

**`tide::rhi::MemoryType` is a three-value intent enum: `DeviceLocal`, `Upload`, `Readback`.** Defined in `engine/rhi/include/tide/rhi/Descriptors.h`:

```cpp
enum class MemoryType : uint8_t {
    DeviceLocal,  // GPU-only; no CPU map.       Metal Private, Vk DEVICE_LOCAL,            DX12 DEFAULT
    Upload,       // CPU-write/GPU-read.         Metal Shared,  Vk HOST_VISIBLE|COHERENT,    DX12 UPLOAD
    Readback,     // GPU-write/CPU-read.         Metal Shared,  Vk HOST_VISIBLE|CACHED,      DX12 READBACK
};
```

Both `BufferDesc` and `TextureDesc` carry a `memory_type` field. The Metal backend translates per `to_mtl_storage()`:

```cpp
MTLStorageMode to_mtl_storage(tide::rhi::MemoryType m) noexcept {
    switch (m) {
        case MemoryType::DeviceLocal: return MTLStorageModePrivate;
        case MemoryType::Upload:      return MTLStorageModeShared;
        case MemoryType::Readback:    return MTLStorageModeShared;
    }
}
```

`upload_buffer` / `upload_texture` / `download_buffer` / `download_texture` route per memory type:

- **Upload/Readback (Shared on Metal):** direct memcpy into/out of `[buf contents]`. No blit.
- **DeviceLocal (Private on Metal):** stage through a temporary Shared buffer with a blit encoder + `waitUntilCompleted`.

## Alternatives considered

- **Expose backend-native flags** (e.g. a `vkMemoryPropertyFlags`-shaped bitmask). Rejected: no useful Vulkan-style flag survives translation to Metal storage modes one-for-one. The bitmask becomes a mess of platform-specific cases at the consumer site.
- **Two states only — DeviceLocal vs HostVisible.** Rejected: the Vulkan/DX12 distinction between Upload (write-combine, uncached) and Readback (cached) is genuine. Lumping them costs perf on real readback workloads (the offscreen-hash sample in task 10 is the canary).
- **Heuristic memory selection inside the backend** (e.g. `BufferDesc::pattern = StreamWrite | StreamRead | Static`). Rejected: pushes the decision into the backend where it's harder to predict and harder to test. Intent enums put the choice at the call site where the author knows the lifecycle.
- **Per-backend memory pools (e.g. `IDevice::create_upload_buffer` vs `create_readback_buffer`).** Rejected: doubles the API surface for marginal benefit.

## Consequences

**Positive.**
- One field, three values. New samples get the model right on the first try (`MemoryType::Upload` for cbuffer rings, `MemoryType::DeviceLocal` for vertex/index, `MemoryType::Readback` for hash readback).
- Apple Silicon UMA "just works": both Upload and Readback are `Shared` storage on Metal, which is the same physical memory as DeviceLocal on UMA. No spurious copies.
- Discrete GPU paths (future Vulkan-on-NVIDIA, DX12-on-Windows) get the right mapping for free: Upload → write-combined, Readback → cached, DeviceLocal → device.
- Per-frame ring buffers (textured-quad's cbuffer, ImGui's vbuf/ibuf) all use `Upload`. Persistent geometry uses `DeviceLocal` + the implicit staging path inside `upload_buffer`.

**Negative / accepted costs.**
- `Upload` and `Readback` collapse to the same Metal storage mode — Phase 1 readback can't distinguish CPU-cached vs uncached. This becomes load-bearing on Vulkan-on-Intel/AMD; documented as a Phase 2 amendment if profiling shows the readback path is slow.
- The intent enum hides whether a buffer is *also* GPU-visible. On Apple Silicon, every buffer is GPU-visible regardless; on Vulkan, `HOST_VISIBLE | DEVICE_LOCAL` (the unicorn ReBAR memory type) doesn't fit the three-state enum cleanly. Reserved as a Phase 2 question.
- Synchronous upload/download blocks the calling thread on `waitUntilCompleted` for `DeviceLocal`. Phase 3+ will add async variants returning a fence; Phase 1 callers are limited to "load at startup" and "test-side readback".

**Reversibility.** 1–2 weeks. The enum has 8 fields' worth of consumers (every `BufferDesc` / `TextureDesc` constructor site) plus the per-backend translation tables. Adding a fourth value (e.g. `DeviceLocalHostCoherent` for Vulkan ReBAR) is a same-day change; switching to a flag bitmask is a multi-week refactor.

## Forward-design hooks

- **`memory_type` is set at create time and immutable.** No "transition memory type" API. If a workload needs both fast GPU-only and CPU-read access, allocate two buffers and copy.
- **`upload_buffer` / `upload_texture` / `download_buffer` / `download_texture` route on memory_type.** No new API needed for new memory types; just extend the routing.
- **The Vulkan port (Phase 2) MUST preserve the three-state enum.** If `HOST_VISIBLE | DEVICE_LOCAL` (ReBAR) becomes important, add a fourth value with explicit semantics, don't bolt it on as a flag.
- **Don't leak `MTLStorageMode` (or any backend type) into public RHI headers.** The translation is in `MetalDevice.mm`'s anonymous namespace and stays there.
- **Cbuffer ring buffers are the canonical `Upload` workload.** When Phase 4 adds the frame graph, ring-allocator allocations should reuse a single `Upload` buffer with byte-offset writes (already the pattern in samples/03_textured_quad and ImGui), not allocate per-frame.

## Related ADRs

- ADR-0003: RHI handle strategy — `BufferHandle` carries no memory_type info; lookup goes through the pool which stores it.
- ADR-0004: Shader pipeline — DXC's `-fvk-use-dx-layout` keeps cbuffer packing consistent across backends, which interacts with how Upload buffers are laid out.
- ADR-0006: Bindless vs descriptor sets — descriptor sets reference buffer handles whose memory type determines the binding cost on Vulkan/DX12.
- ADR-0007: Threading model — upload/download are callable from any thread (the backend serializes via `resource_mutex_`).
