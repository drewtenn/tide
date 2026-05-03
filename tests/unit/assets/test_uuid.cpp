#include "tide/assets/Uuid.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <unordered_set>

namespace {
using tide::assets::Uuid;
using tide::assets::UuidParseError;
} // namespace

TEST_SUITE("assets/Uuid") {
    TEST_CASE("Default-constructed Uuid is nil") {
        Uuid u{};
        CHECK(u.is_nil());
    }

    TEST_CASE("make_v4 produces RFC 4122 v4 bit pattern") {
        const Uuid u = Uuid::make_v4();
        CHECK_FALSE(u.is_nil());
        // Version field: high nibble of byte 6 must be 0x4.
        CHECK((u.octets[6] & 0xF0) == 0x40);
        // Variant field: high two bits of byte 8 must be 0b10.
        CHECK((u.octets[8] & 0xC0) == 0x80);
    }

    TEST_CASE("make_v4 produces distinct UUIDs across many calls") {
        constexpr int N = 256;
        std::unordered_set<std::string> seen;
        for (int i = 0; i < N; ++i) {
            seen.insert(Uuid::make_v4().to_string());
        }
        CHECK(seen.size() == N); // 256 v4s should never collide
    }

    TEST_CASE("to_string then parse round-trips") {
        const Uuid u = Uuid::make_v4();
        const std::string s = u.to_string();
        CHECK(s.size() == 36);
        CHECK(s[8]  == '-');
        CHECK(s[13] == '-');
        CHECK(s[18] == '-');
        CHECK(s[23] == '-');

        const auto parsed = Uuid::parse(s);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == u);
    }

    TEST_CASE("parse accepts uppercase hex and canonicalizes to lowercase") {
        const auto parsed = Uuid::parse("F4E2C1A8-5B6D-4E9F-8A3C-7B2D1E0F9C8B");
        REQUIRE(parsed.has_value());
        CHECK(parsed->to_string() == "f4e2c1a8-5b6d-4e9f-8a3c-7b2d1e0f9c8b");
    }

    TEST_CASE("parse rejects wrong length") {
        const auto p1 = Uuid::parse("too-short");
        REQUIRE_FALSE(p1.has_value());
        CHECK(p1.error() == UuidParseError::WrongLength);

        const auto p2 = Uuid::parse(std::string(40, 'a'));
        REQUIRE_FALSE(p2.has_value());
        CHECK(p2.error() == UuidParseError::WrongLength);
    }

    TEST_CASE("parse rejects missing dashes") {
        // 36 chars but no dashes
        const auto p = Uuid::parse("f4e2c1a85b6d4e9f8a3c7b2d1e0f9c8babcd");
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == UuidParseError::MissingDash);
    }

    TEST_CASE("parse rejects bad hex digits") {
        const auto p = Uuid::parse("z4e2c1a8-5b6d-4e9f-8a3c-7b2d1e0f9c8b");
        REQUIRE_FALSE(p.has_value());
        CHECK(p.error() == UuidParseError::BadHexDigit);
    }

    TEST_CASE("from_raw + raw round-trip preserves bytes") {
        const std::array<std::uint8_t, 16> bytes{
            0xF4, 0xE2, 0xC1, 0xA8, 0x5B, 0x6D, 0x4E, 0x9F,
            0x8A, 0x3C, 0x7B, 0x2D, 0x1E, 0x0F, 0x9C, 0x8B,
        };
        const Uuid u = Uuid::from_raw(bytes);
        const auto round = u.raw();
        for (std::size_t i = 0; i < 16; ++i) {
            CHECK(round[i] == bytes[i]);
        }
    }

    TEST_CASE("equality compares all 16 bytes") {
        const Uuid a = Uuid::make_v4();
        Uuid b = a;
        CHECK(a == b);
        b.octets[15] ^= 0x01;
        CHECK_FALSE(a == b);
    }

    TEST_CASE("hash spreads across UUIDs (no trivial collisions on a small sample)") {
        std::unordered_set<std::uint64_t> seen;
        for (int i = 0; i < 64; ++i) {
            seen.insert(Uuid::make_v4().hash());
        }
        // 64 random 64-bit hashes — collisions are astronomically unlikely.
        CHECK(seen.size() == 64);
    }
}
