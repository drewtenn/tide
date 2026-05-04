// Unit-test the ShaderLoader runtime path with an in-memory cooked-artifact
// blob. End-to-end cook (HLSL -> SPIR-V via DXC -> MSL via SPIRV-Cross)
// is gated on tool availability and lands with the CMake-driven shader-
// fixture pipeline (P3 task 13). For now the test builds a synthetic
// ShaderPayload so the runtime side is covered independently of the
// shader toolchain.

#include "tide/assets/RuntimeFormat.h"
#include "tide/assets/ShaderAsset.h"
#include "tide/assets/ShaderLoader.h"
#include "tide/assets/Uuid.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {

using tide::assets::AssetDescriptorBinding;
using tide::assets::AssetError;
using tide::assets::AssetKind;
using tide::assets::DescriptorType;
using tide::assets::RuntimeHeader;
using tide::assets::ShaderAsset;
using tide::assets::ShaderLoader;
using tide::assets::ShaderPayload;
using tide::assets::ShaderStage;
using tide::assets::Uuid;
using tide::assets::kRuntimeMagic;
using tide::assets::kShaderSchemaVersion;

// Build a minimal cooked-shader byte buffer:
//   * stage = Compute, entry = "main"
//   * 1 binding (UniformBuffer at set=0, slot=0)
//   * 0 vertex inputs
//   * 16 bytes of fake SPIR-V (just for size validation)
//   * "kernel main()" stub MSL string
[[nodiscard]] std::vector<std::byte> build_cooked_shader(const Uuid& uuid) {
    constexpr std::string_view entry  = "main";
    constexpr std::string_view msl    = "// stub";
    constexpr std::size_t      spv_sz = 16;

    auto align_up = [](std::size_t v, std::size_t a) {
        return (v + (a - 1)) & ~(a - 1);
    };

    std::size_t off = sizeof(ShaderPayload);
    const std::size_t entry_off  = off;
    const std::size_t entry_size = entry.size() + 1;
    off = align_up(entry_off + entry_size, 4);

    const std::size_t bindings_off  = off;
    const std::size_t bindings_size = 1 * sizeof(AssetDescriptorBinding);
    off = align_up(bindings_off + bindings_size, 4);

    // No vertex inputs in this fixture (compute stage).
    const std::size_t spv_off = off;
    off = align_up(spv_off + spv_sz, 4);

    const std::size_t msl_off  = off;
    const std::size_t msl_size = msl.size() + 1;
    off = msl_off + msl_size;

    const std::size_t payload_sz = off;
    const std::size_t total      = sizeof(RuntimeHeader) + payload_sz;

    std::vector<std::byte> bytes(total);
    std::byte* base = bytes.data() + sizeof(RuntimeHeader);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    auto* payload = reinterpret_cast<ShaderPayload*>(base);
    payload->stage              = ShaderStage::Compute;
    payload->entry_point_len    = static_cast<std::uint32_t>(entry.size());
    payload->push_constant_size = 0;
    payload->spirv_size         = static_cast<std::uint32_t>(spv_sz);
    payload->msl_size           = static_cast<std::uint32_t>(msl_size);
    payload->binding_count      = 1;
    payload->vertex_input_count = 0;

    auto* entry_dst = base + entry_off;
    std::memcpy(entry_dst, entry.data(), entry.size());
    entry_dst[entry.size()] = std::byte{0};
    payload->entry_point.set_target(entry_dst);

    AssetDescriptorBinding b{0, 0, 1, DescriptorType::UniformBuffer};
    auto* b_dst = base + bindings_off;
    std::memcpy(b_dst, &b, sizeof(b));
    payload->bindings.set_target(b_dst);

    auto* spv_dst = base + spv_off;
    for (std::size_t i = 0; i < spv_sz; ++i) {
        spv_dst[i] = static_cast<std::byte>(i + 0xA0);
    }
    payload->spirv_bytecode.set_target(spv_dst);

    auto* msl_dst = base + msl_off;
    std::memcpy(msl_dst, msl.data(), msl.size());
    msl_dst[msl.size()] = std::byte{0};
    payload->msl_source.set_target(msl_dst);

    const auto hash = XXH3_64bits(base, payload_sz);
    RuntimeHeader header{};
    header.magic          = kRuntimeMagic;
    header.kind           = static_cast<std::uint32_t>(AssetKind::Shader);
    header.schema_version = kShaderSchemaVersion;
    header.cooker_version = 1;
    header.payload_size   = static_cast<std::uint64_t>(payload_sz);
    header.content_hash   = hash;
    header.uuid           = uuid;
    std::memcpy(bytes.data(), &header, sizeof(header));

    return bytes;
}

} // namespace

TEST_SUITE("assets/ShaderLoader") {
    TEST_CASE("Loader returns ShaderAsset payload with binding and bytecode access") {
        const auto uuid = Uuid::parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_shader(*uuid);
        ShaderLoader loader;

        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE(p.has_value());

        const auto* sh = static_cast<const ShaderAsset*>(*p);
        CHECK(sh->stage              == ShaderStage::Compute);
        CHECK(sh->binding_count      == 1);
        CHECK(sh->vertex_input_count == 0);
        CHECK(sh->spirv_size         == 16);

        REQUIRE(sh->entry_point.valid());
        CHECK(std::string_view{sh->entry_point.get(), sh->entry_point_len} == "main");

        REQUIRE(sh->bindings.valid());
        const auto* b = sh->bindings.get();
        CHECK(b[0].set         == 0);
        CHECK(b[0].slot        == 0);
        CHECK(b[0].array_count == 1);
        CHECK(b[0].type        == DescriptorType::UniformBuffer);

        REQUIRE(sh->spirv_bytecode.valid());
        CHECK(static_cast<std::uint8_t>(sh->spirv_bytecode.get()[0]) == 0xA0);

        REQUIRE(sh->msl_source.valid());
        CHECK(std::string_view{sh->msl_source.get()} == "// stub");
    }

    TEST_CASE("Wrong kind -> KindMismatch") {
        const auto uuid = Uuid::parse("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_shader(*uuid);
        const std::uint32_t tex_kind = static_cast<std::uint32_t>(AssetKind::Texture);
        std::memcpy(bytes.data() + offsetof(RuntimeHeader, kind),
                    &tex_kind, sizeof(tex_kind));
        ShaderLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::KindMismatch);
    }

    TEST_CASE("Hash mismatch -> InvalidFormat") {
        const auto uuid = Uuid::parse("cccccccc-dddd-4eee-8fff-000000000000");
        REQUIRE(uuid.has_value());
        auto bytes = build_cooked_shader(*uuid);
        // Flip a byte in the middle of the SPIR-V section.
        bytes[sizeof(RuntimeHeader) + sizeof(ShaderPayload) + 8] ^= std::byte{0xFF};
        ShaderLoader loader;
        auto p = loader.load(*uuid, std::span<const std::byte>(bytes));
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == AssetError::InvalidFormat);
    }

    TEST_CASE("ShaderLoader::reload returns Unsupported (P3 task 10 lands the override)") {
        ShaderLoader loader;
        const auto uuid = Uuid::parse("dddddddd-eeee-4fff-8000-111111111111");
        REQUIRE(uuid.has_value());
        const auto r = loader.reload(*uuid, std::span<const std::byte>{}, nullptr);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error() == AssetError::Unsupported);
    }
}
