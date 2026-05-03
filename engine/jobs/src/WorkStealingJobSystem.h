#pragma once

// WorkStealingJobSystem — Phase 1 task 9 implementation of IJobSystem.
//
// Per-worker Chase-Lev deque + atomic dependency counters. The owner thread
// of each deque does lock-free push/pop_bottom; sibling workers (and the
// main thread participating in wait()) steal_top.
//
// Threading model (ADR-007):
//   - submit() may be called from any thread (worker, main, or other). On a
//     worker thread it pushes to that worker's local deque; otherwise it
//     uses thread-local round-robin across worker deques.
//   - wait() / wait_all() may be called from any thread; the calling thread
//     participates as a thief while spinning to avoid deadlock.
//   - Lifetime: jobs live in a fixed-capacity HandlePool slot, generation-
//     bumped on slot reuse. Phase 1 cap is 16384 — overflow returns an
//     invalid handle with a logged error.
//
// Construction: pool_size = max(1, hardware_concurrency() - 1) workers.
// Override via env var TIDE_JOBS_WORKERS=N for testing.

#include "tide/jobs/IJobSystem.h"
#include "tide/core/Handle.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tide::jobs {

inline constexpr std::size_t kMaxJobsInFlight = 16384;
// Per-worker deque cap. 8192 slots × N workers covers Phase 1 stress tests
// (10k tiny jobs in a tight submit loop) without falling through to the
// inline-spill path. See debate-task9-gate1.md for the rationale.
inline constexpr std::size_t kPerWorkerCapacity = 8192;

class WorkStealingJobSystem final : public IJobSystem {
public:
    explicit WorkStealingJobSystem(std::size_t worker_count = 0);
    ~WorkStealingJobSystem() override;

    WorkStealingJobSystem(const WorkStealingJobSystem&) = delete;
    WorkStealingJobSystem& operator=(const WorkStealingJobSystem&) = delete;

    [[nodiscard]] JobHandle submit(JobDesc desc) override;
    void wait(JobHandle handle) override;
    void wait_all() override;

    [[nodiscard]] std::size_t worker_count() const noexcept override;
    [[nodiscard]] bool is_complete(JobHandle handle) const noexcept override;

    // Internal types accessed by the worker thread function. Public so the
    // .cpp can dispatch into them; treat as an implementation detail of
    // src/WorkStealingJobSystem.cpp.
    struct Job;
    struct WorkerState;

private:
    [[nodiscard]] Job* try_get_job(JobHandle h) const noexcept;
    void worker_loop(std::size_t worker_index);
    void execute(JobHandle h, Job& j);
    void on_job_complete(Job& j);
    void push_to_worker(JobHandle h);
    [[nodiscard]] JobHandle steal_any(std::size_t exclude_index) noexcept;
    void wake_one() noexcept;
    void wake_all() noexcept;

    // Per-worker queue. Phase 1 uses a std::deque guarded by a single
    // mutex — correct under any combination of owner-pop / external-push /
    // thief-steal, at the cost of lock-freedom. Chase-Lev is reserved for
    // Phase 2 alongside a proper MPMC submission queue (see
    // ChaseLevDeque.h, which stays in-tree as the reference impl).
    //
    // Owner pops from BACK (LIFO — recent pushes likely cache-hot);
    // thieves steal from FRONT (FIFO — drains the queue's tail first);
    // submitters push to BACK.
    struct WorkerQueue {
        std::deque<JobHandle> dq;
        mutable std::mutex    mu;

        // Returns false if the queue is at the per-worker cap.
        bool push_back(JobHandle h) {
            std::lock_guard<std::mutex> lock(mu);
            if (dq.size() >= kPerWorkerCapacity) return false;
            dq.push_back(h);
            return true;
        }
        JobHandle pop_back() noexcept {
            std::lock_guard<std::mutex> lock(mu);
            if (dq.empty()) return JobHandle{};
            JobHandle h = dq.back();
            dq.pop_back();
            return h;
        }
        JobHandle steal_front() noexcept {
            std::lock_guard<std::mutex> lock(mu);
            if (dq.empty()) return JobHandle{};
            JobHandle h = dq.front();
            dq.pop_front();
            return h;
        }
        bool empty() const noexcept {
            std::lock_guard<std::mutex> lock(mu);
            return dq.empty();
        }
    };

    HandlePool<Job, JobTag>          job_pool_;
    mutable std::mutex               pool_mutex_;     // guards allocate/release on job_pool_

    std::vector<std::unique_ptr<WorkerQueue>> deques_;
    std::vector<std::unique_ptr<WorkerState>> states_;
    std::vector<std::thread>         workers_;

    std::mutex                       cv_mutex_;
    std::condition_variable          cv_;
    std::atomic<bool>                shutdown_{false};
    std::atomic<std::size_t>         active_jobs_{0};
    std::atomic<std::size_t>         next_submit_target_{0};
};

} // namespace tide::jobs
