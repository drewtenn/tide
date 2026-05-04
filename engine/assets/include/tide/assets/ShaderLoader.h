#pragma once

// tide::assets::ShaderLoader — concrete IAssetLoader for cooked
// AssetKind::Shader artifacts. Validates the RuntimeHeader and content
// hash, returns a `ShaderAsset*` pointing into the AssetDB-owned mmap.
//
// Per ADR-0015 shader hot-reload IS supported in P3 — but the
// `IAssetLoader::reload()` body lands in P3 task 10 alongside the
// filesystem watcher. For tasks 4-6 we keep the default base behaviour
// (returns Unsupported); the loader API is stable from day one so
// hot-reload in task 10 is additive (no ABI bump per ADR-0042).

#include "tide/assets/IAssetLoader.h"

namespace tide::assets {

class ShaderLoader final : public IAssetLoader {
public:
    [[nodiscard]] AssetKind kind() const noexcept override { return AssetKind::Shader; }

    [[nodiscard]] tide::expected<OpaquePayload, AssetError>
        load(Uuid uuid, std::span<const std::byte> cooked_bytes) override;

    void unload(OpaquePayload payload) noexcept override;

    // reload() is left at the IAssetLoader base default (returns Unsupported)
    // for tasks 4-6. P3 task 10 overrides it with the re-cook + atomic-swap
    // pipeline.
};

} // namespace tide::assets
