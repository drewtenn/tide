#include "tide/assets/Uuid.h"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

namespace tide::assets {

namespace {

// Per-thread PRNG. `random_device` per call would be deterministic-but-slow on
// some platforms (Linux /dev/urandom is fine; Windows BCryptGenRandom is fine;
// libc++ on macOS is fine). Per-thread mt19937_64 seeded from a single
// random_device call keeps make_v4() out of the global lock path.
[[nodiscard]] std::mt19937_64& thread_prng() {
    thread_local std::mt19937_64 prng{[] {
        std::random_device rd;
        // Stir 64 bits of entropy from random_device. Two 32-bit draws is
        // portable; libstdc++ random_device is 32-bit on Windows.
        const auto a = static_cast<std::uint64_t>(rd());
        const auto b = static_cast<std::uint64_t>(rd());
        return (a << 32) ^ b;
    }()};
    return prng;
}

constexpr char kHex[] = "0123456789abcdef";

[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

} // namespace

Uuid Uuid::make_v4() {
    auto& prng = thread_prng();
    Uuid u{};
    // Fill with 128 bits of randomness.
    const auto r0 = prng();
    const auto r1 = prng();
    for (std::size_t i = 0; i < 8; ++i) {
        u.octets[i]     = static_cast<std::uint8_t>(r0 >> (8 * i));
        u.octets[i + 8] = static_cast<std::uint8_t>(r1 >> (8 * i));
    }
    // RFC 4122 v4 bit-fields: version 4 in the high nibble of byte 6,
    // variant `10xx` in the high two bits of byte 8.
    u.octets[6] = static_cast<std::uint8_t>((u.octets[6] & 0x0F) | 0x40);
    u.octets[8] = static_cast<std::uint8_t>((u.octets[8] & 0x3F) | 0x80);
    return u;
}

tide::expected<Uuid, UuidParseError> Uuid::parse(std::string_view canonical) {
    if (canonical.size() != 36) {
        return tide::unexpected{UuidParseError::WrongLength};
    }
    // Dashes at positions 8, 13, 18, 23.
    constexpr std::array<std::size_t, 4> dash_positions{8, 13, 18, 23};
    for (auto p : dash_positions) {
        if (canonical[p] != '-') {
            return tide::unexpected{UuidParseError::MissingDash};
        }
    }
    Uuid u{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }
        const int hi = hex_value(canonical[i]);
        const int lo = hex_value(canonical[i + 1]);
        if (hi < 0 || lo < 0) {
            return tide::unexpected{UuidParseError::BadHexDigit};
        }
        u.octets[out++] = static_cast<std::uint8_t>((hi << 4) | lo);
        ++i; // consumed two chars
    }
    return u;
}

std::string Uuid::to_string() const {
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(kHex[(octets[i] >> 4) & 0xF]);
        out.push_back(kHex[octets[i] & 0xF]);
    }
    return out;
}

} // namespace tide::assets
