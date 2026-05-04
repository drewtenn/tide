#include "tide/assets/MmapFile.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Write a temp file with the given bytes. Caller is responsible for
// removing it (the test wraps this in a RAII guard). Uses an atomic
// counter rather than getpid() so the fixture path is deterministic /
// portable (Windows lacks getpid in the global namespace).
[[nodiscard]] std::filesystem::path
write_temp(const std::string& stem, const std::vector<std::byte>& bytes) {
    static std::atomic<std::uint64_t> ctr{0};
    auto p = std::filesystem::temp_directory_path()
           / (stem + "." + std::to_string(ctr.fetch_add(1)) + ".bin");
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    REQUIRE(f.is_open());
    if (!bytes.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — file IO
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    f.close();
    REQUIRE(f.good());
    return p;
}

struct TempFileGuard {
    std::filesystem::path path;
    ~TempFileGuard() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_SUITE("assets/MmapFile") {
    TEST_CASE("open_read on a small file returns its bytes verbatim") {
        std::vector<std::byte> contents(64);
        for (std::size_t i = 0; i < contents.size(); ++i) {
            contents[i] = static_cast<std::byte>(i);
        }
        TempFileGuard g{write_temp("tide_mmap_basic", contents)};

        auto m = tide::assets::MmapFile::open_read(g.path);
        REQUIRE(m.has_value());
        CHECK(m->valid());
        CHECK(m->size() == contents.size());

        const auto bytes = m->bytes();
        REQUIRE(bytes.size() == contents.size());
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            CHECK(bytes[i] == contents[i]);
        }
    }

    TEST_CASE("open_read on a missing path returns IoError") {
        const auto missing =
            std::filesystem::temp_directory_path() / "tide_mmap_does_not_exist";
        std::error_code ec;
        std::filesystem::remove(missing, ec);

        auto m = tide::assets::MmapFile::open_read(missing);
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error() == tide::assets::AssetError::IoError);
    }

    TEST_CASE("open_read on an empty file returns IoError") {
        TempFileGuard g{write_temp("tide_mmap_empty", {})};
        auto m = tide::assets::MmapFile::open_read(g.path);
        REQUIRE_FALSE(m.has_value());
        CHECK(m.error() == tide::assets::AssetError::IoError);
    }

    TEST_CASE("Move ctor transfers ownership; moved-from is empty") {
        std::vector<std::byte> contents(32, std::byte{0x42});
        TempFileGuard g{write_temp("tide_mmap_move", contents)};

        auto m1 = tide::assets::MmapFile::open_read(g.path);
        REQUIRE(m1.has_value());
        const auto* original_ptr = m1->bytes().data();

        tide::assets::MmapFile m2 = std::move(*m1);
        CHECK(m2.valid());
        CHECK(m2.bytes().data() == original_ptr);
        CHECK_FALSE(m1->valid()); // moved-from is empty
    }
}
