#pragma once

#include "tide/input/Action.h"

// Central registry of every action the engine + game know about. New actions
// land here as gameplay needs them; never as string literals at call sites.
//
// IDs are stable for serialization. Do NOT reorder. Do NOT reuse an id whose
// action has been deleted — gap the numbering and add a TIDE_DEPRECATED_ACTION
// comment so future-Drew knows.
//
// Phase 0 ships a handful of generic actions used by samples/00_window for the
// clean-shutdown path; gameplay-specific actions arrive in Phase 5+.

namespace tide::input::Actions {

inline constexpr Action Quit{.name = "Quit", .id = 1, .kind = ActionKind::Button};
inline constexpr Action Confirm{.name = "Confirm", .id = 2, .kind = ActionKind::Button};
inline constexpr Action Cancel{.name = "Cancel", .id = 3, .kind = ActionKind::Button};

inline constexpr Action MoveForward{.name = "MoveForward", .id = 10, .kind = ActionKind::Button};
inline constexpr Action MoveBack{.name = "MoveBack", .id = 11, .kind = ActionKind::Button};
inline constexpr Action MoveLeft{.name = "MoveLeft", .id = 12, .kind = ActionKind::Button};
inline constexpr Action MoveRight{.name = "MoveRight", .id = 13, .kind = ActionKind::Button};
inline constexpr Action Jump{.name = "Jump", .id = 14, .kind = ActionKind::Button};

inline constexpr Action Look{.name = "Look", .id = 20, .kind = ActionKind::Axis2D};
inline constexpr Action Scroll{.name = "Scroll", .id = 21, .kind = ActionKind::Axis1D};

} // namespace tide::input::Actions
