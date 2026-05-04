#include "tide/assets/MmapFile.h"

#include <cstddef>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace tide::assets {

#if defined(_WIN32)

// Windows path is structurally identical to POSIX: open file, query size,
// create mapping, map view. Compiled but not part of the P3 macOS test
// matrix; the discipline of having both side-by-side is the point.
tide::expected<MmapFile, AssetError>
MmapFile::open_read(const std::filesystem::path& path) {
    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return tide::unexpected{AssetError::IoError};
    }
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(hFile, &size) || size.QuadPart == 0) {
        ::CloseHandle(hFile);
        return tide::unexpected{AssetError::IoError};
    }

    HANDLE hMap = ::CreateFileMappingW(hFile, nullptr, PAGE_READONLY,
                                       size.HighPart,
                                       static_cast<DWORD>(size.LowPart),
                                       nullptr);
    if (hMap == nullptr) {
        ::CloseHandle(hFile);
        return tide::unexpected{AssetError::IoError};
    }

    void* view = ::MapViewOfFile(hMap, FILE_MAP_READ, 0, 0,
                                 static_cast<SIZE_T>(size.QuadPart));
    // The mapping object can be closed once a view is held; the file
    // handle is also redundant after MapViewOfFile (Windows keeps the
    // file alive via the view). Both are closed here; the view itself is
    // released in MmapFile::release() via UnmapViewOfFile.
    ::CloseHandle(hMap);
    ::CloseHandle(hFile);
    if (view == nullptr) {
        return tide::unexpected{AssetError::IoError};
    }

    return MmapFile{static_cast<const std::byte*>(view),
                    static_cast<std::size_t>(size.QuadPart)};
}

void MmapFile::release() noexcept {
    if (data_ != nullptr) {
        ::UnmapViewOfFile(data_);
        data_ = nullptr;
        size_ = 0;
    }
}

#else // POSIX

tide::expected<MmapFile, AssetError>
MmapFile::open_read(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return tide::unexpected{AssetError::IoError};
    }

    struct ::stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return tide::unexpected{AssetError::IoError};
    }

    const auto size = static_cast<std::size_t>(st.st_size);
    void*      ptr  = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    // POSIX guarantees the mapping survives close(fd); the kernel keeps
    // a per-mapping reference on the underlying inode.
    ::close(fd);
    if (ptr == MAP_FAILED) {
        return tide::unexpected{AssetError::IoError};
    }

    return MmapFile{static_cast<const std::byte*>(ptr), size};
}

void MmapFile::release() noexcept {
    if (data_ != nullptr) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — munmap signature
        ::munmap(const_cast<std::byte*>(data_), size_);
        data_ = nullptr;
        size_ = 0;
    }
}

#endif

MmapFile::MmapFile(MmapFile&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

MmapFile& MmapFile::operator=(MmapFile&& other) noexcept {
    if (this != &other) {
        release();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

MmapFile::~MmapFile() noexcept { release(); }

} // namespace tide::assets
