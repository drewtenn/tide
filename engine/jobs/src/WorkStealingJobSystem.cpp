// WorkStealingJobSystem.cpp — Phase 1 task 9 implementation.
// See header for design notes; debate-task9-gate1.md for the dep-counter
// memory-order rationale.

#include "WorkStealingJobSystem.h"

#include "tide/core/Log.h"

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#else
#define ZoneScopedN(x)        (void) 0
#define ZoneScopedNS(x, y)    (void) 0
#define TracyPlot(x, y)       (void) 0
namespace tracy { inline void SetThreadName(const char*) {} }
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <pthread.h>     // pthread_setname_np for system-tool visibility

namespace tide::jobs {

// ─── Job record ────────────────────────────────────────────────────────────

struct WorkStealingJobSystem::Job {
    JobFunction              fn;
    const char*              name{nullptr};
    std::atomic<int32_t>     remaining_deps{0};
    std::atomic<bool>        completed{false};
    std::mutex               deps_mutex;
    std::vector<JobHandle>   dependents;     // jobs blocked on this one
};

// ─── Per-worker state (RNG, worker index for thread-local resolution) ─────

struct WorkStealingJobSystem::WorkerState {
    WorkStealingJobSystem*   sys{nullptr};
    std::size_t              index{0};
    uint64_t                 rng_state{0};   // xorshift64

    [[nodiscard]] uint64_t next_random() noexcept {
        uint64_t x = rng_state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        rng_state = x;
        return x;
    }
};

namespace {

// Thread-local pointer to the WorkerState owning the calling thread, set
// at worker startup and cleared at thread join. Non-worker threads observe
// nullptr → submission falls back to round-robin across workers.
thread_local WorkStealingJobSystem::WorkerState* tls_worker = nullptr;

[[nodiscard]] std::size_t resolve_worker_count(std::size_t requested) noexcept {
    if (requested != 0) return requested;
    if (const char* env = std::getenv("TIDE_JOBS_WORKERS")) {
        const auto n = std::strtoul(env, nullptr, 10);
        if (n > 0) return static_cast<std::size_t>(n);
    }
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw <= 1) return 1;
    return static_cast<std::size_t>(hw - 1);
}

} // namespace

// ─── Construction / destruction ────────────────────────────────────────────

WorkStealingJobSystem::WorkStealingJobSystem(std::size_t worker_count) {
    // Pre-reserve the job-pool storage so its underlying vector never
    // reallocates while a worker holds a Job* pointer (debate-task9-gate1
    // / first-run SEGFAULT). With reserve, allocate() emplaces into stable
    // memory; get() pointers stay valid for the JobSystem's lifetime.
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        job_pool_.reserve(kMaxJobsInFlight);
    }

    const std::size_t n = resolve_worker_count(worker_count);
    deques_.reserve(n);
    states_.reserve(n);
    workers_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        deques_.emplace_back(std::make_unique<WorkerQueue>());
        auto state = std::make_unique<WorkerState>();
        state->sys = this;
        state->index = i;
        state->rng_state = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(i + 1) * 0xBF58476D1CE4E5B9ull);
        states_.emplace_back(std::move(state));
    }
    for (std::size_t i = 0; i < n; ++i) {
        workers_.emplace_back([this, i]() { worker_loop(i); });
    }
    TIDE_LOG_INFO("WorkStealingJobSystem: started {} worker threads (cap {} jobs)",
                  n, kMaxJobsInFlight);
}

WorkStealingJobSystem::~WorkStealingJobSystem() {
    // Drain any in-flight work before shutting down.
    wait_all();
    {
        std::lock_guard<std::mutex> lock(cv_mutex_);
        shutdown_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    for (auto& th : workers_) {
        if (th.joinable()) th.join();
    }
}

// ─── Worker loop ───────────────────────────────────────────────────────────

void WorkStealingJobSystem::worker_loop(std::size_t worker_index) {
    tls_worker = states_[worker_index].get();

    char name[24];
    std::snprintf(name, sizeof(name), "tide-worker-%zu", worker_index);
    tracy::SetThreadName(name);
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), name);
#endif

    auto& local = *deques_[worker_index];

    while (!shutdown_.load(std::memory_order_acquire)) {
        // Owner pops the back (LIFO); external pushers add to the back too,
        // and thieves steal from the front. All mutex-protected.
        JobHandle h = local.pop_back();
        if (!h.valid()) {
            h = steal_any(worker_index);
        }

        if (h.valid()) {
            if (Job* j = try_get_job(h)) {
                execute(h, *j);
            }
            continue;
        }

        // No work — spin briefly, then park. Each successful enqueue
        // notify_one()s; shutdown notify_all()s. (Spin reduces wakeup cost
        // for the common "tight burst" case.)
        for (int spin = 0; spin < 100; ++spin) {
            h = steal_any(worker_index);
            if (h.valid()) break;
        }
        if (h.valid()) {
            if (Job* j = try_get_job(h)) execute(h, *j);
            continue;
        }

        // Bounded park: even an unconditional cv_.wait risks the classic
        // lost-wakeup race against submit's wake_one. Cap the park at 1ms
        // so a missed notify becomes latency, not a deadlock. (Caught by
        // the 10k / fan-in / many-submitter stress tests.)
        std::unique_lock<std::mutex> lock(cv_mutex_);
        if (shutdown_.load(std::memory_order_acquire)) break;
        cv_.wait_for(lock, std::chrono::milliseconds(1));
    }

    tls_worker = nullptr;
}

// ─── Job execution + dependent wakeups ─────────────────────────────────────

void WorkStealingJobSystem::execute(JobHandle h, Job& j) {
    // ZoneScopedN takes a string literal; the per-job `name` is dynamic so
    // it goes through ZoneText below instead. Static zone name keeps Tracy
    // call-site decorations on the IMPLEMENTATION_PLAN's locked label list.
    ZoneScopedN("JobExecute");
#if defined(TRACY_ENABLE)
    if (j.name && *j.name) {
        ZoneText(j.name, std::strlen(j.name));
    }
#endif
    if (j.fn) j.fn();
    on_job_complete(j);
    active_jobs_.fetch_sub(1, std::memory_order_acq_rel);
    (void)h;   // handle stays valid until JobSystem dtor; no per-job release
}

void WorkStealingJobSystem::on_job_complete(Job& j) {
    // ORDER (debate-gate-1 finding): completed.store(release) BEFORE we
    // acquire deps_mutex. That way any thread that subsequently locks
    // the mutex (in submit()) observes completed == true via the
    // release/acquire pair on the lock itself.
    j.completed.store(true, std::memory_order_release);

    std::vector<JobHandle> wake;
    {
        std::lock_guard<std::mutex> lock(j.deps_mutex);
        wake.swap(j.dependents);
    }

    for (JobHandle dep_h : wake) {
        Job* dep_j = try_get_job(dep_h);
        if (!dep_j) continue;
        if (dep_j->remaining_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            push_to_worker(dep_h);
            wake_one();
        }
    }
}

// ─── Submit ────────────────────────────────────────────────────────────────

JobHandle WorkStealingJobSystem::submit(JobDesc desc) {
    ZoneScopedN("JobSubmit");

    if (shutdown_.load(std::memory_order_acquire)) {
        TIDE_LOG_WARN("WorkStealingJobSystem::submit after shutdown — dropped");
        return JobHandle{};
    }

    JobHandle h{};
    Job* job = nullptr;
    {
        std::lock_guard<std::mutex> lock(pool_mutex_);
        h = job_pool_.allocate();
        job = job_pool_.get(h);
    }
    if (!job) {
        TIDE_LOG_ERROR("WorkStealingJobSystem: job pool capacity exceeded");
        return JobHandle{};
    }

    job->fn   = std::move(desc.fn);
    job->name = desc.name;
    // +1 submission guard — final decrement happens at the end of submit
    // and ensures the job is pushed to a deque exactly once even if every
    // dep completes between the is_complete check and registration.
    const int32_t initial = static_cast<int32_t>(desc.deps.size()) + 1;
    job->remaining_deps.store(initial, std::memory_order_relaxed);
    job->completed.store(false, std::memory_order_relaxed);

    active_jobs_.fetch_add(1, std::memory_order_acq_rel);

    for (JobHandle dep_h : desc.deps) {
        if (!dep_h.valid()) {
            job->remaining_deps.fetch_sub(1, std::memory_order_acq_rel);
            continue;
        }
        Job* dep_j = try_get_job(dep_h);
        if (!dep_j) {
            // Stale handle (dep was released) — treat as already done.
            job->remaining_deps.fetch_sub(1, std::memory_order_acq_rel);
            continue;
        }

        // Lock the dep's mutex BEFORE checking completed — pairs with
        // on_job_complete's "release before lock" so the lock acquisition
        // synchronizes with the completed store.
        std::lock_guard<std::mutex> lock(dep_j->deps_mutex);
        if (dep_j->completed.load(std::memory_order_acquire)) {
            // Dep already done — decrement now (the lock provided the
            // happens-before edge).
            job->remaining_deps.fetch_sub(1, std::memory_order_acq_rel);
        } else {
            dep_j->dependents.push_back(h);
        }
    }

    // Final guard release. If all deps were already complete, this
    // decrement reaches 0 and we push to a deque ourselves.
    if (job->remaining_deps.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        push_to_worker(h);
        wake_one();
    }

    return h;
}

// ─── Push to worker (worker-local if calling from a worker, else round-robin) ─

void WorkStealingJobSystem::push_to_worker(JobHandle h) {
    const std::size_t n = deques_.size();
    if (n == 0) {
        // Degenerate: no workers — execute inline (e.g. hardware_concurrency==1).
        if (Job* j = try_get_job(h)) execute(h, *j);
        return;
    }

    if (tls_worker && tls_worker->sys == this && tls_worker->index < n) {
        if (deques_[tls_worker->index]->push_back(h)) return;
        // Local queue full — fall through to round-robin spill.
    }

    // Round-robin across workers. push_back is mutex-protected internally,
    // so concurrent submissions from multiple threads are safe.
    for (std::size_t attempt = 0; attempt < n; ++attempt) {
        const std::size_t idx =
            next_submit_target_.fetch_add(1, std::memory_order_relaxed) % n;
        if (deques_[idx]->push_back(h)) return;
    }

    // Every deque is full — execute inline as a last resort. Logs the
    // overflow so the operator sees it.
    TIDE_LOG_ERROR("WorkStealingJobSystem: every per-worker deque full; executing inline");
    if (Job* j = try_get_job(h)) execute(h, *j);
}

// ─── Steal ─────────────────────────────────────────────────────────────────

JobHandle WorkStealingJobSystem::steal_any(std::size_t exclude_index) noexcept {
    const std::size_t n = deques_.size();
    if (n <= 1) return JobHandle{};

    // Random-victim probe with bounded retries; bail when none yield work.
    auto* state = tls_worker;
    for (std::size_t attempt = 0; attempt < n * 2; ++attempt) {
        std::size_t victim;
        if (state) {
            victim = static_cast<std::size_t>(state->next_random() % n);
        } else {
            victim = (next_submit_target_.fetch_add(1, std::memory_order_relaxed)) % n;
        }
        if (victim == exclude_index) continue;
        if (JobHandle h = deques_[victim]->steal_front(); h.valid()) {
            ZoneScopedN("WorkerSteal");
            return h;
        }
    }
    return JobHandle{};
}

// ─── wait / wait_all (cooperative thieving) ────────────────────────────────

WorkStealingJobSystem::Job*
WorkStealingJobSystem::try_get_job(JobHandle h) const noexcept {
    if (!h.valid()) return nullptr;
    std::lock_guard<std::mutex> lock(pool_mutex_);
    return const_cast<HandlePool<Job, JobTag>&>(job_pool_).get(h);
}

void WorkStealingJobSystem::wait(JobHandle handle) {
    if (!handle.valid()) return;
    while (!is_complete(handle)) {
        JobHandle h{};
        if (tls_worker && tls_worker->sys == this) {
            h = deques_[tls_worker->index]->pop_back();
        }
        if (!h.valid()) {
            h = steal_any(tls_worker ? tls_worker->index : ~std::size_t{0});
        }
        if (h.valid()) {
            if (Job* j = try_get_job(h)) execute(h, *j);
        } else {
            std::this_thread::yield();
        }
    }
}

void WorkStealingJobSystem::wait_all() {
    while (active_jobs_.load(std::memory_order_acquire) > 0) {
        JobHandle h = steal_any(~std::size_t{0});
        if (h.valid()) {
            if (Job* j = try_get_job(h)) execute(h, *j);
        } else {
            std::this_thread::yield();
        }
    }
}

// ─── Capabilities ──────────────────────────────────────────────────────────

std::size_t WorkStealingJobSystem::worker_count() const noexcept {
    return workers_.size();
}

bool WorkStealingJobSystem::is_complete(JobHandle handle) const noexcept {
    if (!handle.valid()) return false;
    Job* j = try_get_job(handle);
    return j ? j->completed.load(std::memory_order_acquire) : true;
}

// ─── Wakeups ───────────────────────────────────────────────────────────────

void WorkStealingJobSystem::wake_one() noexcept {
    cv_.notify_one();
}

void WorkStealingJobSystem::wake_all() noexcept {
    cv_.notify_all();
}

} // namespace tide::jobs
