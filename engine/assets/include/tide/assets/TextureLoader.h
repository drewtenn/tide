#pragma once

// tide::assets::TextureLoader — concrete IAssetLoader for cooked
// AssetKind::Texture artifacts. Validates the RuntimeHeader, verifies the
// xxh3 content hash, and returns a `TextureAsset*` pointing into the
// AssetDB-owned mmap.
//
// Per ADR-0015 texture hot-reload is deferred to Phase 5; the inherited
// `IAssetLoader::reload()` returns AssetError::Unsupported in P3.

#include "tide/assets/IAssetLoader.h"

namespace tide::assets {

class TextureLoader final : public IAssetLoader {
public:
    [[nodiscard]] AssetKind kind() const noexcept override { return AssetKind::Texture; }

    [[nodiscard]] tide::expected<OpaquePayload, AssetError>
        load(Uuid uuid, std::span<const std::byte> cooked_bytes) override;

    void unload(OpaquePayload payload) noexcept override;
};

} // namespace tide::assets
