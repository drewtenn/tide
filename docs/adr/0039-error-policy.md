# ADR-0039: Error policy — `std::expected<T, E>`; no exceptions in engine code

**Status:** Accepted (revised 2026-05-02 — initial draft favored `tl::expected`; revised in same session per user direction)
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

C++ has three viable error-signalling styles:

1. **Exceptions.** Zero-cost on the success path; non-trivial cost on throw; require RTTI.
2. **`std::expected<T, E>` (C++23).** Value-or-error in the return type; composable with monadic operators.
3. **Error codes / out-parameters.** Explicit, but composability is poor and the success-value path is awkward.

The engine targets C++20 with `std::expected` (a C++23 type) used selectively. Compiler availability matters because not all toolchains in the matrix ship `std::expected`.

| Platform | Default compiler | `std::expected` available? |
|---|---|---|
| macOS-14 (Apple Clang 16) | C++23 mode | Yes (libc++ 17+) |
| **Ubuntu 24.04** (GCC 13.2 default) | C++23 mode | **Yes** |
| ~~Ubuntu 22.04 (GCC 11)~~ | — | No (out of support; see below) |
| Windows-2022 (MSVC 19.40+) | C++23 mode | Yes (since 19.36) |

## Decision

**`std::expected<T, E>` is the engine's error type.** Engine code returns `std::expected<>` for fallible operations and never throws. Exceptions are caught and converted to `std::expected<>` at boundaries with third-party libraries that throw (sol2/Lua, certain `std::filesystem` implementations, FMOD).

A type alias is provided in `engine/core/include/tide/core/Expected.h` for ergonomics:

```cpp
// engine/core/include/tide/core/Expected.h
#pragma once
#include <expected>

namespace tide {
template <class T, class E>
using expected = std::expected<T, E>;

template <class E>
using unexpected = std::unexpected<E>;
}
```

The alias is for readability and to keep the migration path open if a future toolchain change forces a swap to a polyfill (e.g., `tl::expected`). Engine code uses `tide::expected<T, E>` consistently.

## CI matrix change (deviation from IMPLEMENTATION_PLAN.md §102)

The implementation plan locks `ubuntu-22.04` as the Linux CI runner. **This ADR amends that to `ubuntu-24.04`** because GCC 13+ is required for `std::expected`. The amendment is recorded here and should propagate to `.github/workflows/ci.yml` whenever it's authored (Phase 0 atomic task 3, out of scope for the current session).

If `ubuntu-22.04` matrix coverage is later required (e.g., for a user-base distribution support matrix), an explicit GCC 13 install step is the path: `apt-get install -y gcc-13 g++-13 && update-alternatives` early in the workflow. This adds ~30s but keeps `std::expected` working.

## Boundary policy: third-party libraries that throw

These are caught at the boundary and converted to `tide::expected`:

| Library | When it throws | Wrapper module |
|---|---|---|
| `sol2` (Lua bindings, Phase 8) | Lua runtime errors, type conversion failures | `scripting-lua/` catches and returns `expected<Value, ScriptError>` |
| `std::filesystem` (varies by libstdc++ version) | Some operations throw `std::filesystem_error` | `platform/Filesystem.h` wraps and returns `expected<Path, FsError>` |
| `FMOD` (Phase 7+, optional) | Returns `FMOD_RESULT` codes; doesn't throw | (no wrapping needed, but normalize to `expected`) |
| Vendored prebuilt libs (BLAS, etc.) | Varies | Wrap on first use |

Engine modules MUST NOT propagate exceptions. If a third-party throws and the boundary wrapper misses it, that's a bug — the build flag `-fno-exceptions` is NOT used (it would force changes to STL containers), but `try { ... } catch (...) { return std::unexpected(InternalError{}); }` blocks at every FFI boundary serve as the firewall.

## Error type design

Each module defines its own error enum or struct:

```cpp
namespace tide::rhi {
enum class CreateBufferError {
    OutOfMemory,
    InvalidUsageFlags,
    BackendInternal,
};
}
```

Cross-module aggregation is via `std::variant<...>` when needed (rare in P0). Avoid stringly-typed errors except for "additional context" debug strings attached to a structured enum.

## Alternatives considered

- **Exceptions.** Rejected for engine code: throw-catch unwinding hides failure source in profilers; error-uniformity matters; boundary wrapping at sol2/filesystem is a small fixed cost vs. invasive exception-safety code throughout the engine.
- **`tl::expected` (the polyfill — initial position).** Reconsidered and rejected per user direction: the polyfill exists to bridge the GCC 11 gap, but adopting `ubuntu-24.04` removes the gap entirely. Standard library is preferable when it works on every supported toolchain.
- **Boost.Outcome.** API more powerful (status codes, payload + cause chain) but adds a real dependency for marginal vs. `std::expected` ergonomic gain. Reconsider if error-context propagation becomes a Phase 6+ pain point.
- **Error codes (C-style).** Rejected: composability is poor; "did this succeed AND give me a value" requires two return paths.

## Consequences

**Positive.**
- Standard-library type — no third-party dependency, no header to vendor.
- Future C++ revisions improve `std::expected` (e.g., monadic ops `and_then`, `or_else`, `transform` are stable in C++23).
- Calling code is uniform whether the function returns `expected<T, E>` or `expected<void, E>`.
- Migration to a different error type is mechanical via the type alias.

**Negative / accepted costs.**
- Linux CI runs on ubuntu-24.04, not ubuntu-22.04 — deviates from the implementation plan. Acceptable because the deviation is enumerated here and only affects the runner image label, not the engine code.
- Compiler matrix is implicitly C++23-capable. Apple Clang 16, GCC 13+, MSVC 19.36+ are all C++23-capable in 2026; no realistic compiler in our target set fails this.
- `[[nodiscard]]` on `std::expected` is enforced by the standard, so `bugprone-unused-return-value` clang-tidy rule catches missed checks.

**Reversibility.** Changing the alias to a polyfill (`tl::expected`) is one header edit if a future compiler regression forces it. Migrating away from value-or-error to exceptions would be a multi-day sweep but mechanical.

## Forward-design hooks

- Every engine `expected<T, E>` return is implicitly `[[nodiscard]]`.
- Lint rule (Phase 0 atomic task 11): clang-tidy `bugprone-unused-return-value` enabled.
- A doc page (`docs/error-handling.md`, P0) shows canonical patterns: monadic chain, early-return, error-context attachment.

## Related ADRs

- ADR-0007 (atomic ref counts return `expected` not throw).
- (Future) ADR-0026: Memory model and allocator failure handling.

## Open question deferred

- When (if ever) to add `std::source_location` plumbing for richer error context. Deferred until error-debugging actually hurts.
