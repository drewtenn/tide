#pragma once

// Minimal CLI argument parser for tide-cooker. The flag set is small enough
// (per ADR-0018: --in / --out / --kind / --hints / --cache) that a hand-rolled
// parser stays under 100 lines and avoids pulling in a CLI dependency.

#include "tide/assets/Asset.h"
#include "tide/core/Expected.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tide::cooker {

enum class ArgError : std::uint8_t {
    MissingValue,         // flag given without its value
    UnknownFlag,
    MissingRequired,      // --in / --out / --kind missing
    UnknownKind,          // --kind value not one of mesh|texture|shader
    DuplicateFlag,
};

struct Args {
    std::filesystem::path                input;     // --in
    std::filesystem::path                output;    // --out
    tide::assets::AssetKind              kind{};    // --kind
    std::optional<std::filesystem::path> hints;     // --hints (optional .meta path)
    std::optional<std::filesystem::path> cache_dir; // --cache (optional)
};

[[nodiscard]] tide::expected<Args, ArgError> parse_args(int argc, const char* const argv[]);

[[nodiscard]] const char* to_string(ArgError e) noexcept;

[[nodiscard]] tide::expected<tide::assets::AssetKind, ArgError>
    parse_kind(std::string_view s) noexcept;

} // namespace tide::cooker
