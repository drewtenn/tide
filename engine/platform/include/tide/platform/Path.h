#pragma once

// Process-relative path helpers. The samples need a stable way to find shader
// artefacts the build staged next to their binaries; doing the platform-
// specific dance (_NSGetExecutablePath / readlink / GetModuleFileNameW)
// inline in every sample led to drift, so it lives here.

#include <filesystem>

namespace tide::platform {

// Absolute, canonical path of the running executable. Returns an empty path
// on platforms or environments where the lookup fails (rare). Logs an error
// to the engine logger; callers may also test for empty.
[[nodiscard]] std::filesystem::path executable_path();

// `executable_path().parent_path()`, computed once and cached. Callers that
// want shaders or other build artefacts staged next to the binary should
// resolve relative to this.
[[nodiscard]] std::filesystem::path executable_dir();

} // namespace tide::platform
