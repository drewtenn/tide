# ADR-0002: Coordinate system, handedness, units

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

Every coordinate convention works mathematically; what matters is consistency with the asset pipeline (Phase 3 imports glTF / Assimp), with the rendering API conventions (Metal/Vulkan/DX12 differ on NDC), and with the dominant tooling (Blender, ImGuizmo).

Picking now avoids a per-asset transform flip and a class of physics-vs-rendering coordinate bugs that are expensive to diagnose later.

## Decision

- **Right-handed** coordinate system.
- **+Y is up.**
- **−Z is forward** (camera looks down −Z; objects in front have negative Z).
- **+X is right.**
- **Units: meters.** Default character height 1.8m; default world ground plane at Y=0.
- **Rotation representation:** quaternions in hot paths and serialization. Euler (degrees, ZYX = yaw-pitch-roll) only at user-facing tools and inspector inputs.
- **NDC convention:** the renderer adapts per-API (Metal/Vulkan use [0, 1] depth; DX12 uses [0, 1] depth; OpenGL would use [−1, 1] but we don't ship GL). The engine's clip-space convention is "depth in [0, 1], Y down in framebuffer", and `IDevice` exposes a viewport flip helper for backends that need it.

## Alternatives considered

- **Left-handed +Y up −Z forward (Unity).** Flipping every imported glTF mesh wastes asset-pipeline cycles.
- **Right-handed +Z up (Blender export default, Unreal).** Z-up is unambiguous but disagrees with glTF and with most modern rendering tutorials. Blender's `+Y Forward, +Z Up` glTF export option converts to our convention.
- **Centimeters as units (Unreal).** Forces float scale at every physics interaction; meters match Jolt and PhysX defaults.

## Consequences

**Positive.**
- glTF imports require no flip.
- Jolt physics (Phase 6) uses meters; no conversion at every API call.
- ImGuizmo, RenderDoc, and most tutorial code assume this convention.

**Negative / accepted costs.**
- −Z forward can confuse developers coming from DirectX (left-handed +Z forward). Document the convention prominently in the renderer module README.
- Animation re-targeting from Mixamo (left-handed Y-up) requires a per-clip mirror flag at import time.

**Reversibility.** Locked once the asset cooker (Phase 3) lands. Trivial in P0; multi-week if changed in P5+.

## Forward-design hooks

- `core/Math.h` (P0) defines `Vec3 forward()`, `Vec3 up()`, `Vec3 right()` constants. All gameplay and rendering code uses these — never literal `Vec3(0,0,-1)`.
- The asset cooker (P3) emits a `coord_system` field in the binary asset header so a future migration can detect old assets.

## Related ADRs

- (Future) ADR-0019: NDC mapping and viewport flip per RHI backend.
- (Future) ADR-0020: Asset import pipeline conventions.
