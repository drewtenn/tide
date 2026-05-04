#pragma once

// tide::assets::ShaderAsset — runtime-side shader layout written by the
// cooker (`tools/asset-cooker/src/ShaderCooker.{h,cpp}`) and read by the
// P3 shader loader (`ShaderLoader.h`).
//
// Per ADR-0004 the source is HLSL; the cooker drives DXC -> SPIR-V then
// SPIRV-Cross -> MSL + reflection JSON. The reflection metadata is parsed
// once in the cooker and embedded as flat structs — the runtime never
// parses JSON (ADR-0017).
//
// On macOS the runtime uses `msl_source` (passed to `MTLDevice newLibraryWithSource:`).
// On Vulkan/DX12 (P2/P2.5) the runtime uses `spirv_bytecode`. Both are
// emitted unconditionally so a single cooked artifact runs everywhere.

#include "tide/assets/RuntimeFormat.h"

#include <cstdint>
#include <type_traits>

namespace tide::assets {

// ─── ShaderStage ────────────────────────────────────────────────────────────
// Wire-format stage enum. Values pinned by ADR-0017; mirrors
// `tide::rhi::ShaderStage` but defined separately so the asset wire format
// is independent of the RHI ABI.
enum class ShaderStage : std::uint32_t {  // NOLINT(performance-enum-size) — wire format
    Unknown  = 0,
    Vertex   = 1,
    Fragment = 2,
    Compute  = 3,
};

[[nodiscard]] const char* to_string(ShaderStage s) noexcept;

// ─── DescriptorBindingDesc (asset-side) ─────────────────────────────────────
// Reflection-derived binding info, emitted by the cooker after parsing
// SPIRV-Cross's `--reflect` JSON. The runtime translates these into the
// RHI's `DescriptorBindingDesc` at PSO-build time — we keep an
// asset-format copy so the wire layout doesn't depend on `rhi/Descriptors.h`'s
// ABI version.
enum class DescriptorType : std::uint32_t {  // NOLINT(performance-enum-size) — wire format
    UniformBuffer  = 0,
    StorageBuffer  = 1,
    SampledTexture = 2,
    StorageTexture = 3,
    Sampler        = 4,
};

struct AssetDescriptorBinding {
    std::uint32_t  set;            // Vulkan-style descriptor-set index (0..3)
    std::uint32_t  slot;           // binding number within the set
    std::uint32_t  array_count;    // 1 for non-array; 0 for runtime-sized
    DescriptorType type;           // 4
};
static_assert(sizeof(AssetDescriptorBinding)  == 16);
static_assert(alignof(AssetDescriptorBinding) == 4);
static_assert(std::is_standard_layout_v<AssetDescriptorBinding>);
static_assert(std::is_trivially_copyable_v<AssetDescriptorBinding>);

// ─── VertexInputDesc (vertex-stage only) ────────────────────────────────────
// One entry per HLSL vertex-stage input attribute. The cooker emits these
// from SPIRV-Cross reflection (`stage_inputs`); the runtime uses them to
// build the RHI vertex-input descriptor when creating the PSO.
struct AssetVertexInput {
    std::uint32_t location;        // input slot
    std::uint32_t format_code;     // mirrors TextureFormat-style codes; small set in P3
    std::uint32_t offset;          // P5 will need this for interleaved layouts; 0 for P3
    std::uint32_t reserved;        // pad to 16B
};
static_assert(sizeof(AssetVertexInput)  == 16);
static_assert(alignof(AssetVertexInput) == 4);
static_assert(std::is_standard_layout_v<AssetVertexInput>);
static_assert(std::is_trivially_copyable_v<AssetVertexInput>);

// ─── ShaderPayload ──────────────────────────────────────────────────────────
// Lives immediately after `RuntimeHeader`. SPIR-V is the canonical
// bytecode (Vulkan/DX12 in later phases); MSL source is also emitted so
// macOS-Metal can compile via `MTLDevice` without DXC at runtime. The
// MSL is produced by SPIRV-Cross and is a faithful translation of the
// SPIR-V module.
struct ShaderPayload {
    ShaderStage              stage;                 // 4
    std::uint32_t            entry_point_len;       // 4 — length of the entry-point name (e.g. "main")
    RelOffset<char>          entry_point;           // 4
    std::uint32_t            push_constant_size;    // 4 — bytes of root-constant block
    RelOffset<std::byte>     spirv_bytecode;        // 4
    std::uint32_t            spirv_size;            // 4 — bytes (multiple of 4)
    RelOffset<char>          msl_source;            // 4 — null-terminated, length = msl_size-1
    std::uint32_t            msl_size;              // 4
    RelOffset<AssetDescriptorBinding> bindings;     // 4
    std::uint32_t            binding_count;         // 4
    RelOffset<AssetVertexInput> vertex_inputs;      // 4 — null/0 for non-vertex stages
    std::uint32_t            vertex_input_count;    // 4
};
static_assert(sizeof(ShaderPayload)  == 48);
static_assert(alignof(ShaderPayload) == 4);
static_assert(std::is_standard_layout_v<ShaderPayload>);
static_assert(std::is_trivially_copyable_v<ShaderPayload>);

// Concrete payload type referenced by `AssetHandle<ShaderAsset>`. For P3
// the in-memory representation is identical to `ShaderPayload`; the
// loader returns a pointer to the mmapped bytes. P3 task 10 (shader
// hot-reload) extends this with a runtime PSO handle; that's a P4+ shape.
struct ShaderAsset : ShaderPayload {};

} // namespace tide::assets
