# ADR-0040: Networking — explicitly out of scope for v1

**Status:** Accepted
**Date:** 2026-05-02
**Phase:** P0
**Deciders:** Solo dev (Drew)

## Context

The architectural plan (`game_engine_plan.md` §7.10, §9) lists networking as out-of-scope for v1. The implementation plan reserves ADR-040 to make this decision explicit *and* to enumerate the assumptions that "no networking" allows the engine to bake in. Without explicit enumeration, well-intentioned later work can quietly violate assumptions that would later cost months to undo when networking is added.

This ADR is a contract — not just "no" but "no, AND here are the things that get to be true because of that no."

## Decision

**No networking in v1.** No replication, rollback, client-server, peer-to-peer, matchmaking, or netcode of any kind. The engine targets single-player, local play only.

### Assumptions this ADR makes okay

These design choices are explicitly authorized by the no-networking decision. Any future "small networking layer" work must address each one:

1. **Mutable global singletons are okay.**
   - Logger, jobs system, asset DB, profiler — all singletons accessed via free functions or `Engine::instance()`.
   - In a networked engine these become per-session state; here they don't.
   - Reverting this is mechanical but touches every singleton.

2. **Physics is non-deterministic.**
   - Floats with float-default rounding modes, frame-time-coupled stepping, spatial-hash iteration order not stable across runs.
   - Jolt Physics (Phase 6) is configured for performance, not determinism.
   - **This is the largest single retrofit.** Adding rollback netcode to non-deterministic physics is a from-scratch rewrite of the physics integration layer.

3. **Scripting side effects are immediate.**
   - Lua (Phase 8) reaches into ECS components directly. `entity:setHealth(0)` mutates state synchronously.
   - In a networked engine, Lua would queue intent for sync; here it doesn't.

4. **Asset UUIDs are not network-portable.**
   - v1 asset IDs may use local hashes (path hash, modification-time hash) that differ between machines.
   - In a networked engine, asset IDs must be content-addressed deterministic hashes so client A and client B agree on `mesh:0xDEADBEEF`.
   - This is a Phase 3 cooker decision; flagged here so it's not silently violated.

5. **Time is single-source.**
   - The frame timer is local wall-clock. No network time sync, no authoritative server tick.

6. **No serialization-stability guarantees across versions.**
   - Save files are local; if the binary version changes, save files may break.
   - Network protocols would require versioned schema; we don't.

### What this explicitly does NOT preclude

- **Local cooperative input** (multiple controllers on one machine) is fine; it's still single-process single-state.
- **Replays via input-recording.** Determinism for replay is not required as long as the replay-playback path runs the same physics on the same machine in the same state. (Save-state + reapply-input is the simple model.)
- **Online leaderboards / cloud-saves via HTTPS.** A future Phase-9+ addition could call into a `services/` module. That's not networking in the gameplay sense; it's just file upload. Not blocked by this ADR but should get its own ADR before landing.

## Alternatives considered

- **Defer the decision to v2.** Rejected: deferral allows assumptions to silently propagate. Code written in P5 that relies on mutable globals becomes load-bearing by P8 even though no one wrote it down.
- **"Maybe networking later, design defensively."** Rejected: "design defensively" without a target adds complexity (atomic singletons, deterministic physics) for no current benefit. Better to commit to single-player and document the assumptions.
- **Build a thin networking layer for online services.** Out of scope for v1. ADR can be revisited if a Phase-9+ leaderboard product requirement emerges.

## Consequences

**Positive.**
- Engine code is simpler: singletons, immediate side-effects, non-deterministic physics.
- Documentation surface is honest: "this engine does not network" is a one-line answer.
- Assumptions enumerated so future changes are explicit.

**Negative / accepted costs.**
- A v2 with networking is a major rewrite. This is understood and accepted; v1 is the explicit product.

**Reversibility.** Multi-month for a real netcode integration. Adding online services (HTTPS) is closer to one week.

## Forward-design hooks

- The `gameplay/` module (Phase 8+) does not need any "intent buffering" abstractions.
- The `physics/` integration (Phase 6) is configured for performance, not determinism.
- The `scripting-lua/` module (Phase 8) uses direct ECS access, not deferred command queues.
- Asset cooker (Phase 3) chooses asset-id strategy without considering cross-machine determinism.

If any of those choices ever needs reversal because v2 wants networking, **the new ADR must explicitly supersede this one** and enumerate every reversal.

## Related ADRs

- (Future) ADR-0026: Memory model — singleton lifecycle and per-frame allocators.
- (Future) ADR-0028: Asset ID strategy (Phase 3).
