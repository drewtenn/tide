#pragma once

// Texture cooker: PNG / JPG -> uncompressed RGBA8 + optional mip chain.
//
// P3 scope (per the user-selected Tasks 4-6 cut of IMPLEMENTATION_PLAN.md):
// stb_image decode, deterministic box-filtered mips, RGBA8 (linear or
// sRGB) output. KTX2/Basis is reserved for P4+ — the schema's `format`
// field already has reserved value ranges (10-29) per ADR-0017's
// "append-only enum" discipline.
//
// Cooker hints in the .meta sidecar (P5+ — for P3 we hard-code defaults):
//   - "srgb": bool, default true for color textures
//   - "mips":  bool, default true (box-filter chain down to 1×1)

#include "tide/assets/Uuid.h"
#include "tide/core/Expected.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tide::cooker {

enum class TextureCookError : std::uint8_t {
    OpenFailed,         // file missing / unreadable
    DecodeFailed,       // stb_image rejected the image data
    UnsupportedShape,   // 0×0 or pathologically huge dimensions
    OutOfMemory,        // alloc failed during mip generation
};

[[nodiscard]] const char* to_string(TextureCookError e) noexcept;

struct TextureCookHints {
    bool srgb{true};        // RGBA8_Unorm_sRGB vs RGBA8_Unorm
    bool generate_mips{true};
};

struct TextureCookOutput {
    std::vector<std::byte> payload_bytes;   // TexturePayload + MipDesc[] + pixels
    std::uint64_t          content_hash;    // xxh3-64 over payload_bytes
};

[[nodiscard]] tide::expected<TextureCookOutput, TextureCookError>
    cook_texture(const std::filesystem::path& image_path,
                 const tide::assets::Uuid& uuid,
                 const TextureCookHints& hints);

} // namespace tide::cooker
