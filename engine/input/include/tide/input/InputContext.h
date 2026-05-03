#pragma once

#include "tide/input/Action.h"

#include <cstdint>
#include <string_view>
#include <vector>

// Forward declaration — the input subsystem polls GLFW directly but does not
// expose it to gameplay.
struct GLFWwindow;

namespace tide::platform {
class Window;
}

namespace tide::input {

// Keyboard scancodes (subset). These match GLFW key tokens by value, but we
// re-expose them so gameplay code never includes <GLFW/glfw3.h>.
enum class Key : int16_t {
    Unknown = -1,

    Space = 32,
    Apostrophe = 39,
    Comma = 44,
    Minus = 45,
    Period = 46,
    Slash = 47,
    Num0 = 48,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,
    Semicolon = 59,
    Equal = 61,
    A = 65,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    LeftBracket = 91,
    Backslash = 92,
    RightBracket = 93,
    GraveAccent = 96,
    Escape = 256,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right,
    Left,
    Down,
    Up,
    PageUp = 266,
    PageDown,
    Home,
    End,
    CapsLock = 280,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    F1 = 290,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    LeftShift = 340,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    Menu,
};

enum class MouseButton : uint8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
    X1 = 3,
    X2 = 4,
};

// A binding ties a hardware input to a named action. One action may have
// multiple bindings (keyboard + gamepad). Phase 0 supports keyboard and
// mouse; gamepad adds in Phase 1 alongside the work-stealing pool because
// XInput polling parallelizes well.
struct KeyBinding {
    Action action;
    Key key;
};

struct MouseBinding {
    Action action;
    MouseButton button;
};

// InputContext is one layer in the consumption stack. Higher contexts get
// first claim on input; if claim() returns true the action does not propagate
// down. Use cases: ImGui pushes itself when WantCaptureKeyboard is set;
// pause menu pushes itself when active; gameplay sits at the bottom.
class InputContext {
public:
    explicit InputContext(std::string_view name);
    virtual ~InputContext() = default;

    [[nodiscard]] std::string_view name() const noexcept { return name_; }

    // Default returns false for every action — i.e. "I don't want any of this".
    // Override in derived contexts (ImGuiContext, PauseMenuContext) to selectively
    // claim. Returning true causes the InputSystem to not feed this action to
    // anything below this context in the stack on the current frame.
    virtual bool claim(const Action& action) const {
        (void) action;
        return false;
    }

protected:
    std::string_view name_;
};

// Default contexts. GameplayContext is the bottom of the stack and never claims
// anything (gameplay only reads what survived higher consumers).
class GameplayContext final : public InputContext {
public:
    GameplayContext() : InputContext("Gameplay") {}
};

// RawInputObserver — orthogonal to the action-mapping layer. Modules that need
// raw input events (notably ImGui) register an observer; the InputSystem
// dispatches every key/mouse/scroll/char event to every registered observer in
// addition to the action-mapping pipeline. Observer methods default to no-op
// so subclasses opt in only to the events they care about.
//
// Threading: observer dispatch happens on the GLFW callback thread (the main
// thread, since GLFW callbacks fire from poll_events). Observers must not
// call back into the InputSystem from within their callbacks.
struct RawInputObserver {
    virtual ~RawInputObserver() = default;
    virtual void on_cursor_pos(double /*x*/, double /*y*/) {}
    virtual void on_mouse_button(MouseButton /*btn*/, bool /*down*/) {}
    virtual void on_scroll(double /*xoff*/, double /*yoff*/) {}
    virtual void on_key(Key /*k*/, bool /*down*/, bool /*repeat*/) {}
    virtual void on_char(uint32_t /*codepoint*/) {}
};

// InputSystem is the engine-facing API. One instance per Window; gameplay
// queries it via tide::input::isPressed(Actions::Jump) etc.
class InputSystem {
public:
    explicit InputSystem(::tide::platform::Window& window);
    ~InputSystem();

    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // Bind a hardware key/button to an action. Multiple bindings per action
    // are allowed; any one of them being active produces an `is_pressed`.
    void bind(KeyBinding binding);
    void bind(MouseBinding binding);

    // Push a context onto the consumption stack. Returns a handle index that
    // pop_context() must be given to remove it (so unrelated pushes/pops don't
    // accidentally pop the wrong context).
    [[nodiscard]] uint32_t push_context(InputContext* ctx);
    void pop_context(uint32_t handle);

    // Frame boundary. Call once per frame BEFORE polling Window events; this
    // promotes "just pressed this frame" tracking and clears axis deltas.
    void begin_frame();

    // Per-frame queries — return true/values for the current frame, after
    // begin_frame() and after Window::poll_events() have run.
    [[nodiscard]] bool is_pressed(const Action& a) const;
    [[nodiscard]] bool is_just_pressed(const Action& a) const;
    [[nodiscard]] bool is_just_released(const Action& a) const;
    [[nodiscard]] float get_axis(const Action& a, float deadzone = 0.1f) const;

    struct Axis2D {
        float x = 0.0f;
        float y = 0.0f;
    };

    [[nodiscard]] Axis2D get_axis2d(const Action& a, float deadzone = 0.1f) const;

    // Register a raw observer. Observer is held by pointer; caller owns
    // lifetime and must remove_raw_observer before the observer dies.
    // Re-registering the same pointer is a no-op.
    void add_raw_observer(RawInputObserver* obs);
    void remove_raw_observer(RawInputObserver* obs);

    // Implementation detail — exposed so the GLFW C callbacks (which take a
    // void* user pointer and recover an Impl*) can dispatch into the same
    // type. Treat as private to anyone outside src/InputSystem.cpp.
    struct Impl;

private:
    Impl* impl_ = nullptr;
};

} // namespace tide::input
