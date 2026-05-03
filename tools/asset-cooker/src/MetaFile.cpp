#include "MetaFile.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

namespace tide::cooker {

const char* to_string(MetaError e) noexcept {
    switch (e) {
        case MetaError::FileNotFound:   return "FileNotFound";
        case MetaError::JsonParseError: return "JsonParseError";
        case MetaError::SchemaMismatch: return "SchemaMismatch";
        case MetaError::MissingField:   return "MissingField";
        case MetaError::InvalidUuid:    return "InvalidUuid";
        case MetaError::InvalidKind:    return "InvalidKind";
    }
    return "<invalid MetaError>";
}

namespace {

[[nodiscard]] tide::expected<tide::assets::AssetKind, MetaError>
parse_kind_string(const std::string& s) {
    if (s == "mesh")     return tide::assets::AssetKind::Mesh;
    if (s == "texture")  return tide::assets::AssetKind::Texture;
    if (s == "shader")   return tide::assets::AssetKind::Shader;
    if (s == "material") return tide::assets::AssetKind::Material;
    if (s == "manifest") return tide::assets::AssetKind::Manifest;
    return tide::unexpected{MetaError::InvalidKind};
}

} // namespace

tide::expected<MetaFile, MetaError> read_meta(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return tide::unexpected{MetaError::FileNotFound};
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error&) {
        return tide::unexpected{MetaError::JsonParseError};
    }

    MetaFile out{};

    if (auto it = j.find("schema"); it != j.end() && it->is_number_unsigned()) {
        out.schema = it->get<std::uint32_t>();
    }
    if (out.schema != 1) {
        // Forward-compat hook — when we ever bump .meta schema, the cooker
        // should still know how to read schema 1 too. For now P3 only emits
        // schema 1.
        return tide::unexpected{MetaError::SchemaMismatch};
    }

    auto uuid_it = j.find("uuid");
    auto kind_it = j.find("kind");
    if (uuid_it == j.end() || !uuid_it->is_string()
        || kind_it == j.end() || !kind_it->is_string()) {
        return tide::unexpected{MetaError::MissingField};
    }

    auto uuid = tide::assets::Uuid::parse(uuid_it->get<std::string>());
    if (!uuid) {
        return tide::unexpected{MetaError::InvalidUuid};
    }
    out.uuid = *uuid;

    auto kind = parse_kind_string(kind_it->get<std::string>());
    if (!kind) {
        return tide::unexpected{kind.error()};
    }
    out.kind = *kind;

    return out;
}

} // namespace tide::cooker
