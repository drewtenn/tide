#include "TextureCooker.h"

#include "tide/assets/RuntimeFormat.h"
#include "tide/assets/TextureAsset.h"

// stb_image is header-only; define the implementation in exactly this TU.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include <stb_image.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace tide::cooker {

namespace {

using tide::assets::MipDesc;
using tide::assets::TextureFormat;
using tide::assets::TexturePayload;

// Box-filter downsample by 2× in each dimension. Output dims are
// `std::max(src_w / 2, 1u)` etc — the chain terminates at 1×1.
//
// Inputs are 8-bit RGBA. The filter is a simple unweighted average of
// the four source pixels; for power-of-two textures this is exactly the
// "box" filter and is the cheapest deterministic mip generator. Edge
// cases when one dimension is already 1 fall through to a 1-D average.
//
// sRGB textures should ideally be downsampled in linear space; for P3
// the simple gamma-space box matches the rest of the engine's reference
// images and the visual difference for diffuse maps is small. Linear
// downsample arrives in P4+ alongside the PBR work.
void downsample_box(const std::uint8_t* src, std::uint32_t sw, std::uint32_t sh,
                    std::uint8_t* dst, std::uint32_t dw, std::uint32_t dh) noexcept {
    for (std::uint32_t y = 0; y < dh; ++y) {
        for (std::uint32_t x = 0; x < dw; ++x) {
            const std::uint32_t x0 = std::min(2 * x,     sw - 1);
            const std::uint32_t x1 = std::min(2 * x + 1, sw - 1);
            const std::uint32_t y0 = std::min(2 * y,     sh - 1);
            const std::uint32_t y1 = std::min(2 * y + 1, sh - 1);

            const auto* p00 = src + (y0 * sw + x0) * 4;
            const auto* p01 = src + (y0 * sw + x1) * 4;
            const auto* p10 = src + (y1 * sw + x0) * 4;
            const auto* p11 = src + (y1 * sw + x1) * 4;

            auto*       d   = dst + (y  * dw + x ) * 4;
            for (int c = 0; c < 4; ++c) {
                const std::uint32_t sum = std::uint32_t{p00[c]} + p01[c] + p10[c] + p11[c];
                d[c] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
}

[[nodiscard]] std::uint32_t mip_count_for(std::uint32_t w, std::uint32_t h) noexcept {
    std::uint32_t n = 1;
    while (w > 1 || h > 1) {
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
        ++n;
    }
    return n;
}

} // namespace

const char* to_string(TextureCookError e) noexcept {
    switch (e) {
        case TextureCookError::OpenFailed:       return "OpenFailed";
        case TextureCookError::DecodeFailed:     return "DecodeFailed";
        case TextureCookError::UnsupportedShape: return "UnsupportedShape";
        case TextureCookError::OutOfMemory:      return "OutOfMemory";
    }
    return "<invalid TextureCookError>";
}

tide::expected<TextureCookOutput, TextureCookError>
cook_texture(const std::filesystem::path& image_path,
             const tide::assets::Uuid& /*uuid*/,
             const TextureCookHints& hints) {
    int w = 0;
    int h = 0;
    int channels_in_file = 0;
    // Force 4 channels (RGBA8); stb fills A=255 for opaque inputs.
    std::uint8_t* raw = stbi_load(image_path.string().c_str(),
                                  &w, &h, &channels_in_file, 4);
    if (raw == nullptr) {
        return tide::unexpected{TextureCookError::DecodeFailed};
    }
    struct StbiCleanup {
        std::uint8_t* p;
        ~StbiCleanup() { if (p) stbi_image_free(p); }
    } cleanup{raw};

    if (w <= 0 || h <= 0) {
        return tide::unexpected{TextureCookError::UnsupportedShape};
    }
    // Sanity bound — 16Ki on a side covers any realistic asset and
    // pre-empts integer overflow in the byte-count math below.
    if (w > 16384 || h > 16384) {
        return tide::unexpected{TextureCookError::UnsupportedShape};
    }

    const auto width  = static_cast<std::uint32_t>(w);
    const auto height = static_cast<std::uint32_t>(h);
    const auto mip_n  = hints.generate_mips ? mip_count_for(width, height) : 1u;

    // Build mip pixel data into a single contiguous arena.
    std::vector<MipDesc>       mips;
    std::vector<std::uint8_t>  pixels;
    mips.reserve(mip_n);

    {
        const auto base_size = static_cast<std::size_t>(width) * height * 4u;
        pixels.resize(base_size);
        std::memcpy(pixels.data(), raw, base_size);
        mips.push_back(MipDesc{
            .byte_offset = 0,
            .byte_size   = static_cast<std::uint32_t>(base_size),
            .width       = width,
            .height      = height,
        });
    }

    for (std::uint32_t i = 1; i < mip_n; ++i) {
        const auto& prev = mips.back();
        const auto  pw   = std::max(prev.width  / 2, 1u);
        const auto  ph   = std::max(prev.height / 2, 1u);
        const auto  size = static_cast<std::size_t>(pw) * ph * 4u;

        const std::size_t prev_off = prev.byte_offset;
        const std::size_t this_off = pixels.size();
        pixels.resize(this_off + size);

        downsample_box(pixels.data() + prev_off, prev.width, prev.height,
                       pixels.data() + this_off, pw, ph);

        mips.push_back(MipDesc{
            .byte_offset = static_cast<std::uint32_t>(this_off),
            .byte_size   = static_cast<std::uint32_t>(size),
            .width       = pw,
            .height      = ph,
        });
    }

    // Pack TexturePayload + MipDesc[] + pixels into a single buffer with
    // self-relative offsets. Layout:
    //   [TexturePayload][MipDesc × mip_n][pixel bytes]
    // Alignment: TexturePayload and MipDesc are 4-byte; pixels are 1-byte
    // and follow MipDesc[] without padding.
    const std::size_t mips_bytes  = mips.size() * sizeof(MipDesc);
    const std::size_t pixels_size = pixels.size();
    const std::size_t total       = sizeof(TexturePayload) + mips_bytes + pixels_size;

    std::vector<std::byte> buf(total);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    auto* payload = reinterpret_cast<TexturePayload*>(buf.data());
    payload->format          = hints.srgb ? TextureFormat::RGBA8_Unorm_sRGB
                                          : TextureFormat::RGBA8_Unorm;
    payload->width           = width;
    payload->height          = height;
    payload->mip_count       = static_cast<std::uint32_t>(mips.size());
    payload->array_layers    = 1;
    payload->reserved        = 0;
    payload->mips_count      = payload->mip_count;
    payload->pixel_data_size = static_cast<std::uint32_t>(pixels_size);

    std::byte* mips_dst   = buf.data() + sizeof(TexturePayload);
    std::byte* pixels_dst = mips_dst + mips_bytes;

    if (!mips.empty()) {
        std::memcpy(mips_dst, mips.data(), mips_bytes);
        payload->mips.set_target(mips_dst);
    }
    if (pixels_size > 0) {
        std::memcpy(pixels_dst, pixels.data(), pixels_size);
        payload->pixel_data.set_target(pixels_dst);
    }

    const auto hash = XXH3_64bits(buf.data(), buf.size());
    return TextureCookOutput{std::move(buf), hash};
}

} // namespace tide::cooker
