# RHI is interface-only (Phase 0 scope = headers + handle types).
# Concrete backends (rhi-metal, rhi-vulkan, rhi-d3d12) land in P1/P2/P2.5.
tide_add_module(
    NAME rhi
    PUBLIC_DEPS
        Tide::core
)
