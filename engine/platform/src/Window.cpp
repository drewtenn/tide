#include "tide/platform/Window.h"

#include "tide/core/Assert.h"
#include "tide/core/Log.h"

#include <atomic>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
// Both X11 and Wayland are available at runtime via GLFW_PLATFORM.
// Including both natives is fine — the inactive one returns null.
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#include <GLFW/glfw3native.h>

namespace tide::platform {

namespace {

// GLFW init/term reference count. The first window inits GLFW; the last
// destruction tears it down. Atomic because Window may be moved across
// threads, though resource creation per ADR-0007 is mutex-protected.
std::atomic<int>& glfw_refcount() {
    static std::atomic<int> rc{0};
    return rc;
}

void glfw_error_callback(int code, const char* description) {
    ::tide::log::engine().error("GLFW error {}: {}", code, description);
}

bool ensure_glfw_init() {
    if (glfw_refcount().fetch_add(1, std::memory_order_acq_rel) > 0) {
        return true;
    }
    glfwSetErrorCallback(glfw_error_callback);
    if (glfwInit() == GLFW_FALSE) {
        glfw_refcount().fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

void release_glfw() {
    if (glfw_refcount().fetch_sub(1, std::memory_order_acq_rel) == 1) {
        glfwTerminate();
    }
}

} // namespace

tide::expected<Window, WindowError> Window::create(const WindowDesc& desc) {
    if (!ensure_glfw_init()) {
        return tide::unexpected(WindowError::InitFailed);
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Tide manages GPU APIs itself.
    glfwWindowHint(GLFW_RESIZABLE, desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, desc.start_visible ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    GLFWwindow* h = glfwCreateWindow(desc.width, desc.height, desc.title.c_str(), nullptr, nullptr);
    if (!h) {
        release_glfw();
        return tide::unexpected(WindowError::CreationFailed);
    }

    Window w;
    w.handle_ = h;
    return w;
}

Window::Window(Window&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (handle_) {
            glfwDestroyWindow(handle_);
            release_glfw();
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Window::~Window() {
    if (handle_) {
        glfwDestroyWindow(handle_);
        release_glfw();
        handle_ = nullptr;
    }
}

void Window::poll_events() noexcept {
    glfwPollEvents();
}

bool Window::should_close() const noexcept {
    return handle_ ? glfwWindowShouldClose(handle_) == GLFW_TRUE : true;
}

void Window::request_close() noexcept {
    if (handle_) glfwSetWindowShouldClose(handle_, GLFW_TRUE);
}

WindowSize Window::framebuffer_size() const noexcept {
    WindowSize s{};
    if (handle_) glfwGetFramebufferSize(handle_, &s.width, &s.height);
    return s;
}

WindowSize Window::window_size() const noexcept {
    WindowSize s{};
    if (handle_) glfwGetWindowSize(handle_, &s.width, &s.height);
    return s;
}

float Window::content_scale() const noexcept {
    if (!handle_) return 1.0f;
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(handle_, &xs, &ys);
    return xs;
}

void Window::set_title(std::string_view title) {
    if (handle_) {
        glfwSetWindowTitle(handle_, std::string(title).c_str());
    }
}

void* Window::cocoa_window() const noexcept {
#if defined(__APPLE__)
    return handle_ ? static_cast<void*>(glfwGetCocoaWindow(handle_)) : nullptr;
#else
    return nullptr;
#endif
}

void* Window::win32_hwnd() const noexcept {
#if defined(_WIN32)
    return handle_ ? static_cast<void*>(glfwGetWin32Window(handle_)) : nullptr;
#else
    return nullptr;
#endif
}

void* Window::x11_window() const noexcept {
#if defined(__linux__)
    if (!handle_) return nullptr;
    if (glfwGetPlatform() != GLFW_PLATFORM_X11) return nullptr;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(glfwGetX11Window(handle_)));
#else
    return nullptr;
#endif
}

void* Window::x11_display() const noexcept {
#if defined(__linux__)
    if (glfwGetPlatform() != GLFW_PLATFORM_X11) return nullptr;
    return static_cast<void*>(glfwGetX11Display());
#else
    return nullptr;
#endif
}

void* Window::wayland_surface() const noexcept {
#if defined(__linux__)
    if (!handle_) return nullptr;
    if (glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) return nullptr;
    return static_cast<void*>(glfwGetWaylandWindow(handle_));
#else
    return nullptr;
#endif
}

} // namespace tide::platform
