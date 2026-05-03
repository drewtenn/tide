#pragma once

// POD descriptor structs and enums for the tide RHI. Per the locked Phase 1
// DEFINE document, these surfaces are forward-designed to map cleanly onto Metal,
// Vulkan 1.3 (with VK_KHR_dynamic_rendering), and DX12 without leaking platform
// concepts. The Metal backend ignores fields that only matter on Vulkan/DX12.

#include "tide/rhi/Handles.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace tide::rhi {

// ─── Format ─────────────────────────────────────────────────────────────────
// Minimal format set for Phase 1; expand as samples need them.

enum class Format : uint16_t {
    Undefined = 0,

    // 8-bit
    R8_Unorm,
    R8_Uint,
    RG8_Unorm,
    RGBA8_Unorm,
    RGBA8_Unorm_sRGB,
    BGRA8_Unorm,
    BGRA8_Unorm_sRGB,    // Phase 1 swapchain default on macOS

    // 16-bit float
    R16_Float,
    RG16_Float,
    RGBA16_Float,        // HDR path, Phase 4+

    // 32-bit
    R32_Uint,
    R32_Float,
    RG32_Float,
    RGB32_Float,
    RGBA32_Float,

    // Depth / stencil
    D16_Unorm,
    D24_Unorm_S8_Uint,
    D32_Float,
    D32_Float_S8_Uint,
};

// ─── ResourceState ──────────────────────────────────────────────────────────
// Bitmask of resource states. Vulkan/DX12 allow combined read states (e.g.
// DepthRead | PixelShaderResource) so OR/AND ops are required.
// On Metal, transition() is a no-op — automatic hazard tracking handles this.

enum class ResourceState : uint32_t {
    Undefined              = 0,
    Common                 = 1u <<  0,
    Present                = 1u <<  1,
    RenderTarget           = 1u <<  2,
    DepthWrite             = 1u <<  3,
    DepthRead              = 1u <<  4,
    PixelShaderResource    = 1u <<  5,
    NonPixelShaderResource = 1u <<  6,
    UnorderedAccess        = 1u <<  7,
    CopySource             = 1u <<  8,
    CopyDest               = 1u <<  9,
    VertexBuffer           = 1u << 10,
    IndexBuffer            = 1u << 11,
    UniformBuffer          = 1u << 12,
    IndirectArgument       = 1u << 13,

    ShaderResource         = PixelShaderResource | NonPixelShaderResource,
};

constexpr ResourceState operator|(ResourceState a, ResourceState b) noexcept {
    return static_cast<ResourceState>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr ResourceState operator&(ResourceState a, ResourceState b) noexcept {
    return static_cast<ResourceState>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr ResourceState& operator|=(ResourceState& a, ResourceState b) noexcept {
    a = a | b;
    return a;
}
constexpr bool any(ResourceState s) noexcept {
    return static_cast<uint32_t>(s) != 0;
}

// ─── MemoryType ─────────────────────────────────────────────────────────────
// Three intent-based values — see ADR-005. Apple Silicon UMA: Upload maps to
// MTLStorageMode::Shared. R9 in the DEFINE risk register tracks the per-frame
// uniform-ring profiling task.

enum class MemoryType : uint8_t {
    DeviceLocal,  // GPU-only; no CPU map. Metal Private, Vk DEVICE_LOCAL, DX12 DEFAULT
    Upload,       // CPU-write/GPU-read.   Metal Shared,  Vk HOST_VISIBLE|COHERENT, DX12 UPLOAD
    Readback,     // GPU-write/CPU-read.   Metal Shared,  Vk HOST_VISIBLE|CACHED,   DX12 READBACK
};

// ─── BufferUsage / TextureUsage ─────────────────────────────────────────────
// Bitfield of how the resource will be used. Backend translates to native flags.

enum class BufferUsage : uint32_t {
    None             = 0,
    VertexBuffer     = 1u << 0,
    IndexBuffer      = 1u << 1,
    UniformBuffer    = 1u << 2,
    StorageBuffer    = 1u << 3,
    IndirectBuffer   = 1u << 4,
    CopySource       = 1u << 5,
    CopyDest         = 1u << 6,
};

constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr BufferUsage operator&(BufferUsage a, BufferUsage b) noexcept {
    return static_cast<BufferUsage>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr bool any(BufferUsage u) noexcept {
    return static_cast<uint32_t>(u) != 0;
}

enum class TextureUsage : uint32_t {
    None             = 0,
    Sampled          = 1u << 0,  // SRV
    Storage          = 1u << 1,  // UAV
    RenderTarget     = 1u << 2,
    DepthStencil     = 1u << 3,
    CopySource       = 1u << 4,
    CopyDest         = 1u << 5,
    Present          = 1u << 6,  // swapchain texture
};

constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr TextureUsage operator&(TextureUsage a, TextureUsage b) noexcept {
    return static_cast<TextureUsage>(
        static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
constexpr bool any(TextureUsage u) noexcept {
    return static_cast<uint32_t>(u) != 0;
}

// ─── BufferDesc / TextureDesc ───────────────────────────────────────────────

struct BufferDesc {
    uint64_t    size_bytes{0};
    BufferUsage usage{BufferUsage::None};
    MemoryType  memory{MemoryType::DeviceLocal};
    const char* debug_name{nullptr};
};

enum class TextureDimension : uint8_t { Tex1D, Tex2D, Tex3D, TexCube };

struct TextureDesc {
    TextureDimension dimension{TextureDimension::Tex2D};
    Format           format{Format::Undefined};
    uint32_t         width{0};
    uint32_t         height{1};
    uint32_t         depth_or_layers{1};
    uint32_t         mip_levels{1};
    uint32_t         sample_count{1};
    TextureUsage     usage{TextureUsage::None};
    MemoryType       memory{MemoryType::DeviceLocal};
    const char*      debug_name{nullptr};
};

struct TextureViewDesc {
    TextureHandle    texture;
    TextureDimension dimension{TextureDimension::Tex2D};
    Format           format{Format::Undefined};  // Undefined = inherit from texture
    uint32_t         base_mip{0};
    uint32_t         mip_count{1};
    uint32_t         base_layer{0};
    uint32_t         layer_count{1};
    const char*      debug_name{nullptr};
};

// ─── SamplerDesc ────────────────────────────────────────────────────────────

enum class FilterMode : uint8_t { Nearest, Linear };
enum class MipFilterMode : uint8_t { Nearest, Linear, NotMipmapped };
enum class AddressMode : uint8_t {
    Repeat,
    MirrorRepeat,
    ClampToEdge,
    ClampToBorder,
};
enum class CompareOp : uint8_t {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,
    NotEqual,
    GreaterOrEqual,
    Always,
};
enum class BorderColor : uint8_t {
    TransparentBlack,
    OpaqueBlack,
    OpaqueWhite,
};

struct SamplerDesc {
    FilterMode    min_filter{FilterMode::Linear};
    FilterMode    mag_filter{FilterMode::Linear};
    MipFilterMode mip_filter{MipFilterMode::Linear};
    AddressMode   address_u{AddressMode::Repeat};
    AddressMode   address_v{AddressMode::Repeat};
    AddressMode   address_w{AddressMode::Repeat};
    float         mip_lod_bias{0.0f};
    float         min_lod{0.0f};
    float         max_lod{1000.0f};
    uint32_t      max_anisotropy{1};       // 1 = off
    bool          compare_enable{false};
    CompareOp     compare_op{CompareOp::Always};
    BorderColor   border_color{BorderColor::TransparentBlack};
    const char*   debug_name{nullptr};
};

// ─── ShaderDesc ─────────────────────────────────────────────────────────────

enum class ShaderStage : uint16_t {
    None     = 0,
    Vertex   = 1u << 0,
    Fragment = 1u << 1,
    Compute  = 1u << 2,
    Geometry = 1u << 3,
    All      = Vertex | Fragment | Compute | Geometry,
};

constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) noexcept {
    return static_cast<ShaderStage>(
        static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
constexpr ShaderStage operator&(ShaderStage a, ShaderStage b) noexcept {
    return static_cast<ShaderStage>(
        static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
constexpr bool any(ShaderStage s) noexcept {
    return static_cast<uint16_t>(s) != 0;
}

struct ShaderDesc {
    ShaderStage              stage{ShaderStage::None};
    std::span<const std::byte> bytecode{};   // raw .spv (Vk/DX12) or .metallib (Metal)
    const char*              entry_point{"main"};
    const char*              debug_name{nullptr};
};

// ─── DescriptorSet ──────────────────────────────────────────────────────────

enum class DescriptorType : uint16_t {
    UniformBuffer,
    StorageBuffer,
    SampledTexture,
    StorageTexture,
    Sampler,
    CombinedImageSampler,   // Vulkan-only convenience; DX12/Metal split it
};

enum class DescriptorBindingFlags : uint8_t {
    None             = 0,
    VariableCount    = 1u << 0,  // VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
    PartiallyBound   = 1u << 1,  // VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
    UpdateAfterBind  = 1u << 2,  // VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
};

constexpr DescriptorBindingFlags operator|(DescriptorBindingFlags a,
                                           DescriptorBindingFlags b) noexcept {
    return static_cast<DescriptorBindingFlags>(
        static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
constexpr bool any(DescriptorBindingFlags f) noexcept {
    return static_cast<uint8_t>(f) != 0;
}

struct DescriptorBindingDesc {
    uint16_t               slot{0};
    uint16_t               array_count{1};
    DescriptorType         type{DescriptorType::UniformBuffer};
    ShaderStage            stages{ShaderStage::All};
    DescriptorBindingFlags flags{DescriptorBindingFlags::None};
};

struct DescriptorSetLayoutDesc {
    std::span<const DescriptorBindingDesc> bindings;
    const char*                            debug_name{nullptr};
    // Bindless promotion is the responsibility of the backend, transparent to
    // call sites — see ADR-006. No `bindless_layout` flag (per amendment A4).
};

struct DescriptorWrite {
    uint16_t       slot{0};
    uint16_t       array_index{0};
    DescriptorType type{DescriptorType::UniformBuffer};
    BufferHandle      buffer;        // for {Uniform,Storage}Buffer
    TextureViewHandle texture;       // for {Sampled,Storage}Texture
    SamplerHandle     sampler;       // for Sampler / CombinedImageSampler
    uint64_t       buffer_offset{0};
    uint64_t       buffer_range{~0ull};  // ~0 = entire buffer
};

struct DescriptorSetDesc {
    DescriptorSetLayoutHandle           layout;
    std::span<const DescriptorWrite>    initial_writes;
    const char*                         debug_name{nullptr};
};

// ─── RenderPass ─────────────────────────────────────────────────────────────
// Dynamic-rendering style: POD passed inline. No persistent RenderPass/Framebuffer.

enum class LoadOp  : uint8_t { Load, Clear, DontCare };
enum class StoreOp : uint8_t { Store, DontCare, Resolve };

struct ClearColorValue {
    float r{0.0f}, g{0.0f}, b{0.0f}, a{1.0f};
};

struct ClearDepthStencilValue {
    float    depth{1.0f};   // Phase 4 reversed-Z swaps to 0.0f via ADR-010
    uint32_t stencil{0};
};

// AttachmentTarget allows Phase 1 callers to attach a TextureHandle directly
// (treated as default whole-resource view). `view` takes precedence if valid.
struct AttachmentTarget {
    TextureHandle     texture{};
    TextureViewHandle view{};

    [[nodiscard]] constexpr bool uses_view() const noexcept { return view.valid(); }
    [[nodiscard]] constexpr bool uses_texture() const noexcept {
        return !view.valid() && texture.valid();
    }
};

struct ColorAttachmentDesc {
    AttachmentTarget target{};
    LoadOp           load_op{LoadOp::DontCare};
    StoreOp          store_op{StoreOp::Store};
    ClearColorValue  clear_value{};
    AttachmentTarget resolve_target{};   // MSAA, Phase 4+
};

struct DepthAttachmentDesc {
    AttachmentTarget       target{};
    LoadOp                 load_op{LoadOp::DontCare};
    StoreOp                store_op{StoreOp::DontCare};
    ClearDepthStencilValue clear_value{};
};

struct Rect2D {
    int32_t  x{0};
    int32_t  y{0};
    uint32_t width{0};
    uint32_t height{0};
};

struct Viewport {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};
    float min_depth{0.0f};
    float max_depth{1.0f};
};

constexpr uint32_t kMaxColorAttachments = 8;  // DX12 cap

struct RenderPassDesc {
    std::array<ColorAttachmentDesc, kMaxColorAttachments> color_attachments{};
    uint32_t                                              color_attachment_count{0};
    std::optional<DepthAttachmentDesc>                    depth;
    Rect2D                                                render_area{};  // Vulkan needs it
};

// ─── Pipeline (Phase 1 task 5; declared now for completeness) ───────────────

enum class PrimitiveTopology : uint8_t {
    PointList,
    LineList,
    LineStrip,
    TriangleList,
    TriangleStrip,
};

enum class PolygonMode : uint8_t { Fill, Line, Point };
enum class CullMode    : uint8_t { None, Front, Back };
enum class FrontFace   : uint8_t { CounterClockwise, Clockwise };

enum class DepthCompare : uint8_t {
    Never,
    Less,
    Equal,
    LessOrEqual,
    Greater,         // reversed-Z (ADR-010)
    NotEqual,
    GreaterOrEqual,  // reversed-Z (ADR-010)
    Always,
};

enum class BlendFactor : uint8_t {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    ConstantColor,
    OneMinusConstantColor,
};

enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

struct ColorBlendAttachmentState {
    bool         blend_enable{false};
    BlendFactor  src_color{BlendFactor::One};
    BlendFactor  dst_color{BlendFactor::Zero};
    BlendOp      color_op{BlendOp::Add};
    BlendFactor  src_alpha{BlendFactor::One};
    BlendFactor  dst_alpha{BlendFactor::Zero};
    BlendOp      alpha_op{BlendOp::Add};
    uint8_t      write_mask{0xF};   // RGBA
};

struct DepthStencilState {
    bool         depth_test_enable{false};
    bool         depth_write_enable{false};
    DepthCompare depth_compare{DepthCompare::Less};
};

struct RasterizationState {
    PolygonMode polygon{PolygonMode::Fill};
    CullMode    cull{CullMode::Back};
    FrontFace   front_face{FrontFace::CounterClockwise};
    bool        depth_clamp_enable{false};
    float       line_width{1.0f};
};

enum class IndexType : uint8_t { Uint16, Uint32 };
enum class VertexInputRate : uint8_t { Vertex, Instance };

struct VertexAttributeDesc {
    uint32_t location{0};
    uint32_t binding{0};
    Format   format{Format::Undefined};
    uint32_t offset{0};
};

struct VertexBindingDesc {
    uint32_t        binding{0};
    uint32_t        stride{0};
    VertexInputRate input_rate{VertexInputRate::Vertex};
};

struct VertexInputState {
    std::span<const VertexBindingDesc>   bindings;
    std::span<const VertexAttributeDesc> attributes;
};

struct GraphicsPipelineDesc {
    ShaderHandle                                          vertex_shader;
    ShaderHandle                                          fragment_shader;
    VertexInputState                                      vertex_input{};
    PrimitiveTopology                                     topology{PrimitiveTopology::TriangleList};
    RasterizationState                                    rasterization{};
    DepthStencilState                                     depth_stencil{};
    std::array<ColorBlendAttachmentState, kMaxColorAttachments> color_blend{};
    uint32_t                                              color_attachment_count{0};
    std::array<Format, kMaxColorAttachments>              color_formats{};
    Format                                                depth_format{Format::Undefined};
    uint32_t                                              sample_count{1};
    std::span<const DescriptorSetLayoutHandle>            descriptor_layouts;
    uint32_t                                              push_constant_size{0};
    const char*                                           debug_name{nullptr};
};

struct ComputePipelineDesc {
    ShaderHandle                               compute_shader;
    std::span<const DescriptorSetLayoutHandle> descriptor_layouts;
    uint32_t                                   push_constant_size{0};
    // Threads-per-threadgroup, matching the shader's [numthreads(...)] /
    // OpExecutionMode LocalSize. Metal's dispatchThreadgroups requires
    // both group count AND threads-per-group; the latter must match the
    // kernel's compiled local size. Phase 1 plumbs this through the
    // pipeline desc so consumers don't need to remember per-call.
    uint32_t                                   threads_per_group[3]{1, 1, 1};
    const char*                                debug_name{nullptr};
};

// ─── DeviceCapabilities ─────────────────────────────────────────────────────

struct DeviceCapabilities {
    uint32_t max_color_attachments{kMaxColorAttachments};
    uint32_t max_descriptor_sets{4};
    uint32_t max_push_constant_size{128};
    uint32_t max_threadgroup_invocations{1024};
    bool     supports_bindless{false};
    bool     supports_compute{true};
    bool     supports_geometry_shaders{false};
    bool     supports_tessellation{false};
    bool     supports_ray_tracing{false};
    bool     uniform_memory_architecture{false};   // true on Apple Silicon, integrated GPUs
    const char* device_name{""};
    const char* backend_name{""};
};

} // namespace tide::rhi
