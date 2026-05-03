# Architecture Decision Records

ADRs document irrevocable-or-expensive decisions made during the engine's development. Each ADR is one file: status, context, decision, alternatives, consequences, reversibility, and forward-design hooks.

The companion design doc (`game_engine_plan.md`) stays the architectural source of truth; ADRs document the *choices* made within that frame.

## Index — Phase 0 ADRs

| # | Title | Status | Phase | Load-bearing? |
|---|---|---|---|---|
| [0001](./0001-engine-name-and-namespace.md) | Engine name and namespace (`tide`) | Accepted | P0 | — |
| [0002](./0002-coordinate-system.md) | Coordinate system, handedness, units | Accepted | P0 | — |
| [0007](./0007-threading-model.md) | Threading model — single-threaded with P4 contractual split | Accepted | P0 | **yes** |
| [0008](./0008-package-manager.md) | Package manager — vcpkg manifest mode | Accepted | P0 | **yes** |
| [0009](./0009-window-platform-library.md) | Window/platform library — GLFW 3.4 | Accepted | P0 | **yes** |
| [0037](./0037-input-action-map.md) | Input model — action-map over event queue | Accepted | P0 | — |
| [0038](./0038-string-type.md) | String type — `std::string` now, `tide::Name` in P3 | Accepted | P0 | — |
| [0039](./0039-error-policy.md) | Error policy — `std::expected<T,E>` (no exceptions); CI Linux runner amended to ubuntu-24.04 | Accepted | P0 | — |
| [0040](./0040-networking-deferral.md) | Networking — explicitly out of scope for v1 | Accepted | P0 | — |
| [0041](./0041-cpp-hot-reload.md) | C++ hot-reload — none for P0–P7 | Accepted | P0 | — |

## Reserved numbers (future ADRs from the implementation plan)

ADRs 0003–0006, 0010–0036, 0042–onward are reserved by the implementation plan for decisions that land in later phases. Do not reuse these numbers.

## Authoring template

Use [`0000-template.md`](./0000-template.md) as the starting point. ADR numbers are 4-digit, zero-padded, monotonic.
