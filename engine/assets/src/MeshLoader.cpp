#include "tide/assets/MeshLoader.h"

#include "tide/assets/MeshAsset.h"
#include "tide/assets/RuntimeFormat.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstddef>
#include <cstring>

namespace tide::assets {

namespace {

// Validate the runtime header for a `Mesh`-kind cooked artifact.
// Returns the offset of the payload bytes (== sizeof(RuntimeHeader)) on
// success, or an AssetError on validation failure. Hash check is done
// after the trivial-validity checks; a header that fails the magic or
// schema check is rejected before we ever read the payload bytes.
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
    if (header.kind != static_cast<std::uint32_t>(AssetKind::Mesh)) {
        return tide::unexpected{AssetError::KindMismatch};
    }
    if (header.schema_version != kMeshSchemaVersion) {
        return tide::unexpected{AssetError::SchemaMismatch};
    }
    // payload_size is uint64 from untrusted bytes; compare via subtraction to
    // dodge wrap-around on `sizeof(RuntimeHeader) + payload_size`. Reject if
    // payload_size is larger than the remaining buffer or smaller than the
    // minimum payload struct.
    const std::size_t remaining = bytes.size() - sizeof(RuntimeHeader);
    if (header.payload_size > remaining) {
        return tide::unexpected{AssetError::InvalidFormat};
    }
    if (header.payload_size < sizeof(MeshPayload)) {
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
MeshLoader::load(Uuid /*uuid*/, std::span<const std::byte> cooked_bytes) {
    auto offset = validate_header(cooked_bytes);
    if (!offset) {
        return tide::unexpected{offset.error()};
    }

    // The MeshPayload lives in-place at `cooked_bytes.data() + sizeof(RuntimeHeader)`.
    // RelOffset<T> resolves relative to the offset's own address, so the
    // returned pointer is valid as long as the AssetDB keeps the underlying
    // mmap alive (which it does for the slot's lifetime).
    //
    // const_cast: the runtime API contract is that loaded payloads are
    // logically read-only; the AssetDB stores them as `void*` for
    // type-erasure. Callers go through `AssetDB::get<T>()` which returns
    // `const T*`, so no consumer mutates the bytes.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<std::byte*>(cooked_bytes.data()) + *offset;
}

void MeshLoader::unload(OpaquePayload /*payload*/) noexcept {
    // Mesh payloads live in the AssetDB-owned mmap. The mmap is released
    // when the last AssetDB ref drops (AssetDB::release_impl frees the
    // slot's MmapFile via Slot's destructor). No per-payload work needed.
}

} // namespace tide::assets
