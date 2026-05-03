#pragma once

// tide::assets::IAssetLoader — per-AssetKind loader interface.
//
// One concrete loader per kind: ShaderLoader, MeshLoader, TextureLoader.
// AssetDB owns the registry mapping `AssetKind` → `IAssetLoader*` and routes
// `request<T>()` calls to the right loader on a worker thread.
//
// Per ADR-0015 the `reload()` hook is part of the P3 ABI so the interface is
// stable from day one; only ShaderLoader actually does the swap in P3.
// MeshLoader / TextureLoader inherit the default-deleted base behaviour
// returning `AssetError::Unsupported`. Phase 5 fills the bodies in (additive,
// non-breaking, no `kAbiVersion` bump).

#include "tide/assets/Asset.h"
#include "tide/assets/Uuid.h"
#include "tide/core/Expected.h"

#include <cstddef>
#include <span>

namespace tide::assets {

// Opaque payload pointer carried by AssetDB on behalf of the loader.
// AssetDB never dereferences it; loaders cast to their own concrete type.
using OpaquePayload = void*;

class IAssetLoader {
public:
    IAssetLoader() = default;
    IAssetLoader(const IAssetLoader&) = delete;
    IAssetLoader& operator=(const IAssetLoader&) = delete;
    IAssetLoader(IAssetLoader&&) = delete;
    IAssetLoader& operator=(IAssetLoader&&) = delete;
    virtual ~IAssetLoader();

    // Which AssetKind this loader handles. AssetDB indexes by this value.
    [[nodiscard]] virtual AssetKind kind() const noexcept = 0;

    // Decode a cooked artifact into a runtime payload. Called on a worker
    // thread. The byte span's lifetime is the AssetDB's mmap region; loaders
    // that want zero-copy access may retain pointers into it across the
    // call's return.
    //
    // Returns the payload pointer that AssetDB stores in the slot, or an
    // AssetError. AssetDB transitions slot state to Loaded on success,
    // Failed on error.
    [[nodiscard]] virtual tide::expected<OpaquePayload, AssetError>
        load(Uuid uuid, std::span<const std::byte> cooked_bytes) = 0;

    // Free a payload returned by load(). Called when the AssetDB ref-count
    // drops to zero, after the deferred-destroy fence completes (P4+).
    virtual void unload(OpaquePayload payload) noexcept = 0;

    // Hot-reload swap. Default base implementation returns Unsupported per
    // ADR-0015; ShaderLoader overrides in P3. Mesh / Texture loaders inherit
    // the default until P5.
    //
    // On success the loader has produced a new payload pointer to replace
    // the old one (which AssetDB will retire after a frame fence).
    [[nodiscard]] virtual tide::expected<OpaquePayload, AssetError>
        reload(Uuid uuid,
               std::span<const std::byte> new_cooked_bytes,
               OpaquePayload old_payload);
};

} // namespace tide::assets
