#pragma once

// tide::assets — public asset-system types: handles, state machine, error enum.
//
// `AssetHandle<T>` reuses `tide::Handle<Tag>` (ADR-0003) where T is the runtime
// payload type itself (e.g. `MeshAsset`). Payload structs are forward-declared
// here; concrete layouts land alongside their cooker emitters per ADR-0017.
//
// Phase 3 scope per IMPLEMENTATION_PLAN.md:411–420.

#include "tide/core/Handle.h"

#include <cstdint>

namespace tide::assets {

// ─── Payload type forward declarations ──────────────────────────────────────
// Concrete layouts: ADR-0017's per-kind payload structs, defined alongside the
// cooker emitter for each kind (P3 atomic tasks 3 / 4 / 5).
struct MeshAsset;
struct TextureAsset;
struct ShaderAsset;
// `MaterialAsset` lands in P5+; deliberately not forward-declared yet.

// ─── AssetHandle<T> ─────────────────────────────────────────────────────────
// Same `(index, generation)` shape as the RHI handles. The payload type is the
// `Handle<Tag>` tag — `AssetHandle<MeshAsset>` and `AssetHandle<TextureAsset>`
// are not interconvertible at the type level.
template <class T> using AssetHandle = tide::Handle<T>;

using MeshHandle    = AssetHandle<MeshAsset>;
using TextureHandle = AssetHandle<TextureAsset>;
using ShaderHandle  = AssetHandle<ShaderAsset>;

// ─── State machine ──────────────────────────────────────────────────────────
// Per IMPLEMENTATION_PLAN.md:476. Reads of state are lock-free (the AssetDB
// stores per-slot state in a `std::atomic<AssetState>`).
enum class AssetState : std::uint8_t {
    Pending,    // load_job submitted; payload not yet ready
    Loaded,     // payload + GPU resources ready; AssetDB::get<T>() returns non-null
    Failed,     // load failed; AssetDB::get<T>() returns null
};

// ─── AssetKind ──────────────────────────────────────────────────────────────
// Mirrors ADR-0017 `AssetKind` values used in the on-disk `RuntimeHeader`.
// Underlying `uint32_t` is load-bearing — it pins the on-disk encoding for
// `RuntimeHeader::kind`. Do not shrink to a smaller type even though the
// value set fits.
//
// At runtime, type-safe `AssetHandle<T>` is preferred almost everywhere; this
// enum surfaces only at the cooker / runtime serialization boundary and in
// diagnostic messages.
enum class AssetKind : std::uint32_t {  // NOLINT(performance-enum-size) — on-disk format pinned by ADR-0017
    Manifest = 1,
    Mesh     = 2,
    Texture  = 3,
    Shader   = 4,
    Material = 5,   // P5+
};

// Compile-time mapping from payload type → AssetKind. Enabled via explicit
// specializations in this header; an unknown T is a compile error.
template <class T> [[nodiscard]] constexpr AssetKind kind_of() noexcept = delete;
template <> [[nodiscard]] constexpr AssetKind kind_of<MeshAsset>()    noexcept { return AssetKind::Mesh; }
template <> [[nodiscard]] constexpr AssetKind kind_of<TextureAsset>() noexcept { return AssetKind::Texture; }
template <> [[nodiscard]] constexpr AssetKind kind_of<ShaderAsset>()  noexcept { return AssetKind::Shader; }

// ─── AssetError ─────────────────────────────────────────────────────────────
// `uint32_t` to match the project convention for error enums (cf. `RhiError`).
enum class AssetError : std::uint32_t {  // NOLINT(performance-enum-size) — matches RhiError convention
    NotFound,         // UUID not in manifest
    KindMismatch,     // UUID exists but is a different AssetKind than requested
    SchemaMismatch,   // cooked artifact's schema_version != runtime expected version
    InvalidFormat,    // RuntimeHeader magic / payload corrupt
    InvalidArgument,  // bad UUID string / bad caller-supplied input
    IoError,          // mmap / open failed
    LoadFailed,       // payload-specific decode or GPU-upload error
    Unsupported,      // operation not supported (e.g. reload() for non-shader in P3 — ADR-0015)
    PoolExhausted,    // AssetDB slot pool full
    AlreadyInFlight,  // load_async called for a UUID whose load job is already submitted
};

[[nodiscard]] const char* to_string(AssetState s) noexcept;
[[nodiscard]] const char* to_string(AssetKind k) noexcept;
[[nodiscard]] const char* to_string(AssetError e) noexcept;

} // namespace tide::assets
