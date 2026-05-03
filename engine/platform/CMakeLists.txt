find_package(glfw3 CONFIG REQUIRED)

tide_add_module(
    NAME platform
    PUBLIC_DEPS
        Tide::core
        glfw
)
