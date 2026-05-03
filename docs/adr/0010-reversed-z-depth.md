# ADR-0010: Reversed-Z depth — enum supports it now, adopt in Phase 4

**Status:** Accepted (deferred adoption)
**Date:** 2026-05-03
**Phase:** P1 (forward-design); P4 (adoption)
**Deciders:** Solo dev (Drew)

## Context

The traditional depth-buffer convention uses `near = 0.0`, `far = 1.0`, with `LessOrEqual` as the depth-test comparison. Geometry closer to the camera writes lower depth values; the test `frag_depth <= depth_buffer` accepts the new fragment when it's nearer.

This convention has a well-known precision problem: the floating-point density curve concentrates precision near 0.0, but a perspective-projected NDC pushes most scene depth toward 1.0. Result: precision is *worst* where scenes need it *most* (in the middle distance), causing z-fighting in shadow maps, decals, and translucent overlays.

**Reversed-Z** swaps the convention: `near = 1.0`, `far = 0.0`, with `Greater` (or `GreaterOrEqual`) as the comparison, and `clear_depth = 0.0`. The float exponent's density now matches NDC's bias toward the far plane — precision is best where the scene's actual depth-fighting risk is highest. This is the modern AAA convention (Frostbite, Naughty Dog's GDC talks, Doom Eternal's renderer).

The cost of reversed-Z is non-zero:
- **Pipeline state:** all materials must declare `DepthCompare::Greater(OrEqual)` instead of `Less(OrEqual)`.
- **Render pass:** `ClearDepthStencilValue::depth = 0.0f` instead of `1.0f`.
- **Camera math:** projection matrices need to swap the depth mapping (or use the swap matrix, depending on the math library convention).
- **Vulkan only:** requires `VK_KHR_depth_clip_control` (Vulkan 1.0 ext, core 1.4) or fragment-shader inversion. Metal and DX12 are convention-agnostic at the API level.

## Decision

**Phase 1's RHI surface declares `DepthCompare::Greater` and `DepthCompare::GreaterOrEqual` as first-class enum values, alongside the traditional `Less` / `LessOrEqual`. Phase 1 samples and pipelines use the traditional convention. Phase 4 (frame graph + materials + shadow maps) is the adoption point — by then the engine has the systemic depth budget that reversed-Z is meant to protect.**

Concrete shape (`engine/rhi/include/tide/rhi/Descriptors.h`):

```cpp
enum class DepthCompare : uint8_t {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,         // reversed-Z
    NotEqual,
    GreaterOrEqual,  // reversed-Z
    Always,
};
```

The enum is exhaustive — every depth-test that any backend supports is reachable. Metal's `to_mtl_compare()` translation handles all eight values (and the symmetric `CompareOp` for samplers, see ADR-0005's sampler discussion).

## Alternatives considered

- **Adopt reversed-Z now, in Phase 1.** Rejected:
  - Phase 1 has no scene depth budget to protect (the textured quad is 2D; the offscreen-hash sample doesn't use depth at all).
  - The accompanying camera-math + projection-matrix update is touching territory we don't have yet (no camera system; no math conventions for projection matrices in `engine/core/Math.h`).
  - Without shadow maps or decals, the precision win is invisible — adopting it now is pure tax.
- **Don't put `Greater`/`GreaterOrEqual` in the enum yet.** Rejected:
  - Adding enum values later is an ABI break for downstream consumers (descriptor-set layouts and pipeline descs persist as data structures).
  - Forward-design discipline: enums are cheap to declare, expensive to add to.
- **Make reversed-Z a per-pipeline opt-in, with a global default elsewhere.** Rejected: pipelines and render passes need to agree (clear value, depth comparison, camera math). A single project-wide convention is the right granularity; making it per-pipeline invites mismatched pairs.

## Consequences

**Positive.**
- Phase 4's adoption is "swap the projection matrix builder + change every PSO from `LessOrEqual` to `GreaterOrEqual` + change every render pass clear depth from 1.0 to 0.0" — a focused day of work, not a cross-cutting refactor.
- The enum stays stable: existing consumers don't break when reversed-Z lands.
- The Vulkan port (Phase 2) discovers the `VK_KHR_depth_clip_control` requirement before adoption, not during.

**Negative / accepted costs.**
- Phase 1 / 2 / 3 ship with the traditional convention. Any z-fighting users hit during those phases is "as expected, fixed in Phase 4". Documented.
- The presence of `Greater`/`GreaterOrEqual` in the enum without a project that uses them creates a discoverability question for code-readers. Inline comments on each enum value make the intent explicit.
- A future consumer (e.g. a debug-overlay renderer) that picks `Greater` mid-Phase-2 would silently break against `LessOrEqual` materials. Phase 4 amendment: lint that all PSOs in a single render pass use the same depth comparison family.

**Reversibility.** Trivial. Removing the unused enum values is a one-line change; flipping the convention is the multi-touch commit Phase 4 plans for.

## Forward-design hooks

- **The enum is exhaustive — never add a new comparison without a backend translation.** `to_mtl_compare()` covers all eight values today; future backends must also.
- **`ClearDepthStencilValue::depth` defaults to 1.0.** When Phase 4 adopts reversed-Z, the default flips to 0.0 — a single change in the default-init. All existing pipelines that explicitly clear to 1.0 will continue to do so until they're updated.
- **Sampler `CompareOp` mirrors `DepthCompare` value-for-value.** Same enumerators, same numeric meaning. Don't let the two diverge — a sampler used for hardware shadow comparison must agree with the depth-buffer's convention.
- **Phase 4 ADR amendment.** When reversed-Z lands in Phase 4, write ADR-0010-amend (or a new ADR if the scope is broader) documenting:
  - The math-library helper that produces a reversed-Z perspective matrix.
  - The lint/CI rule that catches mismatched depth comparisons within a render pass.
  - Vulkan's `VK_KHR_depth_clip_control` adoption (or the fragment-shader fallback if a target driver lacks it).

## Related ADRs

- ADR-0003: RHI handle strategy — pipeline state objects carry the depth comparison; the handle is the consumer's view.
- ADR-0007: Threading model — pipeline creation is callable from any thread.
- (Future) ADR-0010-amend / ADR-0042+: Phase 4 frame graph + reversed-Z adoption.
