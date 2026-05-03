# ADR-0041: C++ hot-reload — none for P0–P7; revisit DLL-reload for `gameplay/` at P8

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

C++ iteration speed is a real productivity multiplier on multi-year projects. Three viable hot-reload strategies exist:

1. **Live++** (paid, ~$300/yr/seat) — Windows-focused, near-zero-friction patching of running processes.
2. **DLL-reload** — partition the engine into shared libraries; reload one on file change. Requires PIMPL discipline, registry indirection, and a startup-time investment.
3. **None** — relink and restart. Rely on fast incremental builds (ccache, mold, modular CMake) and short startup times.

Each adds complexity disproportionate to its current payoff for this project's phase.

## Decision

**No hot-reload of C++ code for Phase 0 through Phase 7.** Re-evaluate **DLL-reload for the `gameplay/` module only** at the start of Phase 8 if iteration on the demo-game inner loop becomes painful.

Asset hot-reload (textures, shaders, scenes) is a separate concern handled in Phase 3 (assets) and Phase 4 (renderer). It is in scope and unaffected by this ADR.

## Why no hot-reload in P0–P7

- **No iteration loop exists yet.** P0–P3 work is building the engine; iteration is "edit code → relink → run sample → check screenshot." Each cycle is 30s with ccache + LLD/mold/MSVC-incremental. Hot-reload helps inner-loop edit-test cycles, which we don't have.
- **Forcing PIMPL boundaries early constrains design.** DLL-reload requires every reloadable interface to be PIMPL'd or accessed through a registry. In P0–P3 we don't yet know the right module boundaries; locking them via PIMPL constraints is premature.
- **Live++ adds a per-seat license and onboarding step.** Reasonable for paid teams; overkill for solo indie.

## Why revisit at Phase 8 for `gameplay/` only

- Phase 8 is the demo game. The inner loop is `tweak-gameplay-code → playtest`.
- The `gameplay/` module is the natural reload boundary: it depends on `scene/`, `physics/`, `input/`, `scripting/` (all stable by P8), and is itself the volatile thing.
- DLL-reload at one specific module boundary, after the engine is mature, is a much smaller surface than engine-wide.
- Lua (Phase 8) reload is cheap — `dofile()` reloads a script. For most demo-game iteration, Lua hot-reload covers the use case without C++ DLL-reload at all.

## What to do instead for iteration speed in P0–P7

These are free, cross-platform, and solve 80% of the problem:

- **`ccache`** (Linux/macOS) / **`sccache`** (Windows) — caches compilation outputs; recompiles only changed translation units.
- **Linker choice:** `lld` (clang/Linux), `mold` (Linux), MSVC incremental link (Windows). Halves link time at minimum.
- **Modular CMake.** Each `engine/<module>/` is a separate static library; touching one module's source doesn't recompile others.
- **Doctest's lightweight runner.** Single header, no heavy fixture setup; tests rebuild fast.
- **Smaller samples.** `samples/01_triangle` shouldn't link the entire engine; each sample links the minimum modules it needs.
- **Tracy ON-demand.** Don't profile every frame in development builds; turn on when investigating.

These give roughly 5–30s edit-build-run cycles for typical changes. Hot-reload's payoff is going from 30s to ~1s, which matters in P8 demo iteration but not in P0 module bring-up.

## Alternatives considered

- **Live++ from Phase 0.** Rejected: paid, Windows-centric, premature.
- **DLL-reload for the entire engine from Phase 0.** Rejected: forces PIMPL discipline before module boundaries are known. Per-module engineering cost ~3 days; cross-engine cost ~2 weeks; benefit zero until P8.
- **CRTP-based static reload** (custom implementations recompile to a different binary, swap function pointers). Rejected: niche, fragile, debugger pain.

## Consequences

**Positive.**
- Zero up-front complexity in P0.
- Module structure is unconstrained by reload requirements.
- ccache + modular CMake + LLD give acceptable iteration speed.

**Negative / accepted costs.**
- P8 demo-game iteration may want a faster inner loop. Re-evaluation at P8 is the planned remediation.
- If the demo-game phase reveals real pain, the DLL-reload retrofit for `gameplay/` is roughly 1 week of work (PIMPL the gameplay-engine interface, watcher loop, dlopen/dlsym wiring).

**Reversibility.** Easy. Adding DLL-reload at P8 is additive, not replacing.

## Forward-design hooks

- Don't introduce singletons in `gameplay/` (P5+) that hold non-trivial state — they would become reload obstacles.
- The `gameplay/` module exposes only POD-ish interfaces to the rest of the engine; any function it needs from `engine/` is called via `extern "C"` boundaries that DLL-reload can survive.

## Related ADRs

- (Future) ADR-0015: Asset hot-reload (different concern; in scope for Phase 3).
- (Future) ADR-0030: Shader hot-reload (Phase 3).
