#pragma once

// Mesh cooker: glTF 2.0 → flat MeshPayload bytes.
//
// Reads `--in <path>.gltf` (or .glb) via cgltf, walks scene nodes, applies
// each node's bind-pose world transform to its primitives' positions and
// normals, packs the result into the runtime binary format defined by
// ADR-0017 (`engine/assets/include/tide/assets/MeshAsset.h`).
//
// Phase 3 scope: position + normal + UV; uint32 indices (uint16 inputs are
// widened); one submesh per glTF primitive. Skinning, tangents, morphs,
// materials are out of scope and arrive in P4 / P4.5 / P5.

#include "tide/assets/Uuid.h"
#include "tide/core/Expected.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tide::cooker {

enum class MeshCookError : std::uint8_t {
    OpenFailed,             // file missing / unreadable
    GltfParseError,         // cgltf rejected the file
    BufferLoadFailed,       // .bin / data-URI load failure
    NoMeshes,               // glTF has no mesh nodes
    MissingPosition,        // primitive has no POSITION attribute
    MissingNormal,
    MissingUV,
    UnsupportedTopology,    // not GL_TRIANGLES (only triangles supported in P3)
    UnsupportedIndexType,   // not u8/u16/u32 (P3 only u16/u32; widens u8)
    AccessorReadFailed,     // cgltf_accessor_read_float / _read_index failed
    EmptyPrimitive,         // 0 vertices or 0 indices in a primitive
};

// Output of a successful cook: the bytes that will be written verbatim
// (after the `RuntimeHeader`) to `--out`. The caller (main.cpp) prepends
// the header and writes the file.
struct MeshCookOutput {
    std::vector<std::byte> payload_bytes;   // MeshPayload + arrays, contiguous
    std::uint64_t          content_hash;    // xxh3-64 over payload_bytes
};

[[nodiscard]] tide::expected<MeshCookOutput, MeshCookError>
    cook_mesh(const std::filesystem::path& gltf_path, const tide::assets::Uuid& uuid);

[[nodiscard]] const char* to_string(MeshCookError e) noexcept;

} // namespace tide::cooker
