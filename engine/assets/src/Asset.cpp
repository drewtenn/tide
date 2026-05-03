#include "tide/assets/Asset.h"

namespace tide::assets {

const char* to_string(AssetState s) noexcept {
    switch (s) {
        case AssetState::Pending: return "Pending";
        case AssetState::Loaded:  return "Loaded";
        case AssetState::Failed:  return "Failed";
    }
    return "<invalid AssetState>";
}

const char* to_string(AssetKind k) noexcept {
    switch (k) {
        case AssetKind::Manifest: return "Manifest";
        case AssetKind::Mesh:     return "Mesh";
        case AssetKind::Texture:  return "Texture";
        case AssetKind::Shader:   return "Shader";
        case AssetKind::Material: return "Material";
    }
    return "<invalid AssetKind>";
}

const char* to_string(AssetError e) noexcept {
    switch (e) {
        case AssetError::NotFound:        return "NotFound";
        case AssetError::KindMismatch:    return "KindMismatch";
        case AssetError::SchemaMismatch:  return "SchemaMismatch";
        case AssetError::InvalidFormat:   return "InvalidFormat";
        case AssetError::InvalidArgument: return "InvalidArgument";
        case AssetError::IoError:         return "IoError";
        case AssetError::LoadFailed:      return "LoadFailed";
        case AssetError::Unsupported:     return "Unsupported";
        case AssetError::PoolExhausted:   return "PoolExhausted";
    }
    return "<invalid AssetError>";
}

} // namespace tide::assets
