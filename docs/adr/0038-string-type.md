# ADR-0038: String type — `std::string` now, `tide::Name` in Phase 3

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

Game engines that lookup-by-string thousands of times per frame (asset paths, shader uniforms, animation joints) intern strings into integer handles for cache-friendly comparisons. AAA engines do this universally. Indie engines often do not — and in P0–P3 the engine looks up an asset path twice on load and never again. Premature interning is a real anti-pattern: it complicates debug output, requires a global table with thread-safe init, and pays back only when there's a hot loop that actually compares strings.

The decision is *when* to introduce interning, not *whether*.

## Decision

**`std::string` everywhere from Phase 0.** Introduce `tide::Name` in **Phase 3** when the asset DB needs path-keyed lookup at scale. `Name` will be:

```cpp
namespace tide {
struct Name {
    uint64_t hash;       // xxh3-64 of the string
    // Debug-only: pointer back into a thread-safe interned string table for
    // pretty-printing in error messages. Compiled out in release.
    #if !defined(TIDE_DIST_BUILD)
    const char* debug_str;
    #endif

    bool operator==(Name) const noexcept;
    constexpr Name from_literal(const char* s) noexcept;  // CTFE hash
};
}
```

### Why xxh3-64, not CRC32

CRC32 has ~0.005% collision probability at 50,000 unique strings (asset count for an indie game). That's unacceptable for content addressing. xxh3-64 has effectively zero collision risk in practice (~10^-8 at 50k entries) and is faster than CRC32 on modern CPUs. xxh3 is a single-header MIT-licensed library; cost to add to vcpkg.json in P3 is negligible.

### Migration plan in Phase 3

When P3 (asset pipeline) lands, do a mechanical sweep:

1. `using AssetPath = tide::Name;` typedef in `assets/`.
2. Replace `std::string` parameters in `assets/`, `renderer/`, `animation/` with `tide::Name` where the parameter names a *thing* (asset id, shader uniform name, joint name) rather than user-facing text.
3. User-facing text (log messages, ImGui labels, scene serialization debug names) stays `std::string`.

Estimated cost: one afternoon. The diff is mechanical and clean because P0–P2 doesn't generate many string-keyed lookups yet.

## Alternatives considered

- **`tide::Name` from Phase 0.** Rejected: nothing in P0 reads strings at frame rate. The Name type adds a layer of indirection at every API for zero current benefit, and creating a thread-safe global string table requires deciding `Mutex|Lock-free|RCU` before we have a benchmark. Wait until P3.
- **`std::string_view` everywhere.** Useful as a parameter type to avoid copies; doesn't address the lookup-key problem. Use freely as parameters; not a replacement for `Name`.
- **`std::pmr::string` with a per-frame arena.** Solves heap fragmentation. Deferred to ADR on memory management (not in P0 scope).
- **`fmt::string` / `eastl::string` / custom small-string-optimized string.** Rejected: marginal wins, real maintenance cost, unfamiliar to contributors.

## Consequences

**Positive.**
- P0–P2 code reads naturally with standard-library types.
- Name introduction in P3 is mechanical and isolated.
- Hash-based comparison is O(1) regardless of string length.

**Negative / accepted costs.**
- Name's debug pointer adds 8 bytes to non-dist builds. Acceptable.
- Two systems for string identity (path-as-string in P0–P2, Name in P3+) coexist briefly during migration.

**Reversibility.** P0–P2 use of `std::string` is freely reversible; the P3 migration to `Name` is mechanical via grep.

## Forward-design hooks

- All asset-id parameters in P0–P2 are `const std::string&` or `std::string_view`. They migrate to `tide::Name` in P3 by typedef and a parameter-type sweep.
- `Name::from_literal()` is constexpr so static initialization (e.g., `static constexpr Name kJumpAction = Name::from_literal("Jump")`) is free.

## Related ADRs

- (Future) ADR-0026: Memory model — when to introduce per-frame arenas, pool allocators, etc.
- ADR-0037: Input model (Actions are NOT `Name` — they have stable integer `id`).
