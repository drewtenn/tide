# ADR-0015: Hot-reload scope — shaders in P3, mesh/texture/material in P5+

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P3 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

`game_engine_plan.md:19` calls hot-reload a first-class tool: "These are what make the engine feel alive while you're building it." The Phase 3 scope (`IMPLEMENTATION_PLAN.md:411`) calls for an asset pipeline that supports it, but a literal reading of "hot-reload everything" expands the phase past its 5–8 week budget.

The four candidate asset kinds in Phase 3 are shaders, meshes, textures, and (later) materials. They differ in how hard the hot-swap is to get right:

| Kind | Hot-swap cost | Runtime references | GPU lifetime |
|---|---|---|---|
| **Shader** | Re-compile, re-create PSO, swap pointer atomically. No vertex / index buffers tied to the shader instance — the PSO is rebound per draw. | One reference per material (in P3, one per pipeline). | PSO release after fence. Trivial — no in-flight buffer dependencies. |
| **Texture** | Re-cook, re-upload, swap descriptor. Every draw that referenced the old texture is in-flight. | Many — every material / mesh / sample using it. | GPU may still be sampling. Need deferred destroy gated on a fence. |
| **Mesh** | Re-cook, re-upload vertex+index buffers, swap pointer. Every command buffer that recorded a draw against the old buffers is in-flight. | One per `MeshRenderer`. | Same deferred-destroy problem, plus draw-call invalidation. |
| **Material** | Re-cook, re-bind. References both shaders and textures. | One per `MeshRenderer` (P5+). | Compound problem. |

Shader hot-reload is the *highest-ROI subset* (the shader iteration loop is the single biggest dev-velocity win in graphics work) *and* the easiest to get right (no buffer lifetime; PSO release is a one-fence dance).

Mesh / texture / material hot-reload share a harder problem: a deferred-destroy infrastructure gated on per-frame retired-resource lists, plus a scene-aware invalidation pass (so `MeshRenderer` re-binds the new handle). The retired-resource list comes naturally with the frame graph in Phase 4. The scene-aware invalidation comes naturally with the scene module in Phase 5. Trying to land both in P3, before the systems they depend on exist, means building stub versions that get rewritten in P4/P5.

## Decision

**Phase 3 ships shader hot-reload only.** Mesh / texture / material hot-reload land in **Phase 5** alongside the scene serialization work, where the deferred-destroy infrastructure (P4 frame graph) and the asset-reference invalidation pathway (P5 scene module) are already in place.

Phase 3's hot-reload pipeline:

```
filesystem watcher (FSEvents on macOS, P3 Metal-only)
  → on shader source change: re-cook (HLSL → SPIR-V → MSL)
  → on cooker success: create new PSO via IDevice
  → atomic-swap PSO handle (std::atomic<PipelineHandle>)
  → release old PSO after one frame fence
```

Mesh and texture loads in Phase 3 are **fire-once on startup**. Editing the source asset requires a restart. The asset-cooker watching the source for re-cook is allowed (so the cooked output stays current); the runtime side just won't reload it.

The interface in `assets/IAssetLoader` exposes a `reload()` hook from day one — implementations return `Unsupported` for non-shader types in P3, and the Phase 5 expansion is then additive (no breaking change to consumers).

## Alternatives considered

- **Full hot-reload in P3** (mesh, texture, material, shader). Rejected: requires the frame graph's retired-resource list (Phase 4) and the scene's reference-invalidation pathway (Phase 5) to exist as preconditions. Building a P3-only stand-in for both is throwaway code that also fails to actually work for the mesh/texture cases without the missing pieces.
- **No hot-reload at all in P3** (defer everything to P5). Rejected: shader iteration is the single most painful loop without hot-reload — every shader edit becomes a multi-second app restart for the rest of P3 + P4 development. The cost-of-not-having-it is exactly the dev-velocity tax `game_engine_plan.md:19` warns against. Shader hot-reload is also the easy one to get right, so deferring it would be paying the cost without spending the budget.
- **Mesh hot-reload but not texture/material.** Rejected: the deferred-destroy problem is the same shape for mesh as for texture, so solving it costs the full P3+P4 frame-graph integration. If we're paying the price, we should pay it once and get all three; if we're not paying, we should ship none of them and confine P3 to shaders.

## Consequences

**Positive.**
- P3 ships within budget. Shader hot-reload is ~3 days of work (`IMPLEMENTATION_PLAN.md:485` task 10), not weeks.
- The shader iteration loop becomes <2 seconds round-trip from disk save → frame on screen, which is a genuine quality-of-life win for all subsequent phases.
- No throwaway stand-ins. The deferred-destroy infrastructure is built once in P4, the scene-invalidation pathway is built once in P5, and full hot-reload lights up cleanly when both are in place.

**Negative / accepted costs.**
- Mesh / texture iteration in P3–P4 requires a restart. Painful for testing the asset cooker but mostly inert in practice — most mesh/texture work in those phases is one-time-import-then-render.
- The `IAssetLoader::reload()` interface is present from P3 but only one implementation actually does anything until P5. Mild architectural surface that doesn't earn its keep until later. Accepted — the alternative is a breaking interface change in P5 (which would bump `kAbiVersion`).

**Reversibility.** Easy in either direction. Adding mesh/texture/material reload sooner is a P5 task that gets pulled forward; restricting scope further (shader-only forever) means deleting the `reload()` hook — both are localized changes.

## Forward-design hooks

- **`IAssetLoader::reload(handle)` is part of the P3 ABI.** Even though only the shader loader implements it usefully, every concrete loader (`MeshLoader`, `TextureLoader`, future `MaterialLoader`) declares it from day one and returns `expected<void, AssetError::Unsupported>` for now. P5 fills in the bodies; no interface change.
- **Filesystem watcher abstraction is general-purpose.** `assets/FileWatcher.h` (`IMPLEMENTATION_PLAN.md:484`) does *not* know it's for shaders specifically. It reports source-asset changes by UUID; the dispatch table decides what to do. P3 dispatch table only routes shaders to the re-cook+swap pipeline. P5 adds mesh/texture/material entries to the same table.
- **Atomic PSO swap pattern is the template.** The mesh/texture/material P5 implementation uses the same shape: `std::atomic<Handle<Tag>>` for the live handle, deferred destroy of the old handle after a frame fence. `MeshRenderer` and friends in P5 read the atomic per-frame.
- **Shader hot-reload tests must work cross-backend from day one.** Even though P3 is Metal-only, the test harness (`samples/02_textured_mesh` per `IMPLEMENTATION_PLAN.md:429`) drives the reload path through `IDevice` only — no Metal-specific code in the test. When P2 eventually lands, the test runs unchanged.
- **No re-cook in the runtime.** The shader hot-reload pipeline calls *out* to the cooker process (or in-process cooker library). The runtime never directly invokes DXC/SPIRV-Cross — same rule as ADR-0018's tripwire on source-format parsers in the runtime.

## Related ADRs

- ADR-0004: Shader pipeline (HLSL → SPIR-V → MSL via DXC + SPIRV-Cross). The hot-reload path reuses the same toolchain; only the trigger differs.
- ADR-0017 (this batch): Runtime binary format. Hot-reload swap requires the new artifact to be in the format the runtime already mmap'd — same schema version or no swap.
- ADR-0018 (this batch): Asset cooker — invoked from the file watcher in dev, not just from CMake.
- ADR-0041: C++ hot-reload (none for P0–P7). This ADR is asset hot-reload; that one is *code* hot-reload. Different decision, different ADR.
- ADR-0042: Interface ABI versioning. Adding mesh/texture reload bodies in P5 is non-breaking (does not bump `kAbiVersion`); removing the `Unsupported` return path *would* bump.
