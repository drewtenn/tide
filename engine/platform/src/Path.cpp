#include "tide/platform/Path.h"

#include "tide/core/Log.h"

#include <climits>
#include <cstdint>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
// _WIN32 path lookup not yet wired; gated below.
#endif

namespace tide::platform {

namespace {

// Cache the canonical path on first call. Cheap to compute but called once
// per Context construction + once per sample, and the result never changes.
const std::filesystem::path& cached_path() {
    static const std::filesystem::path p = []() -> std::filesystem::path {
#if defined(__APPLE__)
        char raw[PATH_MAX];
        uint32_t bufsize = sizeof(raw);
        if (_NSGetExecutablePath(raw, &bufsize) != 0) {
            TIDE_LOG_ERROR("executable_path: _NSGetExecutablePath buffer too small ({} bytes)",
                           bufsize);
            return {};
        }
        std::error_code ec;
        auto canonical = std::filesystem::canonical(raw, ec);
        if (ec) {
            TIDE_LOG_ERROR("executable_path: canonical({}) failed: {}", raw, ec.message());
            return {};
        }
        return canonical;
#elif defined(__linux__)
        std::error_code ec;
        auto canonical = std::filesystem::canonical("/proc/self/exe", ec);
        if (ec) {
            TIDE_LOG_ERROR("executable_path: canonical(/proc/self/exe) failed: {}",
                           ec.message());
            return {};
        }
        return canonical;
#else
        TIDE_LOG_ERROR("executable_path: unsupported platform — returning empty path");
        return {};
#endif
    }();
    return p;
}

} // namespace

std::filesystem::path executable_path() {
    return cached_path();
}

std::filesystem::path executable_dir() {
    const auto& p = cached_path();
    if (p.empty()) return {};
    return p.parent_path();
}

} // namespace tide::platform
