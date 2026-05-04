#pragma once

// Shader cooker: HLSL -> SPIR-V (via DXC) + MSL (via SPIRV-Cross) +
// reflection metadata. The cooker invokes both tools as subprocesses,
// then parses SPIRV-Cross's `--reflect` JSON to extract binding
// descriptors. The runtime never sees JSON (ADR-0017) — the cooker
// emits flat structs into the ShaderPayload.
//
// Tool discovery: the cooker honours the same env vars CompileShader.cmake
// does (VULKAN_SDK, /opt/homebrew/bin) — see ADR-0004 / locked DEFINE D21.
// The exact dxc/spirv-cross paths are passed in via --tool-paths, set by
// CMake at configure time (see tools/asset-cooker/CMakeLists.txt).
//
// P3 scope: vs / ps / cs stages, single entry point per file ("main" by
// default), descriptor reflection (uniform/storage buffers + sampled/
// storage textures + samplers), vertex-stage input reflection. Push
// constants are detected and the size is recorded; struct layout of
// push-constant blocks lands in P5 alongside the material system.

#include "tide/assets/ShaderAsset.h"
#include "tide/assets/Uuid.h"
#include "tide/core/Expected.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tide::cooker {

enum class ShaderCookError : std::uint8_t {
    OpenFailed,            // .hlsl missing / unreadable
    DxcMissing,            // dxc binary not found
    SpirvCrossMissing,     // spirv-cross binary not found
    DxcInvocationFailed,   // dxc returned non-zero
    SpirvCrossInvocationFailed,
    ReflectionParseError,  // SPIRV-Cross --reflect emitted invalid JSON
    UnsupportedStage,      // stage hint missing or unrecognised
    OutOfMemory,
};

[[nodiscard]] const char* to_string(ShaderCookError e) noexcept;

struct ShaderCookHints {
    tide::assets::ShaderStage stage{tide::assets::ShaderStage::Unknown};
    std::string               entry_point{"main"};
    // Tool paths supplied by the cooker host build (CMake-configured),
    // not by the .meta sidecar — they're build-environment, not
    // per-asset state.
    std::filesystem::path     dxc_path{};
    std::filesystem::path     spirv_cross_path{};
};

struct ShaderCookOutput {
    std::vector<std::byte> payload_bytes;   // ShaderPayload + bindings + spirv + msl
    std::uint64_t          content_hash;    // xxh3-64 over payload_bytes
};

[[nodiscard]] tide::expected<ShaderCookOutput, ShaderCookError>
    cook_shader(const std::filesystem::path& hlsl_path,
                const tide::assets::Uuid& uuid,
                const ShaderCookHints& hints);

} // namespace tide::cooker
