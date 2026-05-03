#pragma once

// tide::assets::MeshAsset — runtime-side mesh layout written by the cooker
// (`tools/asset-cooker/src/MeshCooker.{h,cpp}`) and read by the P3 mesh
// loader (lands in P3 atomic tasks 6–7).
//
// Per ADR-0017 the payload is position-independent: every pointer is a
// `RelOffset<T>` (offset relative to the field's own address), so mmap +
// cast is the entire load path.
//
// Vertex layout (P3): position + normal + UV. Tangent space comes in P4
// alongside material work; skinning data in P4.5. The vertex struct
// is forward-compatible — additions go at the end and bump
// `kMeshSchemaVersion`.

#include "tide/assets/RuntimeFormat.h"

#include <cstdint>
#include <type_traits>

namespace tide::assets {

// ─── Vertex ─────────────────────────────────────────────────────────────────
// 32 bytes, 4-byte aligned. Identical layout cooker-side and runtime-side;
// the cooker emits `Vertex` arrays into the cooked file, and the runtime
// reinterprets them in place via `RelOffset<Vertex>::get()`.
struct Vertex {
    float position[3];   // object-local (post bind-pose transform)
    float normal[3];     // unit length
    float uv[2];
};
static_assert(sizeof(Vertex)  == 32);
static_assert(alignof(Vertex) == 4);
static_assert(std::is_standard_layout_v<Vertex>);
static_assert(std::is_trivially_copyable_v<Vertex>);

// ─── SubMesh ────────────────────────────────────────────────────────────────
// One submesh = one (first_index, index_count) range bound to one material
// slot. P3 emits submesh entries but `material_slot` is unused until P5
// adds the material system.
struct SubMesh {
    std::uint32_t first_index;
    std::uint32_t index_count;
    std::uint32_t material_slot;   // P5+; written as 0 in P3
    std::uint32_t reserved;        // padding to 16 bytes
};
static_assert(sizeof(SubMesh)  == 16);
static_assert(alignof(SubMesh) == 4);
static_assert(std::is_standard_layout_v<SubMesh>);

// ─── AABB ───────────────────────────────────────────────────────────────────
struct AABB {
    float min[3];
    float max[3];
};
static_assert(sizeof(AABB)  == 24);
static_assert(alignof(AABB) == 4);
static_assert(std::is_standard_layout_v<AABB>);

// ─── MeshPayload ────────────────────────────────────────────────────────────
// Lives immediately after `RuntimeHeader` in the cooked file. The three
// `RelOffset` fields point at the vertex / index / submesh arrays
// (typically following the payload struct contiguously, but the runtime
// must not assume so — only the offsets are load-bearing).
//
// Index format is unsigned 32-bit. cgltf reports glTF index types of
// either uint16 or uint32; the cooker normalizes to uint32 so the runtime
// has a single index path.
struct MeshPayload {
    RelOffset<Vertex>        vertices;       // 4
    std::uint32_t            vertex_count;   // 4
    RelOffset<std::uint32_t> indices;        // 4
    std::uint32_t            index_count;    // 4
    RelOffset<SubMesh>       submeshes;      // 4
    std::uint32_t            submesh_count;  // 4
    AABB                     local_bounds;   // 24
};
static_assert(sizeof(MeshPayload)  == 48);
static_assert(alignof(MeshPayload) == 4);
static_assert(std::is_standard_layout_v<MeshPayload>);
static_assert(std::is_trivially_copyable_v<MeshPayload>);

// Concrete payload type referenced by `AssetHandle<MeshAsset>` — `Asset.h`
// forward-declares this; the body of MeshAsset is the in-memory loaded
// representation, which is identical to MeshPayload for P3 (the runtime
// loader simply mmaps and casts). P5+ may extend MeshAsset with non-cooked
// runtime-only fields; until then they're aliases.
struct MeshAsset : MeshPayload {};

} // namespace tide::assets
