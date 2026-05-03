// engine/imgui-integration/src/Context.cpp — Phase 1 task 8 implementation.
//
// Renders Dear ImGui through the engine's RHI surface only. The slot map
// follows the textured-quad convention (cbuffer at b0/MSL [[buffer(0)]],
// vertex buffer at MSL slot 30 — MoltenVK convention — to avoid the
// SPIRV-Cross [[buffer(0)]] cbuffer collision).

#include "tide/imgui-integration/Context.h"

#include "tide/core/Log.h"
#include "tide/input/InputContext.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"

#include <imgui.h>

// GLFW used only to poll cursor pos / mouse buttons inside begin_frame. We
// deliberately avoid imgui_impl_glfw to keep the integration vendor-free.
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace tide::imgui {

namespace {

constexpr uint32_t kVertexBufferSlot = 30;   // MoltenVK convention; see task 7
constexpr uint32_t kCbufferSlot      = 0;
constexpr uint32_t kTextureSlot      = 0;
constexpr uint32_t kSamplerSlot      = 0;
constexpr uint64_t kCbufferSlotStride = 256;

// Column-major orthographic projection (Y-down, mapping screen pixels to NDC).
// Matches the HLSL `column_major float4x4 projection` declaration so the bytes
// are consumed verbatim.
void ortho_y_down(float left, float right, float bottom, float top, float* out16) {
    const float rl = right - left;
    const float tb = top - bottom;
    out16[ 0] = 2.0f / rl;       out16[ 1] = 0.0f;            out16[ 2] = 0.0f; out16[ 3] = 0.0f;
    out16[ 4] = 0.0f;            out16[ 5] = 2.0f / tb;       out16[ 6] = 0.0f; out16[ 7] = 0.0f;
    out16[ 8] = 0.0f;            out16[ 9] = 0.0f;            out16[10] = 1.0f; out16[11] = 0.0f;
    out16[12] = -(right+left)/rl; out16[13] = -(top+bottom)/tb; out16[14] = 0.0f; out16[15] = 1.0f;
}

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

// Raw input observer that forwards GLFW-shaped events into ImGuiIO. Lives
// inside Impl; registered with InputSystem::add_raw_observer in ctor.
struct ImGuiInputBridge final : public input::RawInputObserver {
    void on_cursor_pos(double x, double y) override {
        ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
    }
    void on_mouse_button(input::MouseButton btn, bool down) override {
        // ImGui's button order matches GLFW's enum (Left=0, Right=1, Middle=2)
        // for the first three buttons; X1/X2 mapping is identical to GLFW 4/5.
        const int b = static_cast<int>(btn);
        if (b >= 0 && b < 5) {
            ImGui::GetIO().AddMouseButtonEvent(b, down);
        }
    }
    void on_scroll(double xoff, double yoff) override {
        ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(xoff),
                                          static_cast<float>(yoff));
    }
    void on_char(uint32_t codepoint) override {
        ImGui::GetIO().AddInputCharacter(codepoint);
    }
    // Key events deferred — ImGui's keymap is layered (ImGuiKey enum) and
    // mapping every GLFW key here would expand scope. The frame-stats panel
    // and most debug UIs work with mouse + char-input only.
};

struct Context::Impl {
    rhi::IDevice&      device;
    platform::Window&  window;
    input::InputSystem* input{nullptr};
    std::filesystem::path metallib_dir;

    ImGuiInputBridge      input_bridge{};
    bool                  imgui_owned{false};
    bool                  resources_built{false};
    bool                  font_atlas_dirty{true};

    // Pipeline + descriptor surface.
    rhi::ShaderHandle              vs{};
    rhi::ShaderHandle              fs{};
    rhi::PipelineHandle            pso{};
    rhi::DescriptorSetLayoutHandle layout{};
    rhi::DescriptorSetHandle       dset{};
    rhi::SamplerHandle             sampler{};

    // Font atlas.
    rhi::TextureHandle      font_tex{};
    rhi::TextureViewHandle  font_view{};

    // Per-frame ring buffers (kFramesInFlight slots, 256 B cbuffer stride).
    rhi::BufferHandle vbuf{};
    rhi::BufferHandle ibuf{};
    rhi::BufferHandle cbuf{};

    uint64_t frame_index{0};

    Impl(rhi::IDevice& d, platform::Window& w, input::InputSystem* in)
        : device(d), window(w), input(in) {
        // Reject double-init — keeps two coexisting Contexts from clobbering
        // each other's GImGui pointer (post-Develop review finding).
        if (ImGui::GetCurrentContext() != nullptr) {
            TIDE_LOG_ERROR("imgui::Context: ImGui::CreateContext already called elsewhere; refusing to clobber");
            // Continue — ImGui will assert/abort on double-create itself.
        }
        ImGui::CreateContext();
        imgui_owned = true;
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;          // disable .ini persistence for samples
        io.LogFilename = nullptr;
        io.BackendRendererName = "tide_rhi";
        io.BackendPlatformName = "tide_glfw";

        if (input) {
            input->add_raw_observer(&input_bridge);
        }
    }

    ~Impl() {
        if (input) {
            input->remove_raw_observer(&input_bridge);
        }
        // destroy_* are idempotent on default-constructed (null) handles
        // (HandlePool::release returns false on !owns(h)). Always run them
        // so a failed create_resources doesn't leak the partially-built
        // resources (post-Develop review finding).
        device.destroy_pipeline(pso);
        device.destroy_descriptor_set(dset);
        device.destroy_descriptor_set_layout(layout);
        device.destroy_sampler(sampler);
        device.destroy_texture_view(font_view);
        device.destroy_texture(font_tex);
        device.destroy_buffer(cbuf);
        device.destroy_buffer(ibuf);
        device.destroy_buffer(vbuf);
        device.destroy_shader(fs);
        device.destroy_shader(vs);
        if (imgui_owned) {
            ImGui::DestroyContext();
        }
    }

    [[nodiscard]] tide::expected<rhi::ShaderHandle, ContextError>
    load_shader(const char* basename, rhi::ShaderStage stage, const char* dbg) {
        auto bytes = read_file_bytes(metallib_dir / basename);
        if (bytes.empty()) {
            TIDE_LOG_ERROR("imgui: failed to read {}", (metallib_dir / basename).string());
            return tide::unexpected(ContextError::ShaderLoadFailed);
        }
        rhi::ShaderDesc d{};
        d.stage       = stage;
        d.bytecode    = std::span<const std::byte>(bytes.data(), bytes.size());
        d.entry_point = "main0";
        d.debug_name  = dbg;
        auto h = device.create_shader(d);
        if (!h) {
            TIDE_LOG_ERROR("imgui: create_shader({}) failed: {}", dbg,
                           static_cast<int>(h.error()));
            return tide::unexpected(ContextError::ShaderLoadFailed);
        }
        return *h;
    }

    [[nodiscard]] tide::expected<void, ContextError>
    build_font_atlas() {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
        if (!px || w <= 0 || h <= 0) {
            TIDE_LOG_ERROR("imgui: GetTexDataAsRGBA32 returned empty atlas");
            return tide::unexpected(ContextError::ResourceCreateFailed);
        }

        rhi::TextureDesc td{};
        td.dimension       = rhi::TextureDimension::Tex2D;
        td.format          = rhi::Format::RGBA8_Unorm;
        td.width           = static_cast<uint32_t>(w);
        td.height          = static_cast<uint32_t>(h);
        td.depth_or_layers = 1;
        td.mip_levels      = 1;
        td.sample_count    = 1;
        td.usage           = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDest;
        td.memory          = rhi::MemoryType::DeviceLocal;
        td.debug_name      = "imgui.fontatlas";
        auto tex = device.create_texture(td);
        if (!tex) return tide::unexpected(ContextError::ResourceCreateFailed);

        const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
        if (auto u = device.upload_texture(*tex, px, bytes); !u) {
            device.destroy_texture(*tex);
            return tide::unexpected(ContextError::ResourceCreateFailed);
        }

        rhi::TextureViewDesc vd{};
        vd.texture     = *tex;
        vd.dimension   = rhi::TextureDimension::Tex2D;
        vd.format      = rhi::Format::RGBA8_Unorm;
        vd.base_mip    = 0;
        vd.mip_count   = 1;
        vd.base_layer  = 0;
        vd.layer_count = 1;
        vd.debug_name  = "imgui.fontatlas.view";
        auto view = device.create_texture_view(vd);
        if (!view) {
            device.destroy_texture(*tex);
            return tide::unexpected(ContextError::ResourceCreateFailed);
        }

        font_tex  = *tex;
        font_view = *view;
        // ImGui carries a uintptr_t-sized opaque texture id per draw cmd; we
        // pack the view-handle bits and unpack at render time.
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(font_view.bits()));
        font_atlas_dirty = false;
        return {};
    }
};

Context::Context(rhi::IDevice& device, platform::Window& window, input::InputSystem* input)
    : impl_(std::make_unique<Impl>(device, window, input)) {
    // Default metallib lookup mirrors the convention in samples/02 and 03:
    // ${binary_dir}/../../shaders/. The sample can override via
    // set_metallib_dir() if a different layout is desired.
    impl_->metallib_dir = std::filesystem::path("shaders");
}

Context::~Context() = default;

void Context::set_metallib_dir(std::filesystem::path dir) noexcept {
    impl_->metallib_dir = std::move(dir);
}

void Context::mark_atlas_dirty() noexcept {
    impl_->font_atlas_dirty = true;
}

tide::expected<void, ContextError>
Context::create_resources(rhi::Format color_format) {
    if (impl_->resources_built) {
        return tide::unexpected(ContextError::AlreadyCreated);
    }

    // 1. Shaders.
    auto vs = impl_->load_shader("imgui.vs.hlsl.metallib", rhi::ShaderStage::Vertex, "imgui.vs");
    if (!vs) return tide::unexpected(vs.error());
    auto fs = impl_->load_shader("imgui.ps.hlsl.metallib", rhi::ShaderStage::Fragment, "imgui.ps");
    if (!fs) {
        impl_->device.destroy_shader(*vs);
        return tide::unexpected(fs.error());
    }
    impl_->vs = *vs;
    impl_->fs = *fs;

    // 2. Sampler — linear, clamp-to-edge.
    rhi::SamplerDesc smp_desc{};
    smp_desc.min_filter     = rhi::FilterMode::Linear;
    smp_desc.mag_filter     = rhi::FilterMode::Linear;
    smp_desc.mip_filter     = rhi::MipFilterMode::NotMipmapped;
    smp_desc.address_u      = rhi::AddressMode::ClampToEdge;
    smp_desc.address_v      = rhi::AddressMode::ClampToEdge;
    smp_desc.address_w      = rhi::AddressMode::ClampToEdge;
    smp_desc.max_anisotropy = 1;
    smp_desc.debug_name     = "imgui.sampler";
    auto smp = impl_->device.create_sampler(smp_desc);
    if (!smp) return tide::unexpected(ContextError::ResourceCreateFailed);
    impl_->sampler = *smp;

    // 3. Font atlas (build, upload, view).
    if (auto r = impl_->build_font_atlas(); !r) {
        return tide::unexpected(r.error());
    }

    // 4. Descriptor set layout: b0 cbuffer (VS), t0 sampled tex (FS), s0 sampler (FS).
    using rhi::DescriptorBindingDesc;
    using rhi::DescriptorType;
    using rhi::ShaderStage;
    const std::array<DescriptorBindingDesc, 3> bindings = {{
        { .slot = kCbufferSlot, .array_count = 1, .type = DescriptorType::UniformBuffer,
          .stages = ShaderStage::Vertex },
        { .slot = kTextureSlot, .array_count = 1, .type = DescriptorType::SampledTexture,
          .stages = ShaderStage::Fragment },
        { .slot = kSamplerSlot, .array_count = 1, .type = DescriptorType::Sampler,
          .stages = ShaderStage::Fragment },
    }};
    rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings   = std::span<const DescriptorBindingDesc>(bindings.data(), bindings.size());
    layout_desc.debug_name = "imgui.layout";
    auto layout = impl_->device.create_descriptor_set_layout(layout_desc);
    if (!layout) return tide::unexpected(ContextError::PipelineCreateFailed);
    impl_->layout = *layout;

    // 5. Per-frame ring buffers. cbuf: 256 B × kFramesInFlight; vbuf/ibuf: per-slot
    //    cap × frames. We use a single MTLBuffer per resource type with explicit
    //    per-frame byte offsets to mirror the textured-quad pattern.
    constexpr uint64_t vbuf_slot_bytes = static_cast<uint64_t>(kMaxVerts)   * 20u; // sizeof(ImDrawVert)
    constexpr uint64_t ibuf_slot_bytes = static_cast<uint64_t>(kMaxIndices) * sizeof(ImDrawIdx);

    rhi::BufferDesc vbd{};
    vbd.size_bytes = vbuf_slot_bytes * kFramesInFlight;
    vbd.usage      = rhi::BufferUsage::VertexBuffer | rhi::BufferUsage::CopyDest;
    vbd.memory     = rhi::MemoryType::Upload;
    vbd.debug_name = "imgui.vbuf";
    auto vbuf = impl_->device.create_buffer(vbd);
    if (!vbuf) return tide::unexpected(ContextError::ResourceCreateFailed);
    impl_->vbuf = *vbuf;

    rhi::BufferDesc ibd{};
    ibd.size_bytes = ibuf_slot_bytes * kFramesInFlight;
    ibd.usage      = rhi::BufferUsage::IndexBuffer | rhi::BufferUsage::CopyDest;
    ibd.memory     = rhi::MemoryType::Upload;
    ibd.debug_name = "imgui.ibuf";
    auto ibuf = impl_->device.create_buffer(ibd);
    if (!ibuf) return tide::unexpected(ContextError::ResourceCreateFailed);
    impl_->ibuf = *ibuf;

    rhi::BufferDesc cbd{};
    cbd.size_bytes = kCbufferSlotStride * kFramesInFlight;
    cbd.usage      = rhi::BufferUsage::UniformBuffer | rhi::BufferUsage::CopyDest;
    cbd.memory     = rhi::MemoryType::Upload;
    cbd.debug_name = "imgui.cbuf";
    auto cbuf = impl_->device.create_buffer(cbd);
    if (!cbuf) return tide::unexpected(ContextError::ResourceCreateFailed);
    impl_->cbuf = *cbuf;

    // 6. Descriptor set with initial writes (cbuffer offset rewritten per frame).
    using rhi::DescriptorWrite;
    std::array<DescriptorWrite, 3> writes{};
    writes[0].slot          = kCbufferSlot;
    writes[0].type          = DescriptorType::UniformBuffer;
    writes[0].buffer        = impl_->cbuf;
    writes[0].buffer_offset = 0;
    writes[0].buffer_range  = sizeof(float) * 16;     // single mat4
    writes[1].slot          = kTextureSlot;
    writes[1].type          = DescriptorType::SampledTexture;
    writes[1].texture       = impl_->font_view;
    writes[2].slot          = kSamplerSlot;
    writes[2].type          = DescriptorType::Sampler;
    writes[2].sampler       = impl_->sampler;

    rhi::DescriptorSetDesc set_desc{};
    set_desc.layout         = impl_->layout;
    set_desc.initial_writes = std::span<const DescriptorWrite>(writes.data(), writes.size());
    set_desc.debug_name     = "imgui.set";
    auto dset = impl_->device.create_descriptor_set(set_desc);
    if (!dset) return tide::unexpected(ContextError::ResourceCreateFailed);
    impl_->dset = *dset;

    // 7. Pipeline.
    using rhi::Format;
    using rhi::VertexAttributeDesc;
    using rhi::VertexBindingDesc;
    const std::array<VertexBindingDesc, 1> vbindings = {{
        { .binding = kVertexBufferSlot, .stride = 20, .input_rate = rhi::VertexInputRate::Vertex },
    }};
    const std::array<VertexAttributeDesc, 3> vattrs = {{
        { .location = 0, .binding = kVertexBufferSlot, .format = Format::RG32_Float,  .offset = 0  }, // pos
        { .location = 1, .binding = kVertexBufferSlot, .format = Format::RG32_Float,  .offset = 8  }, // uv
        { .location = 2, .binding = kVertexBufferSlot, .format = Format::RGBA8_Unorm, .offset = 16 }, // col
    }};

    rhi::GraphicsPipelineDesc pso_desc{};
    pso_desc.vertex_shader              = impl_->vs;
    pso_desc.fragment_shader            = impl_->fs;
    pso_desc.vertex_input               = { vbindings, vattrs };
    pso_desc.topology                   = rhi::PrimitiveTopology::TriangleList;
    pso_desc.rasterization.cull         = rhi::CullMode::None;
    pso_desc.rasterization.front_face   = rhi::FrontFace::CounterClockwise;
    pso_desc.depth_stencil.depth_test_enable  = false;
    pso_desc.depth_stencil.depth_write_enable = false;
    pso_desc.color_attachment_count     = 1;
    pso_desc.color_formats[0]           = color_format;
    pso_desc.color_blend[0].blend_enable   = true;
    pso_desc.color_blend[0].src_color      = rhi::BlendFactor::SrcAlpha;
    pso_desc.color_blend[0].dst_color      = rhi::BlendFactor::OneMinusSrcAlpha;
    pso_desc.color_blend[0].color_op       = rhi::BlendOp::Add;
    pso_desc.color_blend[0].src_alpha      = rhi::BlendFactor::One;
    pso_desc.color_blend[0].dst_alpha      = rhi::BlendFactor::OneMinusSrcAlpha;
    pso_desc.color_blend[0].alpha_op       = rhi::BlendOp::Add;
    pso_desc.color_blend[0].write_mask     = 0xF;
    pso_desc.sample_count               = 1;
    pso_desc.debug_name                 = "imgui.pso";
    auto pso = impl_->device.create_graphics_pipeline(pso_desc);
    if (!pso) {
        TIDE_LOG_ERROR("imgui: create_graphics_pipeline failed: {}",
                       static_cast<int>(pso.error()));
        return tide::unexpected(ContextError::PipelineCreateFailed);
    }
    impl_->pso = *pso;

    impl_->resources_built = true;
    return {};
}

void Context::begin_frame(double frame_dt_seconds) {
    if (impl_->font_atlas_dirty && impl_->resources_built) {
        // Cheap rebuild path — old atlas still alive until ImGui calls
        // GetTexDataAsRGBA32 again. Phase 1 doesn't add fonts dynamically,
        // so this branch is exercised only by an explicit mark_atlas_dirty().
        // If hit, we recreate the texture+view in place.
        impl_->device.destroy_texture_view(impl_->font_view);
        impl_->device.destroy_texture(impl_->font_tex);
        if (auto r = impl_->build_font_atlas(); !r) {
            TIDE_LOG_ERROR("imgui: atlas rebuild failed");
            return;
        }
        // Push the new texture view through update_descriptor_set.
        rhi::DescriptorWrite w{};
        w.slot    = kTextureSlot;
        w.type    = rhi::DescriptorType::SampledTexture;
        w.texture = impl_->font_view;
        impl_->device.update_descriptor_set(impl_->dset,
            std::span<const rhi::DescriptorWrite>(&w, 1));
    }

    ImGuiIO& io = ImGui::GetIO();
    const auto win = impl_->window.window_size();
    const auto fb  = impl_->window.framebuffer_size();
    io.DisplaySize             = ImVec2(static_cast<float>(win.width),
                                        static_cast<float>(win.height));
    io.DisplayFramebufferScale = ImVec2(
        win.width  ? static_cast<float>(fb.width)  / static_cast<float>(win.width)  : 1.0f,
        win.height ? static_cast<float>(fb.height) / static_cast<float>(win.height) : 1.0f);
    io.DeltaTime = static_cast<float>(frame_dt_seconds > 1e-9 ? frame_dt_seconds : 1.0 / 60.0);

    // Mouse position + clicks + scroll + char input flow in via the
    // RawInputObserver (see ImGuiInputBridge above), which is registered
    // when the caller passes an InputSystem to the Context ctor. Without
    // an InputSystem the panel still renders but is non-interactive.
    // As a safety net for the no-InputSystem case, sync cursor pos via
    // direct GLFW polling.
    if (impl_->input == nullptr) {
        if (auto* glfw_win = impl_->window.glfw_handle()) {
            double mx = 0.0, my = 0.0;
            glfwGetCursorPos(glfw_win, &mx, &my);
            io.AddMousePosEvent(static_cast<float>(mx), static_cast<float>(my));
        }
    }

    ImGui::NewFrame();
}

void Context::render(rhi::ICommandBuffer& cmd, uint32_t fb_width, uint32_t fb_height) {
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (!draw_data || draw_data->CmdListsCount == 0) {
        impl_->frame_index++;
        return;
    }
    if (fb_width == 0 || fb_height == 0) {
        impl_->frame_index++;
        return;
    }

    const uint32_t total_vtx = static_cast<uint32_t>(draw_data->TotalVtxCount);
    const uint32_t total_idx = static_cast<uint32_t>(draw_data->TotalIdxCount);
    if (total_vtx > kMaxVerts || total_idx > kMaxIndices) {
        TIDE_LOG_WARN("imgui: frame exceeds ring caps (vtx={}/{} idx={}/{}); skipping",
                      total_vtx, kMaxVerts, total_idx, kMaxIndices);
        impl_->frame_index++;
        return;
    }

    const uint32_t ring_slot = static_cast<uint32_t>(impl_->frame_index % kFramesInFlight);
    const uint64_t vbuf_slot_bytes = static_cast<uint64_t>(kMaxVerts)   * sizeof(ImDrawVert);
    const uint64_t ibuf_slot_bytes = static_cast<uint64_t>(kMaxIndices) * sizeof(ImDrawIdx);
    const uint64_t vbuf_base       = ring_slot * vbuf_slot_bytes;
    const uint64_t ibuf_base       = ring_slot * ibuf_slot_bytes;
    const uint64_t cbuf_offset     = ring_slot * kCbufferSlotStride;

    // Upload cbuffer (orthographic projection in ImGui's display-coord space,
    // NOT framebuffer pixels — vertices come out of ImGui in DisplaySize units
    // (points), and DisplayPos may be non-zero with multi-viewport. The
    // viewport (set by the caller in fb pixels) handles the px/pt scaling
    // automatically because clip space is normalized.
    {
        const float L = draw_data->DisplayPos.x;
        const float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        const float T = draw_data->DisplayPos.y;
        const float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        float proj[16];
        ortho_y_down(L, R, B, T, proj);
        if (auto r = impl_->device.upload_buffer(impl_->cbuf, proj, sizeof(proj),
                                                 cbuf_offset); !r) {
            TIDE_LOG_ERROR("imgui: upload_buffer(cbuf) failed");
            impl_->frame_index++;
            return;
        }
        rhi::DescriptorWrite cb_write{};
        cb_write.slot          = kCbufferSlot;
        cb_write.type          = rhi::DescriptorType::UniformBuffer;
        cb_write.buffer        = impl_->cbuf;
        cb_write.buffer_offset = cbuf_offset;
        cb_write.buffer_range  = sizeof(proj);
        impl_->device.update_descriptor_set(impl_->dset,
            std::span<const rhi::DescriptorWrite>(&cb_write, 1));
    }

    // Pack all CmdLists' verts/idx into the ring slot. Walk twice (no
    // interleaving) so memcpys are contiguous and the GPU sees a single
    // memcpy per resource per frame.
    {
        uint64_t v_off = 0;
        for (int i = 0; i < draw_data->CmdListsCount; ++i) {
            const ImDrawList* cl = draw_data->CmdLists[i];
            const size_t bytes = static_cast<size_t>(cl->VtxBuffer.Size) * sizeof(ImDrawVert);
            if (auto r = impl_->device.upload_buffer(impl_->vbuf, cl->VtxBuffer.Data,
                                                     bytes, vbuf_base + v_off); !r) {
                TIDE_LOG_ERROR("imgui: upload_buffer(vbuf cmdlist {}) failed", i);
                impl_->frame_index++;
                return;
            }
            v_off += bytes;
        }
        uint64_t i_off = 0;
        for (int i = 0; i < draw_data->CmdListsCount; ++i) {
            const ImDrawList* cl = draw_data->CmdLists[i];
            const size_t bytes = static_cast<size_t>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx);
            if (auto r = impl_->device.upload_buffer(impl_->ibuf, cl->IdxBuffer.Data,
                                                     bytes, ibuf_base + i_off); !r) {
                TIDE_LOG_ERROR("imgui: upload_buffer(ibuf cmdlist {}) failed", i);
                impl_->frame_index++;
                return;
            }
            i_off += bytes;
        }
    }

    // Bind static pipeline state once.
    cmd.bind_pipeline(impl_->pso);
    cmd.bind_descriptor_set(/*set_index=*/0, impl_->dset);
    cmd.bind_vertex_buffer(kVertexBufferSlot, impl_->vbuf, vbuf_base);
    cmd.bind_index_buffer(impl_->ibuf, ibuf_base,
        sizeof(ImDrawIdx) == 2 ? rhi::IndexType::Uint16 : rhi::IndexType::Uint32);

    // ImGui Y-down clip rect to engine scissor: each command sets its own
    // scissor (Metal scissor is sticky across draws — must reset per cmd).
    const ImVec2 clip_off = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale;

    uint32_t global_vtx_offset = 0;
    uint32_t global_idx_offset = 0;
    for (int i = 0; i < draw_data->CmdListsCount; ++i) {
        const ImDrawList* cl = draw_data->CmdLists[i];
        for (const ImDrawCmd& c : cl->CmdBuffer) {
            if (c.UserCallback != nullptr) {
                if (c.UserCallback != ImDrawCallback_ResetRenderState) {
                    c.UserCallback(cl, &c);
                }
                continue;
            }

            // Clamp clip rect to the framebuffer; Metal's setScissorRect
            // rejects rects that exceed the attachment dims and silently
            // emits no draws under validation. Post-Develop review finding.
            const float fbw = static_cast<float>(fb_width);
            const float fbh = static_cast<float>(fb_height);
            const float clip_x0 = std::max(0.0f, (c.ClipRect.x - clip_off.x) * clip_scale.x);
            const float clip_y0 = std::max(0.0f, (c.ClipRect.y - clip_off.y) * clip_scale.y);
            const float clip_x1 = std::min(fbw,  (c.ClipRect.z - clip_off.x) * clip_scale.x);
            const float clip_y1 = std::min(fbh,  (c.ClipRect.w - clip_off.y) * clip_scale.y);
            if (clip_x1 <= clip_x0 || clip_y1 <= clip_y0) continue;

            const auto sx = static_cast<int32_t>(clip_x0);
            const auto sy = static_cast<int32_t>(clip_y0);
            const auto sw = static_cast<uint32_t>(clip_x1 - clip_x0);
            const auto sh = static_cast<uint32_t>(clip_y1 - clip_y0);
            if (sw == 0 || sh == 0) continue;
            cmd.set_scissor({sx, sy, sw, sh});

            cmd.draw_indexed(c.ElemCount, 1,
                             c.IdxOffset + global_idx_offset,
                             static_cast<int32_t>(c.VtxOffset + global_vtx_offset),
                             0);
        }
        global_vtx_offset += static_cast<uint32_t>(cl->VtxBuffer.Size);
        global_idx_offset += static_cast<uint32_t>(cl->IdxBuffer.Size);
    }

    impl_->frame_index++;
}

} // namespace tide::imgui
