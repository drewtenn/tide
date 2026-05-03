# ADR-0012: Error handling in RHI — `tide::expected<T, RhiError>` everywhere; `void`-returning recording with deferred error accumulator

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P1 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

ADR-0039 established `std::expected<T, E>` as the engine-wide error policy (no exceptions). The RHI inherits that policy but layers on a recording-vs-submit-time tension specific to graphics APIs:

- **Resource creation** (`create_buffer`, `create_pipeline`, ...) is rare, allocates, may fail — fits `expected<Handle, RhiError>` cleanly.
- **Frame lifecycle** (`begin_frame`, `acquire_swapchain_texture`, `end_frame`) — fits `expected<>` because `SwapchainOutOfDate` is a meaningful runtime case.
- **Command recording** (`bind_pipeline`, `draw`, `set_scissor`, `transition`, etc.) is *very* hot. Returning `expected<void, RhiError>` from each call would force every consumer to write `if (auto r = cmd->draw(...); !r) return ...;` — cripples readability and adds a branch per draw.

Modern explicit GPU APIs choose:

- **Vulkan** returns `VkResult` from every recording call (most return `VK_SUCCESS` unconditionally; the surface clutter is famously a usability tax).
- **DX12** returns `HRESULT` from non-trivial recording calls; the simple ones return `void`.
- **Metal** uses Objective-C exceptions on `[encoder ...]` calls and surfaces all errors at submit time via `MTLCommandBuffer.error` after `waitUntilCompleted`.

The engine's choice has to satisfy:

1. ADR-0039: no exceptions.
2. The consumer ergonomics goal: gameplay-side rendering code reads naturally, without `if (auto r = ...; !r)` boilerplate per draw.
3. The validation-layer goal: a backend that detects an error during recording must surface it before the user shrugs at a black frame.

## Decision

**Two-tier error handling, codified in `engine/rhi/include/tide/rhi/IDevice.h`:**

1. **Resource creation, frame lifecycle, and uploads return `tide::expected<T, RhiError>`.** Examples:
    ```cpp
    [[nodiscard]] virtual tide::expected<BufferHandle, RhiError>
        create_buffer(const BufferDesc& desc) = 0;
    [[nodiscard]] virtual tide::expected<void, RhiError> begin_frame() = 0;
    [[nodiscard]] virtual tide::expected<TextureHandle, RhiError>
        acquire_swapchain_texture() = 0;
    ```
    Errors here are unrecoverable for the call site — the buffer wasn't created, the frame can't start. Caller checks and bails.

2. **Recording calls (`ICommandBuffer::*`) return `void`. Errors accumulate in the command buffer's internal `errored_` / `last_error_` state. `IDevice::submit(cmd)` is the surface where the accumulated error becomes visible — it returns `void` (matching DX12's `GetDeviceRemovedReason` pattern), but the next `IDevice::begin_frame()` returns `RhiError::DeviceLost` if the previous frame's submit observed a hard error.**

```cpp
class ICommandBuffer {
public:
    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    virtual void draw(uint32_t vertex_count, ...) = 0;
    virtual void set_scissor(const Rect2D& rect) = 0;
    // ... all recording is void

    // Test/diagnostic surface only.
    [[nodiscard]] bool has_error() const noexcept;
    [[nodiscard]] RhiError last_error() const noexcept;
    void clear_error() noexcept;
};
```

`RhiError` is a small enum (`engine/rhi/include/tide/rhi/IDevice.h`) covering the cases backends actually distinguish:

```cpp
enum class RhiError : uint32_t {
    OutOfMemory,
    InvalidDescriptor,
    DeviceLost,
    SwapchainOutOfDate,
    BackendInternal,
    UnsupportedFeature,
};
```

## Alternatives considered

- **`expected<void, RhiError>` from every recording call.** Rejected: the boilerplate at every call site (`if (!cmd->draw(3, 1, 0, 0).has_value()) return ...`) is anti-ergonomic and adds a branch per draw. The accumulator pattern keeps the hot path clean.
- **Throw on failure (with a `std::exception` derived `RhiException`).** Rejected: ADR-0039 forbids exceptions in engine code. This would also mean wrapping every recording call in `try`/`catch` — same boilerplate, different syntax.
- **Vulkan-style `VkResult` return, but consumers ignore it.** Rejected: returning a value that callers won't check makes the API look more checked than it is. Either commit to checking everywhere or accumulate on the side.
- **Single global `LastError` (D3D9/OpenGL pattern).** Rejected: thread-unsafe; obscures which command buffer failed when many are in flight.

## Consequences

**Positive.**
- Consumer code reads naturally: `cmd->bind_pipeline(pso); cmd->draw(3); cmd->end_render_pass();` is a clean linear sequence — no per-line error check.
- Resource creation errors are immediate and explicit. The first sample (`02_triangle`) demonstrates the pattern at every step.
- The accumulator integrates with backends that surface errors lazily (Metal's `MTLCommandBuffer.error`); `DeviceLost` from a corrupt previous frame becomes the next frame's `begin_frame` failure, which is the natural recovery point.
- The `[[nodiscard]]` on every `expected<T, RhiError>` return means the compiler enforces error-checking on the high-stakes paths.

**Negative / accepted costs.**
- Recording errors are out-of-band relative to the failing call. A caller who sets a wrong scissor doesn't get the failure at the `set_scissor` line — they get it at submit-time. Mitigated by clear `TIDE_LOG_ERROR` + `TIDE_LOG_WARN` instrumentation in the backend (every error path logs the specific cause).
- Backends can't refuse to record forward calls after the first error — Phase 1 backends just keep no-oping. A future "abort on first error" mode is reserved as a per-cmd flag.
- The accumulator pattern means a flaky validation-layer error in the middle of a frame poisons the entire submit. Acceptable for Phase 1 (validation is dev-time only); Phase 4+ may add a "soft error" tier for non-fatal validation hits.

**Reversibility.** Switching to `expected<>` everywhere is a multi-week refactor (every recording call site has to re-check). Switching to exceptions would require rewriting ADR-0039 first.

## Forward-design hooks

- **`tide::expected<T, RhiError>` is the canonical creation-result.** `.unexpected(RhiError::...)` is the construction pattern; never throw, never `assert(false)`.
- **`[[nodiscard]]` on every `expected<>` return.** Compiler-enforced for the high-stakes paths.
- **Error messages live in the backend's TIDE_LOG_ERROR call, not in `RhiError`.** The enum is for programmatic dispatch (the consumer knows whether to retry on `SwapchainOutOfDate`); the human-readable detail goes to the log.
- **Backends MUST translate native errors to a single `RhiError` value, not pass them through.** The Vulkan port (Phase 2) cannot surface raw `VkResult` to consumers — it maps to `OutOfMemory` / `DeviceLost` / etc.
- **`UnsupportedFeature` is the catch-all for backends that don't yet implement an API.** During Phase 1's gradual fill-out, methods returned `UnsupportedFeature` as stubs. Consumers checking for that value and falling back gracefully is OK; consumers that crash because a stub returned it is a backend bug to fix.

## Related ADRs

- ADR-0039: Error policy — `std::expected<T, E>` is the engine-wide convention; this ADR is the RHI-specific instantiation.
- ADR-0003: RHI handle strategy — handle types appear in every `expected<Handle, RhiError>` return.
- ADR-0007: Threading model — error state is per-`ICommandBuffer` (which is externally synchronized per the threading invariants).
- ADR-0011: Jobs system — `JobDesc::fn` is `void()`; jobs that need RHI errors access them via captured `IDevice&` references.
