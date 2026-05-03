// samples/05_offscreen_hash — Phase 1 task 10 deliverable.
//
// Renders the deterministic colored-triangle (samples/02 shaders) to an
// offscreen 256x256 RGBA8 texture, downloads the pixels via the new
// IDevice::download_texture path, and prints an xxh3 64-bit hash. With
// `--check <hex>` it compares against the expected hash and exits 0 on
// match, 1 on mismatch — the CI assertion that catches "build looks fine
// but pixels are wrong" regressions like the recent Task 7/8 black-quad bug.
//
// No window. The program is a single-shot CLI: create device with no
// swapchain (we render into our own MTLTexture), draw, blit, hash, exit.

#include "tide/core/Log.h"
#include "tide/platform/Path.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kRenderW = 256;
constexpr uint32_t kRenderH = 256;

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

} // namespace

int main(int argc, char** argv) {
    tide::log::init();

    // Optional: --check <hex>  — assert the printed hash matches and return 0/1.
    bool        check_mode = false;
    std::string expected_hex;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--check" && i + 1 < argc) {
            check_mode = true;
            expected_hex = argv[++i];
        }
    }

    // We need a Window to construct a MetalDevice (the constructor wires up
    // the CAMetalLayer to the window's contentView). The window stays
    // hidden — task 10 is headless.
    auto window_result = tide::platform::Window::create({
        .width  = 64,
        .height = 64,
        .title  = "tide — 05_offscreen_hash",
        .resizable = false,
        .start_visible = false,
    });
    if (!window_result) {
        TIDE_LOG_ERROR("Window::create failed: {}",
                       static_cast<int>(window_result.error()));
        return 2;
    }
    auto window = std::move(*window_result);

    auto device_result = tide::rhi::metal::create_device(window);
    if (!device_result) {
        TIDE_LOG_ERROR("create_device failed: {}",
                       static_cast<int>(device_result.error()));
        return 2;
    }
    auto device = std::move(*device_result);

    // ─── Shaders ─────────────────────────────────────────────────────────────
    const auto shaders = tide::platform::executable_dir() / ".." / ".." / "shaders";
    auto vs_bytes = read_file_bytes(shaders / "triangle.vs.hlsl.metallib");
    auto fs_bytes = read_file_bytes(shaders / "triangle.ps.hlsl.metallib");
    if (vs_bytes.empty() || fs_bytes.empty()) {
        TIDE_LOG_ERROR("Failed to read triangle metallibs from {}", shaders.string());
        return 3;
    }

    tide::rhi::ShaderDesc vs_desc{};
    vs_desc.stage       = tide::rhi::ShaderStage::Vertex;
    vs_desc.bytecode    = std::span<const std::byte>(vs_bytes.data(), vs_bytes.size());
    vs_desc.entry_point = "main0";
    vs_desc.debug_name  = "off.vs";
    auto vs = device->create_shader(vs_desc);

    tide::rhi::ShaderDesc fs_desc{};
    fs_desc.stage       = tide::rhi::ShaderStage::Fragment;
    fs_desc.bytecode    = std::span<const std::byte>(fs_bytes.data(), fs_bytes.size());
    fs_desc.entry_point = "main0";
    fs_desc.debug_name  = "off.ps";
    auto fs = device->create_shader(fs_desc);

    if (!vs || !fs) { TIDE_LOG_ERROR("create_shader failed"); return 4; }

    // ─── Offscreen RT (256x256 RGBA8, used as render target + copy source) ───
    tide::rhi::TextureDesc rt_desc{};
    rt_desc.dimension       = tide::rhi::TextureDimension::Tex2D;
    rt_desc.format          = tide::rhi::Format::RGBA8_Unorm;
    rt_desc.width           = kRenderW;
    rt_desc.height          = kRenderH;
    rt_desc.depth_or_layers = 1;
    rt_desc.mip_levels      = 1;
    rt_desc.sample_count    = 1;
    rt_desc.usage           = tide::rhi::TextureUsage::RenderTarget |
                              tide::rhi::TextureUsage::CopySource;
    rt_desc.memory          = tide::rhi::MemoryType::DeviceLocal;
    rt_desc.debug_name      = "off.rt";
    auto rt = device->create_texture(rt_desc);
    if (!rt) { TIDE_LOG_ERROR("create_texture failed"); return 5; }

    // ─── Pipeline matching the offscreen format ──────────────────────────────
    tide::rhi::GraphicsPipelineDesc pso_desc{};
    pso_desc.vertex_shader              = *vs;
    pso_desc.fragment_shader            = *fs;
    pso_desc.topology                   = tide::rhi::PrimitiveTopology::TriangleList;
    pso_desc.rasterization.cull         = tide::rhi::CullMode::None;
    pso_desc.rasterization.front_face   = tide::rhi::FrontFace::CounterClockwise;
    pso_desc.color_attachment_count     = 1;
    pso_desc.color_formats[0]           = tide::rhi::Format::RGBA8_Unorm;
    pso_desc.color_blend[0].write_mask  = 0xF;
    pso_desc.sample_count               = 1;
    pso_desc.debug_name                 = "off.pso";
    auto pso = device->create_graphics_pipeline(pso_desc);
    if (!pso) { TIDE_LOG_ERROR("create_graphics_pipeline failed"); return 6; }

    // ─── Render the deterministic triangle into the offscreen RT ─────────────
    if (auto begin = device->begin_frame(); !begin) {
        TIDE_LOG_ERROR("begin_frame failed"); return 7;
    }
    auto* cmd = device->acquire_command_buffer();
    cmd->transition(*rt, tide::rhi::ResourceState::Undefined,
                          tide::rhi::ResourceState::RenderTarget);

    tide::rhi::RenderPassDesc rp{};
    rp.color_attachment_count                = 1;
    rp.color_attachments[0].target.texture   = *rt;
    rp.color_attachments[0].load_op          = tide::rhi::LoadOp::Clear;
    rp.color_attachments[0].store_op         = tide::rhi::StoreOp::Store;
    rp.color_attachments[0].clear_value      = {0.05f, 0.07f, 0.10f, 1.0f};
    rp.render_area = {0, 0, kRenderW, kRenderH};
    cmd->begin_render_pass(rp);

    cmd->set_viewport({0.0f, 0.0f, static_cast<float>(kRenderW),
                       static_cast<float>(kRenderH), 0.0f, 1.0f});
    cmd->set_scissor({0, 0, kRenderW, kRenderH});
    cmd->bind_pipeline(*pso);
    cmd->draw(3, 1, 0, 0);

    cmd->end_render_pass();
    cmd->transition(*rt, tide::rhi::ResourceState::RenderTarget,
                          tide::rhi::ResourceState::CopySource);
    device->submit(cmd);
    (void)device->end_frame();

    // ─── Download + hash ─────────────────────────────────────────────────────
    std::vector<uint8_t> pixels(static_cast<size_t>(kRenderW) * kRenderH * 4);
    if (auto r = device->download_texture(*rt, pixels.data(), pixels.size());
        !r) {
        TIDE_LOG_ERROR("download_texture failed: {}", static_cast<int>(r.error()));
        return 8;
    }

    const uint64_t hash = XXH3_64bits(pixels.data(), pixels.size());
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hash));
    TIDE_LOG_INFO("offscreen hash: {} (size {}x{} = {} B)",
                  hex, kRenderW, kRenderH, pixels.size());

    int rc = 0;
    if (check_mode) {
        if (expected_hex == hex) {
            TIDE_LOG_INFO("hash match: PASS");
        } else {
            TIDE_LOG_ERROR("hash mismatch: expected {} got {}", expected_hex, hex);
            rc = 1;
        }
    }

    device->destroy_pipeline(*pso);
    device->destroy_texture(*rt);
    device->destroy_shader(*vs);
    device->destroy_shader(*fs);
    return rc;
}
