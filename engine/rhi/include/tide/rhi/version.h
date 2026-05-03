// engine/rhi — public interface ABI version.
//
// Bump `kAbiVersion` (and the matching macro) on any breaking change to a
// public header in this module — signature change, struct layout change,
// enum reorder, removal, or semantic break. Non-breaking additions do not
// bump. See docs/adr/0042-interface-abi-versioning.md for the full rule.
#pragma once

namespace tide::rhi {
inline constexpr unsigned kAbiVersion = 1;
}

// Macro mirror for `#if`-style guards in headers shared with C-style
// cooker tooling that may not have access to the C++ constexpr above.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — intentional, per ADR-0042.
#define TIDE_RHI_ABI_VERSION 1
