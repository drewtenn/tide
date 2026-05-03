#include "Args.h"

#include <string>
#include <string_view>

namespace tide::cooker {

const char* to_string(ArgError e) noexcept {
    switch (e) {
        case ArgError::MissingValue:    return "MissingValue";
        case ArgError::UnknownFlag:     return "UnknownFlag";
        case ArgError::MissingRequired: return "MissingRequired";
        case ArgError::UnknownKind:     return "UnknownKind";
        case ArgError::DuplicateFlag:   return "DuplicateFlag";
    }
    return "<invalid ArgError>";
}

tide::expected<tide::assets::AssetKind, ArgError> parse_kind(std::string_view s) noexcept {
    if (s == "mesh")     return tide::assets::AssetKind::Mesh;
    if (s == "texture")  return tide::assets::AssetKind::Texture;
    if (s == "shader")   return tide::assets::AssetKind::Shader;
    if (s == "manifest") return tide::assets::AssetKind::Manifest;
    return tide::unexpected{ArgError::UnknownKind};
}

namespace {

// Returns the next argument's value or MissingValue if `argv[i+1]` is absent.
[[nodiscard]] tide::expected<std::string_view, ArgError>
read_value(int argc, const char* const argv[], int& i) {
    if (i + 1 >= argc) {
        return tide::unexpected{ArgError::MissingValue};
    }
    return std::string_view{argv[++i]};
}

} // namespace

tide::expected<Args, ArgError> parse_args(int argc, const char* const argv[]) {
    Args            out{};
    bool            saw_in    = false;
    bool            saw_out   = false;
    bool            saw_kind  = false;
    bool            saw_hints = false;
    bool            saw_cache = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        if (a == "--in") {
            if (saw_in) return tide::unexpected{ArgError::DuplicateFlag};
            auto v = read_value(argc, argv, i);
            if (!v) return tide::unexpected{v.error()};
            out.input = std::filesystem::path{*v};
            saw_in    = true;
        } else if (a == "--out") {
            if (saw_out) return tide::unexpected{ArgError::DuplicateFlag};
            auto v = read_value(argc, argv, i);
            if (!v) return tide::unexpected{v.error()};
            out.output = std::filesystem::path{*v};
            saw_out    = true;
        } else if (a == "--kind") {
            if (saw_kind) return tide::unexpected{ArgError::DuplicateFlag};
            auto v = read_value(argc, argv, i);
            if (!v) return tide::unexpected{v.error()};
            auto k = parse_kind(*v);
            if (!k) return tide::unexpected{k.error()};
            out.kind = *k;
            saw_kind = true;
        } else if (a == "--hints") {
            if (saw_hints) return tide::unexpected{ArgError::DuplicateFlag};
            auto v = read_value(argc, argv, i);
            if (!v) return tide::unexpected{v.error()};
            out.hints = std::filesystem::path{*v};
            saw_hints = true;
        } else if (a == "--cache") {
            if (saw_cache) return tide::unexpected{ArgError::DuplicateFlag};
            auto v = read_value(argc, argv, i);
            if (!v) return tide::unexpected{v.error()};
            out.cache_dir = std::filesystem::path{*v};
            saw_cache     = true;
        } else {
            return tide::unexpected{ArgError::UnknownFlag};
        }
    }

    if (!saw_in || !saw_out || !saw_kind) {
        return tide::unexpected{ArgError::MissingRequired};
    }
    return out;
}

} // namespace tide::cooker
