#pragma once

// tide::assets::MeshLoader — concrete IAssetLoader for cooked
// AssetKind::Mesh artifacts. Validates the RuntimeHeader, verifies the
// payload's xxh3 content hash, and returns a `MeshAsset*` pointing into
// the AssetDB-owned mmap (zero-copy).
//
// Per ADR-0015 mesh hot-reload is deferred to Phase 5; the inherited
// `IAssetLoader::reload()` returns AssetError::Unsupported in P3.
//
// Per ADR-0017 the validation set is: magic (kRuntimeMagic), kind == Mesh,
// schema_version == kMeshSchemaVersion, content_hash == xxh3-64 of
// payload bytes. Mismatch → `AssetError::SchemaMismatch` /
// `AssetError::InvalidFormat` per the relevant case.

#include "tide/assets/IAssetLoader.h"

namespace tide::assets {

class MeshLoader final : public IAssetLoader {
public:
    [[nodiscard]] AssetKind kind() const noexcept override { return AssetKind::Mesh; }

    [[nodiscard]] tide::expected<OpaquePayload, AssetError>
        load(Uuid uuid, std::span<const std::byte> cooked_bytes) override;

    void unload(OpaquePayload payload) noexcept override;
};

} // namespace tide::assets
