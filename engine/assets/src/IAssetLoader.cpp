#include "tide/assets/IAssetLoader.h"

namespace tide::assets {

// Out-of-line vtable anchor. See Sutter, GotW #31.
IAssetLoader::~IAssetLoader() = default;

// Default reload() returns Unsupported per ADR-0015 (P3 scope). Concrete
// loaders override only when they actually implement hot-reload — ShaderLoader
// in P3, MeshLoader / TextureLoader in P5+.
tide::expected<OpaquePayload, AssetError>
IAssetLoader::reload(Uuid /*uuid*/,
                     std::span<const std::byte> /*new_cooked_bytes*/,
                     OpaquePayload /*old_payload*/) {
    return tide::unexpected{AssetError::Unsupported};
}

} // namespace tide::assets
