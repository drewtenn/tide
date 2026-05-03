# ADR-0018: Asset cooker — standalone CLI invoked by CMake, content-hash cache

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P3 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

`game_engine_plan.md:176` and `IMPLEMENTATION_PLAN.md:434` are explicit: the cooker is a separate offline tool, never inline at runtime. *"If runtime code parses any source format directly (cgltf, stb_image, JSON), the cooker isn't doing its job"* (`IMPLEMENTATION_PLAN.md:447`) is a hard tripwire.

The decisions left open for this ADR:

1. **Process model.** Standalone executable invoked per-asset, or long-running daemon, or in-process library called by another tool?
2. **Build integration.** CMake `add_custom_command` per asset, or one cooker step that takes a manifest of all assets?
3. **Caching.** What is the cache key? Where does the cache live?
4. **Determinism.** Same input → same bytes? (Required for build reproducibility and cache correctness.)
5. **Source format dependencies.** cgltf and stb_image are header-only — link them statically into the cooker; what's the policy when a future kind needs a heavier dep (e.g. assimp for FBX)?

## Decision

**`tide-cooker` is a standalone CLI executable, invoked by CMake `add_custom_command`, one invocation per source asset, with a content-hash cache keyed on `(source_bytes ⊕ cooker_version ⊕ hint_set)`.**

### Process model

Standalone executable. Each invocation cooks one source asset to one cooked artifact. Lives at `tools/asset-cooker/`. Built as part of the project's CMake graph (target `tide_cooker`); produced binary lives in the build tree.

Invocation contract:

```
tide-cooker --in  assets/meshes/sponza/floor.gltf
            --out build/cooked/<uuid>.tide
            --kind mesh
            --cache build/cooked/.cache/
            [--hints assets/meshes/sponza/floor.gltf.meta]
            [--cooker-version-stamp build/.cooker-version]
```

The cooker reads the source asset, the `.meta` sidecar (for UUID + hints), and emits a cooked artifact in the runtime binary format (ADR-0017). It exits 0 on success, nonzero on failure with a structured error message on stderr.

### Build integration

CMake creates one `add_custom_command` per source asset, with `tide_cooker` as the dependency target. The output cooked artifact is named by UUID, not source path, so renames don't break the build:

```cmake
# Conceptual; helper function lives in cmake/CookAsset.cmake
tide_cook_asset(
    SOURCE assets/meshes/sponza/floor.gltf
    KIND   mesh
    OUTPUT build/cooked/${uuid}.tide
)
```

The helper reads the `.meta` sidecar to extract the UUID at configure time, so the output filename is stable across CMake re-runs. The cooker target itself is built first (because every per-asset custom command depends on it).

A separate `tide_manifest` step (also a custom command) runs after all per-asset cooks to emit the `manifest.bin` (a cooked artifact of `AssetKind::Manifest` per ADR-0017) listing UUID → cooked-file-path + content-hash for every asset.

### Caching

Cache key is the SHA-256 of:

```
[ source file bytes ] ‖ [ cooker version (TIDE_COOKER_VERSION u32) ] ‖ [ canonical hint blob ]
```

(SHA-256 not xxh3 here — the cache key is a content-addressed lookup, not a hot-path hash, and SHA-256 collisions would silently corrupt cooked output. Cooked artifacts inside the cache use xxh3 per ADR-0017.)

The cache lives at `build/cooked/.cache/<sha256-hex>/output.tide`. On invocation:

1. Compute cache key.
2. If `build/cooked/.cache/<key>/output.tide` exists, hardlink (or copy on platforms without hardlinks) to `--out` and exit 0.
3. Otherwise: cook, write to a temp file, atomically rename into the cache directory, hardlink to `--out`, exit 0.

The cache is build-tree-local (not user-global) by default. P5+ may add `--cache=$HOME/.cache/tide-cooker` for cross-build sharing; out of scope for P3.

### Determinism

Same input bytes + same cooker version + same hints → same output bytes, byte-for-byte. The cooker is deterministic by construction:

- glTF parsing via cgltf is deterministic.
- Texture compression (KTX2/Basis) uses a fixed seed where the codec exposes one.
- Floating-point emission uses bit-exact representations (no `std::to_chars` round-tripping).
- Temporary file paths use a content-derived name, not `/tmp/<pid>` randomness.

Determinism is verified by CI: a "cook twice, compare outputs" step in the test suite ensures regressions get caught the moment they're introduced.

### Cooker versioning

`TIDE_COOKER_VERSION` is a `uint32_t` defined in `tools/asset-cooker/include/tide/cooker/version.h`, baked into the cooker binary at build time. **Every change to cooked output bytes** — a new tangent-space algorithm, a tightened texture-encoder setting, a cleanup pass on mesh indices — bumps this version.

The cache key includes the cooker version. A bumped cooker version invalidates the entire cache (every entry's key changes), forcing a full re-cook. This is the intended behaviour: a cooker change should re-cook everything, and the disk cost is bounded.

The cooker version is independent of the runtime format's `schema_version` (ADR-0017): a cooker version bump may or may not bump a payload schema. They co-vary but are not the same number.

### Source format dependencies

cgltf and stb_image (and miniz / basis_universal as needed) are **header-only and statically linked into the cooker**. They never appear in the runtime's link line. The runtime depends only on the cooked binary format defined in `engine/assets/include/tide/assets/RuntimeFormat.h`.

Heavier source-format dependencies — assimp for FBX, OpenImageIO for HDR, etc. — when added in later phases, link into the cooker only. The runtime never sees them.

## Alternatives considered

- **In-process library cooker.** Would let a single host program (the engine, or `samples/02_textured_mesh`) cook on demand. Rejected because it pulls cgltf/stb_image into the runtime's link line, breaking `IMPLEMENTATION_PLAN.md:447`'s rule. Also breaks the `add_custom_command` build integration: the engine has to be built before it can cook itself.
- **Long-running cooker daemon.** A persistent process that watches the source tree and re-cooks on change. Rejected for P3 — the file watcher pathway in `assets/FileWatcher.h` (`IMPLEMENTATION_PLAN.md:484`) covers the dev use case, and the per-invocation cooker is plenty fast (header-only deps, hash-keyed cache hit returns in microseconds). May reconsider in P5+ if cooker startup time becomes measurable on full project builds.
- **One cooker invocation for all assets** (manifest-driven). Rejected because it serializes the build: changing one asset would re-run the cooker on the whole tree (CMake can't track per-asset dependencies through a single invocation). Per-asset `add_custom_command` lets `make`/`ninja` parallelize and skip-on-cache-hit at fine grain.
- **Cache key = source path + mtime** (Unix `make`-style). Rejected: mtime is unreliable on `git checkout` (sets all mtimes to checkout time), on filesystems without sub-second resolution, and on cache-restore in CI. Content-hash is the only correct cache key.
- **Use a third-party build-system-aware cooker** (Bazel, Buck). Rejected as out of scope. The project's build system is CMake (`vcpkg.json:1`); adding a second build system to handle assets is overkill.

## Consequences

**Positive.**
- Runtime stays free of source-format parsers. The tripwire at `IMPLEMENTATION_PLAN.md:447` is structurally enforced — cgltf and stb_image cannot accidentally end up in the runtime's link line.
- Incremental builds are fast. Touching one mesh re-cooks one mesh; the cache absorbs every other asset.
- CI cache-friendly: `build/cooked/.cache/` is a content-addressed pile of files. Easy to round-trip through GitHub Actions cache.
- Cross-platform: cooker is a CLI, runs on macOS/Linux/Windows the same way. CI matrix gets cooked artifacts from a Linux runner if desired and they work on the macOS dev box (subject to little-endian platform constraint per ADR-0017).

**Negative / accepted costs.**
- The build graph gets denser. Every source asset is a CMake custom command. CMake configure time scales with the asset count. Mitigated because configure is rare and incremental CMake builds skip the cook step on cache hits.
- Determinism discipline is real engineering work. Floating-point emitters, texture-encoder seed-fixing, sorted iteration over hash maps in the cooker — every drift source is a future bug. Accepted; CI verifies determinism.
- Cooker bugs invalidate every cooked output of that kind. A fix requires bumping `TIDE_COOKER_VERSION`. The full re-cook is what we want — silent stale outputs would be worse — but CI build times take a one-time hit on each bump.

**Reversibility.** Cooker process model is replaceable. Switching to a daemon, or a third-party tool, or an in-engine library, would mean rewriting `tools/asset-cooker/` — but the runtime stays the same as long as the output format (ADR-0017) doesn't change. ~2 weeks of work in the absolute-worst case; unlikely to ever be needed.

## Forward-design hooks

- **Cooker output filenames are UUIDs, never source paths.** The mapping UUID → source-path is the manifest's job. The cooker output directory is a flat directory of `<uuid>.tide` files, plus the cache subdirectory.
- **Determinism is enforced in CI.** A `cooker_determinism` test cooks the full asset set twice and `cmp`'s the byte-for-byte outputs. First failure is a CI hard error; the cause is always a non-determinism bug, not a flaky test.
- **`TIDE_COOKER_VERSION` is bumped on cooker behaviour changes**, not on cooker code refactors. A pure refactor that does not touch cook output bytes does not bump. The CI determinism test (above) catches the case where you forgot to bump after a behaviour change.
- **The cooker's link line is allowed to grow.** New source-format support adds new deps to the cooker; that's expected. The runtime's link line is the disciplined one (ADR-0042 gates ABI changes there).
- **No fork/exec inside the cooker.** A cook of one asset is one process. Internal subprocesses (calling DXC for shader compilation, etc.) are allowed but discouraged — prefer linking the library form when one exists, since process startup is amortizable across many cooks but not within one.
- **Cache eviction policy: none.** The cache grows unboundedly. Manual cleanup is `rm -rf build/cooked/.cache/`. P5+ may add an LRU bound; P3 does not bother.
- **Cache lookup must be `O(1)` in cache size.** The single-directory scheme (one subdir per key) lets the OS handle the lookup. Do not introduce a database or index file — the filesystem is the database here.

## Related ADRs

- ADR-0017: Runtime binary format. The cooker emits this format.
- ADR-0016: Asset GUID. The cooker reads `.meta` sidecars to determine the UUID; the runtime reads UUIDs from the manifest.
- ADR-0015: Hot-reload. The shader hot-reload pathway invokes the cooker on file-watcher trigger.
- ADR-0008: Package manager — vcpkg. cgltf, stb_image, etc. land via vcpkg manifest features `cooker` (added in P3 task 1).
- ADR-0042: Interface ABI versioning. The cooker's *invocation* CLI is a stable contract too — adding a required flag is a breaking change. Versioning is implicit through `TIDE_COOKER_VERSION`; we may add `--protocol=N` if a CLI break is ever needed.
