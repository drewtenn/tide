// Unit-test the TextureLoader runtime path with an in-memory cooked-artifact
// blob. Exercises:
//   1. Header validation (magic / kind / schema_version / payload_size).
//   2. xxh3 content-hash check.
//   3. Self-relative offset resolution into the mip table and pixel arena.
//
// End-to-end cook-then-load coverage for textures lands with the
// CMake-driven texture-fixture pipeline (P3 task 13). For now, building
// the cooked bytes by hand keeps the test independent of stb_image and
// the file system.

#include "tide/assets/RuntimeFormat.h"
#include "tide/assets/TextureAsset.h"
#include "tide/assets/TextureLoader.h"
#include "tide/assets/Uuid.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

using tide::assets::AssetError;
using tide::assets::AssetKind;
using tide::assets::MipDesc;
using tide::assets::RuntimeHeader;
using tide::assets::TextureAsset;
using tide::assets::TextureFormat;
using tide::assets::TextureLoader;
using tide::assets::TexturePayload;
using tide::assets::Uuid;
using tide::assets::kRuntimeMagic;
using tide::assets::kTextureSchemaVersion;

constexpr std::uint32_t kBaseW = 2;
constexpr std::uint32_t kBaseH = 2;
constexpr std::uint32_t kMip1W = 1;
constexpr std::uint32_t kMip1H = 1;
constexpr std::size_t   kBaseSz = static_cast<std::size_t>(kBaseW) * kBaseH * 4;
constexpr std::size_t   kMip1Sz = static_cast<std::size_t>(kMip1W) * kMip1H * 4;

// Build a cooked-texture byte buffer with a 2×2 RGBA8 base mip (one
// distinguishable color per pixel) and a single 1×1 mip whose color is
// the pixel-average. Returns the bytes ready to be fed into the loader.
[[nodiscard]] std::vector<std::byte> build_cooked_texture(const Uuid& uuid) {
    const std::size_t mips_off    = sizeof(TexturePayload);
    const std::size_t mips_bytes  = 2 * sizeof(MipDesc);
    const std::size_t pixels_off  = mips_off + mips_bytes;
    const std::size_t payload_sz  = pixels_off + kBaseSz + kMip1Sz;
    const std::size_t total       = sizeof(RuntimeHeader) + payload_sz;

    std::vector<std::byte> bytes(total);

    // ---- Payload (after header) ----
    std::byte* payload_base = bytes.data() + sizeof(RuntimeHeader);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    auto* payload = reinterpret_cast<TexturePayload*>(payload_base);
    payload->format          = TextureFormat::RGBA8_Unorm_sRGB;
    payload->width           = kBaseW;
    payload->height          = kBaseH;
    payload->mip_count       = 2;
    payload->array_layers    = 1;
    payload->reserved        = 0;
    payload->mips_count      = 2;
    payload->pixel_data_size = static_cast<std::uint32_t>(kBaseSz + kMip1Sz);

    auto* mips_dst = payload_base + mips_off;
    MipDesc mips[2]{
        MipDesc{0,                                 static_cast<std::uint32_t>(kBaseSz), kBaseW, kBaseH},
        MipDesc{static_cast<std::uint32_t>(kBaseSz), static_cast<std::uint32_t>(kMip1Sz), kMip1W, kMip1H},
    };
    std::memcpy(mips_dst, mips, sizeof(mips));
    payload->mips.set_target(mips_dst);

    auto* pixels_dst = payload_base + mips_off + mips_bytes;
    const std::uint8_t base_pixels[kBaseSz] = {
        10, 0, 0, 255,    20, 0, 0, 255,
        30, 0, 0, 255,    40, 0, 0, 255,
    };
    std::memcpy(pixels_dst, base_pixels, kBaseSz);
    const std::uint8_t mip1_pixels[kMip1Sz] = { 25, 0, 0, 255 };
    std::memcpy(pixels_dst + kBaseSz, mip1_pixels, kMip1Sz);
    payload->pixel_data.set_target(pixels_dst);

    // ---- Header ----
    const auto hash = XXH3_64bits(payload_base, payload_sz);
    RuntimeHeader header{};
    header.magic          = kRuntimeMagic;
    header.kind           = static_cast<std::uint32_t>(AssetKind::Texture);
    header.schema_version = kTextureSchemaVersion;
    header.cooker_version = 1;
    header.payload_size   = static_cast<std::uint64_t>(payload_sz);
    header.content_hash   = hash;
    header.uuid           = uuid;
    std::memcpy(bytes.data(), &header, sizeof(header));

    return bytes;
}

} // namespace

TEST_SUITE("assets/TextureLoader") {
    TEST_CASE("Loader returns TextureAsset payload with valid mip table + pixels") {
        const auto uuid = Uuid::parse("11111111-2222-4333-8444-555555555555");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        TextureLoader loader;

        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE(p.has_value());

        const auto* tex = static_cast<const TextureAsset*>(*p);
        CHECK(tex->format       == TextureFormat::RGBA8_Unorm_sRGB);
        CHECK(tex->width        == kBaseW);
        CHECK(tex->height       == kBaseH);
        CHECK(tex->mip_count    == 2);
        CHECK(tex->array_layers == 1);

        REQUIRE(tex->mips.valid());
        const auto* mips = tex->mips.get();
        CHECK(mips[0].width  == kBaseW);
        CHECK(mips[0].height == kBaseH);
        CHECK(mips[1].width  == kMip1W);
        CHECK(mips[1].height == kMip1H);

        REQUIRE(tex->pixel_data.valid());
        const auto* px = tex->pixel_data.get();
        CHECK(static_cast<std::uint8_t>(px[0])       == 10);
        CHECK(static_cast<std::uint8_t>(px[kBaseSz]) == 25); // mip1 first pixel
    }

    TEST_CASE("Magic mismatch -> InvalidFormat") {
        const auto uuid = Uuid::parse("22222222-2222-4222-8222-222222222222");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        bytes[0] = std::byte{0xFF};
        TextureLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::InvalidFormat);
    }

    TEST_CASE("Wrong kind -> KindMismatch") {
        const auto uuid = Uuid::parse("33333333-3333-4333-8333-333333333333");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        const std::uint32_t mesh_kind = static_cast<std::uint32_t>(AssetKind::Mesh);
        std::memcpy(bytes.data() + offsetof(RuntimeHeader, kind),
                    &mesh_kind, sizeof(mesh_kind));
        TextureLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::KindMismatch);
    }

    TEST_CASE("Wrong schema version -> SchemaMismatch") {
        const auto uuid = Uuid::parse("44444444-4444-4444-8444-444444444444");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        const std::uint32_t bad_schema = kTextureSchemaVersion + 99;
        std::memcpy(bytes.data() + offsetof(RuntimeHeader, schema_version),
                    &bad_schema, sizeof(bad_schema));
        TextureLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::SchemaMismatch);
    }

    TEST_CASE("Truncated payload -> InvalidFormat") {
        const auto uuid = Uuid::parse("55555555-5555-4555-8555-555555555555");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        // Lie about payload_size so the magic / kind / schema checks pass,
        // but truncate the buffer below header + payload_size so the
        // bounds check fires.
        const std::uint64_t bad_size = sizeof(TexturePayload) - 1;
        std::memcpy(bytes.data() + offsetof(RuntimeHeader, payload_size),
                    &bad_size, sizeof(bad_size));
        bytes.resize(sizeof(RuntimeHeader) + sizeof(TexturePayload) - 4);
        TextureLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::InvalidFormat);
    }

    TEST_CASE("Hash mismatch -> InvalidFormat") {
        const auto uuid = Uuid::parse("66666666-6666-4666-8666-666666666666");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        // Flip a byte in the middle of the payload (mip table) — header
        // magic/kind/schema/size all still pass, but xxh3 won't match.
        bytes[sizeof(RuntimeHeader) + sizeof(TexturePayload) + 4] ^= std::byte{0xFF};
        TextureLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::InvalidFormat);
    }

    TEST_CASE("Adversarial payload_size = UINT64_MAX -> InvalidFormat (no overflow)") {
        // Exercise the integer-overflow guard added after the P3 review:
        // a hostile / corrupt header claiming payload_size near UINT64_MAX
        // would wrap `sizeof(RuntimeHeader) + payload_size` and let a tiny
        // buffer pass the bounds check. The loader must reject before
        // reading any payload bytes.
        const auto uuid = Uuid::parse("77777777-7777-4777-8777-777777777777");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_texture(*uuid);
        const std::uint64_t huge = ~std::uint64_t{0};
        std::memcpy(bytes.data() + offsetof(RuntimeHeader, payload_size),
                    &huge, sizeof(huge));
        TextureLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::InvalidFormat);
    }
}
