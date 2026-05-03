#include "MeshCooker.h"

#include "tide/assets/MeshAsset.h"
#include "tide/assets/RuntimeFormat.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace tide::cooker {

namespace {

using tide::assets::AABB;
using tide::assets::MeshPayload;
using tide::assets::SubMesh;
using tide::assets::Vertex;

// ─── glTF helpers ───────────────────────────────────────────────────────────

[[nodiscard]] const cgltf_attribute*
find_attribute(const cgltf_primitive& prim, cgltf_attribute_type type) {
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        if (prim.attributes[i].type == type) {
            return &prim.attributes[i];
        }
    }
    return nullptr;
}

// Apply a 4x4 row-major-ish glTF transform (cgltf returns column-major
// floats: m[0..3] = column 0, etc.) to a position. Treats input as a vec3
// with implicit w=1.
void transform_point(const float m[16], const float in[3], float out[3]) {
    const float x = in[0];
    const float y = in[1];
    const float z = in[2];
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

// Transform a normal by the upper-3x3 then normalize. Correct for rigid +
// uniform-scale transforms (the inverse-transpose simplifies to the matrix
// itself); non-uniform scaling produces incorrect normals — log-and-warn is
// a P4 nice-to-have (ADR-0018 forward-design hook for a quality lint).
void transform_normal(const float m[16], const float in[3], float out[3]) {
    const float x = in[0];
    const float y = in[1];
    const float z = in[2];
    float nx = m[0] * x + m[4] * y + m[8]  * z;
    float ny = m[1] * x + m[5] * y + m[9]  * z;
    float nz = m[2] * x + m[6] * y + m[10] * z;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len > 0.0f) {
        nx /= len;
        ny /= len;
        nz /= len;
    }
    out[0] = nx;
    out[1] = ny;
    out[2] = nz;
}

// ─── Per-primitive extract ──────────────────────────────────────────────────

struct ExtractedPrimitive {
    std::vector<Vertex>        vertices;
    std::vector<std::uint32_t> indices;   // re-based to start at 0; will be biased on append
};

[[nodiscard]] tide::expected<ExtractedPrimitive, MeshCookError>
extract_primitive(const cgltf_primitive& prim, const float world[16]) {
    if (prim.type != cgltf_primitive_type_triangles) {
        return tide::unexpected{MeshCookError::UnsupportedTopology};
    }

    const cgltf_attribute* pos_attr = find_attribute(prim, cgltf_attribute_type_position);
    const cgltf_attribute* nrm_attr = find_attribute(prim, cgltf_attribute_type_normal);
    const cgltf_attribute* uv_attr  = find_attribute(prim, cgltf_attribute_type_texcoord);
    if (pos_attr == nullptr) return tide::unexpected{MeshCookError::MissingPosition};
    if (nrm_attr == nullptr) return tide::unexpected{MeshCookError::MissingNormal};
    if (uv_attr  == nullptr) return tide::unexpected{MeshCookError::MissingUV};

    const cgltf_accessor* pos_acc = pos_attr->data;
    const cgltf_accessor* nrm_acc = nrm_attr->data;
    const cgltf_accessor* uv_acc  = uv_attr->data;

    const cgltf_size vcount = pos_acc->count;
    if (vcount == 0 || pos_acc->count != nrm_acc->count || pos_acc->count != uv_acc->count) {
        return tide::unexpected{MeshCookError::EmptyPrimitive};
    }

    ExtractedPrimitive out;
    out.vertices.resize(vcount);
    for (cgltf_size i = 0; i < vcount; ++i) {
        float p_local[3]{};
        float n_local[3]{};
        float t[2]{};
        if (cgltf_accessor_read_float(pos_acc, i, p_local, 3) == 0
            || cgltf_accessor_read_float(nrm_acc, i, n_local, 3) == 0
            || cgltf_accessor_read_float(uv_acc,  i, t, 2)       == 0) {
            return tide::unexpected{MeshCookError::AccessorReadFailed};
        }
        Vertex& v = out.vertices[i];
        transform_point (world, p_local, v.position);
        transform_normal(world, n_local, v.normal);
        v.uv[0] = t[0];
        v.uv[1] = t[1];
    }

    // Indices: glTF allows non-indexed primitives (sequential 0..vcount-1).
    if (prim.indices != nullptr) {
        const cgltf_accessor* idx_acc = prim.indices;
        if (idx_acc->component_type != cgltf_component_type_r_8u
            && idx_acc->component_type != cgltf_component_type_r_16u
            && idx_acc->component_type != cgltf_component_type_r_32u) {
            return tide::unexpected{MeshCookError::UnsupportedIndexType};
        }
        const cgltf_size icount = idx_acc->count;
        if (icount == 0) {
            return tide::unexpected{MeshCookError::EmptyPrimitive};
        }
        out.indices.resize(icount);
        for (cgltf_size i = 0; i < icount; ++i) {
            const cgltf_size raw = cgltf_accessor_read_index(idx_acc, i);
            out.indices[i] = static_cast<std::uint32_t>(raw);
        }
    } else {
        out.indices.resize(vcount);
        for (cgltf_size i = 0; i < vcount; ++i) {
            out.indices[i] = static_cast<std::uint32_t>(i);
        }
    }

    return out;
}

// ─── Pack flat arrays into the wire format ──────────────────────────────────

[[nodiscard]] std::vector<std::byte>
pack_payload(const std::vector<Vertex>&        vertices,
             const std::vector<std::uint32_t>& indices,
             const std::vector<SubMesh>&       submeshes,
             const AABB&                       bounds) {
    const std::size_t v_bytes = vertices.size()  * sizeof(Vertex);
    const std::size_t i_bytes = indices.size()   * sizeof(std::uint32_t);
    const std::size_t s_bytes = submeshes.size() * sizeof(SubMesh);
    const std::size_t total   = sizeof(MeshPayload) + v_bytes + i_bytes + s_bytes;

    std::vector<std::byte> buf(total);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    auto* payload = reinterpret_cast<MeshPayload*>(buf.data());
    payload->vertex_count   = static_cast<std::uint32_t>(vertices.size());
    payload->index_count    = static_cast<std::uint32_t>(indices.size());
    payload->submesh_count  = static_cast<std::uint32_t>(submeshes.size());
    payload->local_bounds   = bounds;

    std::byte* v_ptr = buf.data() + sizeof(MeshPayload);
    std::byte* i_ptr = v_ptr + v_bytes;
    std::byte* s_ptr = i_ptr + i_bytes;

    if (!vertices.empty()) {
        std::memcpy(v_ptr, vertices.data(), v_bytes);
        payload->vertices.set_target(v_ptr);
    }
    if (!indices.empty()) {
        std::memcpy(i_ptr, indices.data(), i_bytes);
        payload->indices.set_target(i_ptr);
    }
    if (!submeshes.empty()) {
        std::memcpy(s_ptr, submeshes.data(), s_bytes);
        payload->submeshes.set_target(s_ptr);
    }

    return buf;
}

} // namespace

// ─── Public ─────────────────────────────────────────────────────────────────

const char* to_string(MeshCookError e) noexcept {
    switch (e) {
        case MeshCookError::OpenFailed:           return "OpenFailed";
        case MeshCookError::GltfParseError:       return "GltfParseError";
        case MeshCookError::BufferLoadFailed:     return "BufferLoadFailed";
        case MeshCookError::NoMeshes:             return "NoMeshes";
        case MeshCookError::MissingPosition:      return "MissingPosition";
        case MeshCookError::MissingNormal:        return "MissingNormal";
        case MeshCookError::MissingUV:            return "MissingUV";
        case MeshCookError::UnsupportedTopology:  return "UnsupportedTopology";
        case MeshCookError::UnsupportedIndexType: return "UnsupportedIndexType";
        case MeshCookError::AccessorReadFailed:   return "AccessorReadFailed";
        case MeshCookError::EmptyPrimitive:       return "EmptyPrimitive";
    }
    return "<invalid MeshCookError>";
}

tide::expected<MeshCookOutput, MeshCookError>
cook_mesh(const std::filesystem::path& gltf_path, const tide::assets::Uuid& /*uuid*/) {
    cgltf_options options{};
    cgltf_data*   data = nullptr;
    const std::string path_str = gltf_path.string();

    cgltf_result r = cgltf_parse_file(&options, path_str.c_str(), &data);
    if (r != cgltf_result_success) {
        return tide::unexpected{MeshCookError::GltfParseError};
    }
    struct Cleanup {
        cgltf_data* d;
        ~Cleanup() { if (d) cgltf_free(d); }
    } cleanup{data};

    r = cgltf_load_buffers(&options, data, path_str.c_str());
    if (r != cgltf_result_success) {
        return tide::unexpected{MeshCookError::BufferLoadFailed};
    }

    if (data->meshes_count == 0) {
        return tide::unexpected{MeshCookError::NoMeshes};
    }

    // Walk scene nodes (deterministic order: scene → root nodes → DFS).
    // For nodes without a parent in any scene, cgltf still places them in
    // `data->nodes`; we iterate `data->nodes` directly so unreferenced
    // mesh-bearing nodes are still cooked (matches the
    // "cook everything in the file" P3 scope; per-scene filtering is a
    // P5+ refinement).

    std::vector<Vertex>        all_vertices;
    std::vector<std::uint32_t> all_indices;
    std::vector<SubMesh>       all_submeshes;

    float aabb_min[3]{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
    };
    float aabb_max[3]{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node* node = &data->nodes[n];
        if (node->mesh == nullptr) {
            continue;
        }

        float world[16];
        cgltf_node_transform_world(node, world);

        const cgltf_mesh* mesh = node->mesh;
        for (cgltf_size p = 0; p < mesh->primitives_count; ++p) {
            const cgltf_primitive& prim = mesh->primitives[p];

            auto extracted = extract_primitive(prim, world);
            if (!extracted) {
                return tide::unexpected{extracted.error()};
            }

            const auto first_vertex = static_cast<std::uint32_t>(all_vertices.size());
            const auto first_index  = static_cast<std::uint32_t>(all_indices.size());

            // Append vertices and update bounds.
            for (const Vertex& v : extracted->vertices) {
                aabb_min[0] = std::min(aabb_min[0], v.position[0]);
                aabb_min[1] = std::min(aabb_min[1], v.position[1]);
                aabb_min[2] = std::min(aabb_min[2], v.position[2]);
                aabb_max[0] = std::max(aabb_max[0], v.position[0]);
                aabb_max[1] = std::max(aabb_max[1], v.position[1]);
                aabb_max[2] = std::max(aabb_max[2], v.position[2]);
            }
            all_vertices.insert(all_vertices.end(),
                                extracted->vertices.begin(),
                                extracted->vertices.end());

            // Append indices, biasing by `first_vertex`.
            for (std::uint32_t idx : extracted->indices) {
                all_indices.push_back(idx + first_vertex);
            }

            SubMesh sm{};
            sm.first_index   = first_index;
            sm.index_count   = static_cast<std::uint32_t>(extracted->indices.size());
            sm.material_slot = 0;
            sm.reserved      = 0;
            all_submeshes.push_back(sm);
        }
    }

    if (all_vertices.empty()) {
        return tide::unexpected{MeshCookError::NoMeshes};
    }

    AABB bounds{};
    for (int i = 0; i < 3; ++i) {
        bounds.min[i] = aabb_min[i];
        bounds.max[i] = aabb_max[i];
    }

    auto bytes = pack_payload(all_vertices, all_indices, all_submeshes, bounds);
    const auto hash = XXH3_64bits(bytes.data(), bytes.size());

    return MeshCookOutput{std::move(bytes), hash};
}

} // namespace tide::cooker
