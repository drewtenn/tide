#include "tide/core/Assert.h"
#include "tide/jobs/IJobSystem.h"

#include "WorkStealingJobSystem.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace tide::jobs {

namespace {

// Phase 0 fallback: execute jobs inline on submit(). Dependencies complete
// before submit() returns by definition because every job is synchronous.
//
// The handle pool maintains a "completed" flag per slot; wait() and
// is_complete() are O(1). Generation increments on slot reuse defeat ABA
// against stale handles.
class InlineJobSystem final : public IJobSystem {
public:
    JobHandle submit(JobDesc desc) override {
        // Execute deps-first. They've all already finished (Phase 0
        // synchronicity invariant), but verify in debug.
        for (JobHandle dep : desc.deps) {
            TIDE_ASSERT(is_complete(dep), "Phase 0 invariant: dep should be complete");
        }
        if (desc.fn) {
            desc.fn();
        }
        // Synthesize a "completed" handle. Index is a monotonic counter; we
        // do not actually pool here since Phase 0 doesn't need to inspect
        // pending state. Every handle is born complete.
        const uint32_t idx = next_index_.fetch_add(1, std::memory_order_relaxed);
        return JobHandle{idx, 1};
    }

    void wait(JobHandle handle) override {
        // No-op: every submitted job has already run to completion.
        (void) handle;
    }

    void wait_all() override {
        // No-op for the same reason.
    }

    [[nodiscard]] size_t worker_count() const noexcept override {
        return 1; // The submitting thread, executing inline.
    }

    [[nodiscard]] bool is_complete(JobHandle handle) const noexcept override {
        if (!handle.valid()) return false;
        // Every handle ever issued is complete in Phase 0.
        return handle.index < next_index_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<uint32_t> next_index_{0};
};

} // namespace

IJobSystem& default_job_system() {
    // Env-gated selection. TIDE_JOBS_INLINE=1 forces synchronous execution
    // (handy for bisecting "is it parallelism or my logic" bugs); otherwise
    // the work-stealing pool is the default per Phase 1 task 9.
    static IJobSystem* instance = []() -> IJobSystem* {
        const char* env = std::getenv("TIDE_JOBS_INLINE");
        if (env && *env && std::strcmp(env, "0") != 0) {
            static InlineJobSystem inline_sys;
            return &inline_sys;
        }
        static WorkStealingJobSystem ws_sys;
        return &ws_sys;
    }();
    return *instance;
}

std::unique_ptr<IJobSystem> make_inline_job_system() {
    return std::make_unique<InlineJobSystem>();
}

} // namespace tide::jobs
