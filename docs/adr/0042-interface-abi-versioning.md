# ADR-0042: Interface ABI versioning + `[[deprecated]]` two-phase removal

**Status:** Accepted
**Date:** 2026-05-03
**Phase:** P2-lite (pulled forward from P2 into P3)
**Deciders:** Solo dev (Drew)

## Context

The roadmap originally placed this convention in Phase 2 (`IMPLEMENTATION_PLAN.md:282`), where building the Vulkan backend would have stress-tested the RHI interface and exposed every place the abstraction leaks. We're skipping P2/P2.5 and going straight to P3+ on a Metal-only RHI to push features (assets → renderer → scene → physics → audio → scripting) before tackling a second backend.

That changes the risk profile. With a single backend, breaking changes to `IDevice` / `ICommandBuffer` / asset interfaces will go in unannounced — there's no Vulkan port forcing a parity discussion at the PR-review stage. Whenever P2 eventually lands (or someone else picks the engine up and ports it), there will be no paper trail for which commits broke source / ABI compatibility, and no compile-time guard for consumers (samples, tests, future scripting bindings) that need to assert they're built against a compatible interface.

Three things make this worth a one-day investment now rather than later:

1. **Reverse-engineering breaks is much harder than recording them.** A `git log` of a header file across 18 months of P3–P9 will not tell us "this commit broke the binding signature for descriptor sets" without painful re-reading. A monotonically-increasing integer that we bump on each break makes the breakage list trivial to enumerate.
2. **Two-phase removal needs the convention in place from the start.** `[[deprecated("Use Foo — see ADR-NNNN")]]` only works if the deprecation lands one version *before* the removal — bolted on at P9 it's just verbose. Ramping the discipline now means by the time we add Vulkan, the engine has years of disciplined deprecation messages.
3. **It is one of the cheapest items in the whole P2 list.** Per `IMPLEMENTATION_PLAN.md:346` the original estimate was 1 day. The other expensive P2 items (Vulkan device, golden-image CI, MoltenVK quirks) are explicitly deferred — but golden-frame CI on Metal-only already shipped from P1 (`tests/CMakeLists.txt:83` — `tide_golden_05_offscreen_hash`), so this ADR is the only Phase-2 item still owed.

## Decision

**Per-module `version.h` headers expose `tide::<module>::kAbiVersion` as an `inline constexpr unsigned` integer, mirrored by a `TIDE_<MODULE>_ABI_VERSION` macro.** Modules covered at the start: `rhi`, `assets`, `audio`, `physics`. Every module that exposes a stable interface to consumers gets one. Constants start at `1`; they monotonically increase on **any breaking change to a public header** in that module — signature change, struct layout change, enum reorder, removal, or semantic break that callers must adapt to. Non-breaking additions (new function with a default value, new enum entry at the end with a documented "unknown" fallback) do not bump.

```cpp
// engine/rhi/include/tide/rhi/version.h
#pragma once
namespace tide::rhi {
    // Bump on any breaking change to a public header in engine/rhi/.
    // See docs/adr/0042-interface-abi-versioning.md.
    inline constexpr unsigned kAbiVersion = 1;
}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) — intentional macro mirror.
#define TIDE_RHI_ABI_VERSION 1
```

Naming follows the project convention enforced by `.clang-tidy` (macros are `TIDE_*` per `MacroDefinitionPrefix`); `IMPLEMENTATION_PLAN.md:282` proposed `ENGINE_*` as the working name but the project convention wins. The macro mirror is for `#if`-style guards in headers that may be shared with C-style cooker tooling; the `constexpr` is the canonical form for C++ code. The `cppcoreguidelines-macro-usage` lint is suppressed at the macro definition site rather than module-wide.

**Deprecation policy.** Removing or replacing a public symbol is a two-phase change:

- **Phase A (the deprecation commit).** Mark the old symbol `[[deprecated("Use Foo — see ADR-NNNN")]]`. The replacement lands in the same commit. ABI version bumps. The message *must* name the replacement and the ADR that explains the why.
- **Phase B (the removal commit).** At least one ABI bump later (i.e., one further breaking change in the module must have shipped between the deprecation and the removal), the deprecated symbol is deleted. ABI version bumps again.

This guarantees consumers get a compile-time warning telling them what changed before the symbol vanishes. For samples and tests inside this repo it's mostly courtesy; for future external consumers (scripting, mods, downstream forks) it's the contract.

**Consumer assertion.** A header that depends on a specific minimum interface version can write:

```cpp
#include "tide/rhi/version.h"
static_assert(tide::rhi::kAbiVersion >= 1, "RHI interface too old for this consumer");
```

Phase 3's asset module will start to use this against `tide::rhi::kAbiVersion` once the cooker hands GPU-resource handles back to the runtime loader.

## Alternatives considered

- **Semver-style `<major>.<minor>.<patch>`.** Rejected. On a solo project where breakage is communicated by reading `git log`, the minor/patch distinction adds ceremony without distinguishing signal — every break would just bump major. A single monotonic integer captures the only thing that matters: "did the interface break since version N?".
- **Git tags only (no in-source version constant).** Rejected. Tags answer "what's the current state" but not "is this header compatible with that consumer" at compile time. Without a constant, the `static_assert` guard pattern is impossible. Tags also don't survive vendoring or single-header extraction (likely if anyone ever amalgamates the engine).
- **Hash-based versioning (hash the public header bytes, embed the hash).** Rejected. Cute but noisy: every whitespace-only edit changes the hash, defeating the "monotone, bumps only on break" intent. Bumping a human-curated integer is the explicit signal.
- **No convention (status quo).** Rejected — that's what we'd have without this ADR. Accepted that we'd then have to reverse-engineer the break list when Vulkan eventually lands, which is exactly the kind of compounding cost `IMPLEMENTATION_PLAN.md:350` warns against.

## Consequences

**Positive.**
- The future Vulkan port (whenever it happens) gets a checklist instead of a code review of 18 months of commits — every `kAbiVersion` bump is a signpost: "this is one of the breaks you need to handle."
- Consumers can refuse to build against an incompatible interface at compile time via `static_assert`, instead of crashing or miscompiling at runtime.
- `[[deprecated]]` messages give a structured migration path. Naming the replacement *and* the ADR makes the message useful instead of decorative.
- Cheap to maintain when you remember it; the discipline is "did I touch a public header? bump the version."

**Negative / accepted costs.**
- Discipline tax: the bump must be remembered. Forgotten bumps don't cause test failures — they cause silent loss of paper trail. Mitigated in P5+ by adding a CI lint that diffs `engine/<module>/include/` between the merge base and HEAD and flags PRs that changed public headers without bumping `kAbiVersion`. Until then, this is checklist discipline.
- Bus-factor of one: the rule "bump on break, not on add" requires judgement. Solo project; accepted.

**Reversibility.** Trivial. Removing the convention later is a `git rm` of the version headers and a search/replace of any `static_assert` that references them. Adding new modules to the convention later is also trivial (one-file copy). The cost of *not* having it for the next 18 months is what's expensive — that's the point of doing it now.

## Forward-design hooks

- **Public header = one in `engine/<module>/include/`.** Internal `src/` headers are not subject to ABI versioning. The boundary is "what other modules can `#include`."
- **Bump rule, restated.** Bump `kAbiVersion` if and only if a consumer that compiled against the previous version would now (a) fail to compile, (b) compile but link wrong, or (c) compile and link but exhibit semantically different behaviour at runtime that callers must adapt to.
- **Deprecation message format.** `[[deprecated("Use <replacement> — see ADR-<NNNN>")]]`. Always name the replacement, always name the ADR. A bare `[[deprecated]]` with no message is a CI failure (lint to be added in P5+).
- **Two-phase remove.** Removal commit MUST land at least one ABI bump after the deprecation. If you find yourself wanting to deprecate-and-remove in the same commit, the symbol probably wasn't load-bearing enough to need the dance — just delete it.
- **Module additions.** When P5 adds `scene`, P6 grows `physics`, P7 grows `audio`, P8 adds `scripting`, each new public-header module gets a `version.h`. The same starting integer `1` is fine; modules version independently.
- **The cooker is special.** `tools/asset-cooker/` writes a binary runtime format with its own `uint32_t schemaVersion` header (per ADR-0017, to be drafted in P3). That schema version is *not* the same thing as `TIDE_ASSETS_ABI_VERSION` — one governs source-level interfaces, the other governs the on-disk file format. They will sometimes bump together and sometimes independently.

## Related ADRs

- ADR-0003: RHI handle strategy — the interface this convention is designed to protect.
- ADR-0012: RHI error handling — `tide::expected<...>` shapes that, if changed, would force an ABI bump.
- ADR-0017: Asset runtime binary format — distinct schema-version, see Forward-design hooks above.
- ADR-0042 supersedes nothing; it is additive.
