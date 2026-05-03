find_package(imgui CONFIG REQUIRED)

tide_add_module(
    NAME imgui-integration
    PUBLIC_DEPS
        Tide::core
        Tide::platform
        imgui::imgui
)
