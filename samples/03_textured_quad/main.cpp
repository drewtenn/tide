// samples/03_textured_quad — Phase 1 task 7 deliverable.
//
// Promotes the colored triangle (sample 02) to a rotating textured quad. The
// sample exercises:
//   * texture upload (via IDevice::upload_texture, staged through Metal blit)
//   * sampler creation (filling the previously-stubbed create_sampler)
//   * uniform buffer with a 2-slot ring (host-visible, 256 B per slot)
//   * indexed draw via vertex+index buffers (binding=30 for vertex per task 7
//     debate gate — avoids MSL [[buffer(0)]] cbuffer collision)
//   * production HLSL → SPIR-V → MSL pipeline (cmake/CompileShader.cmake)
//
// Visual contract: a 64×64 white-on-dark-blue checker quad rotates 360° in
// roughly 6 s on a dark-blue clear. Cube faces (mismatched UVs / wrong sampler
// addressing) and BGRA-vs-RGBA mistakes show up as obvious banding.

#include "PerFrame.h"

#include "tide/core/Assert.h"
#include "tide/core/Log.h"
#include "tide/input/Actions.h"
#include "tide/input/InputContext.h"
#include "tide/platform/Path.h"
#include "tide/platform/Time.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#else
#define ZoneScopedN(x) (void) 0
#define FrameMark      (void) 0
namespace tracy {
inline void SetThreadName(const char*) {}
} // namespace tracy
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kTexSize          = 64;
constexpr uint32_t kVertexBufferSlot = 30;   // see debate-task7-gate1.md
constexpr uint32_t kCbufferSlot      = 0;
constexpr uint32_t kTextureSlot      = 0;
constexpr uint32_t kSamplerSlot      = 0;
constexpr uint64_t kCbufferSlotStride = 256;  // Vulkan minUniformBufferOffsetAlignment cap

// Must match MetalSwapchain.h::kMaxFramesInFlight. With <3 the ring wraps onto
// a slot the GPU is still reading from frame N-2 → torn rotation under stalls.
// TODO(Phase 1+): expose via DeviceCapabilities so the sample isn't required
// to know the backend's in-flight depth.
constexpr uint32_t kRingSize          = 3;

struct Vertex {
    float pos[2];
    float uv[2];
};

std::vector<std::byte> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto end = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes;
    bytes.resize(static_cast<size_t>(end));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), end)) return {};
    return bytes;
}

// Resolves the shaders directory by anchoring to the running binary's path
// rather than CWD. CMake stages outputs at ${CMAKE_BINARY_DIR}/shaders/ and
// the sample binary at ${CMAKE_BINARY_DIR}/samples/03_textured_quad/<exe>,
// so `<exe-dir>/../../shaders` reaches the artefacts regardless of how the
// binary was launched (Makefile, IDE debug, raw shell from any cwd).
std::string metallib_path(const char* basename) {
    static const auto base = tide::platform::executable_dir() / ".." / ".." / "shaders";
    return (base / basename).string();
}

// 64×64 RGBA8 UV gradient: R encodes U, G encodes V, B encodes a faint checker
// for spatial reference. With this image, a UV-flip bug shows as red↔green
// swap; a BGRA-vs-RGBA mismatch swaps the gradient axes; a wrong sampler
// addressing mode tiles the corners the wrong way. Each corner has a known
// signature: TL=black, TR=red, BL=green, BR=yellow.
void fill_uv_gradient(uint32_t* dst, uint32_t size) {
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t r = (x * 255u) / (size - 1);
            const uint32_t g = (y * 255u) / (size - 1);
            // Faint blue checker so 0,0 isn't pure black (BGRA debug aid).
            const uint32_t b = (((x >> 3) ^ (y >> 3)) & 1u) ? 0x40u : 0x00u;
            dst[y * size + x] = 0xFF000000u | (b << 16) | (g << 8) | r;  // ABGR LE
        }
    }
}

// Build a column-major rotation matrix around Z. Column-major matches the
// HLSL `column_major` keyword on the shader's PerFrame cbuffer.
void z_rotation_matrix(float angle_rad, float* out16) {
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    // Column 0
    out16[ 0] =  c; out16[ 1] =  s; out16[ 2] = 0.0f; out16[ 3] = 0.0f;
    // Column 1
    out16[ 4] = -s; out16[ 5] =  c; out16[ 6] = 0.0f; out16[ 7] = 0.0f;
    // Column 2
    out16[ 8] = 0.0f; out16[ 9] = 0.0f; out16[10] = 1.0f; out16[11] = 0.0f;
    // Column 3
    out16[12] = 0.0f; out16[13] = 0.0f; out16[14] = 0.0f; out16[15] = 1.0f;
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    tide::log::init();
    tracy::SetThreadName("main");

    TIDE_LOG_INFO("tide samples/03_textured_quad — rotating textured quad on Metal RHI");
    TIDE_LOG_INFO("Press Escape or close the window to exit.");

    auto window_result = tide::platform::Window::create({
        .width  = 1280,
        .height = 720,
        .title  = "tide — 03_textured_quad",
    });
    if (!window_result) {
        TIDE_LOG_ERROR("Window::create failed: {}",
                       static_cast<int>(window_result.error()));
        return 1;
    }
    auto window = std::move(*window_result);

    tide::input::InputSystem input(window);
    input.bind(tide::input::KeyBinding{
        .action = tide::input::Actions::Quit,
        .key    = tide::input::Key::Escape,
    });
    tide::input::GameplayContext gameplay_ctx;
    const uint32_t gameplay_handle = input.push_context(&gameplay_ctx);

    auto device_result = tide::rhi::metal::create_device(window);
    if (!device_result) {
        TIDE_LOG_ERROR("create_device failed: {}",
                       static_cast<int>(device_result.error()));
        return 2;
    }
    auto device = std::move(*device_result);

    const auto& caps = device->capabilities();
    TIDE_LOG_INFO("RHI ready: backend={} device={} UMA={}",
                  caps.backend_name, caps.device_name,
                  caps.uniform_memory_architecture ? "yes" : "no");

    // ─── Shaders ─────────────────────────────────────────────────────────────
    auto vs_bytes = read_file_bytes(metallib_path("textured_quad.vs.hlsl.metallib"));
    auto fs_bytes = read_file_bytes(metallib_path("textured_quad.ps.hlsl.metallib"));
    if (vs_bytes.empty() || fs_bytes.empty()) {
        TIDE_LOG_ERROR("Failed to read textured_quad metallibs from {}",
                       metallib_path(""));
        return 3;
    }

    tide::rhi::ShaderDesc vs_desc{};
    vs_desc.stage       = tide::rhi::ShaderStage::Vertex;
    vs_desc.bytecode    = std::span<const std::byte>(vs_bytes.data(), vs_bytes.size());
    vs_desc.entry_point = "main0";
    vs_desc.debug_name  = "textured_quad.vs";

    tide::rhi::ShaderDesc fs_desc{};
    fs_desc.stage       = tide::rhi::ShaderStage::Fragment;
    fs_desc.bytecode    = std::span<const std::byte>(fs_bytes.data(), fs_bytes.size());
    fs_desc.entry_point = "main0";
    fs_desc.debug_name  = "textured_quad.ps";

    auto vs = device->create_shader(vs_desc);
    auto fs = device->create_shader(fs_desc);
    if (!vs || !fs) {
        TIDE_LOG_ERROR("create_shader failed");
        return 4;
    }

    // ─── Descriptor set layout (b0 cbuffer for VS, t0 texture + s0 sampler for PS) ─
    using tide::rhi::DescriptorBindingDesc;
    using tide::rhi::DescriptorType;
    using tide::rhi::ShaderStage;

    const std::array<DescriptorBindingDesc, 3> bindings = {{
        { .slot = kCbufferSlot, .array_count = 1, .type = DescriptorType::UniformBuffer,
          .stages = ShaderStage::Vertex },
        { .slot = kTextureSlot, .array_count = 1, .type = DescriptorType::SampledTexture,
          .stages = ShaderStage::Fragment },
        { .slot = kSamplerSlot, .array_count = 1, .type = DescriptorType::Sampler,
          .stages = ShaderStage::Fragment },
    }};

    tide::rhi::DescriptorSetLayoutDesc layout_desc{
        .bindings = std::span<const DescriptorBindingDesc>(bindings.data(), bindings.size()),
        .debug_name = "textured_quad.layout",
    };
    auto layout = device->create_descriptor_set_layout(layout_desc);
    if (!layout) {
        TIDE_LOG_ERROR("create_descriptor_set_layout failed");
        return 5;
    }

    // ─── Geometry: 4 verts, 6 indices (two triangles, CCW front-face) ────────
    constexpr float kQuadHalf = 0.5f;
    const std::array<Vertex, 4> verts = {{
        {{-kQuadHalf, -kQuadHalf}, {0.0f, 1.0f}},  // 0: BL  (UV V flipped: 0,1 = bottom-left)
        {{ kQuadHalf, -kQuadHalf}, {1.0f, 1.0f}},  // 1: BR
        {{ kQuadHalf,  kQuadHalf}, {1.0f, 0.0f}},  // 2: TR
        {{-kQuadHalf,  kQuadHalf}, {0.0f, 0.0f}},  // 3: TL
    }};
    const std::array<uint16_t, 6> indices = {0, 1, 2,  0, 2, 3};

    tide::rhi::BufferDesc vbuf_desc{};
    vbuf_desc.size_bytes = sizeof(verts);
    vbuf_desc.usage      = tide::rhi::BufferUsage::VertexBuffer | tide::rhi::BufferUsage::CopyDest;
    vbuf_desc.memory     = tide::rhi::MemoryType::DeviceLocal;
    vbuf_desc.debug_name = "quad.vbuf";
    auto vbuf = device->create_buffer(vbuf_desc);

    tide::rhi::BufferDesc ibuf_desc{};
    ibuf_desc.size_bytes = sizeof(indices);
    ibuf_desc.usage      = tide::rhi::BufferUsage::IndexBuffer | tide::rhi::BufferUsage::CopyDest;
    ibuf_desc.memory     = tide::rhi::MemoryType::DeviceLocal;
    ibuf_desc.debug_name = "quad.ibuf";
    auto ibuf = device->create_buffer(ibuf_desc);

    if (!vbuf || !ibuf) {
        TIDE_LOG_ERROR("create_buffer (vbuf/ibuf) failed");
        return 6;
    }
    // upload_buffer returns tide::expected<void, RhiError>; bail on failure so a
    // wrong-format vbuf doesn't quietly render a black quad.
    if (auto r = device->upload_buffer(*vbuf, verts.data(), sizeof(verts)); !r) {
        TIDE_LOG_ERROR("upload_buffer(vbuf) failed: {}", static_cast<int>(r.error()));
        return 6;
    }
    if (auto r = device->upload_buffer(*ibuf, indices.data(), sizeof(indices)); !r) {
        TIDE_LOG_ERROR("upload_buffer(ibuf) failed: {}", static_cast<int>(r.error()));
        return 6;
    }

    // ─── Per-frame uniform ring buffer (host-visible) ────────────────────────
    tide::rhi::BufferDesc cbuf_desc{};
    cbuf_desc.size_bytes = kCbufferSlotStride * kRingSize;
    cbuf_desc.usage      = tide::rhi::BufferUsage::UniformBuffer | tide::rhi::BufferUsage::CopyDest;
    cbuf_desc.memory     = tide::rhi::MemoryType::Upload;
    cbuf_desc.debug_name = "quad.cbuf";
    auto cbuf = device->create_buffer(cbuf_desc);
    if (!cbuf) {
        TIDE_LOG_ERROR("create_buffer (cbuf) failed");
        return 7;
    }

    // ─── Texture + view + sampler ────────────────────────────────────────────
    tide::rhi::TextureDesc tex_desc{};
    tex_desc.dimension       = tide::rhi::TextureDimension::Tex2D;
    tex_desc.format          = tide::rhi::Format::RGBA8_Unorm;
    tex_desc.width           = kTexSize;
    tex_desc.height          = kTexSize;
    tex_desc.depth_or_layers = 1;
    tex_desc.mip_levels      = 1;
    tex_desc.sample_count    = 1;
    tex_desc.usage           = tide::rhi::TextureUsage::Sampled | tide::rhi::TextureUsage::CopyDest;
    tex_desc.memory          = tide::rhi::MemoryType::DeviceLocal;
    tex_desc.debug_name      = "quad.checker";
    auto tex = device->create_texture(tex_desc);
    if (!tex) {
        TIDE_LOG_ERROR("create_texture failed");
        return 8;
    }

    {
        std::array<uint32_t, kTexSize * kTexSize> pixels{};
        fill_uv_gradient(pixels.data(), kTexSize);
        if (auto up = device->upload_texture(*tex, pixels.data(), sizeof(pixels)); !up) {
            TIDE_LOG_ERROR("upload_texture(checker) failed: {}",
                           static_cast<int>(up.error()));
            return 8;
        }
    }

    tide::rhi::TextureViewDesc tv_desc{};
    tv_desc.texture     = *tex;
    tv_desc.dimension   = tide::rhi::TextureDimension::Tex2D;
    tv_desc.format      = tide::rhi::Format::RGBA8_Unorm;
    tv_desc.base_mip    = 0;
    tv_desc.mip_count   = 1;
    tv_desc.base_layer  = 0;
    tv_desc.layer_count = 1;
    tv_desc.debug_name  = "quad.checker.view";
    auto tex_view = device->create_texture_view(tv_desc);
    if (!tex_view) {
        TIDE_LOG_ERROR("create_texture_view failed");
        return 9;
    }

    tide::rhi::SamplerDesc smp_desc{};
    smp_desc.min_filter     = tide::rhi::FilterMode::Linear;
    smp_desc.mag_filter     = tide::rhi::FilterMode::Linear;
    smp_desc.mip_filter     = tide::rhi::MipFilterMode::NotMipmapped;
    smp_desc.address_u      = tide::rhi::AddressMode::ClampToEdge;
    smp_desc.address_v      = tide::rhi::AddressMode::ClampToEdge;
    smp_desc.address_w      = tide::rhi::AddressMode::ClampToEdge;
    smp_desc.max_anisotropy = 1;
    smp_desc.debug_name     = "quad.sampler";
    auto smp = device->create_sampler(smp_desc);
    if (!smp) {
        TIDE_LOG_ERROR("create_sampler failed");
        return 10;
    }

    // ─── Descriptor set (initial writes; cbuffer offset rewritten per frame) ─
    using tide::rhi::DescriptorWrite;
    std::array<DescriptorWrite, 3> writes{};
    writes[0].slot          = kCbufferSlot;
    writes[0].type          = DescriptorType::UniformBuffer;
    writes[0].buffer        = *cbuf;
    writes[0].buffer_offset = 0;
    writes[0].buffer_range  = sizeof(tide::shaders::PerFrame);
    writes[1].slot          = kTextureSlot;
    writes[1].type          = DescriptorType::SampledTexture;
    writes[1].texture       = *tex_view;
    writes[2].slot          = kSamplerSlot;
    writes[2].type          = DescriptorType::Sampler;
    writes[2].sampler       = *smp;

    tide::rhi::DescriptorSetDesc set_desc{};
    set_desc.layout         = *layout;
    set_desc.initial_writes = std::span<const DescriptorWrite>(writes.data(), writes.size());
    set_desc.debug_name     = "textured_quad.set";
    auto dset = device->create_descriptor_set(set_desc);
    if (!dset) {
        TIDE_LOG_ERROR("create_descriptor_set failed");
        return 11;
    }

    // ─── Pipeline ────────────────────────────────────────────────────────────
    using tide::rhi::Format;
    using tide::rhi::VertexAttributeDesc;
    using tide::rhi::VertexBindingDesc;

    const std::array<VertexBindingDesc, 1> vbindings = {{
        { .binding = kVertexBufferSlot,
          .stride  = sizeof(Vertex),
          .input_rate = tide::rhi::VertexInputRate::Vertex },
    }};
    const std::array<VertexAttributeDesc, 2> vattrs = {{
        { .location = 0, .binding = kVertexBufferSlot, .format = Format::RG32_Float, .offset = offsetof(Vertex, pos) },
        { .location = 1, .binding = kVertexBufferSlot, .format = Format::RG32_Float, .offset = offsetof(Vertex, uv)  },
    }};

    tide::rhi::GraphicsPipelineDesc pso_desc{};
    pso_desc.vertex_shader              = *vs;
    pso_desc.fragment_shader            = *fs;
    pso_desc.vertex_input               = { vbindings, vattrs };
    pso_desc.topology                   = tide::rhi::PrimitiveTopology::TriangleList;
    pso_desc.rasterization.cull         = tide::rhi::CullMode::None;
    pso_desc.rasterization.front_face   = tide::rhi::FrontFace::CounterClockwise;
    pso_desc.color_attachment_count     = 1;
    pso_desc.color_formats[0]           = device->swapchain_format();
    pso_desc.color_blend[0].write_mask  = 0xF;
    pso_desc.sample_count               = 1;
    pso_desc.debug_name                 = "textured_quad.pso";

    auto pso = device->create_graphics_pipeline(pso_desc);
    if (!pso) {
        TIDE_LOG_ERROR("create_graphics_pipeline failed: {}",
                       static_cast<int>(pso.error()));
        return 12;
    }
    TIDE_LOG_INFO("Pipeline built. Entering frame loop.");

    // ─── Frame loop ──────────────────────────────────────────────────────────
    tide::platform::FrameTimer frame_timer;
    uint64_t frame_index = 0;
    float    elapsed_s   = 0.0f;

    while (!window.should_close()) {
        ZoneScopedN("Main");

        input.begin_frame();
        tide::platform::Window::poll_events();

        if (input.is_just_pressed(tide::input::Actions::Quit)) {
            TIDE_LOG_INFO("Quit action triggered — closing window.");
            window.request_close();
        }

        if (auto begin = device->begin_frame(); !begin) {
            TIDE_LOG_ERROR("begin_frame failed: {}",
                           static_cast<int>(begin.error()));
            break;
        }

        auto sw = device->acquire_swapchain_texture();
        if (!sw) {
            (void)device->end_frame();
            continue;
        }

        const auto fb = window.framebuffer_size();
        const float aspect = (fb.height > 0)
                                 ? static_cast<float>(fb.width) / static_cast<float>(fb.height)
                                 : 1.0f;

        // Update the per-frame uniform into the next ring slot, then re-point
        // the descriptor write at that offset. The slot stride (256 B) keeps
        // us safely above any backend's minUniformBufferOffsetAlignment.
        const uint64_t ring_offset = (frame_index % kRingSize) * kCbufferSlotStride;
        {
            tide::shaders::PerFrame pf{};
            z_rotation_matrix(elapsed_s * 1.0f /* rad/s */, pf.rotation);
            pf.time   = elapsed_s;
            pf.aspect = aspect;
            (void)device->upload_buffer(*cbuf, &pf, sizeof(pf), ring_offset);

            DescriptorWrite cb_write = writes[0];
            cb_write.buffer_offset   = ring_offset;
            device->update_descriptor_set(*dset,
                std::span<const DescriptorWrite>(&cb_write, 1));
        }

        auto* cmd = device->acquire_command_buffer();
        cmd->transition(*sw, tide::rhi::ResourceState::Undefined,
                              tide::rhi::ResourceState::RenderTarget);

        tide::rhi::RenderPassDesc rp{};
        rp.color_attachment_count                = 1;
        rp.color_attachments[0].target.texture   = *sw;
        rp.color_attachments[0].load_op          = tide::rhi::LoadOp::Clear;
        rp.color_attachments[0].store_op         = tide::rhi::StoreOp::Store;
        rp.color_attachments[0].clear_value      = {0.05f, 0.07f, 0.10f, 1.0f};
        rp.render_area = {0, 0, static_cast<uint32_t>(fb.width),
                                static_cast<uint32_t>(fb.height)};

        cmd->begin_render_pass(rp);

        const tide::rhi::Viewport vp{
            .x = 0.0f, .y = 0.0f,
            .width  = static_cast<float>(fb.width),
            .height = static_cast<float>(fb.height),
            .min_depth = 0.0f, .max_depth = 1.0f,
        };
        cmd->set_viewport(vp);
        cmd->set_scissor({0, 0, static_cast<uint32_t>(fb.width),
                                static_cast<uint32_t>(fb.height)});

        cmd->bind_pipeline(*pso);
        cmd->bind_descriptor_set(/*set_index=*/0, *dset);
        cmd->bind_vertex_buffer(kVertexBufferSlot, *vbuf, /*offset=*/0);
        cmd->bind_index_buffer(*ibuf, /*offset=*/0, tide::rhi::IndexType::Uint16);
        cmd->draw_indexed(/*index_count=*/static_cast<uint32_t>(indices.size()),
                          /*instance_count=*/1,
                          /*first_index=*/0,
                          /*vertex_offset=*/0,
                          /*first_instance=*/0);

        cmd->end_render_pass();
        cmd->transition(*sw, tide::rhi::ResourceState::RenderTarget,
                              tide::rhi::ResourceState::Present);

        device->submit(cmd);
        (void)device->end_frame();

        const auto dt = frame_timer.tick();
        elapsed_s += static_cast<float>(tide::platform::seconds(dt));
        if (++frame_index % 120 == 0) {
            TIDE_LOG_DEBUG("frame {} dt={:.3f}ms",
                           frame_index, tide::platform::milliseconds(dt));
        }
        FrameMark;
    }

    // Tear down before the device — handles must outlive their referents.
    device->destroy_pipeline(*pso);
    device->destroy_descriptor_set(*dset);
    device->destroy_descriptor_set_layout(*layout);
    device->destroy_sampler(*smp);
    device->destroy_texture_view(*tex_view);
    device->destroy_texture(*tex);
    device->destroy_buffer(*cbuf);
    device->destroy_buffer(*ibuf);
    device->destroy_buffer(*vbuf);
    device->destroy_shader(*vs);
    device->destroy_shader(*fs);

    input.pop_context(gameplay_handle);
    TIDE_LOG_INFO("Clean shutdown after {} frames.", frame_index);
    return 0;
}
