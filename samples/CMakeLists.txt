# Each sample is its own subdirectory and links the minimum it needs.
# Phase 0 deliverable: samples/00_window — empty window, FrameMark heartbeat,
# clean shutdown via Quit action.
# Phase 1 task 2 smoke: samples/01_clear — Metal RHI clear-to-color
# (Apple-only, gated by TIDE_RHI_METAL inside the subdir).

add_subdirectory(00_window)
add_subdirectory(01_clear)
add_subdirectory(02_triangle)
add_subdirectory(03_textured_quad)
add_subdirectory(04_imgui_overlay)
add_subdirectory(05_offscreen_hash)
add_subdirectory(06_compute_dispatch)
