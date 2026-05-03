#pragma once

// tide::assets::RuntimeHeader — on-disk format prefix for every cooked
// asset. Defined here so both `tools/asset-cooker/` (writer) and the
// runtime mmap loader (reader, P3 task 6) share one source of truth.
//
// See docs/adr/0017-runtime-binary-format.md for the rationale, the
// self-relative-offset payload encoding, and the schema-version policy.

#include "tide/assets/Asset.h"
#include "tide/assets/Uuid.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tide::assets {

// ─── Self-relative offset (ADR-0017) ────────────────────────────────────────
//
// `RelOffset<T>` stores `target_address - field_address` as an `int32_t`. The
// payload is position-independent: mmapping the file at any virtual address
// resolves correctly without a relocation pass.
//
// Cooker writes via `set_target()`. Runtime reads via `get()`. A zero offset
// is the null sentinel; clients check `valid()` before dereferencing.
template <class T>
class RelOffset {
public:
    [[nodiscard]] constexpr bool valid() const noexcept { return offset_ != 0; }

    [[nodiscard]] T* get() noexcept {
        if (offset_ == 0) {
            return nullptr;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
        return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(this) + offset_);
    }
    [[nodiscard]] const T* get() const noexcept {
        if (offset_ == 0) {
            return nullptr;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
        return reinterpret_cast<const T*>(reinterpret_cast<const std::byte*>(this) + offset_);
    }

    // Cooker-side: encode the target's byte distance from `this`.
    void set_target(const void* target) noexcept {
        if (target == nullptr) {
            offset_ = 0;
            return;
        }
        const auto* dst = static_cast<const std::byte*>(target);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
        const auto* self = reinterpret_cast<const std::byte*>(this);
        offset_ = static_cast<std::int32_t>(dst - self);
    }

    [[nodiscard]] constexpr std::int32_t raw_offset() const noexcept { return offset_; }

private:
    std::int32_t offset_{0};
};

static_assert(sizeof(RelOffset<int>) == 4, "RelOffset must be 4 bytes — ADR-0017 wire format");
static_assert(std::is_standard_layout_v<RelOffset<int>>);
static_assert(std::is_trivially_copyable_v<RelOffset<int>>);


// "TIDE" (little-endian). Consumers read the magic to detect a cooked
// artifact vs. a stray file with the wrong extension.
inline constexpr std::uint32_t kRuntimeMagic = 0x45444954u;

// Per-kind schema versions. Bump on a layout change to the corresponding
// payload struct. `Manifest` is bumped when the cooker's manifest emitter
// changes; the per-kind payload schemas are bumped alongside their cooker
// emitters (P3 atomic tasks 3 / 4 / 5).
inline constexpr std::uint32_t kManifestSchemaVersion = 1;
inline constexpr std::uint32_t kMeshSchemaVersion     = 1;
inline constexpr std::uint32_t kTextureSchemaVersion  = 1;
inline constexpr std::uint32_t kShaderSchemaVersion   = 1;

// Header layout: 48 bytes. Naturally aligned for an 8-byte mmap base.
// The cooker writes this, the runtime mmaps and casts to it.
struct RuntimeHeader {
    std::uint32_t magic;            // == kRuntimeMagic
    std::uint32_t kind;             // AssetKind value
    std::uint32_t schema_version;   // kKindSchemaVersion[kind]
    std::uint32_t cooker_version;   // TIDE_COOKER_VERSION at cook time
    std::uint64_t payload_size;     // bytes after this header
    std::uint64_t content_hash;     // xxh3-64 of the payload bytes
    Uuid          uuid;             // identity from .meta sidecar (ADR-0016)
};

static_assert(sizeof(RuntimeHeader) == 48, "RuntimeHeader is wire-format pinned by ADR-0017");
static_assert(alignof(RuntimeHeader) == 8);
static_assert(std::is_standard_layout_v<RuntimeHeader>);

// Schema lookup by AssetKind. Used by both cooker (when emitting) and runtime
// (when verifying). Returning 0 for unrecognized kinds is a sentinel — neither
// side should ever see it in practice.
[[nodiscard]] constexpr std::uint32_t schema_version_for(AssetKind k) noexcept {
    switch (k) {
        case AssetKind::Manifest: return kManifestSchemaVersion;
        case AssetKind::Mesh:     return kMeshSchemaVersion;
        case AssetKind::Texture:  return kTextureSchemaVersion;
        case AssetKind::Shader:   return kShaderSchemaVersion;
        case AssetKind::Material: return 0; // P5+
    }
    return 0;
}

} // namespace tide::assets
