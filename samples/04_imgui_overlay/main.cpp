// samples/04_imgui_overlay — Phase 1 task 8 deliverable.
//
// Builds on samples/03_textured_quad (rotating textured quad on Metal RHI) and
// adds a Dear ImGui frame-stats overlay. ImGui draws are issued through the
// engine's existing ICommandBuffer surface — no vendor backend.
//
// Demonstrates:
//   - tide::imgui::Context lifecycle (ctor → create_resources → begin_frame
//     → render → dtor)
//   - In-pass continuation: scene draws then ImGui draws, single render pass.
//   - Retina DPI: io.DisplaySize (points) + io.DisplayFramebufferScale (px/pt).
//   - Keyboard scaffold for the Phase 1 task 12 (capture) and 10 (hash) hooks.

#include "PerFrame.h"

#include "tide/core/Assert.h"
#include "tide/core/Log.h"
#include "tide/imgui-integration/Context.h"
#include "tide/input/Actions.h"
#include "tide/input/InputContext.h"
#include "tide/platform/Path.h"
#include "tide/platform/Time.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#include <imgui.h>

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
#include <string>
#include <vector>

namespace {

// Sample-local debug actions. IDs intentionally outside the engine-registry
// range (100+) so they don't collide with predefined Actions.
constexpr tide::input::Action kCaptureFrame{ .name = "CaptureFrame", .id = 100, .kind = tide::input::ActionKind::Button };
constexpr tide::input::Action kSaveHash    { .name = "SaveHash",     .id = 101, .kind = tide::input::ActionKind::Button };

constexpr uint32_t kTexSize          = 64;
constexpr uint32_t kVertexBufferSlot = 30;
constexpr uint32_t kCbufferSlot      = 0;
constexpr uint32_t kTextureSlot      = 0;
constexpr uint32_t kSamplerSlot      = 0;
constexpr uint64_t kCbufferSlotStride = 256;
constexpr uint32_t kRingSize          = 3;

struct Vertex {
    float pos[2];
    float uv[2];
};

std::vector<std::byte> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto end = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes;
    bytes.resize(static_cast<size_t>(end));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), end)) return {};
    return bytes;
}

void fill_uv_gradient(uint32_t* dst, uint32_t size) {
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t r = (x * 255u) / (size - 1);
            const uint32_t g = (y * 255u) / (size - 1);
            const uint32_t b = (((x >> 3) ^ (y >> 3)) & 1u) ? 0x40u : 0x00u;
            dst[y * size + x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

void z_rotation_matrix(float angle_rad, float* out16) {
    const float c = std::cos(angle_rad);
    const float s = std::sin(angle_rad);
    out16[ 0] =  c; out16[ 1] =  s; out16[ 2] = 0.0f; out16[ 3] = 0.0f;
    out16[ 4] = -s; out16[ 5] =  c; out16[ 6] = 0.0f; out16[ 7] = 0.0f;
    out16[ 8] = 0.0f; out16[ 9] = 0.0f; out16[10] = 1.0f; out16[11] = 0.0f;
    out16[12] = 0.0f; out16[13] = 0.0f; out16[14] = 0.0f; out16[15] = 1.0f;
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    tide::log::init();
    tracy::SetThreadName("main");

    TIDE_LOG_INFO("tide samples/04_imgui_overlay — frame-stats panel via the engine RHI");
    TIDE_LOG_INFO("Press Escape to exit, [C] to capture a .gputrace, [H] to save hash (TODO task 10).");
    TIDE_LOG_INFO("  capture prereq: MTL_CAPTURE_ENABLED=1 (terminal launches; Xcode sets it automatically)");
    TIDE_LOG_INFO("  capture output: $TIDE_CAPTURE_DIR or NSTemporaryDirectory");

    auto window_result = tide::platform::Window::create({
        .width  = 1280,
        .height = 720,
        .title  = "tide — 04_imgui_overlay",
    });
    if (!window_result) {
        TIDE_LOG_ERROR("Window::create failed: {}", static_cast<int>(window_result.error()));
        return 1;
    }
    auto window = std::move(*window_result);

    tide::input::InputSystem input(window);
    input.bind(tide::input::KeyBinding{ .action = tide::input::Actions::Quit, .key = tide::input::Key::Escape });
    input.bind(tide::input::KeyBinding{ .action = kCaptureFrame, .key = tide::input::Key::C });
    input.bind(tide::input::KeyBinding{ .action = kSaveHash,     .key = tide::input::Key::H });
    tide::input::GameplayContext gameplay_ctx;
    const uint32_t gameplay_handle = input.push_context(&gameplay_ctx);

    auto device_result = tide::rhi::metal::create_device(window);
    if (!device_result) {
        TIDE_LOG_ERROR("create_device failed: {}", static_cast<int>(device_result.error()));
        return 2;
    }
    auto device = std::move(*device_result);

    const auto& caps = device->capabilities();
    TIDE_LOG_INFO("RHI ready: backend={} device={} UMA={}",
                  caps.backend_name, caps.device_name,
                  caps.uniform_memory_architecture ? "yes" : "no");

    // ─── Scene shaders + pipeline (textured quad — same as sample 03) ─────────
    const auto shaders = tide::platform::executable_dir() / ".." / ".." / "shaders";
    auto vs_bytes = read_file_bytes(shaders / "textured_quad.vs.hlsl.metallib");
    auto fs_bytes = read_file_bytes(shaders / "textured_quad.ps.hlsl.metallib");
    if (vs_bytes.empty() || fs_bytes.empty()) {
        TIDE_LOG_ERROR("Failed to read textured_quad metallibs from {}", shaders.string());
        return 3;
    }

    tide::rhi::ShaderDesc vs_desc{};
    vs_desc.stage       = tide::rhi::ShaderStage::Vertex;
    vs_desc.bytecode    = std::span<const std::byte>(vs_bytes.data(), vs_bytes.size());
    vs_desc.entry_point = "main0";
    vs_desc.debug_name  = "scene.vs";
    auto vs = device->create_shader(vs_desc);

    tide::rhi::ShaderDesc fs_desc{};
    fs_desc.stage       = tide::rhi::ShaderStage::Fragment;
    fs_desc.bytecode    = std::span<const std::byte>(fs_bytes.data(), fs_bytes.size());
    fs_desc.entry_point = "main0";
    fs_desc.debug_name  = "scene.ps";
    auto fs = device->create_shader(fs_desc);

    if (!vs || !fs) { TIDE_LOG_ERROR("create_shader failed"); return 4; }

    using tide::rhi::DescriptorBindingDesc;
    using tide::rhi::DescriptorType;
    using tide::rhi::ShaderStage;
    const std::array<DescriptorBindingDesc, 3> bindings = {{
        { .slot = kCbufferSlot, .array_count = 1, .type = DescriptorType::UniformBuffer, .stages = ShaderStage::Vertex },
        { .slot = kTextureSlot, .array_count = 1, .type = DescriptorType::SampledTexture, .stages = ShaderStage::Fragment },
        { .slot = kSamplerSlot, .array_count = 1, .type = DescriptorType::Sampler, .stages = ShaderStage::Fragment },
    }};
    tide::rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings = std::span<const DescriptorBindingDesc>(bindings.data(), bindings.size());
    layout_desc.debug_name = "scene.layout";
    auto layout = device->create_descriptor_set_layout(layout_desc);
    if (!layout) { TIDE_LOG_ERROR("create_descriptor_set_layout failed"); return 5; }

    constexpr float kQuadHalf = 0.5f;
    const std::array<Vertex, 4> verts = {{
        {{-kQuadHalf, -kQuadHalf}, {0.0f, 1.0f}},
        {{ kQuadHalf, -kQuadHalf}, {1.0f, 1.0f}},
        {{ kQuadHalf,  kQuadHalf}, {1.0f, 0.0f}},
        {{-kQuadHalf,  kQuadHalf}, {0.0f, 0.0f}},
    }};
    const std::array<uint16_t, 6> indices = {0, 1, 2,  0, 2, 3};

    tide::rhi::BufferDesc vbuf_desc{};
    vbuf_desc.size_bytes = sizeof(verts);
    vbuf_desc.usage      = tide::rhi::BufferUsage::VertexBuffer | tide::rhi::BufferUsage::CopyDest;
    vbuf_desc.memory     = tide::rhi::MemoryType::DeviceLocal;
    vbuf_desc.debug_name = "scene.vbuf";
    auto vbuf = device->create_buffer(vbuf_desc);

    tide::rhi::BufferDesc ibuf_desc{};
    ibuf_desc.size_bytes = sizeof(indices);
    ibuf_desc.usage      = tide::rhi::BufferUsage::IndexBuffer | tide::rhi::BufferUsage::CopyDest;
    ibuf_desc.memory     = tide::rhi::MemoryType::DeviceLocal;
    ibuf_desc.debug_name = "scene.ibuf";
    auto ibuf = device->create_buffer(ibuf_desc);

    if (!vbuf || !ibuf) { TIDE_LOG_ERROR("create_buffer failed"); return 6; }
    if (auto r = device->upload_buffer(*vbuf, verts.data(), sizeof(verts)); !r) {
        TIDE_LOG_ERROR("upload_buffer(vbuf) failed: {}", static_cast<int>(r.error())); return 6;
    }
    if (auto r = device->upload_buffer(*ibuf, indices.data(), sizeof(indices)); !r) {
        TIDE_LOG_ERROR("upload_buffer(ibuf) failed: {}", static_cast<int>(r.error())); return 6;
    }

    tide::rhi::BufferDesc cbuf_desc{};
    cbuf_desc.size_bytes = kCbufferSlotStride * kRingSize;
    cbuf_desc.usage      = tide::rhi::BufferUsage::UniformBuffer | tide::rhi::BufferUsage::CopyDest;
    cbuf_desc.memory     = tide::rhi::MemoryType::Upload;
    cbuf_desc.debug_name = "scene.cbuf";
    auto cbuf = device->create_buffer(cbuf_desc);
    if (!cbuf) { TIDE_LOG_ERROR("create_buffer(cbuf) failed"); return 7; }

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
    tex_desc.debug_name      = "scene.checker";
    auto tex = device->create_texture(tex_desc);
    if (!tex) { TIDE_LOG_ERROR("create_texture failed"); return 8; }
    {
        std::array<uint32_t, kTexSize * kTexSize> pixels{};
        fill_uv_gradient(pixels.data(), kTexSize);
        if (auto u = device->upload_texture(*tex, pixels.data(), sizeof(pixels)); !u) {
            TIDE_LOG_ERROR("upload_texture failed: {}", static_cast<int>(u.error())); return 8;
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
    tv_desc.debug_name  = "scene.checker.view";
    auto tex_view = device->create_texture_view(tv_desc);
    if (!tex_view) { TIDE_LOG_ERROR("create_texture_view failed"); return 9; }

    tide::rhi::SamplerDesc smp_desc{};
    smp_desc.min_filter     = tide::rhi::FilterMode::Linear;
    smp_desc.mag_filter     = tide::rhi::FilterMode::Linear;
    smp_desc.mip_filter     = tide::rhi::MipFilterMode::NotMipmapped;
    smp_desc.address_u      = tide::rhi::AddressMode::ClampToEdge;
    smp_desc.address_v      = tide::rhi::AddressMode::ClampToEdge;
    smp_desc.address_w      = tide::rhi::AddressMode::ClampToEdge;
    smp_desc.max_anisotropy = 1;
    smp_desc.debug_name     = "scene.sampler";
    auto smp = device->create_sampler(smp_desc);
    if (!smp) { TIDE_LOG_ERROR("create_sampler failed"); return 10; }

    using tide::rhi::DescriptorWrite;
    std::array<DescriptorWrite, 3> writes{};
    writes[0].slot = kCbufferSlot; writes[0].type = DescriptorType::UniformBuffer;
    writes[0].buffer = *cbuf; writes[0].buffer_offset = 0;
    writes[0].buffer_range = sizeof(tide::shaders::PerFrame);
    writes[1].slot = kTextureSlot; writes[1].type = DescriptorType::SampledTexture;
    writes[1].texture = *tex_view;
    writes[2].slot = kSamplerSlot; writes[2].type = DescriptorType::Sampler;
    writes[2].sampler = *smp;

    tide::rhi::DescriptorSetDesc set_desc{};
    set_desc.layout = *layout;
    set_desc.initial_writes = std::span<const DescriptorWrite>(writes.data(), writes.size());
    set_desc.debug_name = "scene.set";
    auto dset = device->create_descriptor_set(set_desc);
    if (!dset) { TIDE_LOG_ERROR("create_descriptor_set failed"); return 11; }

    using tide::rhi::Format;
    using tide::rhi::VertexAttributeDesc;
    using tide::rhi::VertexBindingDesc;
    const std::array<VertexBindingDesc, 1> vbindings = {{
        { .binding = kVertexBufferSlot, .stride = sizeof(Vertex), .input_rate = tide::rhi::VertexInputRate::Vertex },
    }};
    const std::array<VertexAttributeDesc, 2> vattrs = {{
        { .location = 0, .binding = kVertexBufferSlot, .format = Format::RG32_Float, .offset = offsetof(Vertex, pos) },
        { .location = 1, .binding = kVertexBufferSlot, .format = Format::RG32_Float, .offset = offsetof(Vertex, uv)  },
    }};

    tide::rhi::GraphicsPipelineDesc pso_desc{};
    pso_desc.vertex_shader            = *vs;
    pso_desc.fragment_shader          = *fs;
    pso_desc.vertex_input             = { vbindings, vattrs };
    pso_desc.topology                 = tide::rhi::PrimitiveTopology::TriangleList;
    pso_desc.rasterization.cull       = tide::rhi::CullMode::None;
    pso_desc.rasterization.front_face = tide::rhi::FrontFace::CounterClockwise;
    pso_desc.color_attachment_count   = 1;
    pso_desc.color_formats[0]         = device->swapchain_format();
    pso_desc.color_blend[0].write_mask = 0xF;
    pso_desc.sample_count             = 1;
    pso_desc.debug_name               = "scene.pso";
    auto pso = device->create_graphics_pipeline(pso_desc);
    if (!pso) { TIDE_LOG_ERROR("create_graphics_pipeline failed: {}", static_cast<int>(pso.error())); return 12; }

    // ─── ImGui ──────────────────────────────────────────────────────────────
    tide::imgui::Context imgui_ctx(*device, window, &input);
    imgui_ctx.set_metallib_dir(shaders);
    if (auto r = imgui_ctx.create_resources(device->swapchain_format()); !r) {
        TIDE_LOG_ERROR("imgui::create_resources failed: {}", static_cast<int>(r.error()));
        return 13;
    }
    {
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }

    TIDE_LOG_INFO("Pipelines built. Entering frame loop.");

    // Frame timing + dt history for the ImGui plot.
    tide::platform::FrameTimer frame_timer;
    uint64_t frame_index = 0;
    float    elapsed_s   = 0.0f;
    float    dt_history[200] = {};
    size_t   dt_history_idx = 0;

    // Frame-capture state. capture_armed is set by [C] / the ImGui button;
    // begin_frame_capture is called at the top of the next frame, end at
    // the bottom of THAT same frame. MTLCaptureManager requires the
    // MTL_CAPTURE_ENABLED=1 environment variable for non-Xcode launches —
    // we log that in the failure path so the user knows the prerequisite.
    bool capture_armed  = false;
    bool capture_active = false;

    while (!window.should_close()) {
        ZoneScopedN("Main");

        input.begin_frame();
        tide::platform::Window::poll_events();

        if (input.is_just_pressed(tide::input::Actions::Quit)) {
            window.request_close();
        }
        if (input.is_just_pressed(kCaptureFrame)) {
            capture_armed = true;
            TIDE_LOG_INFO("[C] capture armed — next frame will write a .gputrace");
        }
        if (input.is_just_pressed(kSaveHash)) {
            TIDE_LOG_INFO("[H] SaveHash action — TODO: wire to offscreen-readback path (Phase 1 task 10)");
        }

        if (auto begin = device->begin_frame(); !begin) {
            TIDE_LOG_ERROR("begin_frame failed: {}", static_cast<int>(begin.error()));
            break;
        }

        // Arm a one-shot frame capture. Wraps THIS frame's submit; the
        // .gputrace lands in TIDE_CAPTURE_DIR (or NSTemporaryDirectory if
        // unset). Requires MTL_CAPTURE_ENABLED=1 outside Xcode.
        if (capture_armed) {
            capture_armed = false;
            capture_active = tide::rhi::metal::begin_frame_capture(*device, "04_imgui_overlay");
            if (!capture_active) {
                TIDE_LOG_WARN("capture: begin_frame_capture failed — set MTL_CAPTURE_ENABLED=1 "
                              "and re-run from a terminal (not Xcode), or run via Xcode itself");
            }
        }

        const auto dt = frame_timer.tick();
        const float dt_seconds = static_cast<float>(tide::platform::seconds(dt));
        elapsed_s += dt_seconds;
        dt_history[dt_history_idx % 200] = dt_seconds * 1000.0f;
        dt_history_idx = (dt_history_idx + 1) % 200;

        // Acquire the swapchain BEFORE ImGui::NewFrame so a SwapchainOutOfDate
        // bail doesn't leave ImGui mid-frame (NewFrame without Render asserts
        // on the next iteration). Post-Develop review finding.
        auto sw = device->acquire_swapchain_texture();
        if (!sw) {
            (void)device->end_frame();
            continue;
        }

        // Build the UI inside the same frame.
        imgui_ctx.begin_frame(dt_seconds);
        {
            ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(280.0f, 200.0f), ImGuiCond_FirstUseEver);
            ImGui::Begin("tide - frame stats");
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::Text("FPS:    %.1f", io.Framerate);
            ImGui::Text("dt:     %.2f ms", dt_seconds * 1000.0f);
            ImGui::Text("frame:  %llu", static_cast<unsigned long long>(frame_index));
            ImGui::Text("display: %.0f x %.0f pt @ %.1fx",
                        io.DisplaySize.x, io.DisplaySize.y, io.DisplayFramebufferScale.x);
            ImGui::Separator();
            ImGui::PlotLines("dt (ms)", dt_history, 200, 0, nullptr, 0.0f, 33.3f, ImVec2(0, 60));
            ImGui::Separator();
            ImGui::Text("[C] Capture next frame  (.gputrace via MTLCaptureManager)");
            ImGui::Text("[H] Save offscreen hash (TODO task 10)");
            if (ImGui::Button("Capture Frame")) {
                capture_armed = true;
                TIDE_LOG_INFO("[ImGui] capture armed");
            }
            ImGui::SameLine();
            if (ImGui::Button("Save Hash")) {
                TIDE_LOG_INFO("[ImGui click] Save Hash — TODO task 10");
            }
            ImGui::End();
        }

        const auto fb = window.framebuffer_size();
        const float aspect = (fb.height > 0)
            ? static_cast<float>(fb.width) / static_cast<float>(fb.height)
            : 1.0f;

        // Write per-frame uniform + repoint descriptor at the ring offset.
        const uint64_t ring_offset = (frame_index % kRingSize) * kCbufferSlotStride;
        {
            tide::shaders::PerFrame pf{};
            z_rotation_matrix(elapsed_s * 1.0f, pf.rotation);
            pf.time   = elapsed_s;
            pf.aspect = aspect;
            (void)device->upload_buffer(*cbuf, &pf, sizeof(pf), ring_offset);
            DescriptorWrite cb_write = writes[0];
            cb_write.buffer_offset   = ring_offset;
            device->update_descriptor_set(*dset, std::span<const DescriptorWrite>(&cb_write, 1));
        }

        auto* cmd = device->acquire_command_buffer();
        cmd->transition(*sw, tide::rhi::ResourceState::Undefined, tide::rhi::ResourceState::RenderTarget);

        tide::rhi::RenderPassDesc rp{};
        rp.color_attachment_count                = 1;
        rp.color_attachments[0].target.texture   = *sw;
        rp.color_attachments[0].load_op          = tide::rhi::LoadOp::Clear;
        rp.color_attachments[0].store_op         = tide::rhi::StoreOp::Store;
        rp.color_attachments[0].clear_value      = {0.05f, 0.07f, 0.10f, 1.0f};
        rp.render_area = {0, 0, static_cast<uint32_t>(fb.width), static_cast<uint32_t>(fb.height)};

        cmd->begin_render_pass(rp);

        const tide::rhi::Viewport vp{
            .x = 0.0f, .y = 0.0f,
            .width  = static_cast<float>(fb.width),
            .height = static_cast<float>(fb.height),
            .min_depth = 0.0f, .max_depth = 1.0f,
        };
        cmd->set_viewport(vp);
        cmd->set_scissor({0, 0, static_cast<uint32_t>(fb.width), static_cast<uint32_t>(fb.height)});

        // Scene draw.
        cmd->bind_pipeline(*pso);
        cmd->bind_descriptor_set(0, *dset);
        cmd->bind_vertex_buffer(kVertexBufferSlot, *vbuf, 0);
        cmd->bind_index_buffer(*ibuf, 0, tide::rhi::IndexType::Uint16);
        cmd->draw_indexed(static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);

        // ImGui draws inside the same render pass (Metal TBDR avoids a tile flush).
        imgui_ctx.render(*cmd, static_cast<uint32_t>(fb.width), static_cast<uint32_t>(fb.height));

        cmd->end_render_pass();
        cmd->transition(*sw, tide::rhi::ResourceState::RenderTarget, tide::rhi::ResourceState::Present);

        device->submit(cmd);
        (void)device->end_frame();

        if (capture_active) {
            tide::rhi::metal::end_frame_capture();
            capture_active = false;
            const char* dir = std::getenv("TIDE_CAPTURE_DIR");
            TIDE_LOG_INFO("capture: .gputrace written to {}",
                          dir ? dir : "$TMPDIR (NSTemporaryDirectory)");
        }

        if (++frame_index % 120 == 0) {
            TIDE_LOG_DEBUG("frame {} dt={:.3f}ms", frame_index, dt_seconds * 1000.0f);
        }
        FrameMark;
    }

    // Tear-down: ImGui ctx first (its dtor releases its rhi handles), then the
    // scene resources, then the device falls out of scope.
    // imgui_ctx falls out of scope at the end of main; its dtor handles cleanup.

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
