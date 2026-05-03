#pragma once

// tide::assets::Uuid — 16-byte UUID v4, stored in `.meta` sidecar files
// alongside source assets. Identity that survives edit, rename, and move.
// See docs/adr/0016-asset-guid-strategy.md.
//
// Layout: a 16-byte standard-layout struct (POD-equivalent in C++23 terms).
// The `octets` array is the canonical big-endian byte representation
// (network byte order). Equality / hashing / serialization derive from
// these bytes; clients SHOULD NOT index `octets` directly — go through
// `raw()` for serialization or comparison helpers for everything else.
//
// Parse / format use the canonical `8-4-4-4-12` lowercase hex form, e.g.
//   "f4e2c1a8-5b6d-4e9f-8a3c-7b2d1e0f9c8b"

#include "tide/core/Expected.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tide::assets {

enum class UuidParseError : std::uint8_t {
    WrongLength,    // not exactly 36 chars
    BadHexDigit,    // non-hex character outside the dash positions
    MissingDash,    // dash absent at one of positions 8/13/18/23
};

struct Uuid {
    std::array<std::uint8_t, 16> octets{};

    // ─── Construction ───────────────────────────────────────────────────────

    // Generate a fresh RFC-4122 v4 UUID. Thread-safe; uses a per-thread
    // PRNG seeded from `std::random_device`.
    [[nodiscard]] static Uuid make_v4();

    // Parse the canonical 8-4-4-4-12 hex form. Lowercase or uppercase hex
    // accepted; emit lowercase via `to_string()` for canonicalization.
    [[nodiscard]] static tide::expected<Uuid, UuidParseError>
        parse(std::string_view canonical);

    // Adopt 16 raw bytes verbatim. The cooker uses this when reading a
    // `.meta` sidecar; the runtime uses it when reading the embedded UUID
    // from a `RuntimeHeader` (per ADR-0017). Most other code should not
    // need it — prefer `parse()` from string forms.
    [[nodiscard]] static constexpr Uuid from_raw(std::span<const std::uint8_t, 16> bytes) noexcept {
        Uuid u{};
        for (std::size_t i = 0; i < 16; ++i) {
            u.octets[i] = bytes[i];
        }
        return u;
    }

    // ─── Inspection ─────────────────────────────────────────────────────────

    [[nodiscard]] constexpr bool is_nil() const noexcept {
        // NOLINTNEXTLINE(readability-use-anyofallof) — constexpr-friendly loop
        for (auto b : octets) {
            if (b != 0) {
                return false;
            }
        }
        return true;
    }

    // 16-byte big-endian view. Documented escape hatch for ADR-0017
    // serialization — use `from_raw()` for the reverse direction.
    [[nodiscard]] constexpr std::span<const std::uint8_t, 16> raw() const noexcept {
        return std::span<const std::uint8_t, 16>(octets);
    }

    [[nodiscard]] std::string to_string() const;

    // 64-bit fold for hashing into `unordered_map`. Not cryptographic.
    [[nodiscard]] constexpr std::uint64_t hash() const noexcept {
        std::uint64_t hi = 0;
        std::uint64_t lo = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            hi = (hi << 8) | octets[i];
            lo = (lo << 8) | octets[i + 8];
        }
        return hi ^ lo;
    }

    [[nodiscard]] friend constexpr bool operator==(const Uuid& a, const Uuid& b) noexcept {
        // Element-wise loop rather than `= default` because `std::array`'s
        // operator== is not constexpr on every standard library we ship to.
        for (std::size_t i = 0; i < 16; ++i) {
            if (a.octets[i] != b.octets[i]) {
                return false;
            }
        }
        return true;
    }
};

static_assert(sizeof(Uuid) == 16, "Uuid must pack to 16 bytes for ADR-0017 RuntimeHeader embedding");
static_assert(alignof(Uuid) == 1);

} // namespace tide::assets

template <> struct std::hash<tide::assets::Uuid> {
    [[nodiscard]] std::size_t operator()(const tide::assets::Uuid& u) const noexcept {
        return static_cast<std::size_t>(u.hash());
    }
};
