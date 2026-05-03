#pragma once

// .meta sidecar reader. Per ADR-0016 each source asset has a sibling
// <source>.meta JSON file containing the asset's UUID and cooker hints.
// The cooker reads it; the runtime never touches `.meta` directly.

#include "tide/assets/Asset.h"
#include "tide/assets/Uuid.h"
#include "tide/core/Expected.h"

#include <filesystem>

namespace tide::cooker {

enum class MetaError : std::uint8_t {
    FileNotFound,
    JsonParseError,
    SchemaMismatch,         // unsupported `schema` field value
    MissingField,           // required field absent (uuid / kind)
    InvalidUuid,            // uuid string failed to parse
    InvalidKind,            // kind string not one of mesh|texture|shader|...
};

struct MetaFile {
    std::uint32_t           schema{1};
    tide::assets::Uuid      uuid{};
    tide::assets::AssetKind kind{};
    // cooker_hints intentionally not parsed in P3 task 4 — the scaffold only
    // needs uuid + kind. Per-kind cookers (P3 tasks 3 / 4 / 5) will plumb
    // their own hint structs through here.
};

[[nodiscard]] tide::expected<MetaFile, MetaError>
    read_meta(const std::filesystem::path& path);

[[nodiscard]] const char* to_string(MetaError e) noexcept;

} // namespace tide::cooker
