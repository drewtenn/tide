# ADR-0001: Engine name and namespace

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

The engine needs a project name, a top-level C++ namespace, a CMake target prefix, and a public include path. Picking late is more expensive than picking now because the include path appears in every public header.

The architectural plan (`game_engine_plan.md` §6) uses `<engine_namespace>` as a placeholder; the implementation plan reserves this as ADR-001.

## Decision

- Engine name: **`tide`**
- C++ namespace: **`tide`**
- Sub-namespaces match module names: `tide::core`, `tide::rhi`, `tide::renderer`, etc.
- CMake target prefix: `tide_<module>` for the static library; `Tide::<module>` for the alias target.
- Public include path: `engine/<module>/include/tide/<module>/...`, used as `#include <tide/core/Handle.h>`.

## Alternatives considered

- **`engine`** — too generic; future grep pain.
- **`forge`** — collides with The-Forge graphics middleware.
- **First-name initial** — narcissistic at the API surface.

## Consequences

**Positive.**
- Short (4 chars) keeps include lines readable.
- No known major-project collision.
- "`tide`" reads neutrally; carries no marketing weight that would later embarrass.

**Negative / accepted costs.**
- Renaming later requires one mechanical pass over every public header and every CMake target. Cheap in P0; multi-day in P5+.

**Reversibility.** Trivial in P0 (~30 min find/replace). 1–2 days in P3+. Multi-day in P10+.

## Forward-design hooks

- All public headers use angle-bracket includes with the `tide/` prefix from day one.
- All CMake targets are declared via the `tide_add_module()` helper (defined in `cmake/tide_module.cmake`), not raw `add_library()`.

## Related ADRs

- ADR-0008: Package manager (the `vcpkg.json` `name` field uses `tide-engine`).
