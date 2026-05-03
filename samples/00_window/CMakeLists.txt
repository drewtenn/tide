add_executable(00_window main.cpp)

target_link_libraries(00_window PRIVATE
    Tide::core
    Tide::platform
    Tide::input
    Tide::jobs
)

set_target_properties(00_window PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/00_window"
)
