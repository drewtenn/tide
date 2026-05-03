#pragma once

#include <cstdint>
#include <string_view>

namespace tide::input {

// Kind of input the action represents. Most actions are Button (a digital
// pressed/released state); axis actions read analog magnitude. ADR-0037.
enum class ActionKind : uint8_t {
    Button, // Pressed/Released — keyboard, mouse button, gamepad button.
    Axis1D, // Single analog magnitude in [-1, 1] — trigger, scroll, mouse axis.
    Axis2D, // Two-component analog — left stick, right stick, mouse delta.
};

// Stable identifier for an Action. The id is the serialization key — config
// files reference id, not name, so renaming for the inspector is safe.
struct Action {
    std::string_view name{};
    uint32_t id{0};
    ActionKind kind{ActionKind::Button};

    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
};

constexpr bool operator==(const Action& a, const Action& b) noexcept {
    return a.id == b.id;
}

constexpr bool operator!=(const Action& a, const Action& b) noexcept {
    return !(a == b);
}

} // namespace tide::input
