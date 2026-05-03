# C++ Game Engine — Implementation Plan

**Status:** v1.0 — locked 2026-05-02
**Companion to:** [`game_engine_plan.md`](./game_engine_plan.md) (architectural source-of-truth)
**Calibrated by:** multi-LLM research synthesis (codex × claude × copilot, May 2026)

---

## Document conventions

This plan is the **execution roadmap**. The architecture, module boundaries, library choices, and out-of-scope list live in `game_engine_plan.md` and are not duplicated here. Where this plan modifies a design-doc decision (only three places — see §"Calibrations"), the modification is called out inline.

**How to use.**
- Work phases sequentially. Don't skip exit criteria.
- Each phase ends with a runnable demo. If the demo doesn't run, the phase isn't done.
- Risk-gate tripwires are **stop-and-rework** signals, not warnings. If any tripwire fires, halt the phase and fix the abstraction before continuing.
- Atomic tasks are sized at 1–3 days of focused solo work (10–15 hr/week pace). Tag in your tracker; check off as you finish.
- ADRs live under `docs/adr/NNNN-title.md`. Every ADR-touching task must close with the ADR file written or amended.

**Status legend** (suggested for live use):
- ⬜ pending  ·  🟦 in-progress  ·  ✅ done  ·  🛑 blocked  ·  ⚠️ tripwire fired

---

## Calibrations from research synthesis

These are the only deviations from `game_engine_plan.md` §5 phasing. Everything else stands.

| # | Calibration | Reason |
|---|---|---|
| C1 | **Phase 1 vertical slice = textured quad + uniform buffer + ImGui + offscreen readback**, not plain triangle | A triangle compiles with constants baked into the shader and never exercises descriptor binding, sampler state, staging upload, or readback — the parts of the RHI that diverge most between Metal/Vulkan/DX12. bgfx and The Forge both use a textured sample as their canonical hello-world for this reason. |
| C2 | **Second RHI = Vulkan via MoltenVK first**; DX12 deferred to Phase 2.5 (parallel with Phase 3 assets) | Vulkan on MoltenVK runs on the same Mac you develop on (no Windows VM context-switch), exposes the explicit-sync model that Metal hides, and unlocks Linux CI. DX12 adds Windows-build complexity that stalls macOS work. The design-doc escape hatch ("ship MoltenVK if a backend drags") becomes the default path for Phase 2. |
| C3 | **No fibers in the jobs system.** Work-stealing thread pool only, until profiling proves otherwise | Fibers (custom stack allocators, debugger confusion, ASAN false positives) cost weeks of engineering and produce zero observable performance gain until very deep workloads. Naughty Dog's GDC 2015 fiber model is right for AAA-scale; it's wrong for an indie engine in year one. |
| C4 | **Timeline range = 13–20 months to playable demo** (vs. design-doc median of 18 months) | The design-doc estimate is accurate for an engineer with prior Metal/Vulkan exposure. Without prior exposure, P1 (four parallel systems) and P4 (frame graph edge cases) consistently slip; the realistic median moves to 16–20 months at 12 hr/week. The 13-month low end requires prior shader-toolchain experience. |

---

## Cross-phase dependency graph

Critical-path dependencies. An arrow `A → B` means **B cannot start without A's deliverable**.

```
P0 jobs API stub ──────────┐
P0 platform window ────────┤
P0 ADR-007 threading ──────┴──► P1 work-stealing pool ──► P1 textured quad
                                                            │
                       ┌────────────────────────────────────┘
                       ▼
P1 RHI explicit-sync hooks (load_op, memory_type, transition) ──► P2 Vulkan ──► P2.5 DX12
                       │
                       ▼
P1 offscreen readback hash test ──► P2 multi-backend SSIM golden-frame CI
                       │
                       ▼
P1 work-stealing pool ──► P3 async asset loader ──► P4 frame graph ──► P4.5 animation ──► P5 scene/ECS
                                                                        │
P3 binary runtime format ──────────────────────────────────────────────┤
                                                                        ▼
                                              P5 scene/ECS ──► P6 physics ──► P7 audio ──► P8 scripting + demo
                                                                                            │
                                                                                            ▼
                                                                        P9 second-backend parity ──► P10 editor v0
```

**Hidden dependency (most common cause of late-phase rework).** P4's frame graph requires P1 to have shipped explicit `load_op`/`store_op` on `RenderPassDesc`, `memory_type` on `BufferDesc`, and `transition()` hooks on `ICommandBuffer` — even if the Phase-1 Metal backend ignores them. If these aren't in place by end of Phase 1, P4 retrofits cost 6+ weeks. **This is the single most important sequencing rule in the plan.**

---

## Cumulative timeline

At **12 hr/week** (median of 10–15 range). Numbers are weeks; cumulative column is months.

| Phase | Low | High | Cumulative low (mo) | Cumulative high (mo) | Notes |
|---|---|---|---|---|---|
| P0 — Bootstrap | 2 | 4 | 0.5 | 1.0 | Cross-platform CI consistently underestimated |
| P1 — Metal + textured quad + jobs | 6 | 10 | 2.0 | 3.5 | Four independent systems |
| P2 — Vulkan via MoltenVK + golden-frame CI | 6 | 8 | 3.5 | 5.5 | Validates abstraction |
| P2.5 — DX12 (parallel with P3) | (4) | (8) | — | — | Time absorbed during P3 |
| P3 — Assets + cooker + async + shader hot-reload | 5 | 8 | 5.0 | 7.5 | |
| P4 — Renderer + frame graph + shadows | 10 | 16 | 7.5 | 11.5 | **Most underestimated phase** |
| P4.5 — Animation + skinning + blend trees | 6 | 8 | 9.0 | 13.5 | |
| P5 — Scene + EnTT + serialization + inspector | 3 | 5 | 9.5 | 14.5 | |
| P6 — Physics + Jolt + char controller | 4 | 6 | 10.5 | 16.0 | |
| P7 — Audio + miniaudio | 1 | 3 | 11.0 | 16.5 | |
| P8 — Scripting + demo game | 8 | 14 | 13.0 | 20.0 | Demo scope discipline |
| **Playable demo milestone** | | | **13 mo** | **20 mo** | |
| P9 — PhysX + FMOD parity | 6 | 10 | 14.5 | 22.5 | |
| P10 — Editor v0 | 10 | 16 | 17.0 | 26.5 | Open-ended; timebox |

If the timeline is intolerable, the lever to pull is **P8 demo scope** (cut to a 5-minute walking sim) and **P10 editor scope** (use ImGui debug panels indefinitely instead of building an editor app).

---

# Phases

## Phase 0 — Bootstrap

**Goals.** Stand up the cross-platform build, CI, and a single empty-window sample on macOS / Linux / Windows. Lock the threading model decision (ADR-007) before any code that depends on it. Establish the jobs API shape so later modules can compile against it.

**Scope (in).**
- CMake project with vcpkg manifest mode (or Conan; pick one in ADR-008).
- GitHub Actions matrix: `macos-14` (Apple Silicon), `ubuntu-22.04`, `windows-2022`.
- Vendored or fetched: `glm`, `spdlog`, `fmt`, `GLFW` (or SDL3 — ADR-009), `doctest`, `Tracy`, `Dear ImGui`.
- `core/` skeleton: assert/log macros, basic math typedefs, handle pool primitive, allocator stubs.
- `platform/` skeleton: window creation, event pump, monotonic timer, filesystem path helpers.
- `input/` skeleton: keyboard/mouse polling via platform window, gamepad stub. Action-mapping API (named action → binding) so later phases never reach for raw key codes. ADR-037.
- `jobs/` interface (header only; impl is single-threaded inline-execute fallback).
- `clang-format`, `.editorconfig`, `clang-tidy` CI lane.
- Empty-window sample: `samples/00_window/` opens a window on all three OSes, polls events, exits cleanly.

**Scope (out).**
- Any RHI work (Phase 1).
- Real thread pool (Phase 1).
- Asset loading (Phase 3).
- Render-thread implementation (Phase 4 — but **the architectural decision** is locked here in ADR-007).

**Deliverable demo.** `samples/00_window`: an empty native window opens on macOS / Linux / Windows, with a clean shutdown path. CI is green on all three runners. `tracy-profiler` connects and shows a `FrameMark` heartbeat.

**Exit criteria checklist.**
- [ ] CI matrix green on `macos-14`, `ubuntu-22.04`, `windows-2022`
- [ ] `samples/00_window` runs on all three; window closes cleanly on Cmd-Q / Alt-F4 / `:q`
- [ ] `tracy-profiler` (released app, not the engine) connects to a running `samples/00_window` and shows `FrameMark`
- [ ] `clang-format --dry-run` and `clang-tidy` pass on all of `engine/`
- [ ] All unit tests in `tests/unit/` pass with `ctest`
- [ ] ADR-007 (threading model) committed
- [ ] ADR-008 (package manager: vcpkg vs Conan) committed
- [ ] ADR-009 (window/platform: GLFW vs SDL3) committed
- [ ] `core/Handle.h` provides `Handle<Tag>` with generation+index pair, with unit test for ABA-safe reuse
- [ ] `jobs/IJobSystem.h` defines `submit(fn, deps)` / `wait(handle)` even though impl is inline-synchronous

**Risk gate tripwires.**
- Stop and re-architect if **CMake target structure** doesn't make each module a static-library target with public/private include split. This is the cheapest moment to fix; later it requires touching every CMakeLists.
- Stop if **vcpkg manifest** can't reproduce the build on a fresh clone within 30 minutes on a low-spec runner. Either fix the manifest or switch to Conan.
- Stop if you are tempted to defer ADR-007 (threading model). The decision is binary (single-threaded vs main+render-thread split) and the retrofit cost in Phase 4 is 3–6 weeks. Commit, even if implementation is single-threaded for now.

**Time estimate.** **2–4 weeks** (12 hr/week). Cross-platform CI debugging is the variable; one bad weekend on Windows symlinks or Mac codesigning eats a week.

**ADRs touched.**
- ADR-001: Engine namespace and naming convention (create)
- ADR-002: Coordinate system — handedness, up axis, units (create)
- ADR-007: Threading model — single-threaded vs main+render split (**create — load-bearing**)
- ADR-008: Package manager — vcpkg vs Conan (create)
- ADR-009: Window/platform library — GLFW vs SDL3 (create)
- ADR-037: Input — action-map model, polling vs event semantics (create)
- ADR-038: String type — `std::string` vs interned `Name` (create — design-doc §7.4)
- ADR-039: Engine-wide error policy — exceptions / `std::expected` / codes (create — design-doc §7.5; broader than ADR-012 which scopes RHI only)
- ADR-040: Networking decision — explicit "no for v1" with assumptions documented (mutable globals, non-deterministic physics, scripting side effects allowed) (create — design-doc §7.10 mandates explicit decision even if "no")
- ADR-041: C++ hot-reload — Live++ / DLL-reload / none (create — design-doc §7.11; distinct from ADR-015 asset hot-reload)

**Tracy zones added.**
```cpp
FrameMark;                            // every frame
ZoneScopedN("Main")                   // main loop body
tracy::SetThreadName("main");
TracyAllocN, TracyFreeN               // any custom allocator
```

**Atomic tasks.**
1. Create CMake top-level + module skeletons (`core`, `platform`, `jobs`, `samples/00_window`). Add `cmake/find-modules` for any non-vcpkg deps. (2d)
2. Set up vcpkg manifest with `glm`, `spdlog`, `fmt`, `glfw3`, `tracy`, `doctest`, `imgui`. Verify a fresh clone builds. (1d)
3. Author GitHub Actions workflow for `macos-14` / `ubuntu-22.04` / `windows-2022`. Cache vcpkg installed tree. Run unit tests. (2d)
4. Implement `core/Handle.h` (generation+index pair, ABA-safe pool). Doctest covers reuse, invalid-handle, and exhaustion. (2d)
5. Implement `core/Log.h` (spdlog wrapper) and `core/Assert.h` (debug-break + log). (1d)
6. Implement `platform/Window.h` (GLFW or SDL3 wrapper) and `platform/Time.h` (high-res monotonic clock). (2d)
7. Implement `jobs/IJobSystem.h` interface with inline-synchronous impl. Hooks are real; threading isn't. (1d)
8. Wire Tracy: `FrameMark` in main loop, named main thread. Verify `tracy-profiler` connects to `samples/00_window`. (1d)
9. Implement `input/Action.h` (action-map: named actions → keyboard/mouse/gamepad bindings) and `input/InputContext.h`. Hook to platform event pump. (2d)
10. Write ADR-001, -002, -007, -008, -009, -037, -038, -039, -040, -041 to `docs/adr/`. (3d)
11. Add `clang-format` config, `clang-tidy` CI lane, `.editorconfig`. (1d)

**Cross-phase notes.** Everything later assumes: handle pool exists, jobs API shape is stable, threading-model ADR is committed, CI is green. Don't rationalize "I'll fix CI in Phase 1" — flaky CI compounds.

---

## Phase 1 — Metal RHI + textured quad + work-stealing jobs + Tracy

**Goals.** Bring up `rhi/` (interface) and `rhi-metal/` (first concrete backend) end-to-end. Replace the jobs stub with a real work-stealing thread pool. Achieve a runnable textured-quad sample with a uniform buffer and an ImGui overlay. Add an offscreen readback path so Phase 2 has a hash-comparison smoke test ready.

**Scope (in).**
- `rhi/` interface: `IDevice`, `ICommandBuffer`, `IFence`. Descriptor structs (`BufferDesc`, `TextureDesc`, `PipelineDesc`, `RenderPassDesc`, `SamplerDesc`). All handle types.
- **Forward-design for Vulkan/DX12** even though only Metal is implemented: `BufferDesc::memory_type` (Host/Device/HostVisible), `RenderPassDesc::load_op`/`store_op`, `ICommandBuffer::transition(handle, old_state, new_state)`, full `DepthCompare` enum including `GREATER`/`GREATER_OR_EQUAL` for reversed-Z. **Metal backend ignores most of these for now**; Vulkan will need them.
- `rhi-metal/`: device, swapchain, buffer, texture, pipeline, command buffer, sampler, fence, present.
- Shader pipeline: HLSL → SPIR-V (DXC) → MSL (SPIRV-Cross). CMake custom-command. **Test all shader stages** (vertex, fragment, compute) through the round-trip in this phase, not Phase 4.5.
- `jobs/` real implementation: work-stealing thread pool sized to `std::thread::hardware_concurrency() - 1`. Lock-free Chase-Lev deque. Inline-synchronous mode behind a debug flag.
- ImGui Metal backend wired and rendering. Frame stats overlay.
- Offscreen-readback path: render to an `MTLTexture` instead of swapchain, copy to CPU-visible buffer, hash with xxh3.
- Sample: `samples/01_textured_quad/` — spinning textured quad, animated uniform, ImGui frame stats.

**Scope (out).**
- Vulkan / DX12 backends (Phase 2 / 2.5).
- Frame graph (Phase 4).
- Bindless resource binding (deferred to Phase 4 ADR; design `DescriptorSet` API to allow promotion).
- Async asset loading (Phase 3).
- ECS, scene graph (Phase 5).

**Deliverable demo.** `samples/01_textured_quad`: spinning quad with a checker texture, uniform-driven rotation, ImGui frame-time graph showing `FrameMark` cadence, "Capture next frame" debug button (Metal `MTLCaptureManager`), and a "Save offscreen hash" button that writes a 64-bit xxh3 of an offscreen-rendered frame to disk for later comparison.

**Exit criteria checklist.**
- [ ] `samples/01_textured_quad` runs at 60+ fps on Apple Silicon
- [ ] ImGui overlay renders without corrupting main scene descriptors
- [ ] Tracy shows: `FrameMark`, named threads (main + N workers), `Update`, `RhiAcquire`, `RhiRecord`, `RhiSubmit`, `RhiPresent`, per-job zones
- [ ] Worker threads are visibly utilized in Tracy when test workloads are submitted (not pegged at 0%)
- [ ] Shader cross-compile is a CMake step, not a manual command
- [ ] Vertex, fragment, AND compute shaders all round-trip cleanly through HLSL→SPIR-V→MSL
- [ ] `MTLCaptureManager` debug button produces a `.gputrace` file
- [ ] Offscreen-readback path produces a stable xxh3 hash across runs on the same machine
- [ ] `BufferDesc::memory_type`, `RenderPassDesc::load_op`/`store_op`, `ICommandBuffer::transition()` exist in interface even though Metal ignores them — verified by code review against ADR-003
- [ ] `DepthCompare` enum has `GREATER`/`GREATER_OR_EQUAL` (for reversed-Z in Phase 4)
- [ ] No `#ifdef METAL`, `#ifdef APPLE_SILICON`, etc. outside `rhi-metal/`
- [ ] Unit tests for handle pool, job submission, allocator, math all pass
- [ ] ImGui renders correctly on both Retina and non-Retina displays (DPI scale via `io.DisplayFramebufferScale`)

**Risk gate tripwires.**
- 🛑 If the textured-quad sample requires backend-specific fields (`void*` escape hatch) — your `BufferDesc`/`TextureDesc` is incomplete. Stop, design the missing field, restart.
- 🛑 If shader cross-compile fails on **compute** shaders specifically (not vertex/fragment), discover this **now**, not in Phase 4.5. Fix SPIRV-Cross or DXC config.
- 🛑 If Tracy shows the main thread blocking on submit/present, the descriptor heap or upload buffer is single-buffered. Triple-buffer it before exiting Phase 1; it's much cheaper to fix now.
- 🛑 If the work-stealing pool shows >50% time in `wait` zones in Tracy under a synthetic stress test, the deque is wrong (lock contention). Don't ship a broken scheduler — fix it before Phase 2.
- 🛑 If you find yourself adding a virtual method to `IDevice` to make ImGui work, the abstraction is leaking. ImGui rendering goes through `ICommandBuffer::draw()` like everything else.

**Time estimate.** **6–10 weeks** at 12 hr/week. **Historically underestimated.** The four parallel workstreams (RHI design, Metal backend, jobs system, shader cross-compile) compete for attention; expect to context-switch and lose a week to the wrong sequencing. **Recommended order:** jobs interface → Metal device+swapchain+present → shader cross-compile → first colored quad → texture upload → uniform buffer → ImGui → offscreen readback → finalize ADRs. Don't build the work-stealing pool until the textured quad runs single-threaded.

**ADRs touched.**
- ADR-003: RHI handle strategy — opaque integer (generation+index), no virtual dispatch on resources (create)
- ADR-004: Shader pipeline — HLSL→SPIR-V→MSL via DXC + SPIRV-Cross (create)
- ADR-005: Memory model — `BufferDesc::memory_type` enum (create)
- ADR-006: Bindless vs descriptor sets — defer with explicit migration plan, document fallback design (create, **lean bindless**)
- ADR-010: Reversed-Z depth (note: enum supports it, will adopt in Phase 4) (create)
- ADR-011: Jobs system — work-stealing thread pool, no fibers (create)
- ADR-012: Error handling in RHI — `std::expected<T, RhiError>` vs codes vs exceptions (create)

**Tracy zones added.**
```cpp
ZoneScopedN("Update")
ZoneScopedN("RhiAcquire")
ZoneScopedN("RhiRecord")
ZoneScopedN("RhiSubmit")
ZoneScopedN("RhiPresent")
ZoneScopedN("ShaderCompile")
ZoneScopedN("PipelineCreate")
ZoneScopedN("BufferUpload")
ZoneScopedN("TextureUpload")
ZoneScopedN("JobSubmit")            // per submission
ZoneScopedN("JobExecute")           // per worker
ZoneScopedN("WorkerSteal")          // when work-stealing fires
TracyLockable(std::mutex, ...);     // command queue mutexes
TracyPlotN("draw_calls", n);
TracyPlotN("workers_active", n);
TracyMtlZone(...)                   // optional: one GPU zone per pass (Tracy 0.12+)
```

**Atomic tasks.**
1. Author `rhi/IDevice.h`, `rhi/Descriptors.h`, `rhi/Handles.h` with full forward-designed surface (memory_type, load_op, transition, full DepthCompare, sampler desc). (3d)
2. Implement Metal device + swapchain + present + clear-to-color in `rhi-metal/`. Hardcoded clear color in window. (3d)
3. Add CMake DXC + SPIRV-Cross integration. Build a `cmake/CompileShader.cmake` macro. Verify HLSL→SPIR-V→MSL on a trivial vertex+fragment shader. (2d)
4. Implement `BufferHandle`/`TextureHandle` create/upload/destroy on Metal. (2d)
5. Implement `PipelineHandle` (PSO equivalent) on Metal. (2d)
6. First colored triangle on Metal via the engine's RHI. (1d)
7. Add texture upload + sampler + uniform buffer. Promote triangle → textured quad with rotating uniform. (2d)
8. Wire ImGui Metal backend. Frame stats panel. Verify Retina DPI scaling. (2d)
9. Implement work-stealing thread pool in `jobs/JobSystem.cpp`. Chase-Lev deque, hardware-concurrency sizing, Tracy zones. (3d)
10. Add offscreen-readback render path: render to `MTLTexture`, copy to host-visible `MTLBuffer`, xxh3 the bytes, write hash to `golden/01_quad.hash`. (2d)
11. Test compute-shader round-trip (a no-op compute kernel that touches a buffer). Catch SPIRV-Cross issues now. (1d)
12. Wire `MTLCaptureManager` triggerable from ImGui debug menu. (1d)
13. Add **TSAN CI lane** on Linux: build a debug variant with `-fsanitize=thread`; run `samples/01_textured_quad` for 30 seconds with the work-stealing pool active; fail CI on any race. (1d)
14. Write ADR-003, -004, -005, -006, -010, -011, -012. (3d)

**Cross-phase notes.** Everything past P1 assumes: jobs API works for real, Tracy shows clean frames, RHI interface is forward-designed for Vulkan/DX12. The interface forward-design is the load-bearing decision; if it's missing fields, P2 becomes a redesign, not a port.

---

## Phase 2 — Vulkan via MoltenVK + multi-backend golden-frame CI + RenderDoc

**Goals.** Validate the RHI abstraction by implementing a second backend (Vulkan-on-MoltenVK on the same Mac you're using). Set up multi-backend golden-image CI with SSIM tolerance. Wire RenderDoc programmatic capture so every backend has a one-button capture path.

**Scope (in).**
- `rhi-vulkan/` complete: instance, device, queue family selection, VMA allocator, swapchain (via MoltenVK on macOS, native on Linux), descriptor pools, pipelines, render passes, command buffers, fences/semaphores.
- The **same** `samples/01_textured_quad` runs unmodified on Vulkan via a CMake build flag.
- Multi-backend golden-frame CI: render `01_textured_quad` offscreen on Metal and on Vulkan (via SwiftShader on Linux runners for determinism), compare with SSIM ≥ 0.97 tolerance.
- RenderDoc API integration in `rhi-vulkan/` — programmatic `StartFrameCapture()` / `EndFrameCapture()` triggerable from ImGui.
- Metal `MTLCaptureManager` polished: ImGui menu item, GPU trace gets a sensible filename.
- ADR-013: cross-backend parity definition (SSIM threshold, what "different" means).
- **Interface ABI versioning convention** (per ADR-042): `#define ENGINE_RHI_ABI_VERSION N` in `rhi/version.h`; bump on any breaking interface change. `[[deprecated("Use X — see ADR-NNN")]]` macros for two-phase removal (deprecate now, remove in next phase). Same convention applied to `physics/`, `audio/`, `assets/` interfaces as those phases land.
- **Scene-submission replay tool** scaffold in `tools/scene-replay/`: record `ICommandBuffer` calls from a sample frame, replay across backends, diff resulting state. Full implementation in P9 (when second-physics/audio backends land); harness in place from P2 so RHI parity gets it for free.

**Scope (out).**
- DX12 backend (Phase 2.5).
- Real game features (Phases 3+).
- Editor (Phase 10).

**Deliverable demo.** `samples/01_textured_quad` runs on **two backends** (Metal native on macOS; Vulkan via MoltenVK on macOS, native Vulkan on Linux). CI compares offscreen-rendered images per backend and fails on regression. ImGui menu has a working "Capture frame in RenderDoc" button on Vulkan and "Capture frame to .gputrace" button on Metal.

**Exit criteria checklist.**
- [ ] `samples/01_textured_quad` source code is **byte-identical** between Metal and Vulkan paths (no `#ifdef VULKAN` in sample code)
- [ ] Vulkan validation layers report zero errors on the textured-quad sample
- [ ] Linux CI runs the Vulkan path via SwiftShader (deterministic) and produces a stable image
- [ ] macOS CI runs the Metal path and the Vulkan-via-MoltenVK path; cross-backend SSIM ≥ 0.97
- [ ] Golden image regenerator script (`tools/regenerate-golden.sh`) is documented and works
- [ ] RenderDoc capture button on Vulkan produces a `.rdc` file that opens in RenderDoc desktop
- [ ] Metal capture button produces a `.gputrace` that opens in Xcode
- [ ] Tracy shows GPU zones via `TracyVkZone` on Vulkan paths
- [ ] No new virtual methods added to `IDevice` or `ICommandBuffer` since Phase 1 (verified by `git diff` against P1 tag)
- [ ] ADR-013 (parity definition) committed
- [ ] ADR-006 (bindless) re-evaluated: does Vulkan path use descriptor sets or bindless?

**Risk gate tripwires.**
- 🛑 If implementing Vulkan requires adding **virtual methods** to `IDevice` or `ICommandBuffer`, the Phase 1 interface was incomplete. **Do not just add them and move on** — the deltas reveal what was missed. Document the omission in ADR-003 amendment, then add the methods. Repeated occurrences mean redesign.
- 🛑 If `RenderPassDesc::load_op`/`store_op` from Phase 1 turn out to be insufficient for Vulkan, you skipped the forward-design. Roll back, fix the interface, then continue.
- 🛑 If MoltenVK can't render the sample at all (compatibility bug), you have two options: file a MoltenVK issue and use SwiftShader on Linux for the parity test (ship the Vulkan backend on Linux only for now), or roll back the feature triggering the bug.
- 🛑 If golden-image SSIM is below 0.97 on the same hardware between runs, you have **non-determinism in the renderer** (likely floating-point reduction order or undefined sampler behavior). Fix it; don't just relax the threshold.
- 🛑 If you find yourself thinking "DX12 will be different anyway" while designing `rhi-vulkan/`, stop. Phase 2.5 should be a port, not a redesign.

**Time estimate.** **6–8 weeks** at 12 hr/week. Vulkan instance/device/queue family selection eats 3–4 weeks alone for someone new to Vulkan. Golden-image CI infra is 1–2 weeks. MoltenVK quirks add 1 week.

**ADRs touched.**
- ADR-003: RHI handle strategy (**amend** with Phase 2 lessons)
- ADR-013: Cross-backend parity definition — SSIM threshold, deterministic-render rules (create)
- ADR-014: GPU capture integration — RenderDoc + MTLCaptureManager (create)
- ADR-006: Bindless vs descriptor sets (**re-evaluate** — Vulkan 1.2+ bindless decision)
- ADR-042: Interface ABI versioning + deprecation policy (`ENGINE_*_ABI_VERSION` constants, `[[deprecated]]` macros, two-phase remove) (create)

**Tracy zones added.**
```cpp
TracyVkZone(...)                    // one per render pass (Vulkan)
TracyVkCollect(...)                 // GPU timestamp collection
ZoneScopedN("ValidationCheck")      // dev-only Vulkan validation layer cost
ZoneScopedN("RenderDocBeginFrame")
ZoneScopedN("RenderDocEndFrame")
ZoneScopedN("GoldenFrameReadback")
ZoneScopedN("SSIMCompare")          // CI tool
```

**Atomic tasks.**
1. Add Vulkan SDK + VMA to vcpkg manifest. Wire MoltenVK on macOS (linkage flags, MTLDevice → VkDevice). (1d)
2. Implement `rhi-vulkan/` instance + physical-device selection + logical device + queue families. Validation layers on in debug. (3d)
3. Implement Vulkan swapchain (via MoltenVK on macOS, KHR on Linux). (2d)
4. Implement Vulkan buffer + texture create/upload via VMA. (3d)
5. Implement Vulkan render pass + framebuffer (or VK_KHR_dynamic_rendering — pick one in ADR-003 amendment). (2d)
6. Implement Vulkan pipeline state + descriptor set layouts + descriptor pools. (3d)
7. Run `samples/01_textured_quad` on Vulkan. Iterate on missing interface fields (each one is an ADR-003 amendment). (3d)
8. Add SwiftShader to Linux CI for deterministic Vulkan rendering. (1d)
9. Implement `tools/golden-compare/` — SSIM diff with threshold gate. (2d)
10. Wire offscreen-render + xxh3-on-pixels to multi-backend in CI. Fail on threshold breach. (2d)
11. Integrate RenderDoc programmatic API. ImGui debug menu item. (1d)
12. Polish Metal `MTLCaptureManager` integration. (1d)
13. Add `TracyVkZone` to Vulkan command buffer recording. (1d)
14. Define `ENGINE_RHI_ABI_VERSION` in `rhi/version.h`; add `[[deprecated]]` policy notes; document in ADR-042. Apply same pattern preemptively to `physics/version.h`, `audio/version.h`, `assets/version.h`. (1d)
15. Scaffold `tools/scene-replay/`: a record/replay harness that captures an `ICommandBuffer` sequence and replays it. Full diffing in P9. (2d)
16. Write ADR-013, -014, -042; amend ADR-003, -006. (2d)

**Cross-phase notes.** P2 is the abstraction stress-test. If anything wrong in P1's interface design surfaces here, **fix it now** — every subsequent phase compounds the cost. P2.5 (DX12) becomes a port, not a redesign.

---

## Phase 2.5 — DX12 backend (parallel with Phase 3)

**Goals.** Add the third RHI backend now that the abstraction is proven. Run in parallel with Phase 3 (asset work) — switch contexts on bad MoltenVK/macOS days.

**Scope (in).**
- `rhi-d3d12/` complete: device, swapchain (DXGI), command queues, command lists, command allocators, descriptor heaps, pipeline state, root signatures, fences.
- Same textured-quad sample running on Windows via DX12.
- DX12 added to multi-backend golden-frame CI matrix.
- Bindless via descriptor heaps (DX12's strength).

**Scope (out).**
- New abstraction work — if you need new interface methods, you're not porting, you're redesigning. Stop and re-open ADR-003.
- Console-platform variants. PC-only.

**Deliverable demo.** `samples/01_textured_quad` runs on **three backends**. Three-way SSIM parity in CI.

**Exit criteria checklist.**
- [ ] `samples/01_textured_quad` runs on Windows via DX12
- [ ] DX12 debug layer reports zero errors
- [ ] Three-way golden-frame parity in CI (Metal × Vulkan × DX12, SSIM ≥ 0.97 pairwise)
- [ ] Bindless resource access via DX12 descriptor heaps (per ADR-006)
- [ ] No new virtual methods added since Phase 2 tag
- [ ] PIX programmatic capture wired via ImGui menu

**Risk gate tripwires.**
- 🛑 If DX12 forces a new interface method, the abstraction wasn't proven by Phase 2. Stop, ADR-003 amendment, then continue.
- 🛑 If you spend more than 4 weeks on DX12 setup alone, ship without DX12 for v1 and re-attempt after Phase 4. Don't let DX12 yak-shaving block the renderer.

**Time estimate.** **4–8 weeks** running concurrently with Phase 3. Skill-dependent: someone new to DX12 will eat the high end of this range.

**ADRs touched.**
- ADR-003: RHI (amend if any redesign needed — should be zero)
- ADR-014: GPU capture (amend with PIX)
- ADR-006: Bindless (DX12 makes bindless concrete via descriptor heaps)

**Tracy zones added.**
```cpp
TracyD3D12Zone(...)                 // GPU zones via D3D12 timestamps
ZoneScopedN("PIXBeginCapture")
```

**Atomic tasks.**
1. Add `D3D12_CORE` and `dxgi` Windows SDK linkage. (1d)
2. Implement `rhi-d3d12/` device + DXGI swapchain. (3d)
3. Implement DX12 buffer/texture create + upload via custom `D3D12_HEAP_TYPE_UPLOAD` staging. (3d)
4. Implement DX12 root signature + PSO + descriptor heap (bindless). (3d)
5. Implement DX12 command list + queue + fence. (2d)
6. Run `samples/01_textured_quad` on Windows. (1d)
7. Add Windows runner to golden-frame CI. (1d)
8. Wire PIX programmatic capture. (1d)

**Cross-phase notes.** Once P2.5 ships, the RHI is **frozen** until end of Phase 4. Any abstraction change requires breaking-change ADR + parallel update of all three backends.

---

## Phase 3 — Asset pipeline (cooker, runtime format, async loading, shader hot-reload)

**Goals.** Stand up the asset pipeline: an offline cooker that produces a binary runtime format, an async loader on the jobs system, ref-counted handles with stable identity across reloads, and shader hot-reload (highest-ROI hot-reload target).

**Scope (in).**
- `assets/` interface module: `AssetHandle<T>`, `IAssetLoader`, `AssetDB`.
- `tools/asset-cooker/` standalone executable: input = source asset (`.gltf`, `.png`, `.hlsl`); output = binary runtime format with content-hash + cooker-version header.
- Runtime binary format: **memcpy + pointer fixup**, no JSON/string parsing at runtime. `uint32_t schemaVersion` header. Self-relative offsets, not raw pointers.
- glTF mesh loading via `cgltf` in cooker. PNG/JPEG via `stb_image` → KTX2/Basis runtime format.
- Async loading on jobs system: handle returns immediately in `Pending` state, transitions to `Loaded`/`Failed`. No blocking waits in main thread.
- Asset GUID: UUID stored in `.meta` sidecar files (one per asset). Sidecars version-controlled.
- **Shader hot-reload**: shader file change → re-cook → re-create PSO → swap atomically. Per ADR-015 hot-reload is shader-only this phase; full asset hot-reload comes alongside Phase 5.
- Filesystem watcher: `FSEvents` (macOS), `inotify` (Linux), `ReadDirectoryChangesW` (Windows).

**Scope (out).**
- Mesh / texture / material hot-reload (Phase 5+).
- Streaming (out of scope for v1 — see design-doc §9).
- Asset references inside scene files (Phase 5).
- Editor for assets (Phase 10).

**Deliverable demo.** `samples/02_textured_mesh`: load and render a glTF mesh with a PBR base-color texture. Edit the fragment shader source while running; see the change in <2 seconds without restart. ImGui debug shows current asset DB state, pending load count, GPU upload queue.

**Exit criteria checklist.**
- [ ] `samples/02_textured_mesh` loads a glTF model with a PNG base-color texture
- [ ] All asset I/O off the main thread; main thread never blocks on `AssetHandle::wait()`
- [ ] Asset cooker is invoked by CMake `add_custom_command`; never inline at runtime
- [ ] Runtime format has version header; loader rejects mismatched versions with clear message
- [ ] Shader hot-reload works on all three backends (Metal / Vulkan / DX12)
- [ ] Two systems requesting the same asset receive the same handle (deduplication)
- [ ] Asset destruction is deferred until in-flight GPU references complete (no use-after-free on hot-reload)
- [ ] `.meta` sidecar files are committed to git, contain stable UUIDs, survive renames
- [ ] Tracy shows: `AssetCook`, `AssetLoad`, `AssetUpload`, `ShaderRecompile`, `HotReloadApply`
- [ ] Memory plot: VRAM does not grow unbounded under 100 hot-reload cycles
- [ ] ADR-015 (hot-reload scope) committed
- [ ] ADR-016 (asset GUID strategy) committed
- [ ] ADR-017 (binary runtime format schema) committed

**Risk gate tripwires.**
- 🛑 If runtime code parses any source format directly (cgltf, stb_image, JSON), the cooker isn't doing its job. **Move it to the cooker.**
- 🛑 If the GPU destroy on hot-reload races with command-buffer submission, you have a use-after-free. Defer destroys via a per-frame retired-resource list.
- 🛑 If the AssetDB lookup is on a hot path and Tracy shows lock contention, the lookup needs a reader-writer lock or a per-frame snapshot.
- 🛑 If asset identity is unstable (rename → handle invalidation), scenes will be permanently broken in Phase 5. Must be `.meta`-sidecar UUID, not path-based.

**Time estimate.** **5–8 weeks** at 12 hr/week. Hot-reload correctness (deferred destroy, atomic swap, race-free filesystem watch) is what stretches the high end.

**ADRs touched.**
- ADR-015: Hot-reload scope and timing — shader-only in P3, full asset in P5+ (create)
- ADR-016: Asset GUID — `.meta` sidecar UUID approach (create)
- ADR-017: Runtime binary format — schema versioning, pointer fixup (create)
- ADR-018: Asset cooker — invoked by CMake, content-hash cache (create)
- ADR-006: Bindless (**amend** — bindless texture indices reserved at asset-DB level)

**Tracy zones added.**
```cpp
ZoneScopedN("AssetCook")
ZoneScopedN("AssetLoad")
ZoneScopedN("AssetDecode")
ZoneScopedN("AssetUpload")          // GPU upload via staging buffer
ZoneScopedN("ShaderRecompile")
ZoneScopedN("HotReloadApply")
ZoneScopedN("FileWatcherTick")
TracyPlotN("assets_pending", n);
TracyPlotN("vram_used_mb", n);
TracyPlotN("upload_queue_depth", n);
```

**Atomic tasks.**
1. Define `assets/Asset.h` (`AssetHandle<T>`, `AssetDB`). State machine: Pending / Loaded / Failed. (2d)
2. Build `tools/asset-cooker/` CLI scaffold: argparse, manifest read, output dir. (1d)
3. Cooker: glTF mesh import via cgltf → flat vertex+index binary blob with bind-pose transforms. (3d)
4. Cooker: image import via stb_image → KTX2/Basis encoding. (2d)
5. Cooker: HLSL shader import → SPIR-V + reflection metadata + (optional) MSL prebuilt. (2d)
6. Runtime loader: mmap binary blob, fix pointers, version-check, return `AssetHandle`. (2d)
7. Async path: `submit(load_job)` on jobs system; main thread polls `handle.state()`. (2d)
8. Ref-counted dedup: handle pool keyed by UUID; second request returns same handle. (1d)
9. Filesystem watcher abstraction: `assets/FileWatcher.h` with FSEvents/inotify/RDCW backends. (3d)
10. Shader hot-reload pipeline: watcher → re-cook → re-create PSO → atomic swap. Test on all three backends. (3d)
11. Deferred GPU destroy: per-frame retired-resource list; destroy after fence completes. (2d)
12. `.meta` sidecar tooling: generate, validate, sync with source files. (2d)
13. CMake `add_custom_command` wiring: cooker runs on source-asset change. (1d)
14. ImGui asset DB inspector. (1d)
15. Write ADR-015, -016, -017, -018; amend ADR-006. (3d)

**Cross-phase notes.** Phase 5 (scene serialization), Phase 4 (renderer materials), Phase 8 (script-loadable assets) all depend on stable handles + binary format. **The interface is what matters here, not feature completeness** — full hot-reload extends into Phase 5.

---

## Phase 4 — Renderer + frame graph + materials + shadow maps

**Goals.** Build the high-level renderer on top of the proven RHI: frame graph for transient resource and barrier management, material system, forward-or-deferred pipeline, basic lighting with cascaded shadow maps. **This is the largest single phase.** Pad your time estimate.

**Scope (in).**
- `renderer/` module: frame graph, render-pass declarations, transient resource allocation, automatic barrier insertion.
- Material system: shader + descriptor-set layout + PSO factory. Per-frame and per-object uniforms.
- Forward-renderer pipeline (chosen in ADR-019; deferred is also valid). Reversed-Z depth (per ADR-010).
- Cascaded shadow maps for one directional light. Soft shadow filtering (PCF) optional.
- Basic Image-Based Lighting (IBL): cubemap diffuse + specular prefilter (Phase 4 if time, else Phase 4.5).
- Frame graph features: pass declaration, transient buffer/texture aliasing, automatic barrier insertion, dependency-driven pass ordering.
- Per-pass GPU zones (`TracyVkZone`/`TracyD3D12Zone`).
- Render-thread implementation if ADR-007 chose split (otherwise unchanged single-threaded).

**Scope (out).**
- Animation skinning (Phase 4.5).
- Global illumination (out of scope for v1).
- Post-processing beyond tone-map (out of scope for v1).
- Particles (out of scope for v1, or Phase 10+).

**Deliverable demo.** `samples/03_pbr_scene`: a Sponza-style scene (load via cgltf, see Phase 3) with a directional light casting shadows, multiple meshes, basic PBR shading, IBL ambient term. Frame graph debug overlay shows pass list, transient-resource lifetime, barrier insertions. SSIM golden test in CI.

**Exit criteria checklist.**
- [ ] `samples/03_pbr_scene` renders a multi-mesh scene with shadows on all three backends
- [ ] Frame graph correctly aliases transient resources (verified via VRAM plot under stress)
- [ ] Frame graph generates correct barriers (zero validation-layer errors on Vulkan, DX12)
- [ ] Reversed-Z depth in use for the main pass; depth precision visibly better at far range
- [ ] Tracy GPU zones for: `ShadowPass_Cascade0..3`, `OpaquePass`, `IBLDiffusePass`, `IBLSpecularPass`, `Tonemap`
- [ ] CSM cascade transitions are visually smooth (no popping)
- [ ] Multi-backend SSIM golden test on `samples/03_pbr_scene` ≥ 0.97
- [ ] Frame budget: 60+ fps on Apple Silicon for the demo scene
- [ ] ADR-019 (forward vs deferred) committed
- [ ] ADR-020 (frame graph design) committed

**Risk gate tripwires.**
- 🛑 If barrier insertion logic lives in `rhi-*/` (any backend), it's in the wrong layer. Frame graph belongs in `renderer/`; backends just receive transition commands.
- 🛑 If you find yourself special-casing barrier logic per backend, the frame graph design is wrong. Re-read Our Machinery's "High-Level Rendering Using Render Graphs."
- 🛑 If the renderer code visits the same renderable multiple times per frame across passes, you have a data layout problem; group by pass, not by entity.
- 🛑 If multi-backend SSIM falls below 0.95, you have backend-divergent code in `renderer/`. Find it and fix it.
- 🛑 If you've spent 12+ weeks and the demo doesn't show shadows yet, **cut scope**: ship without IBL, ship without soft shadows, ship without async compute. Don't ship without working CSM.
- 🛑 If you haven't written ADR-020 (frame graph) before writing the code, you'll discover the constraints by accident. Spend 2 days on the ADR first.

**Time estimate.** **10–16 weeks** at 12 hr/week. **Most underestimated phase in the plan.** Frame graph correctness (especially split barriers, pass culling, async compute later) is iterative; expect to refactor twice before it sticks.

**ADRs touched.**
- ADR-019: Forward vs deferred renderer (create — recommend forward+ for indie scope)
- ADR-020: Frame graph design — pass declaration, transient pool, barrier inference (create)
- ADR-021: Material system — uniform binding model, shader variants (create)
- ADR-022: Shadow technique — CSM cascade count, distribution scheme (create)
- ADR-007: Threading model (**amend** — render thread implementation if applicable)
- ADR-006: Bindless (**amend** with bindless material indices)

**Tracy zones added.**
```cpp
ZoneScopedN("FrameGraphCompile")
ZoneScopedN("FrameGraphExecute")
ZoneScopedN("CullView")
ZoneScopedN("BuildDrawLists")
ZoneScopedN("MaterialBind")
TracyVkZone(... "ShadowPass_Cascade0")
TracyVkZone(... "ShadowPass_Cascade1")
TracyVkZone(... "ShadowPass_Cascade2")
TracyVkZone(... "ShadowPass_Cascade3")
TracyVkZone(... "OpaquePass")
TracyVkZone(... "IBLDiffusePass")
TracyVkZone(... "IBLSpecularPass")
TracyVkZone(... "Tonemap")
TracyPlotN("draw_calls_opaque", n);
TracyPlotN("transient_vram_mb", n);
```

**Atomic tasks.**
1. Write ADR-020 (frame graph design) before writing code. Capture: pass declaration API, resource type system, transient lifetime, barrier inference algorithm. (3d)
2. Implement `renderer/FrameGraph.h` API: `addPass<T>(name, setup_fn, exec_fn)`, transient resource handles. (3d)
3. Implement frame-graph compile pass: build dependency graph, allocate transient resources, insert barriers. (4d)
4. Implement frame-graph execute pass: per-pass command buffer recording (parallelizable later). (2d)
5. First test: a 2-pass frame graph (clear → present). Validate barriers via debug layers. (1d)
6. Material system: `Material` = shader + uniform layout + bindings. (3d)
7. Forward renderer pipeline: opaque pass → tonemap → present. (2d)
8. Reversed-Z depth: projection matrix + depth compare = `GREATER_OR_EQUAL` + clear to 0. (1d)
9. Cascaded Shadow Maps: 4-cascade design, view splits, per-cascade pass. (5d)
10. PCF shadow filter (3×3 or larger). (1d)
11. IBL diffuse irradiance prefilter + specular split-sum prefilter. (3d)
12. Render Sponza-style scene. SSIM golden frame in CI. (2d)
13. Frame-graph debug overlay (ImGui): pass list, transient-resource Gantt. (2d)
14. Tracy GPU zone wiring per pass. (1d)
15. ADR-019, -020, -021, -022; amend ADR-006, -007. (4d)

**Cross-phase notes.** Phases 4.5, 5, 6, 7 all submit work through the frame graph. P4 must define the pass-registration API in a way that animation skinning (P4.5), ECS-driven submission (P5), and physics-debug rendering (P6) can extend without modifying frame-graph internals.

---

## Phase 4.5 — Skeletal animation + GPU skinning + blend trees

**Goals.** Animate one character end-to-end: glTF skin import, joint hierarchy, GPU vertex skinning, animation clip sampling, 1D and 2D blend trees, root motion extraction.

**Scope (in).**
- glTF skin import in cooker: joints, inverse bind matrices, animation channels.
- Runtime joint hierarchy (`animation/Skeleton.h`), pose evaluation.
- GPU vertex skinning via compute shader (validates compute path from Phase 1).
- Animation clip sampling (linear interpolation; cubic spline if glTF cubic).
- 1D and 2D blend trees (state machine TBD; if doing one, simple).
- Root motion extraction.
- Sample: a walking character with a 1D speed-blend tree (idle → walk → run).

**Scope (out).**
- Two-bone IK (stretch — defer unless time).
- Ragdoll (combines with physics; phase TBD post-P6).
- Facial / additive layers (out of scope for v1).
- Cinematic cutscene tooling (out of scope for v1).

**Deliverable demo.** `samples/04_animated_character`: an animated character mesh playing back a walk cycle, blending between idle and walk based on input. Tracy GPU zone shows skinning compute dispatch.

**Exit criteria checklist.**
- [ ] glTF cooker round-trips skinned meshes with joint data
- [ ] Animation samples interpolate correctly (no popping at clip boundaries)
- [ ] GPU compute skinning runs at <2ms per character on Apple Silicon
- [ ] 1D blend tree (idle ↔ walk) blends smoothly
- [ ] Root motion extracts and applies to the parent transform
- [ ] Tracy: `AnimationSample`, `BlendTreeEvaluate`, `SkinningDispatch` zones
- [ ] Compute path round-trip validated (per Phase 1 exit criteria) is still working
- [ ] ADR-023 (animation system) committed

**Risk gate tripwires.**
- 🛑 If compute skinning produces wrong output that wasn't reproducible in the Phase 1 compute test, you have backend-divergent compute behavior. Add the failing case to the compute round-trip test.
- 🛑 If blend tree evaluation is on the main thread per character, scaling to 100+ characters will tank framerate. Move to jobs system.

**Time estimate.** **6–8 weeks**.

**ADRs touched.**
- ADR-023: Animation — pose representation, blend tree state machine, root motion conventions (create)

**Tracy zones added.**
```cpp
ZoneScopedN("AnimationSample")
ZoneScopedN("BlendTreeEvaluate")
ZoneScopedN("PoseToMatrices")
TracyVkZone(... "SkinningDispatch")
TracyPlotN("animated_chars", n);
```

**Atomic tasks.**
1. Cooker: import glTF skin (joints, inverse bind matrices). Output flat hierarchy + animation channels. (3d)
2. Runtime: `animation/Skeleton.h`, `animation/AnimationClip.h`, `animation/Pose.h`. Linear sampling. (2d)
3. Pose-to-matrices step: world-space joint matrices for skinning. (1d)
4. GPU compute skinning: bind pose-matrices buffer + skinned mesh; output transformed vertex buffer. (2d)
5. Render skinned mesh through frame graph (new pass). (1d)
6. Blend tree: 1D evaluator (parameter → weighted clip blend). (2d)
7. Blend tree: 2D evaluator (movement direction + speed). (2d)
8. State machine wrapping blend trees (`AnimationGraph`). (2d)
9. Root motion extraction → parent transform delta. (1d)
10. `samples/04_animated_character`. (2d)
11. ADR-023. (1d)

**Cross-phase notes.** Pose evaluation can be parallelized per character once on the jobs system. Phase 6 (physics) will need ragdoll bone-driven; design pose representation to allow external write.

---

## Phase 5 — Scene + EnTT ECS + serialization + ImGui inspector

**Goals.** Wrap EnTT into the engine's `scene/` layer. Define core components (Transform, MeshRenderer, Camera, Light, AnimatedMesh, RigidBody fwd-decl, AudioSource fwd-decl). Binary scene serialization with debug JSON. ImGui scene inspector + entity tree. **Establish the component-access declaration pattern now**, before Phase 6 needs it for thread safety.

**Scope (in).**
- `scene/Scene.h`: wraps `entt::registry`, owns the world.
- Components: `Transform`, `LocalTransform`, `WorldTransform`, `Hierarchy` (parent/child), `MeshRenderer`, `Camera`, `Light` (Directional/Point/Spot), `AnimatedMesh`. Forward-declared: `RigidBody`, `AudioSource`.
- Component access declarations: each system declares `reads<T>` / `writes<T>`. Used by Phase 6 jobs scheduler.
- Binary scene serialization: per-component `serialize`/`deserialize`. Schema version per ADR-017.
- Debug JSON serialization (slow path; for diff-friendly source control).
- Transform propagation: parent-to-child world matrix, dirty flag.
- ImGui inspector: scene tree, component editor (per-component drawer registry).
- Frustum culling at entity granularity (Phase 5; per-meshlet later).
- Flycam in `samples/05_scene_walk`.
- Full asset hot-reload (mesh, texture, material) — extends ADR-015.

**Scope (out).**
- Networking-replication scene merging (out of scope for v1).
- Streaming sublevels (out of scope for v1).
- Editor UI beyond the inspector debug panel (Phase 10).
- Prefab system (deferred, Phase 10 candidate).

**Deliverable demo.** `samples/05_scene_walk`: load a binary scene file, walk around with the flycam, edit component values in ImGui, save scene, reload — values persist. Hot-reload a mesh: visible change without restart.

**Exit criteria checklist.**
- [ ] Scene loads from binary file in <100ms for a 1000-entity scene
- [ ] JSON debug serialization round-trips identically to binary
- [ ] Transform propagation handles 5-deep hierarchy correctly
- [ ] Flycam: WASD + mouse-look; ImGui shows component values
- [ ] Each system declares `reads<T>`/`writes<T>` (even if scheduler isn't enforcing yet)
- [ ] Frustum culling reduces draw calls visibly (Tracy plot)
- [ ] Mesh hot-reload works without crash
- [ ] Texture hot-reload works without crash
- [ ] Tracy: `SceneLoad`, `SceneSerialize`, `TransformPropagate`, `Cull`, `BuildDrawLists`, per-system zones
- [ ] ADR-024 (component model + access declarations) committed
- [ ] ADR-025 (scene serialization schema) committed

**Risk gate tripwires.**
- 🛑 If components contain logic (`Transform::lookAt()`) instead of data, refactor. Components are POD; logic goes in systems.
- 🛑 If two systems both write `Transform` without dependency declaration, you'll have a race in Phase 6. Fix the declarations now.
- 🛑 If serialization requires custom ImGui code per scene file, the inspector is too coupled. Use a per-component drawer registry.

**Time estimate.** **3–5 weeks** at 12 hr/week.

**ADRs touched.**
- ADR-024: Component model — POD components, system access declarations, scheduling rules (create)
- ADR-025: Scene serialization — binary + JSON, schema versioning, asset references via UUID (create)
- ADR-015: Hot-reload (**amend** — full mesh/texture reload, not shader-only)

**Tracy zones added.**
```cpp
ZoneScopedN("SceneLoad")
ZoneScopedN("SceneSerialize")
ZoneScopedN("TransformPropagate")
ZoneScopedN("Cull")
ZoneScopedN("BuildDrawLists")
ZoneScopedN("Inspector")
ZoneScopedN("System_<Name>")        // one per system
TracyPlotN("entities", n);
TracyPlotN("draw_calls_after_cull", n);
```

**Atomic tasks.**
1. Re-export EnTT via `core/`. Wrap in `scene/Scene.h`. (1d)
2. Define POD components: Transform, LocalTransform, WorldTransform, Hierarchy, MeshRenderer, Camera, Light, AnimatedMesh. (2d)
3. Component access declaration system: `reads<T>`/`writes<T>` collected per system. (2d)
4. Transform propagation: parent dirty → child world matrix. (2d)
5. Frustum culling system. (1d)
6. Build-draw-lists system: emits draw commands per-pass to frame graph. (2d)
7. Binary scene serialization: per-component serialize/deserialize. (3d)
8. JSON debug serialization (boost::pfr or hand-written). (2d)
9. Inspector: scene tree, per-component drawer registry, transform gizmo. (3d)
10. Mesh hot-reload integration: invalidates `MeshRenderer` cached PSO, swaps GPU buffers. (2d)
11. Texture hot-reload integration. (1d)
12. `samples/05_scene_walk`. (1d)
13. ADR-024, -025; amend ADR-015. (2d)

**Cross-phase notes.** Phase 6 needs the access-declaration table for thread-safe scheduling. Phase 8 scripting will read+write components via Lua bindings — must use entity IDs (per ADR-031), not raw pointers.

---

## Phase 6 — Physics interface + Jolt + character controller

**Goals.** Define `physics/` interface, integrate Jolt, build a character controller. **Establish the contact-listener thread-safety pattern (ADR-026) — this is the most-overlooked correctness issue in physics integration.**

**Scope (in).**
- `physics/IPhysicsWorld.h`, `IRigidBody`, `ICollisionShape` (Box/Sphere/Capsule/ConvexHull/Mesh), raycast/sweep/overlap APIs.
- `physics-jolt/` backend.
- `gameplay/` module standup: home for camera controllers, character controller, common reusable game components. Per design-doc §3 — keeps reusable game patterns out of `samples/`.
- ECS components: `RigidBody`, `Collider`, `CharacterController` (lives in `gameplay/`). Replaces fwd-decl from Phase 5.
- Contact listener with **thread-safe event queue**: Jolt callback runs on physics worker thread → enqueues event; main thread drains queue post-step. ADR-026.
- Character controller: capsule, ground-snap, slope handling.
- Physics step driven by jobs system using Jolt's `JobSystem` interface.
- Debug rendering: `IDebugRenderer` adapter into the renderer's debug-pass.

**Scope (out).**
- Skeletal physics / ragdolls (Phase 9 stretch).
- Soft-body, fluid (out of scope for v1).
- Vehicle physics (out of scope for v1).

**Deliverable demo.** `samples/06_physics_box_pile`: a character walks around a scene, knocks over a pile of dynamic boxes. Raycasts hit-test mouse clicks. ImGui debug overlay toggles physics-debug rendering. Tracy shows physics step on worker threads, contact events drained on main thread.

**Exit criteria checklist.**
- [ ] Falling boxes settle correctly (no jitter, no penetration)
- [ ] Raycasts work (mouse-click → hit-test world)
- [ ] Character controller traverses 30° slopes; 45° blocks; sticks to ground on stairs
- [ ] Contact events delivered on main thread, never directly from Jolt callback
- [ ] No data races in TSAN debug build (`-fsanitize=thread`)
- [ ] Physics step parallelizes across all workers (Tracy shows worker utilization)
- [ ] Debug rendering for shapes / contacts / character controller capsule
- [ ] Tracy: `PhysicsStep`, `BroadPhase`, `NarrowPhase`, `ContactSolve`, `ContactDrain`
- [ ] ADR-026 (contact listener thread safety) committed
- [ ] ADR-027 (physics interface design) committed

**Risk gate tripwires.**
- 🛑 If a contact listener directly modifies an ECS component, Phase 8 (Lua scripts in collision callbacks) will produce undetectable races. **Enforce queue pattern.**
- 🛑 If `ICollisionShape` exposes Jolt-specific shape types (e.g. height field) without a PhysX equivalent, the abstraction leaks. Add it as a capability flag or push to a Jolt-specific extension.
- 🛑 If contact callbacks pass `JPH::ContactManifold*` to game code, the second backend (PhysX in Phase 9) breaks. Use engine-native `ContactInfo` struct.
- 🛑 If physics step runs on the main thread while jobs go idle, you wired Jolt's `JobSystem` adapter wrong.

**Time estimate.** **4–6 weeks**.

**ADRs touched.**
- ADR-026: Contact listener thread safety — enqueue-on-callback, drain-on-main (**create — load-bearing**)
- ADR-027: Physics interface design — shape variants, callback model, raycast result struct (create)

**Tracy zones added.**
```cpp
ZoneScopedN("PhysicsStep")
ZoneScopedN("BroadPhase")
ZoneScopedN("NarrowPhase")
ZoneScopedN("ContactSolve")
ZoneScopedN("ContactDrain")
ZoneScopedN("Raycast")
ZoneScopedN("CharacterControllerUpdate")
TracyPlotN("rigid_bodies", n);
TracyPlotN("contact_events", n);
```

**Atomic tasks.**
1. Add Jolt to vcpkg manifest. (1d)
1b. Stand up `gameplay/` module (CMake target, public/private include split). Move flycam from `samples/` here as the first reusable controller. (1d)
2. Author `physics/IPhysicsWorld.h` interface: shapes, bodies, raycast. (2d)
3. Implement `physics-jolt/PhysicsWorld.cpp`: world creation, step. (2d)
4. Implement Jolt JobSystem adapter that delegates to engine `jobs/`. (2d)
5. Bind `RigidBody` ECS component to Jolt body. Sync transforms in `PrePhysics` system; read-back in `PostPhysics` system. (2d)
6. Implement contact-listener event queue (thread-safe MPSC). Drain on main thread post-step. (2d)
7. Character controller: capsule + Jolt's character-virtual; grounding, slope filter. (3d)
8. Raycast / sweep / overlap APIs. (2d)
9. Debug renderer: extract Jolt's `DebugRenderer` callbacks → render-debug-pass primitives. (2d)
10. `samples/06_physics_box_pile`. (1d)
11. ADR-026 (especially), -027. (2d)
12. TSAN CI lane: build a Linux debug variant with `-fsanitize=thread`; run `samples/06_*` for 30 seconds. (1d)

**Cross-phase notes.** Phase 8 Lua scripts in collision callbacks **must** go through the same drain queue. Physics also needs Phase 5's access-declaration pattern: `PrePhysics writes Transform`, `PostPhysics writes Transform`.

---

## Phase 7 — Audio + miniaudio + 3D positional

**Goals.** Define `audio/` interface, integrate miniaudio (single-header MIT, easy), 3D spatialization wired to scene transform.

**Scope (in).**
- `audio/IAudioEngine.h`, `ISound`, `IAudioSource`, `IListener`.
- `audio-miniaudio/` backend.
- ECS: `AudioSource` component (replaces Phase 5 forward-decl), `AudioListener` typically on the camera entity.
- 3D positional audio: distance attenuation, doppler optional.
- Mixer / bus / volume.

**Scope (out).**
- FMOD backend (Phase 9 — second-backend validation).
- DSP effects beyond reverb (out of scope for v1).
- Music streaming / cue system (out of scope for v1).

**Deliverable demo.** `samples/07_audio_walk`: walking around a scene, footstep sounds spatialized to character position, ambient ambience loop, music. Volume slider in ImGui.

**Exit criteria checklist.**
- [ ] Footsteps audibly attenuate with distance
- [ ] Listener orientation affects panning
- [ ] No audio thread starvation under load
- [ ] Hot-reload audio asset without crash
- [ ] Tracy: `AudioMix`, `AudioUpdate`
- [ ] ADR-028 (audio interface) committed

**Risk gate tripwires.**
- 🛑 If audio runs on the main thread, you'll get pops/glitches under load. miniaudio handles this; verify your wiring doesn't undo it.
- 🛑 If an `IAudioSource` exposes miniaudio types in its public interface, the FMOD backend (Phase 9) won't fit.

**Time estimate.** **1–3 weeks**. miniaudio is genuinely easy; the variable is 3D-listener-on-camera wiring.

**ADRs touched.**
- ADR-028: Audio interface — listener model, source pooling, format support (create)

**Tracy zones added.**
```cpp
ZoneScopedN("AudioUpdate")
ZoneScopedN("AudioMix")             // miniaudio internal — capture if accessible
TracyPlotN("audio_sources_active", n);
```

**Atomic tasks.**
1. Add miniaudio (header-only) to vendored deps. (0.5d)
2. Author `audio/IAudioEngine.h`. (1d)
3. Implement `audio-miniaudio/` backend. (2d)
4. ECS `AudioSource` + `AudioListener` components + system. (1d)
5. 3D positional + listener orientation. (1d)
6. `samples/07_audio_walk`. (1d)
7. ADR-028. (1d)

**Cross-phase notes.** Phase 8 demo game will trigger audio from Lua. Lua bindings must use entity-IDs.

---

## Phase 8 — Scripting (sol2/Lua) + demo game

**Goals.** Wire Lua via sol2 behind `IScriptHost`. **Bind via entity IDs, not raw pointers (ADR-031) — this is the most-overlooked correctness issue in scripting integration.** Build a small playable demo game.

**Scope (in).**
- `scripting/IScriptHost.h`, `IScriptInstance`.
- `scripting-lua/` via sol2.
- Bindings: math (vec3/quat/mat4), scene queries (entity by name, component get/set), input (action map), audio (play sound), physics (raycast). **All entity references via `EntityId` (uint64_t), never raw pointers.**
- Hot-reload of Lua scripts.
- Demo game: a small playable thing — third-person walking sim or top-down shooter. Pick one. Whichever you'll finish.
- Save/load slot, basic UI screens (ImGui-based for v1).

**Scope (out).**
- Visual scripting (out of scope for v1).
- Multiplayer (out of scope for v1).
- Console release (out of scope for v1).
- Steamworks integration (out of scope for v1).

**Deliverable demo.** `samples/99_demo_game`: a complete, playable thing. Hand it to a friend; they can play start-to-finish without instructions. Lua mods reload-able.

**Exit criteria checklist.**
- [ ] Demo game has a complete play arc (start screen → gameplay → end screen)
- [ ] Friend who hasn't seen the engine can play through without help
- [ ] Lua scripts hot-reload without crashing the game
- [ ] No raw pointer bindings — all entity references go through `EntityId`
- [ ] No data races in TSAN debug run for 5 minutes of play
- [ ] Audio, physics, animation, scene serialization all work in the demo
- [ ] Tracy zone for `LuaScriptUpdate`, `LuaScriptCallback`
- [ ] ADR-031 (Lua entity-ID binding) committed
- [ ] ADR-032 (script lifecycle and hot-reload) committed

**Risk gate tripwires.**
- 🛑 If you bind any C++ object by raw pointer (entity, component, mesh), Lua GC will produce undetectable crashes. **Audit every `lua_state.set_function` call.** Use `EntityId` + lookup, or weak handles.
- 🛑 If demo scope expands beyond what you can finish in 14 weeks, **cut**. A 5-minute walking sim shipped is better than a 30-minute shooter abandoned.
- 🛑 If sol2 bindings are slow (Tracy shows >2ms per script-tick on a small game), use `sol::function` caching or move hot logic to C++.

**Time estimate.** **8–14 weeks**. **High variance.** sol2 is 1–2 weeks; the demo is where solo projects historically get stuck.

**ADRs touched.**
- ADR-031: Lua entity-ID binding (no raw pointer to ECS entities/components) (**create — load-bearing**)
- ADR-032: Script lifecycle, hot-reload, sandbox (create)
- ADR-033: Demo game scope freeze (create — write before starting demo, don't expand)

**Tracy zones added.**
```cpp
ZoneScopedN("LuaScriptUpdate")
ZoneScopedN("LuaScriptCallback")
ZoneScopedN("LuaHotReload")
TracyPlotN("script_instances", n);
```

**Atomic tasks.**
1. Add Lua + sol2 to vcpkg manifest. (0.5d)
2. Author `scripting/IScriptHost.h`. (1d)
3. Implement `scripting-lua/ScriptHost.cpp` with sol2. (2d)
4. Bind core types: vec3, quat, mat4. (1d)
5. Bind entity model: `EntityId`, scene queries (find by name). Document in ADR-031 — no raw pointers. (2d)
6. Bind components: getters/setters (Transform, Health, etc) by EntityId. (3d)
7. Bind input action-map. (1d)
8. Bind audio play/stop. (1d)
9. Bind physics raycast. (1d)
10. Lua hot-reload: file watcher → atomic instance swap. (2d)
11. **ADR-033 — demo game scope.** Write before any demo code. Lock it. (1d)
12. Build demo game: gameplay loop, content, polish. (40–60d, **biggest variance**)
13. ADR-031, -032, -033. (2d)

**Cross-phase notes.** P9 (second-backend validation) will run the demo on PhysX/FMOD. If anything in the demo couples to Jolt/miniaudio specifically (it shouldn't), P9 catches it.

---

## Phase 9 — Validate second backends (PhysX + FMOD parity)

**Goals.** Add PhysX 5 alongside Jolt; add FMOD alongside miniaudio. Run the demo game with backend selection at build time. **This is the abstraction-was-real-or-leaky test.**

**Scope (in).**
- `physics-physx/` backend (PhysX 5, BSD-3 since 2022 — no licensing concern).
- `audio-fmod/` backend (FMOD Studio API).
- CMake build flag: `-DENGINE_PHYSICS_BACKEND=jolt|physx`, `-DENGINE_AUDIO_BACKEND=miniaudio|fmod`.
- Demo game runs identically on either selection.
- CI matrix expands to test all 4 combinations on each platform.

**Scope (out).**
- Editor (Phase 10).
- New game features.

**Deliverable demo.** `samples/99_demo_game` runs identically on `(Jolt, miniaudio)`, `(PhysX, miniaudio)`, `(Jolt, FMOD)`, `(PhysX, FMOD)`. Behavior parity (collision feel, audio levels) within tolerance.

**Exit criteria checklist.**
- [ ] All 4 combinations build on each platform
- [ ] Demo gameplay parity: same level completable in same general path with each combination
- [ ] No interface methods added to `IPhysicsWorld`/`IAudioEngine` since their initial creation
- [ ] Game code (in `gameplay/` or Lua) untouched by backend swap
- [ ] CI matrix expanded to include backend permutations (cost-aware: maybe only one platform per combo)
- [ ] ADR-027 amended with PhysX-discovered constraints
- [ ] ADR-028 amended with FMOD-discovered constraints

**Risk gate tripwires.**
- 🛑 If PhysX requires a new method on `IPhysicsWorld`, the abstraction was Jolt-shaped, not engine-shaped. ADR-027 amendment + abstract the right thing.
- 🛑 If FMOD's listener model differs incompatibly with miniaudio's, you have an `IListener` design issue. Refine.
- 🛑 If the demo plays differently (e.g. characters fall through floors on PhysX), you have a backend-dependent assumption (likely a default contact restitution or friction). Make it explicit on the engine-side struct.

**Time estimate.** **6–10 weeks**. PhysX integration is the larger half.

**ADRs touched.**
- ADR-027 (physics interface) — amend
- ADR-028 (audio interface) — amend
- ADR-034: Backend-selection at build time vs runtime (create — recommend build-time)

**Tracy zones added.**
- (No new zones — same names, different impls)

**Atomic tasks.**
1. Add PhysX 5 to deps (vcpkg or vendored — PhysX 5 is BSD-3). (1d)
2. Implement `physics-physx/PhysicsWorld.cpp` mirror of Jolt impl. (10d)
3. PhysX contact callbacks → engine event queue (per ADR-026). (2d)
4. CMake flag `ENGINE_PHYSICS_BACKEND`; switch implementations. (1d)
5. Add FMOD Studio API. (1d)
6. Implement `audio-fmod/AudioEngine.cpp`. (5d)
7. CMake flag `ENGINE_AUDIO_BACKEND`. (1d)
8. CI matrix expansion (cost-bounded). (2d)
9. Run demo with all 4 combos; fix divergences. (5–10d, variable)
10. Amend ADR-027, -028; create ADR-034. (2d)

**Cross-phase notes.** This phase is the proof that Phase 9 of the design doc was correct: abstractions are only real with two implementations. **Don't skip this** — it's the test, and the editor (Phase 10) will compound bugs that hide here.

---

## Phase 10 — Editor v0 + asset browser + scene editor + Tracy live

**Goals.** Stand up an in-engine editor app. Asset browser, scene tree, component inspector, material preview, Tracy live-capture window, in-engine RenderDoc trigger. **Strictly timeboxed**: per design-doc §8, this is the phase that destroys engines.

**Scope (in).**
- `editor/` standalone app: launches the engine, shows ImGui editor UI alongside the scene.
- Asset browser: thumbnail grid, search, drag-drop into scene.
- Scene editor: hierarchy tree, gizmo (translate/rotate/scale), undo/redo (per ADR-035).
- Component inspector with per-component drawers (extends Phase 5 inspector).
- Material preview shader sandbox.
- Live Tracy capture window.
- In-engine RenderDoc trigger.

**Scope (out).**
- Visual scripting / blueprint UI (out of scope for v1).
- Cinematic editor (out of scope for v1).
- Animation graph editor (Phase 11+).
- Particle editor (out of scope for v1).
- Plugin / extension API (out of scope for v1; design-doc §9 already deferred).

**Deliverable demo.** Open the editor, browse the asset library, drag a mesh into the scene, position it with the gizmo, preview the material, trigger a RenderDoc capture, save the scene, reload — everything persists. Total time per workflow: under 5 seconds.

**Exit criteria checklist.**
- [ ] Asset browser shows project assets with thumbnails
- [ ] Drag-drop spawns an entity with `MeshRenderer` + correct asset reference
- [ ] Gizmo works (translate, rotate, scale; world/local toggle)
- [ ] Undo/redo per ADR-035 (command-list pattern, bounded history)
- [ ] Scene save → close → reopen restores exactly
- [ ] Material preview compiles & previews on each backend
- [ ] Tracy live capture works alongside the editor
- [ ] RenderDoc trigger captures from inside the editor
- [ ] Tracy: `Editor`, `EditorAssetBrowser`, `EditorGizmo`, `EditorPreview`
- [ ] ADR-035 (undo/redo) committed
- [ ] ADR-036 (editor-engine boundary) committed

**Risk gate tripwires.**
- 🛑 If the editor needs new methods on engine subsystems (renderer, scene, assets), the boundary is wrong. Editor uses public engine APIs only.
- 🛑 If the editor scope expands (asset import wizards, prefab system, blueprint scripting), **cut**. Each is a multi-month subprojected. Time-box this phase.
- 🛑 If the editor's frame budget noticeably degrades the in-editor preview (<30 fps on demo scene), the editor is too coupled to the renderer; profile and optimize.

**Time estimate.** **10–16 weeks**. **Open-ended, must be timeboxed.** The single largest risk to the project per design-doc §8.

**ADRs touched.**
- ADR-035: Undo/redo — command-list pattern, bounded history (create)
- ADR-036: Editor-engine boundary — public-API only, no internal access (create)

**Tracy zones added.**
```cpp
ZoneScopedN("Editor")
ZoneScopedN("EditorAssetBrowser")
ZoneScopedN("EditorGizmo")
ZoneScopedN("EditorPreview")
ZoneScopedN("EditorCommand")
```

**Atomic tasks.**
1. **Write ADR-033-style scope-freeze ADR for editor before starting.** Lock it. (1d)
2. Editor app skeleton: launches engine, multi-viewport ImGui. (2d)
3. Asset browser: file enumeration, thumbnail rendering, drag-drop. (5d)
4. Scene tree + selection. (2d)
5. ImGuizmo integration: translate/rotate/scale. (2d)
6. Undo/redo command-list. (3d)
7. Material preview pane. (2d)
8. Live Tracy capture window (Tracy provides this; embed it). (2d)
9. RenderDoc trigger from editor menu. (1d)
10. Editor-internal scene save/load polish. (2d)
11. ADR-035, -036. (2d)

**Cross-phase notes.** This phase is open-ended; if you go past 16 weeks, ship v0 and move to a hypothetical Phase 11 for editor-vNext. The engine isn't the editor.

---

# Appendix A — Master ADR list

| # | Title | Created | Amended | Notes |
|---|---|---|---|---|
| ADR-001 | Engine namespace and naming convention | P0 | — | |
| ADR-002 | Coordinate system (handedness, up axis, units) | P0 | — | |
| ADR-003 | RHI handle strategy | P1 | P2, P2.5 | Opaque integer (gen+idx); no virtual on resources |
| ADR-004 | Shader pipeline (HLSL→SPIR-V→MSL) | P1 | — | |
| ADR-005 | Memory model — `BufferDesc::memory_type` | P1 | — | |
| ADR-006 | Bindless vs descriptor sets | P1 | P2, P3, P4 | Lean bindless |
| ADR-007 | Threading model — single vs main+render-thread split | P0 | P4 | **Load-bearing** |
| ADR-008 | Package manager — vcpkg vs Conan | P0 | — | |
| ADR-009 | Window/platform — GLFW vs SDL3 | P0 | — | |
| ADR-010 | Reversed-Z depth | P1 | P4 | Enum supports it; adopt P4 |
| ADR-011 | Jobs system — work-stealing pool, no fibers | P1 | — | |
| ADR-012 | Error handling in RHI — `std::expected` | P1 | — | |
| ADR-013 | Cross-backend parity definition (SSIM threshold) | P2 | — | |
| ADR-014 | GPU capture integration (RenderDoc, MTLCaptureManager, PIX) | P2 | P2.5 | |
| ADR-015 | Hot-reload scope — shader P3, full P5 | P3 | P5 | |
| ADR-016 | Asset GUID — `.meta` sidecar UUID | P3 | — | |
| ADR-017 | Runtime binary format — schema versioning, pointer fixup | P3 | — | |
| ADR-018 | Asset cooker invocation — CMake custom-command | P3 | — | |
| ADR-019 | Forward vs deferred renderer | P4 | — | Recommend forward+ |
| ADR-020 | Frame graph design | P4 | — | |
| ADR-021 | Material system | P4 | — | |
| ADR-022 | Shadow technique — CSM | P4 | — | |
| ADR-023 | Animation system | P4.5 | — | |
| ADR-024 | Component model + access declarations | P5 | — | |
| ADR-025 | Scene serialization schema | P5 | — | |
| ADR-026 | Contact-listener thread safety | P6 | — | **Load-bearing** |
| ADR-027 | Physics interface design | P6 | P9 | |
| ADR-028 | Audio interface | P7 | P9 | |
| ADR-031 | Lua entity-ID binding (no raw pointers) | P8 | — | **Load-bearing** |
| ADR-032 | Script lifecycle, hot-reload, sandbox | P8 | — | |
| ADR-033 | Demo game scope freeze | P8 | — | |
| ADR-034 | Backend-selection at build vs runtime | P9 | — | |
| ADR-035 | Undo/redo (command-list, bounded history) | P10 | — | |
| ADR-036 | Editor-engine boundary | P10 | — | |
| ADR-037 | Input — action-map model | P0 | — | design-doc §3 input module |
| ADR-038 | String type — `std::string` vs interned `Name` | P0 | — | design-doc §7.4 |
| ADR-039 | Engine-wide error policy | P0 | — | design-doc §7.5 (broader than ADR-012's RHI scope) |
| ADR-040 | Networking decision = no for v1, with assumptions documented | P0 | — | design-doc §7.10 mandates explicit decision even when "no" |
| ADR-041 | C++ hot-reload — Live++ / DLL / none | P0 | — | design-doc §7.11; distinct from asset hot-reload |
| ADR-042 | Interface ABI versioning + `[[deprecated]]` policy | P2 | P3, P6, P7 | Two-phase remove convention |

**The three load-bearing ADRs** (high cost if wrong, expensive to retrofit): ADR-007 (P0), ADR-026 (P6), ADR-031 (P8). Get these right first time.

---

# Appendix B — Tracy zone catalog (consolidated)

**Phase 1 zones (always-on):**
```cpp
FrameMark
Update / RhiAcquire / RhiRecord / RhiSubmit / RhiPresent
ShaderCompile / PipelineCreate / BufferUpload / TextureUpload
JobSubmit / JobExecute / WorkerSteal
TracyAllocN / TracyFreeN
```

**Phase 2/2.5 zones (multi-backend):**
```cpp
TracyVkZone(...)             // per-pass GPU
TracyD3D12Zone(...)
ValidationCheck / RenderDocBeginFrame / RenderDocEndFrame
GoldenFrameReadback / SSIMCompare
```

**Phase 3 zones (asset pipeline):**
```cpp
AssetCook / AssetLoad / AssetDecode / AssetUpload
ShaderRecompile / HotReloadApply / FileWatcherTick
TracyPlotN("assets_pending"), TracyPlotN("vram_used_mb"), TracyPlotN("upload_queue_depth")
```

**Phase 4 zones (renderer):**
```cpp
FrameGraphCompile / FrameGraphExecute
CullView / BuildDrawLists / MaterialBind
TracyVkZone("ShadowPass_Cascade0..3" / "OpaquePass" / "IBLDiffusePass" / "IBLSpecularPass" / "Tonemap")
TracyPlotN("draw_calls_opaque"), TracyPlotN("transient_vram_mb")
```

**Phase 4.5+ zones:**
```cpp
AnimationSample / BlendTreeEvaluate / PoseToMatrices
TracyVkZone("SkinningDispatch")
TracyPlotN("animated_chars")
```

**Phase 5+ zones (scene/ECS):**
```cpp
SceneLoad / SceneSerialize / TransformPropagate / Cull / BuildDrawLists / Inspector
System_<Name>           // one per system
TracyPlotN("entities"), TracyPlotN("draw_calls_after_cull")
```

**Phase 6+ zones (physics):**
```cpp
PhysicsStep / BroadPhase / NarrowPhase / ContactSolve / ContactDrain
Raycast / CharacterControllerUpdate
TracyPlotN("rigid_bodies"), TracyPlotN("contact_events")
```

**Phase 7+ zones (audio):**
```cpp
AudioUpdate / AudioMix
TracyPlotN("audio_sources_active")
```

**Phase 8+ zones (scripting):**
```cpp
LuaScriptUpdate / LuaScriptCallback / LuaHotReload
TracyPlotN("script_instances")
```

**Phase 10 zones (editor):**
```cpp
Editor / EditorAssetBrowser / EditorGizmo / EditorPreview / EditorCommand
```

---

# Appendix C — Risk register (top 10 cross-cutting risks)

| # | Risk | Mitigation owner phase | Trigger / detection | Severity |
|---|---|---|---|---|
| R1 | Threading-model decision deferred → P4 frame-graph blocked | P0 (ADR-007) | If P4 starts and ADR-007 isn't committed | **Critical** |
| R2 | Phase 1 RHI interface missing forward-design fields → P2 redesign | P1 (interface forward-design checklist) | New virtual method needed in P2 | **Critical** |
| R3 | Frame graph (P4) takes 2× estimate → schedule slip | P4 (write ADR-020 first; cut P4 IBL/soft shadows if late) | If 12 weeks elapsed and CSM still incomplete | **High** |
| R4 | Jolt contact callback races with ECS → undetectable bugs in P8 Lua | P6 (ADR-026 + TSAN CI) | TSAN reports race; Lua scripts crash randomly | **Critical** |
| R5 | Lua raw-pointer bindings → GC crashes during play | P8 (ADR-031 + binding audit) | Crash on entity destruction during play | **Critical** |
| R6 | Hot-reload race-on-destroy → use-after-free | P3 (deferred destroy via retired-resource list) | GPU hang or validation error after reload | High |
| R7 | DX12 yak-shaving stalls macOS work | P2.5 (escape hatch: ship without DX12) | If P2.5 takes >8 weeks alone | High |
| R8 | Demo game scope creep → 14 weeks slips to 30+ | P8 (ADR-033 scope freeze) | Demo features added after week 4 of P8 | High |
| R9 | Editor scope creep → engine never ships | P10 (timebox + ADR-036) | If P10 exceeds 16 weeks | High |
| R10 | macOS-only thinking — abstraction has Metal-shaped fields | P1, P2 (forward-design + 2nd-backend test) | New methods needed for Vulkan; SSIM falls | High |

---

# Appendix D — Cumulative timeline (ASCII Gantt at 12 hr/week)

```
Months:   1    2    3    4    5    6    7    8    9    10   11   12   13   14   15   16   17   18   19   20
P0       ██
P1            ████████████
P2                    ████████████
P2.5                              ██████████        (parallel with P3)
P3                                ████████████
P4                                          ██████████████████████████
P4.5                                                              ████████████
P5                                                                          ██████
P6                                                                                ████████
P7                                                                                        ██
P8                                                                                          ██████████████████
P9                                                                                                              ██████████
P10                                                                                                                           ██████████████
              [Playable demo at month 13–20] ─────────────────────────^                ^
              [Three-RHI engine + demo + parity] ───────────────────────────────────────^
              [Editor v0] ──────────────────────────────────────────────────────────────────────────────────────^
```

**Compressing the timeline.** Three levers, in order of usefulness:
1. **Skip DX12 from P2.5.** Ship Metal + Vulkan only. Saves 4–8 weeks.
2. **Skip P9 (second-backend validation).** Saves 6–10 weeks. **Risky** — abstraction may not be real. Don't skip if you intend to keep the engine alive past v1.
3. **Skip P10 (editor) entirely.** Use ImGui debug panels indefinitely. Saves 10–16 weeks. Acceptable if you only ship the demo game and never iterate on content.

**Expanding the timeline (if part-time pace drops below 12 hr/week).** Multiply all phase estimates by `12 / actual_weekly_hours`. At 6 hr/week, double everything: 26–40 months to playable demo.

---

# Appendix E — "When you have 4 hours" (re-entry checklist)

After time away from the project, before starting any new work:

**The 4-hour re-entry routine:**
1. Pull latest, build, run the **most recent sample** (or the demo game if Phase 8+). Verify it still works on your platform. (15 min)
2. Read the last 10 commit messages. Skim the most recent ADR if any was added. (15 min)
3. Open Tracy on the running sample. Scan for new zones since last session. (10 min)
4. Pick one task from this list, in priority order, and timebox it to 3 hours:

**Always-available tasks (regardless of phase):**
- [ ] Re-read ADRs created in the current phase. Are any "TBD" sections still open?
- [ ] Run the test suite (`ctest`). Investigate any flaky test.
- [ ] Run `clang-tidy` over a single module. Fix one warning.
- [ ] Open the current phase's atomic-task list. Pick the smallest unstarted task.
- [ ] Look at this phase's risk-gate tripwires. Verify none have fired since last session.
- [ ] Update Tracy zones in newly-touched code (any module without zones is invisible).
- [ ] Write a single doctest for an untested function in `core/` or `rhi/`.

**Phase-specific 3-hour starter tasks:**
- **P0:** finish a CMake target's public-headers split. (3h)
- **P1:** wire one Tracy zone you noticed missing. Or: add a new shader stage to the cross-compile test. (3h)
- **P2/2.5:** investigate the largest SSIM delta in the latest CI run. (3h)
- **P3:** add one asset type to the cooker. (3h)
- **P4:** add one Tracy GPU zone to the latest pass. Or: write an RFC for the next pass design. (3h)
- **P5:** write one component drawer for the inspector. (3h)
- **P6:** add one shape type via the abstraction (and test on Jolt). (3h)
- **P8:** profile Lua hot-path. Add `sol::function` caching where it pays. (3h)
- **P10:** build one editor command (paired with undo entry). (3h)

**Anti-task (don't do these on re-entry):**
- ❌ Refactor "while you're here." You've forgotten constraints; refactor with full context.
- ❌ Bump library versions. Save for a dedicated session.
- ❌ Delete code unless an ADR says so.
- ❌ Promise a deliverable to someone else this session.

---

**End of plan.** Companion files:
- Architectural design: [`game_engine_plan.md`](./game_engine_plan.md)
- Multi-LLM research synthesis: `~/.claude-octopus/results/probe-synthesis-1777770036.md`
