# ADR-0017: Runtime binary format — self-relative offsets, schema-versioned, mmap-friendly

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P3 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

`game_engine_plan.md:175` is unambiguous: *"Define your own runtime binary formats — load-time should be a memcpy plus a few pointer fixups, not a JSON parse."* `IMPLEMENTATION_PLAN.md:416` adds: *"memcpy + pointer fixup, no JSON/string parsing at runtime. uint32_t schemaVersion header. Self-relative offsets, not raw pointers."*

The implementation plan has already pre-decided most of the shape. The remaining design questions:

1. **Self-relative offsets vs absolute pointers + fixup pass.** Both are mmap-friendly. Self-relative offsets are immediately usable after mmap (no fixup pass needed); absolute pointers require a one-time walk to relocate, but reads are simple pointer dereferences thereafter. Self-relative wins because the format is also network/IPC-friendly without modification.
2. **Endianness.** Little-endian only, or platform-flipped? Engine ships on macOS arm64, Linux x64, Windows x64 — all little-endian. Big-endian support would require either a flag bit + per-load swap, or two cooker output paths.
3. **Alignment.** Random-access fields require natural alignment for the read to be a single load. Cooker must enforce padding.
4. **Versioning granularity.** Single global schema version, or per-asset-kind versions?
5. **Should we use a third-party format** (FlatBuffers, Cap'n Proto, msgpack)?

## Decision

**Custom format. Self-relative 32-bit signed offsets. Little-endian only. Header-versioned per asset kind.** No third-party serialization library; the format is defined by header structs and a tiny set of cooker helpers.

### Header layout (every cooked artifact starts with this)

```cpp
// engine/assets/include/tide/assets/RuntimeFormat.h
namespace tide::assets {

inline constexpr uint32_t kMagic = 0x54494445;   // "TIDE" little-endian

enum class AssetKind : uint32_t {
    Manifest    = 1,
    Mesh        = 2,
    Texture     = 3,
    Shader      = 4,
    Material    = 5,   // P5+
};

struct RuntimeHeader {
    uint32_t magic;             // == kMagic
    uint32_t kind;              // AssetKind
    uint32_t schema_version;    // monotonic per-kind; bump on layout change
    uint32_t cooker_version;    // TIDE_COOKER_VERSION at cook time, diagnostic
    uint64_t payload_size;      // bytes after this header
    uint64_t content_hash;      // xxh3-64 of payload, integrity check
    Uuid     uuid;              // identity from .meta sidecar (ADR-0016)
    // Total: 48 bytes. Payload begins at offset 48, naturally aligned.
};
static_assert(sizeof(RuntimeHeader) == 48);
static_assert(alignof(RuntimeHeader) == 8);

} // namespace tide::assets
```

The runtime mmaps the file, reads the header (no allocation, single load), validates `magic` + `schema_version`, then casts `data + sizeof(RuntimeHeader)` to the per-kind payload struct.

### Self-relative offsets

Pointers in the payload are encoded as `int32_t` offsets *relative to the offset's own address*:

```cpp
template <class T>
class RelOffset {
    int32_t offset_;     // T* - this, in bytes; 0 means null
public:
    [[nodiscard]] T* get() noexcept {
        if (offset_ == 0) return nullptr;
        return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + offset_);
    }
    [[nodiscard]] const T* get() const noexcept;  // const overload
    [[nodiscard]] bool valid() const noexcept { return offset_ != 0; }
};
static_assert(sizeof(RelOffset<int>) == 4);
```

Self-relative offsets are position-independent: the same blob mmap'd at any virtual address resolves correctly without a fixup pass. The cost is one extra add per dereference vs. an absolute pointer; given asset access happens at load time and per-frame draws don't traverse asset structs, this is unmeasurable.

Lengths are co-located with their offsets where applicable:

```cpp
struct MeshPayload {
    RelOffset<Vertex>  vertices;
    uint32_t           vertex_count;
    RelOffset<uint32_t> indices;
    uint32_t           index_count;
    RelOffset<SubMesh> submeshes;
    uint32_t           submesh_count;
    AABB               local_bounds;
};
```

### Alignment rules

- Every offset target must be naturally aligned for its element type. Cooker zero-pads payload to enforce this.
- The total file size is multiple-of-8 padded so consecutive arts in an archive (Phase 5+) chain cleanly.
- `RelOffset<T>` itself is 4-byte aligned; aligning the field is the consumer struct's responsibility.

### Endianness

**Little-endian only.** All target platforms in the design doc are little-endian (`game_engine_plan.md:1` lists macOS arm64, Linux x64, Windows x64; all little-endian). The runtime asserts `std::endian::native == std::endian::little` at startup. If a big-endian platform is ever required, this ADR is amended and a `swap_to_native()` pass is added on load.

### Schema versioning

**Per-kind monotonic.** `MeshPayload` schema version increments only when `MeshPayload`'s layout changes. `TexturePayload` has its own counter. The cooker writes the current schema for the kind it's emitting; the runtime reads it from the header and compares against `kSchemaVersion[kind]` constants.

**Mismatch = hard error**, not a migration:

```cpp
if (header.schema_version != kSchemaVersion[header.kind]) {
    return std::unexpected(AssetError::SchemaMismatch);
}
```

Re-cook is the migration. The cooker is fast enough on incremental builds (per ADR-0018's content-hash cache) that "delete cooked output and re-run cooker" is the right answer to a schema bump.

### Content hash

The header carries an `xxh3-64` of the payload. The runtime verifies it on first read and refuses to use the asset on mismatch. This catches:
- Truncated downloads / partial writes.
- Disk corruption on long-running builds.
- A post-cook tampering attempt (not a security boundary, just a sanity check).

The cost is a single xxh3 pass at load — fast enough on multi-GB/s for assets to be invisible.

## Alternatives considered

- **FlatBuffers.** Rejected. Adds a build-time dependency (`flatc`), a runtime-side schema-evaluation step, and a `.fbs` schema file per asset kind. The plan's "memcpy + pointer fixup" line specifically rules out this kind of layered approach. FlatBuffers is genuinely good at *unowned* data interchange (cross-language, cross-network); we're describing one cooker-runtime pair in C++ and don't need that flexibility.
- **Cap'n Proto.** Same objection as FlatBuffers, plus a heavier code generator. The "infinite-precision schema evolution" feature is something we'd opt out of anyway.
- **Absolute pointers + fixup pass.** Rejected because the fixup pass is a one-time walk over the entire payload before the first read, allocating fixup-table state. Self-relative offsets resolve in place with no preliminary walk. Both share the position-independence property; self-relative is strictly simpler.
- **JSON / msgpack / YAML.** Rejected as runtime formats per `game_engine_plan.md:175`. The cooker may emit debug-JSON sidecar files for development inspection (P5+ scene editor uses this), but the runtime never parses them.
- **One global schema_version (monotonic across all kinds).** Rejected because it means a layout change to `MeshPayload` invalidates *all* cooked textures and shaders too. Per-kind versioning lets you re-cook only the affected outputs.
- **Big-endian support via a flag bit.** Rejected — adds per-field endian-handling complexity to the runtime forever, in exchange for supporting platforms we have no plan to ship on. If big-endian becomes a real requirement (PowerPC console port, etc.), it's a one-time cost paid then.

## Consequences

**Positive.**
- Load is mmap + header validate + content-hash check + cast. No allocation in the load path, no parser, no schema lookup. Sub-millisecond for any reasonable asset.
- Self-relative offsets mean the cooked file is its own format, copyable byte-for-byte across machines / over the network without any relocation step.
- Per-kind schema versions let layout iteration be narrow: bumping `MeshPayload` doesn't force a re-cook of every texture.
- Format is owned in this repo. No third-party schema compiler in the build, no version-skew between toolchains.

**Negative / accepted costs.**
- Schema-version bump = full re-cook of all assets of that kind. Acceptable because the cooker's content-hash cache (ADR-0018) means non-affected assets stay cached; only the affected kind re-cooks.
- `RelOffset<T>` in payload structs is a manual discipline: forgetting one (using a raw pointer in a payload) silently produces a corrupt file. Mitigated by `static_assert(std::is_standard_layout_v<MeshPayload>)` and a `static_assert(!std::is_pointer_v<...>)` for each field, but the ergonomics are weaker than a code generator's.
- Little-endian only is a portability ceiling. Accepted; no current target is big-endian.

**Reversibility.** Format change = re-cook everything once. The cooker is the source of truth, not the cooked output. Switching from this format to a different one (e.g. adopting FlatBuffers later) means rewriting the cooker emitters and the runtime mmappers, then deleting `~/cache/cooked/` and re-running CMake. ~1 week of work; the *interface* (handle, UUID, async load) is invariant across this kind of change.

## Forward-design hooks

- **`RelOffset<T>` is the only allowed pointer in payload structs.** A `static_assert` per payload struct enumerates allowed field types. New payload authors who reach for `T*` get a compile error.
- **Payload structs are `[[no_unique_address]]`-disciplined.** No padding bugs across compilers — the cooker writes bytes the runtime reads, and a compiler-introduced pad byte is a corruption. Cooker tests verify `sizeof(MeshPayload) == sum-of-fields` for every kind.
- **Schema version is a `inline constexpr` per kind**, defined alongside the payload struct. The cooker reads it; the runtime reads it; they must match.
- **Manifest is a payload kind too.** `AssetKind::Manifest` writes a `ManifestPayload` mapping UUID → cooked-file-path + content-hash. Runtime mmaps the manifest at startup; subsequent loads are UUID → manifest lookup → cooked-file mmap.
- **Cooker version is diagnostic, not blocking.** A different cooker version is fine as long as the schema version matches. A schema bump *requires* a cooker version bump, but not vice versa.
- **xxh3 selection.** xxh3 is the same hash family the project already uses for the golden-frame test (`samples/05_offscreen_hash`); reuse over MD5/SHA — fast enough that the load-time check is free, not a security boundary.

## Related ADRs

- ADR-0016: Asset GUID. The header carries the UUID; the manifest maps UUID → file path.
- ADR-0018: Asset cooker. The cooker is the only writer of this format; the runtime is the only reader.
- ADR-0015: Hot-reload. Reload requires the new artifact to have *the same schema version* as the loaded one — otherwise the swap is rejected and a warning logged.
- ADR-0042: Interface ABI versioning. The runtime API for asset loading bumps `tide::assets::kAbiVersion` independently of the runtime *format* schema version. Same word, two different things.
- ADR-0017 itself supersedes nothing; it is additive.
