#include "ShaderCooker.h"

#include "tide/assets/RuntimeFormat.h"

#include <nlohmann/json.hpp>

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
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
    #include <process.h>          // _getpid
    #include <windows.h>           // CreateProcessW, WaitForSingleObject
#else
    #include <spawn.h>             // posix_spawnp
    #include <sys/wait.h>          // waitpid, WIFEXITED, WEXITSTATUS
    #include <unistd.h>            // getpid
extern char** environ;             // posix_spawnp signature requires it
#endif

namespace tide::cooker {

namespace {

using tide::assets::AssetDescriptorBinding;
using tide::assets::AssetVertexInput;
using tide::assets::DescriptorType;
using tide::assets::ShaderPayload;
using tide::assets::ShaderStage;

// ─── Stage <-> profile string ───────────────────────────────────────────────

[[nodiscard]] const char* stage_profile(ShaderStage s) noexcept {
    switch (s) {
        case ShaderStage::Vertex:   return "vs_6_7";
        case ShaderStage::Fragment: return "ps_6_7";
        case ShaderStage::Compute:  return "cs_6_7";
        case ShaderStage::Unknown:  return nullptr;
    }
    return nullptr;
}

[[nodiscard]] int current_pid() noexcept {
#if defined(_WIN32)
    return ::_getpid();
#else
    return ::getpid();
#endif
}

[[nodiscard]] std::filesystem::path make_temp_path(const std::filesystem::path& dir,
                                                   const std::string&     stem,
                                                   const std::string&     ext) {
    // Deterministic-name temp file isn't safe for hostile multi-user
    // environments, but the cooker runs single-user during the build.
    // The PID + counter is enough to disambiguate parallel cmake jobs.
    static std::atomic<std::uint64_t> ctr{0};
    std::ostringstream oss;
    oss << stem << '.' << current_pid() << '.' << ctr.fetch_add(1) << ext;
    return dir / oss.str();
}

// Run a tool with an argv vector — no shell, no quoting, no `$VAR` /
// backtick / `$(...)` expansion. Stdout/stderr pass through to the
// cooker's parent so CMake captures DXC diagnostics verbatim. Returns
// the child's exit status (0 on success).
//
// `argv` is by value because we need to terminate it with a nullptr
// without mutating the caller's vector.
[[nodiscard]] int run_command(std::vector<std::string> argv) noexcept {
    if (argv.empty()) {
        return -1;
    }
    std::vector<char*> c_argv;
    c_argv.reserve(argv.size() + 1);
    for (auto& s : argv) {
        c_argv.push_back(s.data());
    }
    c_argv.push_back(nullptr);

#if defined(_WIN32)
    // Build a quoted command line for CreateProcessW, applying the
    // Windows MSVCRT quoting rules. Windows lacks an argv-vector spawn
    // API, but CreateProcess with no shell at least avoids the cmd.exe
    // metacharacter expansion that std::system would trigger. The
    // applet is the executable's full path; `argv[0]` is duplicated
    // into the command-line per convention.
    std::wstring cmdline;
    auto append_arg = [&](const std::string& a) {
        if (!cmdline.empty()) cmdline.push_back(L' ');
        cmdline.push_back(L'"');
        for (char c : a) {
            if (c == L'"' || c == L'\\') {
                cmdline.push_back(L'\\');
            }
            cmdline.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        cmdline.push_back(L'"');
    };
    for (const auto& a : argv) append_arg(a);
    std::wstring exe_w;
    for (char c : argv[0]) {
        exe_w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe_w.c_str(), cmdline.data(), nullptr, nullptr,
                          FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return static_cast<int>(code);
#else
    pid_t pid = -1;
    // posix_spawnp searches PATH if argv[0] isn't absolute. CompileShader.cmake
    // hands us absolute paths via --dxc / --spirv-cross, so PATH lookup is
    // a no-op in the steady state — but keeping the *p* form means a future
    // `--dxc dxc` (bare name) still works.
    if (::posix_spawnp(&pid, c_argv[0], nullptr, nullptr,
                       c_argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1; // signal-killed or stopped; surface non-zero
#endif
}

[[nodiscard]] std::vector<std::byte> read_binary(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) {
        return {};
    }
    const auto end = f.tellg();
    if (end <= 0) {
        return {};
    }
    f.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<std::size_t>(end));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — file IO
    f.read(reinterpret_cast<char*>(buf.data()), end);
    if (!f) {
        return {};
    }
    return buf;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) {
        return {};
    }
    const auto end = f.tellg();
    if (end < 0) {
        return {};
    }
    f.seekg(0, std::ios::beg);
    std::string out;
    out.resize(static_cast<std::size_t>(end));
    f.read(out.data(), end);
    if (!f) {
        return {};
    }
    return out;
}

// ─── Reflection JSON -> flat structs ────────────────────────────────────────
// SPIRV-Cross --reflect produces JSON in this shape (relevant subset):
//   {
//     "entryPoints": [{ "name": "...", "mode": "vert|frag|comp" }],
//     "ubo": [{ "name":..., "set":N, "binding":N, "type": ... }],
//     "ssbo": [...], "textures": [...], "images": [...],
//     "separate_samplers": [...],
//     "inputs": [{ "type":"...", "location": N, "name": "..." }],
//     "push_constants": [{ ... }]
//   }
// Buckets we care about: ubo, ssbo, textures, images, separate_samplers
// (each becomes a binding); inputs (vertex stage only).

[[nodiscard]] tide::expected<void, ShaderCookError>
collect_bindings(const nlohmann::json&                j,
                 const char*                          bucket,
                 DescriptorType                       type,
                 std::vector<AssetDescriptorBinding>& out) {
    auto it = j.find(bucket);
    if (it == j.end() || !it->is_array()) {
        return {};
    }
    for (const auto& entry : *it) {
        AssetDescriptorBinding b{};
        b.set         = entry.value("set",     0u);
        b.slot        = entry.value("binding", 0u);
        b.array_count = 1;
        if (auto a = entry.find("array");
            a != entry.end() && a->is_array() && !a->empty() && a->front().is_number_unsigned()) {
            b.array_count = a->front().get<std::uint32_t>();
        }
        b.type = type;
        out.push_back(b);
    }
    return {};
}

[[nodiscard]] std::uint32_t format_code_from_type(const std::string& t) noexcept {
    // Compact mapping: only the formats we actually emit from canary
    // shaders today. The runtime translates these to rhi::Format. Values
    // are append-only, mirroring the discipline on TextureFormat.
    if (t == "float")  return 1; // R32_Float
    if (t == "vec2")   return 2; // RG32_Float
    if (t == "vec3")   return 3; // RGB32_Float
    if (t == "vec4")   return 4; // RGBA32_Float
    if (t == "uint")   return 5; // R32_Uint
    return 0; // Unknown; cooker still records the location, runtime can warn
}

} // namespace

const char* to_string(ShaderCookError e) noexcept {
    switch (e) {
        case ShaderCookError::OpenFailed:                  return "OpenFailed";
        case ShaderCookError::DxcMissing:                  return "DxcMissing";
        case ShaderCookError::SpirvCrossMissing:           return "SpirvCrossMissing";
        case ShaderCookError::DxcInvocationFailed:         return "DxcInvocationFailed";
        case ShaderCookError::SpirvCrossInvocationFailed:  return "SpirvCrossInvocationFailed";
        case ShaderCookError::ReflectionParseError:        return "ReflectionParseError";
        case ShaderCookError::UnsupportedStage:            return "UnsupportedStage";
        case ShaderCookError::OutOfMemory:                 return "OutOfMemory";
    }
    return "<invalid ShaderCookError>";
}

tide::expected<ShaderCookOutput, ShaderCookError>
cook_shader(const std::filesystem::path& hlsl_path,
            const tide::assets::Uuid&    /*uuid*/,
            const ShaderCookHints&       hints) {
    // ─── Validate inputs ────────────────────────────────────────────────────
    const char* profile = stage_profile(hints.stage);
    if (profile == nullptr) {
        return tide::unexpected{ShaderCookError::UnsupportedStage};
    }
    if (hints.dxc_path.empty()) {
        return tide::unexpected{ShaderCookError::DxcMissing};
    }
    if (hints.spirv_cross_path.empty()) {
        return tide::unexpected{ShaderCookError::SpirvCrossMissing};
    }
    std::error_code ec;
    if (!std::filesystem::exists(hlsl_path, ec) || ec) {
        return tide::unexpected{ShaderCookError::OpenFailed};
    }

    // Temp file paths — siblings of the HLSL input live in the system
    // temp dir to keep the source tree clean.
    const auto tmp = std::filesystem::temp_directory_path(ec);
    if (ec) {
        return tide::unexpected{ShaderCookError::OpenFailed};
    }
    const auto stem = hlsl_path.stem().string();
    const auto spv  = make_temp_path(tmp, stem, ".spv");
    const auto msl  = make_temp_path(tmp, stem, ".msl");
    const auto refl = make_temp_path(tmp, stem, ".refl.json");

    struct Cleanup {
        std::filesystem::path spv, msl, refl;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove(spv, e);
            std::filesystem::remove(msl, e);
            std::filesystem::remove(refl, e);
        }
    } cleanup{spv, msl, refl};

    // ─── Step 1: HLSL -> SPIR-V via DXC ─────────────────────────────────────
    // Mirror the flag set used by `cmake/CompileShader.cmake` so the
    // cooker output is byte-identical to a CMake-driven build. The argv
    // vector approach skips the shell entirely — paths containing `$VAR`,
    // backticks, parens, or whitespace are passed through verbatim.
    {
        std::vector<std::string> argv = {
            hints.dxc_path.string(),
            "-spirv",
            "-T", profile,
            "-E", hints.entry_point,
            "-HV", "2021",
            "-fspv-target-env=vulkan1.3",
            "-fvk-use-dx-layout",
            "-fvk-b-shift",  "0",  "all",
            "-fvk-t-shift", "16",  "all",
            "-fvk-u-shift", "32",  "all",
            "-fvk-s-shift", "48",  "all",
            "-Fo", spv.string(),
            hlsl_path.string(),
        };
        if (run_command(std::move(argv)) != 0) {
            return tide::unexpected{ShaderCookError::DxcInvocationFailed};
        }
    }

    // ─── Step 2: SPIR-V -> MSL via SPIRV-Cross ──────────────────────────────
    {
        std::vector<std::string> argv = {
            hints.spirv_cross_path.string(),
            "--msl",
            "--msl-version", "30000",
            "--output", msl.string(),
            spv.string(),
        };
        if (run_command(std::move(argv)) != 0) {
            return tide::unexpected{ShaderCookError::SpirvCrossInvocationFailed};
        }
    }

    // ─── Step 3: SPIRV-Cross --reflect for binding metadata ─────────────────
    {
        std::vector<std::string> argv = {
            hints.spirv_cross_path.string(),
            "--reflect",
            "--output", refl.string(),
            spv.string(),
        };
        if (run_command(std::move(argv)) != 0) {
            return tide::unexpected{ShaderCookError::SpirvCrossInvocationFailed};
        }
    }

    // ─── Step 4: read intermediates ─────────────────────────────────────────
    auto spv_bytes = read_binary(spv);
    if (spv_bytes.empty()) {
        return tide::unexpected{ShaderCookError::DxcInvocationFailed};
    }
    auto msl_text = read_text(msl);
    if (msl_text.empty()) {
        return tide::unexpected{ShaderCookError::SpirvCrossInvocationFailed};
    }
    const auto refl_text = read_text(refl);
    if (refl_text.empty()) {
        return tide::unexpected{ShaderCookError::ReflectionParseError};
    }

    // ─── Step 5: parse reflection JSON ──────────────────────────────────────
    nlohmann::json reflection;
    try {
        reflection = nlohmann::json::parse(refl_text);
    } catch (const nlohmann::json::parse_error&) {
        return tide::unexpected{ShaderCookError::ReflectionParseError};
    }

    std::vector<AssetDescriptorBinding> bindings;
    (void)collect_bindings(reflection, "ubo",                DescriptorType::UniformBuffer,  bindings);
    (void)collect_bindings(reflection, "ssbo",               DescriptorType::StorageBuffer,  bindings);
    (void)collect_bindings(reflection, "textures",           DescriptorType::SampledTexture, bindings);
    (void)collect_bindings(reflection, "images",             DescriptorType::StorageTexture, bindings);
    (void)collect_bindings(reflection, "separate_samplers",  DescriptorType::Sampler,        bindings);

    std::uint32_t push_constant_size = 0;
    if (auto pc = reflection.find("push_constants");
        pc != reflection.end() && pc->is_array() && !pc->empty()) {
        // SPIRV-Cross does not always include a numeric size; we record
        // the presence (size > 0) so the runtime knows to allocate a
        // root-constant slot. The actual size lands when the material
        // system arrives in P5.
        push_constant_size = pc->front().value("size", 0u);
    }

    std::vector<AssetVertexInput> vertex_inputs;
    if (hints.stage == ShaderStage::Vertex) {
        if (auto inputs = reflection.find("inputs");
            inputs != reflection.end() && inputs->is_array()) {
            for (const auto& in : *inputs) {
                AssetVertexInput vi{};
                vi.location    = in.value("location", 0u);
                const auto t   = in.value("type", std::string{});
                vi.format_code = format_code_from_type(t);
                vi.offset      = 0;
                vi.reserved    = 0;
                vertex_inputs.push_back(vi);
            }
        }
    }

    // ─── Step 6: pack the runtime payload ───────────────────────────────────
    // Layout (all arrays follow the payload header contiguously):
    //   [ShaderPayload]
    //   [entry_point string]                                  4-byte aligned
    //   [AssetDescriptorBinding × binding_count]              4-byte aligned
    //   [AssetVertexInput × vertex_input_count]               4-byte aligned
    //   [SPIR-V bytecode]                                     4-byte aligned
    //   [MSL source string]                                   1-byte aligned, last
    //
    // Each section starts at the offset of its preceding section + size,
    // padded up to the alignment of the next section's element type.

    auto align_up = [](std::size_t v, std::size_t a) {
        return (v + (a - 1)) & ~(a - 1);
    };

    std::size_t offset = sizeof(ShaderPayload);

    const std::size_t entry_off  = offset;
    const std::size_t entry_size = hints.entry_point.size() + 1; // null-terminated
    offset = align_up(entry_off + entry_size, 4);

    const std::size_t bindings_off  = offset;
    const std::size_t bindings_size = bindings.size() * sizeof(AssetDescriptorBinding);
    offset = align_up(bindings_off + bindings_size, 4);

    const std::size_t vinputs_off  = offset;
    const std::size_t vinputs_size = vertex_inputs.size() * sizeof(AssetVertexInput);
    offset = align_up(vinputs_off + vinputs_size, 4);

    const std::size_t spv_off  = offset;
    const std::size_t spv_size = spv_bytes.size();
    offset = align_up(spv_off + spv_size, 4);

    const std::size_t msl_off  = offset;
    const std::size_t msl_size = msl_text.size() + 1; // null-terminated
    offset = msl_off + msl_size;

    const std::size_t total = offset;
    std::vector<std::byte> buf(total);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — wire format
    auto* payload = reinterpret_cast<ShaderPayload*>(buf.data());
    payload->stage              = hints.stage;
    payload->entry_point_len    = static_cast<std::uint32_t>(hints.entry_point.size());
    payload->push_constant_size = push_constant_size;
    payload->spirv_size         = static_cast<std::uint32_t>(spv_size);
    payload->msl_size           = static_cast<std::uint32_t>(msl_size);
    payload->binding_count      = static_cast<std::uint32_t>(bindings.size());
    payload->vertex_input_count = static_cast<std::uint32_t>(vertex_inputs.size());

    // Entry-point string
    auto* entry_dst = buf.data() + entry_off;
    std::memcpy(entry_dst, hints.entry_point.c_str(), entry_size);
    payload->entry_point.set_target(entry_dst);

    // Bindings
    if (!bindings.empty()) {
        auto* dst = buf.data() + bindings_off;
        std::memcpy(dst, bindings.data(), bindings_size);
        payload->bindings.set_target(dst);
    }

    // Vertex inputs
    if (!vertex_inputs.empty()) {
        auto* dst = buf.data() + vinputs_off;
        std::memcpy(dst, vertex_inputs.data(), vinputs_size);
        payload->vertex_inputs.set_target(dst);
    }

    // SPIR-V bytecode
    if (spv_size > 0) {
        auto* dst = buf.data() + spv_off;
        std::memcpy(dst, spv_bytes.data(), spv_size);
        payload->spirv_bytecode.set_target(dst);
    }

    // MSL source (null-terminated)
    if (msl_size > 0) {
        auto* dst = buf.data() + msl_off;
        std::memcpy(dst, msl_text.c_str(), msl_size);
        payload->msl_source.set_target(dst);
    }

    const auto hash = XXH3_64bits(buf.data(), buf.size());
    return ShaderCookOutput{std::move(buf), hash};
}

} // namespace tide::cooker
