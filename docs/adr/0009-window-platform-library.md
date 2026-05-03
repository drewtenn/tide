# ADR-0009: Window/platform library — GLFW 3.4

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

The `platform/` module (P0) needs a cross-platform window, event pump, and input source that supports macOS Metal, Linux Vulkan-via-MoltenVK, and Windows DX12. Two viable options exist in 2026:

1. **GLFW 3.4** — mature (15 years), small, narrow scope, used by Vulkan Tutorial / RenderDoc / most graphics tutorials.
2. **SDL3** — newly stable (Nov 2024), broader scope (audio, file dialogs, controllers), API-incompatible with SDL2.

Both are wrapped behind the `platform/Window.h` façade per the architectural plan, so the choice is genuinely reversible at ~2 days of cost.

## Decision

**GLFW 3.4** for Phase 0 through Phase 7. Revisit at the start of Phase 8 (demo game / controller integration) — if the gamepad API limitations surface materially, migrate to SDL3 then.

### Phase-8 reconsideration trigger (added per debate gate)

At Phase 8 kickoff, evaluate:
- Whether the demo game requires controller features GLFW lacks (rumble, adaptive triggers, gyro, battery state).
- Whether Phase 10 editor file-dialog needs are satisfied by `imgui_filedialog` / `tinyfiledialogs` or require SDL3's native dialog.
- SDL3 ecosystem maturity (ImGui SDL3 backend stability, Stack Overflow coverage in 2027).

If any of those tip toward SDL3, schedule a 2-day migration sprint at the start of Phase 8.

## Cross-provider disagreement (recorded for transparency)

Multi-LLM research (codex × 2 perspectives, copilot, claude) split on this ADR. Codex preferred SDL3 (stronger gamepad, Wayland, HiDPI, editor runway). Copilot preferred GLFW (mature ecosystem, smaller binary, tutorial copy-paste reliability for solo dev learning Metal/Vulkan). Codex's synthesis explicitly notes: *"If you decide Phase 0 minimalism matters more than future controller/editor features, GLFW 3.4 is a respectable fallback, not a mistake."* Copilot's reasoning is the deciding factor for our specific context — this dev has no prior Metal/Vulkan exposure and the P1 cognitive load argues for the most-tutorialized library. This ADR locks GLFW; the Phase-8 reconsideration trigger below provides the off-ramp to SDL3 if codex's predicted controller/editor frictions materialize.

## Why GLFW now

- **Tutorial copy-paste reliability.** Every Vulkan tutorial and most Metal tutorials use GLFW. Solo dev learning two GPU APIs simultaneously benefits from boilerplate that works without modification.
- **Narrow scope.** GLFW does windows, input, monitors, and OpenGL/Vulkan/Metal context creation. We don't want it doing audio (we have miniaudio) or file dialogs (Phase 10 problem).
- **Smaller binary.** ~100KB static vs. ~600KB+ for SDL3.
- **Wayland support arrived in 3.4.** Linux Wayland is explicitly supported with the runtime `GLFW_PLATFORM_WAYLAND` flag (with caveats — see consequences).
- **Gamepad parity is closer than first impression.** GLFW's gamepad mappings reuse the SDL2 community database (`gamecontrollerdb.txt`); the missing features are rumble, gyro, battery, and adaptive triggers. None are P0–P7 needs.

## Why not SDL3

- **API churn.** SDL3 changed every function name from SDL2; ecosystem (tutorials, ImGui SDL3 backend) is young in 2026.
- **Larger surface area than needed.** SDL3 audio, file dialogs, threading, and timers overlap with subsystems we already own (jobs, audio-miniaudio).
- **HWND/CAMetalLayer/Surface extraction is slightly more verbose** in SDL3's properties-bag style vs. GLFW's named accessors. (`SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL)` vs. `glfwGetWin32Window(window)`).

## The CAMetalLayer bridge — codify once, save 2-4 hours of P1 friction

GLFW gives a `NSWindow*` via `glfwGetCocoaWindow()`. Getting a `CAMetalLayer` ready for Metal swapchain creation requires a 5-line Objective-C++ snippet that is not documented in the GLFW reference. Per ADR debate, this snippet is included verbatim here so future-Drew doesn't redebug it during Phase 1:

```objc
// In rhi-metal/MetalSwapchain.mm (Phase 1):
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include <Cocoa/Cocoa.h>
#include <QuartzCore/CAMetalLayer.h>

CAMetalLayer* tide_create_metal_layer(GLFWwindow* window, id<MTLDevice> device) {
    NSWindow* nswin = glfwGetCocoaWindow(window);
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = device;
    layer.opaque = YES;
    nswin.contentView.layer = layer;
    nswin.contentView.wantsLayer = YES;
    return layer;
}
```

Notes:
- `wantsLayer = YES` is the line most often forgotten; without it the view is layer-hosting but doesn't participate in compositing.
- For Retina, also call `layer.contentsScale = nswin.backingScaleFactor;` and update on `windowDidChangeBackingProperties:`.
- The framebuffer size from `glfwGetFramebufferSize()` already accounts for the backing scale.

## Alternatives considered

- **SDL3.** See "Why not SDL3" above.
- **Qt.** Vastly over-scoped. We'd inherit a UI framework we don't want.
- **Roll our own platform layer (Cocoa + Win32 + xcb).** Multi-month yak shave for capability the engine doesn't even use yet. The plan rejected this implicitly by listing GLFW/SDL3 as the only options.
- **GLFW 3.3.** Lacks Wayland support and the runtime platform-selection mechanism.

## Consequences

**Positive.**
- Tutorials, sample code, and ImGui's GLFW backend all work without modification.
- Wraps cleanly behind `platform/Window.h`.
- Gamepad mapping database shared with SDL2 community.

**Negative / accepted costs.**
- Wayland drag-and-drop is broken on some compositors as of GLFW 3.4.0 — accept this until Phase 10 editor needs it.
- Raw mouse motion on Linux Wayland needs `relative-pointer-unstable-v1` protocol; not universal across compositors. (Phase 6 first-person camera concern.)
- Rumble and gyro are inaccessible without Phase 8 SDL3 migration. Acceptable.

**Reversibility.** ~2 days to swap to SDL3 because everything goes through `platform/Window.h` and `input/`.

## Forward-design hooks

- `platform/Window.h` exposes only operations that both GLFW and SDL3 implement: window create/destroy/resize/poll/close, framebuffer size, content scale (DPI), monitor enumeration, native handle accessors (`NSWindow*` / `HWND` / `xcb_window_t`).
- `input/` polls GLFW directly for keyboard/mouse but goes through `IGamepad` interface so a Phase 8 SDL3 migration touches one file (`platform/GlfwGamepad.cpp`).
- The CAMetalLayer snippet above is committed to `rhi-metal/MetalSwapchain.mm` in Phase 1; documented here so the friction never recurs.

## Related ADRs

- ADR-0008: Package manager (vcpkg `glfw3` port; `GLFW_BUILD_WAYLAND=OFF` on Linux).
- ADR-0037: Input model (action-map sits above GLFW polling).
