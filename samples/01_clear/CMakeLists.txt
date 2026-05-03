# samples/01_clear — Metal RHI clear-to-color runtime smoke. Validates Phase 1
# tasks 1–3 end-to-end.
#
# Apple-only: the sample includes tide/rhi-metal/MetalDevice.h which is gated
# behind TIDE_RHI_METAL. Vulkan + DX12 variants will land alongside their
# backends in P2 / P2.5.

if(NOT TIDE_RHI_METAL)
    return()
endif()

add_executable(01_clear main.cpp)

target_link_libraries(01_clear PRIVATE
    Tide::core
    Tide::platform
    Tide::input
    Tide::rhi
    Tide::rhi-metal
)

set_target_properties(01_clear PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/samples/01_clear"
)
