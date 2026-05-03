# Architecture Decision Records

ADRs document irrevocable-or-expensive decisions made during the engine's development. Each ADR is one file: status, context, decision, alternatives, consequences, reversibility, and forward-design hooks.

The companion design doc (`game_engine_plan.md`) stays the architectural source of truth; ADRs document the *choices* made within that frame.

## Index — Phase 0 ADRs

| # | Title | Status | Phase | Load-bearing? |
|---|---|---|---|---|
| [0001](./0001-engine-name-and-namespace.md) | Engine name and namespace (`tide`) | Accepted | P0 | — |
| [0002](./0002-coordinate-system.md) | Coordinate system, handedness, units | Accepted | P0 | — |
| [0007](./0007-threading-model.md) | Threading model — single-threaded with P4 contractual split | Accepted | P0 | **yes** |
| [0008](./0008-package-manager.md) | Package manager — vcpkg manifest mode | Accepted | P0 | **yes** |
| [0009](./0009-window-platform-library.md) | Window/platform library — GLFW 3.4 | Accepted | P0 | **yes** |
| [0037](./0037-input-action-map.md) | Input model — action-map over event queue | Accepted | P0 | — |
| [0038](./0038-string-type.md) | String type — `std::string` now, `tide::Name` in P3 | Accepted | P0 | — |
| [0039](./0039-error-policy.md) | Error policy — `std::expected<T,E>` (no exceptions); CI Linux runner amended to ubuntu-24.04 | Accepted | P0 | — |
| [0040](./0040-networking-deferral.md) | Networking — explicitly out of scope for v1 | Accepted | P0 | — |
| [0041](./0041-cpp-hot-reload.md) | C++ hot-reload — none for P0–P7 | Accepted | P0 | — |

## Index — Phase 1 ADRs

| # | Title | Status | Phase | Load-bearing? |
|---|---|---|---|---|
| [0003](./0003-rhi-handle-strategy.md) | RHI handle strategy — opaque integer (generation+index), no virtual on resources | Accepted | P1 | **yes** |
| [0004](./0004-shader-pipeline.md) | Shader pipeline — HLSL → SPIR-V (DXC) → MSL (SPIRV-Cross), CMake-driven | Accepted | P1 | **yes** |
| [0005](./0005-memory-model.md) | Memory model — `BufferDesc::memory_type` enum (DeviceLocal / Upload / Readback) | Accepted | P1 | **yes** |
| [0006](./0006-bindless-vs-descriptor-sets.md) | Bindless vs descriptor sets — descriptor sets in P1, bindless deferred (lean) | Accepted | P1 | **yes** |
| [0010](./0010-reversed-z-depth.md) | Reversed-Z depth — enum supports it now, adopt in P4 | Accepted | P1/P4 | — |
| [0011](./0011-jobs-system.md) | Jobs system — work-stealing thread pool, no fibers | Accepted | P1 | **yes** |
| [0012](./0012-rhi-error-handling.md) | RHI error handling — `expected<T, RhiError>` + recording-error accumulator | Accepted | P1 | — |

## Index — Phase 2-lite ADRs (pulled forward into Phase 3)

Phase 2 (Vulkan) and Phase 2.5 (DX12) are deferred. The engine continues on Metal-only through P3+. This ADR is the only Phase-2 item still owed; golden-frame CI on Metal already shipped from P1 (`tests/CMakeLists.txt:83`).

| # | Title | Status | Phase | Load-bearing? |
|---|---|---|---|---|
| [0042](./0042-interface-abi-versioning.md) | Interface ABI versioning + `[[deprecated]]` two-phase removal | Accepted | P2-lite | **yes** |

## Index — Phase 3 ADRs

| # | Title | Status | Phase | Load-bearing? |
|---|---|---|---|---|
| [0015](./0015-hot-reload-scope.md) | Hot-reload scope — shaders in P3, mesh/texture/material in P5+ | Accepted | P3 | **yes** |
| [0016](./0016-asset-guid-strategy.md) | Asset GUID — UUID v4 in `.meta` sidecar files, version-controlled | Accepted | P3 | **yes** |
| [0017](./0017-runtime-binary-format.md) | Runtime binary format — self-relative offsets, schema-versioned, mmap-friendly | Accepted | P3 | **yes** |
| [0018](./0018-asset-cooker.md) | Asset cooker — standalone CLI invoked by CMake, content-hash cache | Accepted | P3 | **yes** |

## Reserved numbers (future ADRs from the implementation plan)

ADRs 0013–0014, 0019–0036, 0043–onward are reserved by the implementation plan for decisions that land in later phases. Do not reuse these numbers.

## Authoring template

Use [`0000-template.md`](./0000-template.md) as the starting point. ADR numbers are 4-digit, zero-padded, monotonic.
