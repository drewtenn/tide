# Metal RHI backend (Phase 1). This file only configures on Apple — the parent
# CMakeLists gates `add_subdirectory(engine/rhi-metal)` behind `if(TIDE_RHI_METAL)`
# which itself defaults to OFF on non-Apple platforms.

tide_add_module(
    NAME rhi-metal
    PUBLIC_DEPS
        Tide::rhi
    PRIVATE_DEPS
        Tide::core
        Tide::platform
)

# Frameworks (locked DEFINE D27): Metal + QuartzCore + Foundation are mandatory;
# AppKit is needed because we attach CAMetalLayer to NSWindow's contentView; skip
# MetalKit (no MTKView usage in Phase 1).
target_link_libraries(tide_rhi-metal PRIVATE
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework AppKit"
)

# ARC for Objective-C++ (locked DEFINE D24). Without this, every id<MTL...>
# requires manual retain/release. Apple-Clang doesn't enable ARC by default for
# CMake-built targets the way Xcode does.
target_compile_options(tide_rhi-metal PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)

# Silence Apple's deprecation churn for Metal APIs we use intentionally.
target_compile_options(tide_rhi-metal PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
)
