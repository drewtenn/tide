#pragma once

// tide::assets::MmapFile — RAII read-only memory-mapped file.
//
// POSIX `mmap` on macOS/Linux; `MapViewOfFile` on Windows (compiled but
// untested in P3 — Windows ships post-P2). The cross-platform discipline
// is here from day one so the runtime mmap loader (P3 task 6) holds an
// interface that survives the eventual port (`game_engine_plan.md:1`).
//
// Design notes:
//   * `MAP_PRIVATE | PROT_READ` + `O_RDONLY`. Any future write path adds
//     a separate factory; the read-only one is the load path forever.
//   * Mapping size is the file's `st_size` at open time. The cooked-asset
//     workflow guarantees stable file size — files are atomically replaced
//     by the cooker (rename-into-place per ADR-0018), never appended to.
//   * Empty files are rejected (`EmptyFile`) so callers always get a
//     non-empty span. The runtime header alone is 48 bytes, so any
//     legitimately cooked artifact is bigger than that anyway.
//   * Move-only. Copying a mapping would double-`munmap`.

#include "tide/assets/Asset.h"
#include "tide/core/Expected.h"

#include <cstddef>
#include <filesystem>
#include <span>

namespace tide::assets {

class MmapFile {
public:
    [[nodiscard]] static tide::expected<MmapFile, AssetError>
        open_read(const std::filesystem::path& path);

    MmapFile() noexcept = default;
    ~MmapFile() noexcept;
    MmapFile(MmapFile&& other) noexcept;
    MmapFile& operator=(MmapFile&& other) noexcept;
    MmapFile(const MmapFile&)            = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data_, size_};
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool        valid() const noexcept { return data_ != nullptr; }

private:
    MmapFile(const std::byte* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    void release() noexcept;

    const std::byte* data_{nullptr};
    std::size_t      size_{0};
};

} // namespace tide::assets
