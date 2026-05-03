# ADR-0011: Jobs system — work-stealing thread pool, no fibers

**Status:** Accepted (Phase 1 ships mutex-protected variant; Chase-Lev lock-free deque deferred to Phase 2)
**Date:** 2026-05-03
**Phase:** P1 — load-bearing
**Deciders:** Solo dev (Drew)

## Context

Modern game engines have three viable concurrency models for non-render work (asset loading, animation evaluation, physics stepping, navmesh queries, audio mixing):

1. **Thread pool with mpmc queue.** Workers pull tasks from a single shared queue. Simple; serialization on the queue's mutex caps throughput.
2. **Work-stealing thread pool.** Each worker has a local deque; workers prefer their own deque and steal from siblings when idle. Throughput scales near-linearly with cores; the lock-free Chase-Lev deque is the textbook implementation.
3. **Fiber-based scheduling** (Naughty Dog GDC 2015 talk). User-mode-scheduled fibers, ~100 µs context-switch cost, supports `wait()` from inside a job without blocking a kernel thread. AAA-grade but expensive engineering — fibers + libuv-style I/O + custom debugger integration.

The implementation plan calls this ADR load-bearing. The model dictates the dependency-tracking API (`JobDesc::deps`), the cooperative-wait pattern (callers participate as thieves), and the upgrade path for Phase 4's frame graph.

## Decision

**The jobs system is a work-stealing thread pool with per-worker deques and dependency-counter scheduling. No fibers.**

Phase 1 ships the *mutex-protected* variant: each per-worker deque is a `std::deque<JobHandle>` guarded by a single mutex. All push / pop / steal operations take the mutex. The Chase-Lev lock-free deque is reserved as the Phase 2 upgrade path alongside an MPMC submission queue.

Why ship the mutex-protected variant first: Chase-Lev is *single-pusher*. Submission from non-worker threads (the main thread, an asset-loader thread) corrupts `bottom_` if it pushes directly. The proper Phase 2 design is workers as the only pushers, with non-worker submissions going through an MPMC queue that workers periodically drain. Phase 1 gets the API right; Phase 2 swaps the deque implementation without touching consumers.

Concrete shape (`engine/jobs/include/tide/jobs/IJobSystem.h`, unchanged from Phase 0 stub):

```cpp
struct JobDesc {
    JobFunction fn;
    std::span<const JobHandle> deps{};
    const char* name = nullptr;
};

class IJobSystem {
public:
    [[nodiscard]] virtual JobHandle submit(JobDesc desc) = 0;
    virtual void wait(JobHandle handle) = 0;
    virtual void wait_all() = 0;
    [[nodiscard]] virtual size_t worker_count() const noexcept = 0;
    [[nodiscard]] virtual bool is_complete(JobHandle handle) const noexcept = 0;
};

[[nodiscard]] IJobSystem& default_job_system();              // env-gated: WorkStealing or Inline
[[nodiscard]] std::unique_ptr<IJobSystem> make_inline_job_system();  // tests
```

`default_job_system()` returns the WorkStealing pool unless `TIDE_JOBS_INLINE=1` is set, in which case it returns an `InlineJobSystem` for synchronous bisection.

Dependency counter (the "+1 submission guard" pattern, see Phase 1 task 9 debate gate):

```
submit(JobDesc{fn, deps}):
  job.remaining_deps = deps.size() + 1   // +1 = submission guard
  for dep in deps:
    lock(dep.deps_mutex)
    if dep.completed: dec(job.remaining_deps)
    else:             dep.dependents.push_back(job)
    unlock
  if dec(job.remaining_deps) == 1: push_to_worker(job)

on_job_complete(j):
  j.completed.store(true, release)
  lock(j.deps_mutex); wake = swap(j.dependents); unlock
  for d in wake:
    if dec(d.remaining_deps) == 1: push_to_worker(d)
```

The `+1 guard` closes the race where every dep completes between `is_complete` checks and dependent registration — the final guard release at the end of submit is what triggers the push if all deps finished during registration.

## Alternatives considered

- **Fibers.** Rejected:
  - Solo-dev cognitive load: fibers add a layer of "what thread am I on?" debugging on top of GPU sync, asset lifetime, and lock-free data structures. Each is hard alone; together they're a session-budget vortex.
  - Tooling: clean fiber support in Tracy + Xcode + RenderDoc + the debugger of choice is patchwork. Threads work everywhere out-of-the-box.
  - The cooperative-wait scenario fibers solve (`wait()` from inside a job) is achievable with thieving on threads. Same end state, less plumbing.
- **Single mpmc queue.** Rejected: workers serialize on the queue's mutex; the Phase 1 stress test (10k tiny jobs, 8-thread fan-in) saturates a single mutex. Per-worker deques scale.
- **Ship Chase-Lev immediately.** Rejected: Chase-Lev's single-pusher constraint requires the MPMC submission queue to feed it, doubling the Phase 1 surface. The mutex-protected variant gets the API right without the architectural surface.
- **Atomic counter scheduling without per-job mutex.** Rejected: the dep-registration race (submit observes "not complete", dep completes, wake list closes, submit appends to a list nobody will process) needs *some* synchronization point. The per-job `deps_mutex` is the cheapest correct one; alternatives (lock-free linked lists with hazard pointers) are Phase 2+ territory.

## Consequences

**Positive.**
- Cooperative wait works correctly: `wait(handle)` from any thread thieves while polling `is_complete(handle)`. No deadlock when all workers are blocked on each other (the calling thread can run the job that unblocks them).
- API is stable across the Phase 2 lock-free swap. Phase 1 consumers (currently zero — the engine's main loop hasn't onboarded jobs yet) won't see breakage.
- Worker count is `max(1, hardware_concurrency() - 1)` — leaves the main thread its own core. Override via `TIDE_JOBS_WORKERS=N` for tests.
- Tracy zones (`JobSubmit` / `JobExecute` / `WorkerSteal`) and named threads (`tide-worker-0`, ...) are wired from day one.
- 19/19 stress-test cases pass (10k tiny jobs, chained deps, fan-in 1000-leaf, multi-thread submit, parallel sum 1M-element, reentrant wait_all).

**Negative / accepted costs.**
- Mutex-protected deque doesn't scale to AAA workloads. Phase 1's stress test passes in ~50 ms; a real frame with 10k tasks would saturate the mutexes. Documented as the Phase 2 upgrade path.
- Job pool capped at 16384 (with a 4096-size per-worker deque). Overflow logs an error and refuses the push. Consumers that want unbounded jobs need Phase 2's free-list reclamation.
- `std::function` allocation per submit is a small-fn-optimization-shaped hole. Phase 1 doesn't profile; std::function stays. Phase 3+ replaces with `function2::function` or an inplace function with a fixed buffer.

**Reversibility.** Phase 2 swaps the deque implementation without changing the IJobSystem contract. The "+1 submission guard" pattern carries over. Switching away from work-stealing (e.g. to fibers) is multi-month and rewrites every consumer's wait/sync pattern.

## Forward-design hooks

- **No virtual on JobHandle.** Same forward-design rule as ADR-0003 — the handle is opaque and pool-resolved.
- **`JobDesc::deps` is a span, not a vector.** Spans are zero-allocation views; consumers can pass `std::array`, vector, or a literal initializer list. No allocation on the submit path.
- **`is_complete()` is a hot path.** Implementation must be O(1); no scanning the dependents list, no walking the deque.
- **Phase 2 Chase-Lev migration:** when MPMC submission queue lands, the per-worker deque type changes but the WorkStealingJobSystem methods don't. Consumers don't need to know.
- **No global state from job functions.** Tracy zone names assume a future render-thread split (ADR-0007); jobs that touch RHI handles must respect the resource_mutex_ contract. Pure-CPU work (decompression, animation eval) is the safe default.

## Related ADRs

- ADR-0003: RHI handle strategy — `JobHandle` follows the same `Handle<Tag>` shape.
- ADR-0007: Threading model — RHI work runs on the main thread; jobs runs on workers from Phase 1.
- ADR-0012: Error handling — `JobDesc::fn` is `void()`; jobs that need to surface errors do so via captured state and the `wait()` return path.
- (Future) ADR-0042+ Phase 2: Chase-Lev + MPMC submission queue + free-list reclamation.
