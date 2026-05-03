#pragma once

#include "tide/core/Expected.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

// Forward declaration to keep GLFW out of public headers — clients of this
// module include <tide/platform/Window.h>, never <GLFW/glfw3.h> directly.
struct GLFWwindow;

namespace tide::platform {

enum class WindowError {
    InitFailed,
    CreationFailed,
    UnsupportedPlatform,
};

struct WindowDesc {
    int width = 1280;
    int height = 720;
    std::string title = "tide";
    bool resizable = true;
    bool start_visible = true;
};

struct WindowSize {
    int width = 0;
    int height = 0;
};

// Window owns a GLFW window handle. Created via Window::create(); destroyed
// when the unique_ptr or move-constructed instance falls out of scope. The
// lifetime invariant is "exactly one Window per OS window".
class Window {
public:
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    ~Window();

    // Creates a window with the given description. GLFW is initialized lazily
    // on first call; terminated automatically when the last Window is destroyed.
    [[nodiscard]] static tide::expected<Window, WindowError> create(const WindowDesc& desc);

    // Polls events for ALL windows in the process — GLFW's API is process-global,
    // so even though this is called on an instance, it pumps every window's
    // events. Calling poll_events() on more than one window per frame is a bug.
    static void poll_events() noexcept;

    [[nodiscard]] bool should_close() const noexcept;
    void request_close() noexcept;

    [[nodiscard]] WindowSize framebuffer_size() const noexcept; // pixels (Retina-aware)
    [[nodiscard]] WindowSize window_size() const noexcept;      // virtual coords
    [[nodiscard]] float content_scale() const noexcept;         // DPI scale, e.g. 2.0 on Retina

    void set_title(std::string_view title);

    // Native handle accessors for RHI consumers. Each returns a void* that the
    // backend reinterprets to its expected pointer type. Defined on the
    // platforms that have a meaningful concept of the value:
    //   cocoa_window():   NSWindow*    (macOS only; null elsewhere)
    //   win32_hwnd():     HWND          (Windows only; null elsewhere)
    //   x11_window():     ::Window      (Linux X11; null elsewhere)
    //   x11_display():    Display*      (Linux X11; null elsewhere)
    //   wayland_surface():struct wl_surface*  (Linux Wayland; null elsewhere)
    [[nodiscard]] void* cocoa_window() const noexcept;
    [[nodiscard]] void* win32_hwnd() const noexcept;
    [[nodiscard]] void* x11_window() const noexcept;
    [[nodiscard]] void* x11_display() const noexcept;
    [[nodiscard]] void* wayland_surface() const noexcept;

    // Underlying GLFW handle. Only `platform/` and the input subsystem should
    // touch this; gameplay code goes through the action map.
    [[nodiscard]] GLFWwindow* glfw_handle() const noexcept { return handle_; }

private:
    Window() = default;
    GLFWwindow* handle_ = nullptr;
};

} // namespace tide::platform
