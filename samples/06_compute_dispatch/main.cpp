// samples/06_compute_dispatch — Phase 1 task 11 deliverable.
//
// Runs the compute canary (engine/shaders/compute_canary.cs.hlsl) on a
// 64-element storage buffer and asserts every element is 2 after dispatch.
// The kernel does `buf[tid] = (buf[tid] + 1) * 2` with InterlockedAdd +
// threadgroup_barrier so it exercises atomics + barriers, not just a
// trivial copy. Used as a CI canary that the HLSL → SPIR-V → MSL compute
// path AND the runtime compute encoder are both wired correctly.
//
// Headless single-shot: no window is needed for compute work, but the
// MetalDevice ctor still requires a Window for swapchain wiring; we
// create a hidden one and never present.

#include "tide/core/Log.h"
#include "tide/platform/Path.h"
#include "tide/platform/Window.h"
#include "tide/rhi/IDevice.h"
#include "tide/rhi-metal/MetalDevice.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace {

// Must match [numthreads(64,1,1)] in compute_canary.cs.hlsl.
constexpr uint32_t kThreadsPerGroup = 64;
constexpr uint32_t kElements        = kThreadsPerGroup;

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

int main(int /*argc*/, char** /*argv*/) {
    tide::log::init();

    auto window_result = tide::platform::Window::create({
        .width  = 64,
        .height = 64,
        .title  = "tide — 06_compute_dispatch",
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

    // ─── Compute shader ───────────────────────────────────────────────────────
    const auto shaders = tide::platform::executable_dir() / ".." / ".." / "shaders";
    auto cs_bytes = read_file_bytes(shaders / "compute_canary.cs.hlsl.metallib");
    if (cs_bytes.empty()) {
        TIDE_LOG_ERROR("Failed to read compute_canary.cs.hlsl.metallib from {}",
                       shaders.string());
        return 3;
    }

    tide::rhi::ShaderDesc cs_desc{};
    cs_desc.stage       = tide::rhi::ShaderStage::Compute;
    cs_desc.bytecode    = std::span<const std::byte>(cs_bytes.data(), cs_bytes.size());
    cs_desc.entry_point = "main0";
    cs_desc.debug_name  = "compute_canary";
    auto cs = device->create_shader(cs_desc);
    if (!cs) {
        TIDE_LOG_ERROR("create_shader(compute) failed: {}", static_cast<int>(cs.error()));
        return 4;
    }

    // ─── Descriptor set layout: a single storage buffer at slot 0 ─────────────
    // The HLSL shader uses `register(u0)`. With our DXC flag `-fvk-u-shift 32`,
    // that maps to SPIR-V binding 32, but SPIRV-Cross packs the MSL kernel's
    // [[buffer(N)]] index DENSELY — so the storage buffer lands at MSL slot 0
    // (verified by the emitted compute_canary.cs.hlsl.metal: `buf [[buffer(0)]]`).
    using tide::rhi::DescriptorBindingDesc;
    using tide::rhi::DescriptorType;
    using tide::rhi::ShaderStage;
    const std::array<DescriptorBindingDesc, 1> bindings = {{
        { .slot = 0, .array_count = 1, .type = DescriptorType::StorageBuffer,
          .stages = ShaderStage::Compute },
    }};
    tide::rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings   = std::span<const DescriptorBindingDesc>(bindings.data(), bindings.size());
    layout_desc.debug_name = "compute_canary.layout";
    auto layout = device->create_descriptor_set_layout(layout_desc);
    if (!layout) {
        TIDE_LOG_ERROR("create_descriptor_set_layout failed");
        return 5;
    }

    // ─── Storage buffer (initial value 0) ────────────────────────────────────
    const size_t buf_bytes = sizeof(uint32_t) * kElements;
    tide::rhi::BufferDesc buf_desc{};
    buf_desc.size_bytes = buf_bytes;
    buf_desc.usage      = tide::rhi::BufferUsage::StorageBuffer | tide::rhi::BufferUsage::CopyDest |
                          tide::rhi::BufferUsage::CopySource;
    buf_desc.memory     = tide::rhi::MemoryType::DeviceLocal;
    buf_desc.debug_name = "compute_canary.buf";
    auto buf = device->create_buffer(buf_desc);
    if (!buf) {
        TIDE_LOG_ERROR("create_buffer failed");
        return 6;
    }
    {
        std::array<uint32_t, kElements> zeros{};   // initialized to 0
        if (auto r = device->upload_buffer(*buf, zeros.data(), buf_bytes); !r) {
            TIDE_LOG_ERROR("upload_buffer (initial zeros) failed: {}",
                           static_cast<int>(r.error()));
            return 6;
        }
    }

    // ─── Descriptor set ──────────────────────────────────────────────────────
    using tide::rhi::DescriptorWrite;
    DescriptorWrite write{};
    write.slot          = 0;
    write.type          = DescriptorType::StorageBuffer;
    write.buffer        = *buf;
    write.buffer_offset = 0;
    write.buffer_range  = buf_bytes;

    tide::rhi::DescriptorSetDesc set_desc{};
    set_desc.layout         = *layout;
    set_desc.initial_writes = std::span<const DescriptorWrite>(&write, 1);
    set_desc.debug_name     = "compute_canary.set";
    auto dset = device->create_descriptor_set(set_desc);
    if (!dset) {
        TIDE_LOG_ERROR("create_descriptor_set failed");
        return 7;
    }

    // ─── Pipeline ────────────────────────────────────────────────────────────
    tide::rhi::ComputePipelineDesc pso_desc{};
    pso_desc.compute_shader      = *cs;
    pso_desc.threads_per_group[0] = kThreadsPerGroup;
    pso_desc.threads_per_group[1] = 1;
    pso_desc.threads_per_group[2] = 1;
    pso_desc.debug_name           = "compute_canary.pso";
    auto pso = device->create_compute_pipeline(pso_desc);
    if (!pso) {
        TIDE_LOG_ERROR("create_compute_pipeline failed: {}",
                       static_cast<int>(pso.error()));
        return 8;
    }

    // ─── Dispatch ────────────────────────────────────────────────────────────
    if (auto begin = device->begin_frame(); !begin) {
        TIDE_LOG_ERROR("begin_frame failed");
        return 9;
    }
    auto* cmd = device->acquire_command_buffer();
    cmd->bind_pipeline(*pso);
    cmd->bind_descriptor_set(0, *dset);
    cmd->dispatch(/*group_x=*/kElements / kThreadsPerGroup, 1, 1);
    device->submit(cmd);
    (void)device->end_frame();

    // ─── Read back + assert ──────────────────────────────────────────────────
    std::array<uint32_t, kElements> out{};
    if (auto r = device->download_buffer(*buf, out.data(), buf_bytes); !r) {
        TIDE_LOG_ERROR("download_buffer failed: {}", static_cast<int>(r.error()));
        return 10;
    }

    int rc = 0;
    for (uint32_t i = 0; i < kElements; ++i) {
        if (out[i] != 2u) {
            TIDE_LOG_ERROR("compute canary: out[{}] = {} (expected 2)", i, out[i]);
            rc = 1;
        }
    }
    if (rc == 0) {
        TIDE_LOG_INFO("compute canary: all {} elements == 2 — PASS", kElements);
    }

    device->destroy_pipeline(*pso);
    device->destroy_descriptor_set(*dset);
    device->destroy_descriptor_set_layout(*layout);
    device->destroy_buffer(*buf);
    device->destroy_shader(*cs);
    return rc;
}
