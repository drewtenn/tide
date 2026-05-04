// tools/asset-cooker — `tide-cooker` CLI scaffold.
//
// Phase 3 task 4 (this file): argparse, .meta read, validate-and-emit-stub.
// Per-kind cooker emitters (mesh via cgltf, texture via stb_image, shader via
// DXC + SPIRV-Cross) land in P3 atomic tasks 3 / 4 / 5 — they plug into the
// dispatch table in `cook_one()` below.
//
// See docs/adr/0018-asset-cooker.md for the full design and the determinism /
// content-hash-cache contract.

#include "Args.h"
#include "MeshCooker.h"
#include "MetaFile.h"
#include "ShaderCooker.h"
#include "TextureCooker.h"

#include "tide/assets/RuntimeFormat.h"
#include "tide/assets/ShaderAsset.h"
#include "tide/cooker/Version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace {

using tide::assets::AssetKind;
using tide::assets::RuntimeHeader;
using tide::assets::Uuid;
using tide::assets::kRuntimeMagic;
using tide::assets::schema_version_for;

// Write `RuntimeHeader` + `payload` bytes to `out`. Centralised because
// every per-kind cooker (mesh / texture / shader / manifest) emits the
// same header shape — only the payload bytes differ.
[[nodiscard]] int write_artifact(const std::filesystem::path& out,
                                 AssetKind                    kind,
                                 const Uuid&                  uuid,
                                 const std::byte*             payload,
                                 std::size_t                  payload_size,
                                 std::uint64_t                content_hash) {
    RuntimeHeader header{};
    header.magic          = kRuntimeMagic;
    header.kind           = static_cast<std::uint32_t>(kind);
    header.schema_version = schema_version_for(kind);
    header.cooker_version = TIDE_COOKER_VERSION;
    header.payload_size   = static_cast<std::uint64_t>(payload_size);
    header.content_hash   = content_hash;
    header.uuid           = uuid;

    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (ec) {
        std::fprintf(stderr,
                     "tide-cooker: failed to create output directory '%s': %s\n",
                     out.parent_path().string().c_str(),
                     ec.message().c_str());
        return 1;
    }

    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "tide-cooker: cannot open output '%s' for write\n",
                     out.string().c_str());
        return 1;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    f.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (payload_size > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
        f.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(payload_size));
    }
    f.close();
    if (!f.good()) {
        std::fprintf(stderr, "tide-cooker: failed writing output '%s'\n",
                     out.string().c_str());
        return 1;
    }
    return 0;
}

// Emit a *placeholder* cooked artifact (valid header, 16-byte zero payload)
// for kinds that don't have a real per-kind emitter yet. Replaced as each
// per-kind cooker lands.
[[nodiscard]] int emit_placeholder(const std::filesystem::path& out,
                                   AssetKind                    kind,
                                   const Uuid&                  uuid) {
    constexpr std::size_t kPayloadSize = 16;
    const std::uint8_t    payload[kPayloadSize] = {};
    const auto            hash = XXH3_64bits(payload, kPayloadSize);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    return write_artifact(out, kind, uuid,
                          reinterpret_cast<const std::byte*>(payload),
                          kPayloadSize, hash);
}

// Mesh dispatch: cgltf-driven cook (P3 task 5).
[[nodiscard]] int emit_mesh(const std::filesystem::path& in,
                            const std::filesystem::path& out,
                            const Uuid&                  uuid) {
    auto cooked = tide::cooker::cook_mesh(in, uuid);
    if (!cooked) {
        std::fprintf(stderr, "tide-cooker: mesh cook failed for '%s' (%s)\n",
                     in.string().c_str(),
                     tide::cooker::to_string(cooked.error()));
        return 1;
    }
    return write_artifact(out, AssetKind::Mesh, uuid,
                          cooked->payload_bytes.data(),
                          cooked->payload_bytes.size(),
                          cooked->content_hash);
}

// Texture dispatch: stb_image -> RGBA8 + mips (P3 task 4).
[[nodiscard]] int emit_texture(const std::filesystem::path&       in,
                               const std::filesystem::path&       out,
                               const Uuid&                        uuid,
                               const tide::cooker::Args&          args) {
    tide::cooker::TextureCookHints hints{};
    hints.srgb          = args.texture_srgb;
    hints.generate_mips = args.texture_generate_mips;

    auto cooked = tide::cooker::cook_texture(in, uuid, hints);
    if (!cooked) {
        std::fprintf(stderr, "tide-cooker: texture cook failed for '%s' (%s)\n",
                     in.string().c_str(),
                     tide::cooker::to_string(cooked.error()));
        return 1;
    }
    return write_artifact(out, AssetKind::Texture, uuid,
                          cooked->payload_bytes.data(),
                          cooked->payload_bytes.size(),
                          cooked->content_hash);
}

// Shader dispatch: HLSL -> SPIR-V via DXC + MSL/reflection via SPIRV-Cross
// (P3 task 5).
[[nodiscard]] tide::assets::ShaderStage stage_from_args(tide::cooker::Args::Stage s) noexcept {
    using Cli = tide::cooker::Args::Stage;
    switch (s) {
        case Cli::Vertex:   return tide::assets::ShaderStage::Vertex;
        case Cli::Fragment: return tide::assets::ShaderStage::Fragment;
        case Cli::Compute:  return tide::assets::ShaderStage::Compute;
        case Cli::Unset:    return tide::assets::ShaderStage::Unknown;
    }
    return tide::assets::ShaderStage::Unknown;
}

[[nodiscard]] int emit_shader(const std::filesystem::path&       in,
                              const std::filesystem::path&       out,
                              const Uuid&                        uuid,
                              const tide::cooker::Args&          args) {
    if (args.shader_stage == tide::cooker::Args::Stage::Unset) {
        std::fprintf(stderr,
                     "tide-cooker: --kind shader requires --stage <vs|ps|cs>\n");
        return 2;
    }
    if (!args.shader_dxc || !args.shader_spirv_cross) {
        std::fprintf(stderr,
                     "tide-cooker: --kind shader requires --dxc <path> and --spirv-cross <path>\n");
        return 2;
    }
    tide::cooker::ShaderCookHints hints{};
    hints.stage            = stage_from_args(args.shader_stage);
    hints.entry_point      = args.shader_entry;
    hints.dxc_path         = *args.shader_dxc;
    hints.spirv_cross_path = *args.shader_spirv_cross;

    auto cooked = tide::cooker::cook_shader(in, uuid, hints);
    if (!cooked) {
        std::fprintf(stderr, "tide-cooker: shader cook failed for '%s' (%s)\n",
                     in.string().c_str(),
                     tide::cooker::to_string(cooked.error()));
        return 1;
    }
    return write_artifact(out, AssetKind::Shader, uuid,
                          cooked->payload_bytes.data(),
                          cooked->payload_bytes.size(),
                          cooked->content_hash);
}

} // namespace

int main(int argc, char** argv) {
    auto args = tide::cooker::parse_args(argc, argv);
    if (!args) {
        std::fprintf(stderr,
                     "tide-cooker: argument error (%s)\n"
                     "usage: tide-cooker --in <path> --out <path> --kind <mesh|texture|shader|manifest>\n"
                     "                   [--hints <path-to-.meta>] [--cache <dir>]\n",
                     tide::cooker::to_string(args.error()));
        return 2;
    }

    if (!std::filesystem::exists(args->input)) {
        std::fprintf(stderr, "tide-cooker: input '%s' does not exist\n",
                     args->input.string().c_str());
        return 2;
    }

    // Resolve UUID + kind. If --hints (a .meta file) is provided we honour
    // its uuid/kind; otherwise we fall back to a deterministic nil UUID and
    // the caller-supplied --kind. P3 tasks 3–5 will require --hints and
    // hard-error without it.
    Uuid      uuid{};
    AssetKind kind = args->kind;
    if (args->hints) {
        auto meta = tide::cooker::read_meta(*args->hints);
        if (!meta) {
            std::fprintf(stderr, "tide-cooker: failed to read .meta '%s' (%s)\n",
                         args->hints->string().c_str(),
                         tide::cooker::to_string(meta.error()));
            return 2;
        }
        if (meta->kind != args->kind) {
            std::fprintf(stderr,
                         "tide-cooker: --kind=%u disagrees with .meta kind=%u\n",
                         static_cast<unsigned>(args->kind),
                         static_cast<unsigned>(meta->kind));
            return 2;
        }
        uuid = meta->uuid;
        kind = meta->kind;
    }

    switch (kind) {
        case AssetKind::Mesh:
            return emit_mesh(args->input, args->output, uuid);
        case AssetKind::Texture:
            return emit_texture(args->input, args->output, uuid, *args);
        case AssetKind::Shader:
            return emit_shader(args->input, args->output, uuid, *args);
        case AssetKind::Material:
        case AssetKind::Manifest:
            return emit_placeholder(args->output, kind, uuid);
    }
    std::fprintf(stderr, "tide-cooker: internal: no emitter for kind=%u\n",
                 static_cast<unsigned>(kind));
    return 1;
}
