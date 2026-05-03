# ADR-0037: Input model — action-map over event queue

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

Two valid input shapes exist for game engines:

- **Pure event-driven** — every keystroke, mouse motion, button event is dispatched immediately. Natural for UI, IME, text input.
- **Pure poll** — game code asks "is this action active *right now*?" each frame. Natural for game loops.

Mixing them naively (gameplay polls, UI events) leaves a class of bugs around input consumption: who claims the event when both an open dialog and the player camera want the mouse?

The plan (`IMPLEMENTATION_PLAN.md` task 9, ADR-037) calls for an action-map model: gameplay code references named actions (`Actions::Jump`), never raw key codes.

## Decision

**Action-map at the gameplay-facing API, layered over an internal event queue, with a context stack for input consumption.**

### Layered shape

```
gameplay code
  ↓
input::isPressed(Actions::Jump)
input::isJustPressed(Actions::Jump)
input::getAxis(Actions::Look, deadzone=0.1f)
  ↓
ActionMap (translates event stream → per-frame action state)
  ↓
internal event queue (key down/up, mouse motion, gamepad button, etc.)
  ↓
platform/Window.h (GLFW polling produces events; SDL3-port-shaped)
```

### Action definition (compile-time strongly typed)

Actions are declared once in a central registry, not as string literals at call sites:

```cpp
// engine/input/include/tide/input/Actions.h (P0 stub; populated as gameplay needs)
namespace tide::input::Actions {
    inline constexpr Action Jump        {.name = "Jump",        .id = 1};
    inline constexpr Action MoveForward {.name = "MoveForward", .id = 2};
    inline constexpr Action Look        {.name = "Look",        .id = 3, .kind = ActionKind::Axis2D};
    // ... more added in P5+
}
```

Gameplay code: `if (input.isJustPressed(Actions::Jump)) ...`. Never `input.isJustPressed("Jump")`. This rules out the entire string-typo class of bugs.

### Context stack with consumption

Multiple input contexts may exist simultaneously: gameplay, ImGui, in-game console, pause menu. They form a stack:

```cpp
input.pushContext(ImGuiContext{});      // pushed when ImGui wants input
input.pushContext(GameplayContext{});   // bottom of stack, default
```

When polling, contexts higher in the stack get first claim; if a context returns `Consumed`, the action does not propagate down. ImGui pushes itself when `io.WantCaptureKeyboard || io.WantCaptureMouse`; pops on the next frame when those flags clear.

### `isJustPressed` is edge-detected from the queue, not poll-comparison

A frame-rate spike that drops below the input event rate can lose a press-and-release that occurred within a single frame. Naive poll-comparison (`was_pressed && !is_pressed`) misses this; queue-driven detection counts the events explicitly.

### Axis handling with deadzone

`getAxis(action, deadzone)` returns `0.0f` for input below `deadzone`, then linearly maps `[deadzone, 1.0f] → [0.0f, 1.0f]` — never raw values from the device. Default deadzone for analog sticks is `0.1f`; for triggers `0.05f`.

## Alternatives considered

- **Pure event-driven.** Rejected for gameplay: forces every gameplay system to maintain its own state machine for "is the player still pressing forward?". Wastes effort, fragments behavior.
- **Pure poll, no event queue.** Rejected: loses sub-frame events at low FPS; UI (ImGui) genuinely needs events for IME and text input.
- **String-keyed action lookup at call sites.** Rejected: typos are silent bugs; refactoring an action name needs grep over the entire codebase.
- **Bind raw key codes from gameplay code.** Rejected: rebinding becomes a per-game-system task; localizing to QWERTY/Dvorak/AZERTY layouts requires hardcoded if-trees.

## Consequences

**Positive.**
- Gameplay code is layout-agnostic and rebindable from a config file (Phase 5 inspector).
- ImGui consumption is automatic via context stack.
- Subframe input not lost.
- Deadzone policy centralized.

**Negative / accepted costs.**
- One indirection between platform event and gameplay query — measured cost is sub-microsecond.
- Action definitions must be maintained in a central header. As gameplay grows in P5+, this header becomes a hub. Acceptable; arguably preferable to scattered constants.

**Reversibility.** Trivial — swapping the implementation of `isPressed()` doesn't touch gameplay code.

## Forward-design hooks

- `Action` struct includes a stable `id` field for serialization (so config files survive renames).
- Binding configuration (`Actions::Jump → SPACE | GAMEPAD_A`) lives in a JSON file under `config/input.json` (Phase 5+); P0 has hardcoded defaults in `Actions.h`.
- The context stack supports a `priority` field for cases where two contexts want simultaneous input (e.g., gameplay-camera-look while ImGui wants typing). Default behavior is "topmost wins"; explicit priority is escape hatch.

## Related ADRs

- ADR-0009: Window/platform (GLFW polling feeds the event queue).
- (Future) ADR-0027: Action binding serialization format.
