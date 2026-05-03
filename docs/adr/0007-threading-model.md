# ADR-0007: Threading model — single-threaded rendering with main+render-thread split as a contractual P4 target

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

The implementation plan (`IMPLEMENTATION_PLAN.md` §134) calls this ADR load-bearing. The threading model dictates how every system that touches GPU resources is allowed to interact with the RHI. Deferring this decision until Phase 4 is forbidden — the retrofit cost is 3–6 weeks of cross-cutting work, optimistic.

The choice is between two viable models for an indie engine in 2026 (a job-graph/no-main-thread model is ruled out: appropriate for AAA scale, wrong here):

1. **Single-threaded** — game logic, RHI command recording, and submission all on the main thread.
2. **Main + render-thread split** — main thread runs game logic and records into a command queue; render thread submits to the GPU.

## Decision

**Single-threaded rendering for Phase 0–3.** Main+render-thread split is the **contractual Phase 4 target** — the implementation lands then, but five forward-design invariants are enforced from Phase 0 onward so the retrofit is a focused render-thread implementation, not a cross-cutting refactor.

**This is a decision about which thread submits GPU work.** It is NOT a statement that the engine is single-threaded overall. The jobs system (`jobs/IJobSystem.h`, P0 stub → P1 work-stealing pool) runs on worker threads from Phase 1. Asset decompression, animation evaluation, and physics stepping all run on workers. Only RHI-facing work runs on the main thread until P4.

### The five forward-design invariants

These MUST hold from Phase 0 onward. Any code that violates them is broken for the future render thread.

1. **All `IDevice` resource-creation methods are callable from any thread.** Implementation today: a `std::mutex` inside `rhi-metal/Device.cpp` serializes calls. Implementation in P4: removed when the render thread becomes the sole owner of resource creation.
2. **`AssetHandle<T>` ref counts use `std::atomic<uint32_t>`** from Phase 3 onward. The cost is one atomic op per inc/dec — negligible compared to the benefit of async asset loading from worker threads.
3. **ImGui draw data is double-buffered** from Phase 1. In single-threaded mode the swap is a pointer flip; in P4 the flip becomes the producer/consumer handoff.
4. **Tracy zone names assume the future render thread exists.** Use `ZoneScopedN("Render::Submit")`, `ZoneScopedN("Render::Compile")`, etc., not bare `ZoneScoped`. Per-thread filtering in the Tracy UI will Just Work once the thread shows up.
5. **Backend command-buffer affinity is documented and respected.** All three modern RHIs share the rule that command buffers are externally synchronized:
   - **Metal:** `MTLCommandBuffer` must be committed from the thread it was created on.
   - **DX12:** `ID3D12GraphicsCommandList` must be reset and recorded on the same thread; `ID3D12CommandQueue::ExecuteCommandLists` is thread-safe but the lists themselves are not.
   - **Vulkan:** `VkCommandBuffer` belongs to a `VkCommandPool` which is externally synchronized — only one thread may use the pool at a time.

   In Phase 0, the main thread does all of the above. In Phase 4, the render thread does all of the above. The point is that command-buffer ownership never crosses thread boundaries silently.

### CI invariant enforcement (added per debate gate)

A Phase 0 lint script (`tools/lint/check-threading-invariants.sh`) will fail the build if it finds:
- `std::shared_ptr<AssetHandle>` (must be a custom atomic-refcounted handle, not shared_ptr).
- `ImGui::GetDrawData()` called outside `imgui-integration/`.
- Direct `MTLCommandBuffer` / `ID3D12GraphicsCommandList` / `VkCommandBuffer` references outside the relevant `rhi-*/` module.

**Status of enforcement:** The script is **not** authored in the current Phase 0 session (scope was "ADRs + scaffolding only"). It is bundled with the clang-format / clang-tidy CI lane in atomic task 11 of the implementation plan. Until that lands, the invariants are documented-but-unenforced. Author the script as part of the CI workflow PR; do not let Phase 1 begin without it.

## Alternatives considered

- **Single-threaded forever.** Rejected: at Phase 4, the frame graph's compile pass blocks the game loop for non-trivial scenes; the CPU cost of single-threaded GPU submission becomes the frame-time bottleneck.
- **Implement render thread now (Phase 0).** Rejected: solo dev with no Metal/Vulkan exposure cannot debug GPU validation errors AND threading sync errors simultaneously. The cognitive load alone justifies the deferral.
- **Job-graph model (Naughty Dog 2015 style).** Rejected: appropriate for AAA scale, wrong for an indie engine. Bgfx, Sokol, and Magnum all reject this model for the same reason.
- **Bgfx's render-thread-from-day-one model.** Considered seriously. Rejected because bgfx is mature middleware with multi-team contributions; that scope justifies the up-front complexity. For solo dev in P1–P3, the producer/consumer command queue is overhead without payoff.

## Reference engine survey

| Engine | Model | Notes |
|---|---|---|
| **bgfx** | Render thread by default (`BGFX_CONFIG_SINGLETHREADED=1` opt-out) | Architectural; encoder/frame pattern assumes it |
| **Sokol** | Strictly single-threaded | Documented; no exceptions |
| **Magnum** | Single-threaded by default | Doesn't impose a model; user responsibility |
| **Diligent** | Main+render with deferred contexts | Supports both; immediate context is single-threaded common case |
| **The Forge** | Job-graph from day one | AAA scope, not appropriate reference |

The split between bgfx and the others tracks engine scope: AAA-tier middleware picks render-thread-now; indie/teaching engines pick single-threaded-now. We are explicitly indie scope.

## Consequences

**Positive.**
- P1 (Metal bring-up) and P2 (Vulkan bring-up) are dramatically simpler. GPU validation errors are isolated from thread-sync bugs.
- TSAN CI lane stays green trivially.
- ImGui integration in P1 is straightforward.
- The forward-design invariants are enforced by lint, not by reviewer memory.

**Negative / accepted costs.**
- Phase 4 will need a focused 1–2 week effort to introduce the render thread and thread the command queue through the frame graph's execute pass. This cost is accepted and budgeted.
- `IDevice` carries a mutex through P0–P3 that becomes dead code in P4. Acceptable.

**Reversibility.** The decision to defer the render thread is reversed in P4 by design. Reversing the *contractual invariants* (e.g., dropping the atomic asset handle in favor of a non-atomic one) is forbidden — it would gut the deferred-implementation strategy.

## Forward-design hooks

Captured in the five invariants above. Restated as a checklist:

- [ ] `IDevice::create*` methods take their internal mutex on every call.
- [ ] `AssetHandle<T>` uses `std::atomic<uint32_t>` for reference count.
- [ ] `imgui-integration/` exposes a double-buffered `ImDrawData`-equivalent.
- [ ] All Tracy zones in `renderer/`, `assets/`, `rhi-*/` have explicit names with thread-scope prefixes.
- [ ] `tools/lint/check-threading-invariants.sh` runs in CI and fails on violations.

## Related ADRs

- (Future) ADR-0042: Phase 4 render-thread implementation details.
- ADR-0008: Package manager (pulls in `tracy[on-demand]`).
- ADR-0039: Error policy (atomic-handle ref counts return `expected<>` not throw).
