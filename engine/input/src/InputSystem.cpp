#include "tide/core/Assert.h"
#include "tide/input/InputContext.h"
#include "tide/platform/Window.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace tide::input {

InputContext::InputContext(std::string_view name) : name_(name) {}

namespace {

// Per-action runtime state. We track edge-detected presses by counting events
// from the GLFW callback rather than poll-comparing — this preserves
// press-then-release within a single frame at low FPS, per ADR-0037.
struct ActionState {
    int press_count_this_frame = 0;
    int release_count_this_frame = 0;
    bool held = false; // true between a press and its release
    float axis_x = 0.0f;
    float axis_y = 0.0f;
};

} // namespace

struct InputSystem::Impl {
    ::tide::platform::Window* window = nullptr;

    // Bindings: hardware key/button → list of action ids that subscribe.
    std::unordered_map<int, std::vector<uint32_t>> key_to_actions;
    std::unordered_map<int, std::vector<uint32_t>> mouse_to_actions;

    // Per-action state, keyed by Action::id.
    std::unordered_map<uint32_t, ActionState> states;

    // Context stack — top of stack is the back of the vector.
    struct Entry {
        uint32_t handle;
        InputContext* ctx;
    };

    std::vector<Entry> contexts;
    uint32_t next_handle = 1;

    // Raw input observers — orthogonal to action mapping. Order is insertion;
    // every registered observer receives every event.
    std::vector<RawInputObserver*> raw_observers;

    static Impl* from(GLFWwindow* w) { return static_cast<Impl*>(glfwGetWindowUserPointer(w)); }

    void on_key(int glfw_key, int action) {
        // Raw observer dispatch first — observers see the event regardless of
        // whether the key is bound to any action or claimed by any context.
        for (auto* obs : raw_observers) {
            const bool down = (action == GLFW_PRESS || action == GLFW_REPEAT);
            const bool repeat = (action == GLFW_REPEAT);
            obs->on_key(static_cast<Key>(glfw_key), down, repeat);
        }

        auto it = key_to_actions.find(glfw_key);
        if (it == key_to_actions.end()) return;
        for (uint32_t id : it->second) {
            // Apply consumption: walk top-of-stack down, first context that
            // claims the action stops propagation.
            if (is_consumed(id)) continue;
            ActionState& s = states[id];
            if (action == GLFW_PRESS) {
                ++s.press_count_this_frame;
                s.held = true;
            } else if (action == GLFW_RELEASE) {
                ++s.release_count_this_frame;
                s.held = false;
            }
            // GLFW_REPEAT is ignored on purpose — gameplay uses is_pressed/held.
        }
    }

    void on_mouse_button(int glfw_button, int action) {
        for (auto* obs : raw_observers) {
            obs->on_mouse_button(static_cast<MouseButton>(glfw_button), action == GLFW_PRESS);
        }

        auto it = mouse_to_actions.find(glfw_button);
        if (it == mouse_to_actions.end()) return;
        for (uint32_t id : it->second) {
            if (is_consumed(id)) continue;
            ActionState& s = states[id];
            if (action == GLFW_PRESS) {
                ++s.press_count_this_frame;
                s.held = true;
            } else if (action == GLFW_RELEASE) {
                ++s.release_count_this_frame;
                s.held = false;
            }
        }
    }

    void on_cursor_pos(double x, double y) {
        for (auto* obs : raw_observers) obs->on_cursor_pos(x, y);
    }

    void on_scroll(double xoff, double yoff) {
        for (auto* obs : raw_observers) obs->on_scroll(xoff, yoff);
    }

    void on_char(uint32_t codepoint) {
        for (auto* obs : raw_observers) obs->on_char(codepoint);
    }

    bool is_consumed(uint32_t action_id) const {
        // Iterate top-down. Synthesize a minimal Action-by-id lookup —
        // contexts only see id, not the full Action shape. (ADR-0037 says
        // contexts may filter by category; for Phase 0 we pass id.)
        Action probe{.name = {}, .id = action_id, .kind = ActionKind::Button};
        for (auto it = contexts.rbegin(); it != contexts.rend(); ++it) {
            if (it->ctx && it->ctx->claim(probe)) return true;
        }
        return false;
    }
};

namespace {

void key_callback(GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
    if (auto* impl = InputSystem::Impl::from(w)) impl->on_key(key, action);
}

void mouse_button_callback(GLFWwindow* w, int button, int action, int /*mods*/) {
    if (auto* impl = InputSystem::Impl::from(w)) impl->on_mouse_button(button, action);
}

void scroll_callback(GLFWwindow* w, double xoff, double yoff) {
    if (auto* impl = InputSystem::Impl::from(w)) impl->on_scroll(xoff, yoff);
}

void cursor_pos_callback(GLFWwindow* w, double x, double y) {
    if (auto* impl = InputSystem::Impl::from(w)) impl->on_cursor_pos(x, y);
}

void char_callback(GLFWwindow* w, unsigned int codepoint) {
    if (auto* impl = InputSystem::Impl::from(w)) impl->on_char(static_cast<uint32_t>(codepoint));
}

} // namespace

InputSystem::InputSystem(::tide::platform::Window& window) : impl_(new Impl{}) {
    impl_->window = &window;
    GLFWwindow* h = window.glfw_handle();
    TIDE_ASSERT(h != nullptr, "InputSystem requires a constructed Window");
    glfwSetWindowUserPointer(h, impl_);
    glfwSetKeyCallback(h, &key_callback);
    glfwSetMouseButtonCallback(h, &mouse_button_callback);
    glfwSetScrollCallback(h, &scroll_callback);
    glfwSetCursorPosCallback(h, &cursor_pos_callback);
    glfwSetCharCallback(h, &char_callback);
}

InputSystem::~InputSystem() {
    if (impl_ && impl_->window) {
        if (GLFWwindow* h = impl_->window->glfw_handle()) {
            glfwSetWindowUserPointer(h, nullptr);
            glfwSetKeyCallback(h, nullptr);
            glfwSetMouseButtonCallback(h, nullptr);
            glfwSetScrollCallback(h, nullptr);
            glfwSetCursorPosCallback(h, nullptr);
            glfwSetCharCallback(h, nullptr);
        }
    }
    delete impl_;
}

void InputSystem::bind(KeyBinding b) {
    impl_->key_to_actions[static_cast<int>(b.key)].push_back(b.action.id);
}

void InputSystem::bind(MouseBinding b) {
    impl_->mouse_to_actions[static_cast<int>(b.button)].push_back(b.action.id);
}

uint32_t InputSystem::push_context(InputContext* ctx) {
    const uint32_t handle = impl_->next_handle++;
    impl_->contexts.push_back({handle, ctx});
    return handle;
}

void InputSystem::pop_context(uint32_t handle) {
    auto& v = impl_->contexts;
    v.erase(
        std::remove_if(
            v.begin(), v.end(), [handle](const Impl::Entry& e) { return e.handle == handle; }
        ),
        v.end()
    );
}

void InputSystem::add_raw_observer(RawInputObserver* obs) {
    if (!obs) return;
    auto& v = impl_->raw_observers;
    if (std::find(v.begin(), v.end(), obs) == v.end()) {
        v.push_back(obs);
    }
}

void InputSystem::remove_raw_observer(RawInputObserver* obs) {
    auto& v = impl_->raw_observers;
    v.erase(std::remove(v.begin(), v.end(), obs), v.end());
}

void InputSystem::begin_frame() {
    for (auto& [id, s] : impl_->states) {
        s.press_count_this_frame = 0;
        s.release_count_this_frame = 0;
    }
}

bool InputSystem::is_pressed(const Action& a) const {
    auto it = impl_->states.find(a.id);
    return it != impl_->states.end() && it->second.held;
}

bool InputSystem::is_just_pressed(const Action& a) const {
    auto it = impl_->states.find(a.id);
    return it != impl_->states.end() && it->second.press_count_this_frame > 0;
}

bool InputSystem::is_just_released(const Action& a) const {
    auto it = impl_->states.find(a.id);
    return it != impl_->states.end() && it->second.release_count_this_frame > 0;
}

float InputSystem::get_axis(const Action& a, float deadzone) const {
    auto it = impl_->states.find(a.id);
    if (it == impl_->states.end()) return 0.0f;
    const float v = it->second.axis_x;
    const float mag = v < 0.0f ? -v : v;
    if (mag < deadzone) return 0.0f;
    const float sign = v < 0.0f ? -1.0f : 1.0f;
    return sign * (mag - deadzone) / (1.0f - deadzone);
}

InputSystem::Axis2D InputSystem::get_axis2d(const Action& a, float deadzone) const {
    auto it = impl_->states.find(a.id);
    if (it == impl_->states.end()) return {};
    const float x = it->second.axis_x;
    const float y = it->second.axis_y;
    const float mag2 = x * x + y * y;
    const float dz2 = deadzone * deadzone;
    if (mag2 < dz2) return {};
    return {x, y};
}

} // namespace tide::input
