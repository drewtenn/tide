#pragma once

// tide-cooker version stamp.
//
// Per ADR-0018, this version is part of every cache key — bumping it
// invalidates every cached cooked artifact, forcing a full re-cook.
//
// Bump the constant when the cooker's *output bytes* change for any input.
// Pure refactors that produce identical output do not bump. The CI
// determinism check (cook-twice-and-cmp) catches the case where cook output
// drifted but the version wasn't bumped.

#include <cstdint>

namespace tide::cooker {

inline constexpr std::uint32_t kVersion = 1;

}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — intentional, mirrors the
// constexpr above for #if-style guards in shared headers (per ADR-0042).
#define TIDE_COOKER_VERSION 1
