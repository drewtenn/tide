#include "tide/assets/ShaderLoader.h"

#include "tide/assets/RuntimeFormat.h"
#include "tide/assets/ShaderAsset.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstddef>
#include <cstring>

namespace tide::assets {

namespace {

[[nodiscard]] tide::expected<std::size_t, AssetError>
validate_header(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < sizeof(RuntimeHeader)) {
        return tide::unexpected{AssetError::InvalidFormat};
    }
    RuntimeHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));

    if (header.magic != kRuntimeMagic) {
        return tide::unexpected{AssetError::InvalidFormat};
    }
    if (header.kind != static_cast<std::uint32_t>(AssetKind::Shader)) {
        return tide::unexpected{AssetError::KindMismatch};
    }
    if (header.schema_version != kShaderSchemaVersion) {
        return tide::unexpected{AssetError::SchemaMismatch};
    }
    // payload_size is uint64 from untrusted bytes — see MeshLoader for the
    // overflow rationale.
    const std::size_t remaining = bytes.size() - sizeof(RuntimeHeader);
    if (header.payload_size > remaining) {
        return tide::unexpected{AssetError::InvalidFormat};
    }
    if (header.payload_size < sizeof(ShaderPayload)) {
        return tide::unexpected{AssetError::InvalidFormat};
    }

    const auto hash = XXH3_64bits(
        bytes.data() + sizeof(RuntimeHeader),
        static_cast<std::size_t>(header.payload_size));
    if (hash != header.content_hash) {
        return tide::unexpected{AssetError::InvalidFormat};
    }
    return sizeof(RuntimeHeader);
}

} // namespace

tide::expected<OpaquePayload, AssetError>
ShaderLoader::load(Uuid /*uuid*/, std::span<const std::byte> cooked_bytes) {
    auto offset = validate_header(cooked_bytes);
    if (!offset) {
        return tide::unexpected{offset.error()};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — see MeshLoader rationale
    return const_cast<std::byte*>(cooked_bytes.data()) + *offset;
}

void ShaderLoader::unload(OpaquePayload /*payload*/) noexcept {
    // Same lifetime model as the other loaders: the mmap owns the bytes.
    // P3 task 10 (hot-reload) adds a runtime PSO handle that this loader
    // *will* destroy; that lands with the reload() override.
}

} // namespace tide::assets
