# ADR-0016: Asset GUID — UUID v4 in `.meta` sidecar files, version-controlled

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P3 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

`game_engine_plan.md:302` flagged this as Open Question 8 from the design phase: how do we identify an asset across renames, edits, and folder reorganizations? The downstream consumers in Phase 5 (scene serialization), Phase 4 (renderer materials), and Phase 8 (script-loadable assets) all need a stable identifier they can store in serialized scene files. If that identifier is not stable across reasonable source-tree refactors, scene files break silently — `IMPLEMENTATION_PLAN.md:450` calls this out as a hard tripwire: *"If asset identity is unstable (rename → handle invalidation), scenes will be permanently broken in Phase 5."*

There are three viable shapes:

1. **Path-based** (the asset's relative path *is* its identity, e.g. `assets/meshes/sponza/floor.gltf`).
2. **Content-hash-based** (the asset's identity is a hash of its source bytes).
3. **Explicit UUID stored in a sidecar** (a `.meta` file alongside the source, holding a generated UUID that never changes).

Each handles the three churn events (edit, rename, move) differently:

| Strategy | Edit source bytes | Rename file | Move directory |
|---|---|---|---|
| Path | identity preserved | **identity lost** — every reference broken | **identity lost** |
| Content hash | **identity changes every save** | identity preserved if bytes identical | identity preserved if bytes identical |
| Sidecar UUID | identity preserved | identity preserved | identity preserved |

Path-based fails the rename case, which is the *most common* refactor. Content-hash fails the edit case, which is the *most common* operation, period. Sidecar-UUID is the only option that survives all three.

The cost of sidecar-UUID is one extra file per asset in the source tree, which must be (a) generated automatically when a new asset is added, (b) committed to version control alongside the source, and (c) kept in sync with renames. (c) is non-trivial — `mv foo.gltf bar.gltf` without moving `foo.gltf.meta` orphans the UUID.

## Decision

**Asset identity is a 16-byte UUID v4 stored in a `.meta` sidecar file alongside each source asset, version-controlled.**

Concrete shape:

```
assets/
  meshes/
    sponza/
      floor.gltf
      floor.gltf.meta        ← UUID lives here, committed to git
      column.gltf
      column.gltf.meta
  textures/
    floor_albedo.png
    floor_albedo.png.meta
```

`.meta` file format (small JSON, written and read by the cooker — never the runtime):

```json
{
  "schema": 1,
  "uuid": "f4e2c1a8-5b6d-4e9f-8a3c-7b2d1e0f9c8b",
  "kind": "mesh",
  "cooker_hints": {
    "tangent_space": "mikktspace",
    "compress_textures": true
  }
}
```

The runtime never reads `.meta` files. The cooker reads them, builds an in-memory UUID-to-cooked-output table, and emits a `manifest.bin` (binary format per ADR-0017) that the runtime mmaps on startup. Runtime lookups are by UUID; the manifest does the UUID → cooked-file mapping.

**UUID generation.** The cooker auto-creates a missing `.meta` file when it first sees a source asset without one, generating a fresh UUID v4. The author then commits it. A linter (added by P5) flags source assets without sibling `.meta` files in CI to prevent the "forgot to commit the sidecar" failure mode.

**Rename handling.** `mv foo.gltf bar.gltf` requires `mv foo.gltf.meta bar.gltf.meta`. The cooker emits a hard error if it finds a source asset whose adjacent `.meta` is missing, listing other orphaned `.meta` files as suggestions. A future tooling improvement is `tide-asset-mv` that does both moves atomically; not blocking for P3.

**Cross-asset references.** Materials (P5+) reference textures by UUID. Scene files (P5) reference meshes / textures / materials by UUID. The on-disk format for these references is the raw 16-byte UUID, not a string — string parsing in scene load was rejected for the same reason ADR-0017 rejects JSON parsing in asset load.

## Alternatives considered

- **Path-based identity.** Rejected for the load-bearing reason at the top of the context: every rename or directory reorganization invalidates every reference. Phase 5's scene files would break silently when assets are reorganized — exactly the failure mode `IMPLEMENTATION_PLAN.md:450` flags as a hard tripwire. Path is fine as a *hint* (the cooker uses it for diagnostics), but never as identity.
- **Content-hash identity.** Rejected because every edit to the source bytes — adding a vertex, retouching a texel, adjusting an exported animation — would invalidate every reference. The hot-reload pathway in ADR-0015 specifically depends on the identity surviving an edit, since the reload contract is "same UUID, new bytes." Content-hash also forces the runtime to wait until the cooker finishes hashing before any handle is even nameable, which complicates async load ordering.
- **UUID embedded in source asset.** Rejected because most source formats don't have a place for it: `.png` has limited metadata and tools strip it, `.gltf` has `extras` but not all DCC tools preserve it on export, `.hlsl` is just text. Sidecar `.meta` is the only consistent home that works across every source format we'll touch.
- **Database-backed asset registry** (single SQLite or JSON file mapping path → UUID). Rejected because it serializes all asset additions through one file → merge conflicts on every PR that touches a new asset. Per-asset sidecars are git-friendly: adding `floor.gltf.meta` only conflicts with another simultaneous addition of *the same file*, which is a real conflict, not a tooling artifact.

## Consequences

**Positive.**
- Identity survives all three churn events (edit, rename, move) — the only option that does.
- Distributed-merge friendly: per-asset sidecars produce per-asset diffs, no central registry to bottleneck.
- Trivial to introspect: `cat foo.gltf.meta` shows the UUID. No DB query, no path-resolution logic.
- The `.meta` file is also the natural home for cooker hints (tangent-space algorithm, texture compression mode, mip-bias) — keeps per-asset config local to the asset.

**Negative / accepted costs.**
- Doubles the file count in `assets/`. An order of magnitude noisier in directory listings. Accepted — the protection against silent reference breakage is worth it.
- Manual rename without `mv`-ing the sidecar is a footgun. Mitigated by the cooker hard-erroring on orphaned `.meta` files (any `.meta` whose source-asset path is missing). Future `tide-asset-mv` tool is the proper fix.
- UUID v4 means UUIDs are not lexicographically meaningful. We can't sort assets by ID and get any useful order. Accepted — UUIDs are for identity, not ordering. Sort by path for human-facing tools, sort by UUID for binary lookups.

**Reversibility.** Multi-week retrofit. Switching to a different identity scheme later means rewriting every cooked artifact's manifest entry, every scene file's UUID slot, and the runtime's lookup path. The good news is the runtime's UUID type is opaque — it could conceivably be replaced by a hash without runtime changes, just a re-cook of every asset. The serialized scene files are the load-bearing constraint; they store raw UUIDs.

## Forward-design hooks

- **`tide::assets::Uuid` is the public type.** Defined in `engine/assets/include/tide/assets/Uuid.h` as a 16-byte POD with `uuid_v4()` factory. Equality, hashing, and `from_string`/`to_string` round-trip via the canonical `8-4-4-4-12` hex form. **No exposure of the underlying bytes ordering** — clients never `.bytes[i]` because we may switch UUID encoding (v4 → ulid, etc.) and the bytes shouldn't be load-bearing.
- **The runtime never opens `.meta` files.** The cooker reads `.meta`, the cooker writes `manifest.bin`, the runtime mmaps `manifest.bin`. If runtime code starts to `#include <fstream>` and parse JSON, ADR-0018's tripwire fires.
- **Renames go through a tool (eventually).** P3 ships a hard-error-on-orphan check. P5 adds `tide-asset-mv` that moves source + sidecar atomically. Manual `mv` of source-without-sidecar is allowed (cooker will catch it) but not recommended.
- **Cooker hints in `.meta` are non-identity.** Changing a hint (e.g. flipping `compress_textures`) re-cooks the asset but does *not* change its UUID. Identity is independent of how the asset is cooked.
- **No identity stripping for builds.** Even shipped builds carry the manifest + UUIDs. Stripping UUIDs from final builds (e.g. replacing them with sequential integers) is a tempting "build-size" optimization that destroys the diagnostic value of "which asset is broken in this customer crash dump." Don't do it.

## Related ADRs

- ADR-0017: Runtime binary format. The manifest is in this format; cooked artifacts reference each other by UUID inside it.
- ADR-0018: Asset cooker. The cooker is the only tool that touches `.meta` files; the runtime is forbidden to.
- ADR-0015: Hot-reload. The reload contract is "same UUID, new bytes" — this ADR is what makes that contract well-formed.
- ADR-0042: Interface ABI versioning. Changes to `tide::assets::Uuid`'s public surface bump `kAbiVersion`.
- ADR-0038: String type. UUIDs are not strings; do not use `std::string` for storage even though `to_string()` exists for debug.
