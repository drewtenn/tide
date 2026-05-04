#pragma once

// tide::assets::TextureAsset — runtime-side texture layout written by the
// cooker (`tools/asset-cooker/src/TextureCooker.{h,cpp}`) and read by the
// P3 texture loader (`TextureLoader.h`).
//
// Per ADR-0017 the payload is position-independent: every pointer is a
// `RelOffset<T>`, so mmap + cast is the entire load path.
//
// P3 scope: uncompressed RGBA8 (linear or sRGB), 2D texture, optional
// box-filtered mip chain. BC7 / ASTC compression and KTX2 supercompression
// are P4+ extensions. Schema version bumps when those land.
//
// Format-code rationale: `TextureFormat` is a stable wire-format enum,
// not `tide::rhi::Format`. The runtime translates from `TextureFormat` to
// the backend `rhi::Format` at upload time. Decoupling lets the RHI add /
// reorder formats without invalidating cooked outputs (ADR-0017's
// "format change = re-cook everything once" applies only to the cooker).

#include "tide/assets/RuntimeFormat.h"

#include <cstdint>
#include <type_traits>

namespace tide::assets {

// ─── TextureFormat ──────────────────────────────────────────────────────────
// Wire-format texture-format enum. Values pinned by ADR-0017; never
// renumber, never delete — append-only for forward-compat.
enum class TextureFormat : std::uint32_t {  // NOLINT(performance-enum-size) — wire format
    Undefined        = 0,
    RGBA8_Unorm      = 1,
    RGBA8_Unorm_sRGB = 2,
    // Reserved (P4+): 10..19 = BC7 / BC5 / BC4; 20..29 = ASTC variants.
};

[[nodiscard]] const char* to_string(TextureFormat f) noexcept;

// ─── MipDesc ────────────────────────────────────────────────────────────────
// One entry per mip level. `byte_offset` is relative to the start of the
// pixel-data blob (`TexturePayload::pixel_data.get()`), not relative to
// the MipDesc itself — the data blob is a single contiguous arena, so a
// single offset suffices.
struct MipDesc {
    std::uint32_t byte_offset;
    std::uint32_t byte_size;
    std::uint32_t width;
    std::uint32_t height;
};
static_assert(sizeof(MipDesc)  == 16);
static_assert(alignof(MipDesc) == 4);
static_assert(std::is_standard_layout_v<MipDesc>);
static_assert(std::is_trivially_copyable_v<MipDesc>);

// ─── TexturePayload ─────────────────────────────────────────────────────────
// Lives immediately after `RuntimeHeader` in the cooked file. `array_layers`
// is 1 in P3; cubemaps and texture arrays are P4+ (separate cooker hint
// surface, not a schema bump as long as the wire layout stays compatible).
struct TexturePayload {
    TextureFormat            format;          // 4
    std::uint32_t            width;           // 4
    std::uint32_t            height;          // 4
    std::uint32_t            mip_count;       // 4
    std::uint32_t            array_layers;    // 4 — 1 in P3
    std::uint32_t            reserved;        // 4 — pad to 8B alignment for the offsets below
    RelOffset<MipDesc>       mips;            // 4
    std::uint32_t            mips_count;      // 4 — must equal mip_count; redundant for cooker self-check
    RelOffset<std::byte>     pixel_data;      // 4
    std::uint32_t            pixel_data_size; // 4
};
static_assert(sizeof(TexturePayload)  == 40);
static_assert(alignof(TexturePayload) == 4);
static_assert(std::is_standard_layout_v<TexturePayload>);
static_assert(std::is_trivially_copyable_v<TexturePayload>);

// Concrete payload type referenced by `AssetHandle<TextureAsset>` —
// `Asset.h` forward-declares this; for P3 the in-memory representation is
// identical to `TexturePayload` (the runtime simply mmaps and casts). P4+
// may extend with non-cooked runtime-only fields (RHI handle, descriptor
// index, etc.).
struct TextureAsset : TexturePayload {};

} // namespace tide::assets
