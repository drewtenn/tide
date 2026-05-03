#pragma once

#include "tide/core/Handle.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace tide::jobs {

struct JobTag {};

using JobHandle = Handle<JobTag>;

// Job function signature. The job runs to completion before its handle's
// wait() returns. Phase 0 calls the function inline on submit(); Phase 1
// dispatches to a work-stealing pool.
using JobFunction = std::function<void()>;

// Submission descriptor. Dependencies must complete before this job runs.
// Phase 0 honors deps trivially because submit() runs synchronously.
struct JobDesc {
    JobFunction fn;
    std::span<const JobHandle> deps{};
    const char* name = nullptr; // Tracy zone name; null = "Job"
};

// IJobSystem is the contract every backend (P0 inline, P1 work-stealing)
// implements. The interface shape is locked here so dependent modules can
// link against it without waiting for the real pool in Phase 1.
class IJobSystem {
public:
    virtual ~IJobSystem() = default;

    [[nodiscard]] virtual JobHandle submit(JobDesc desc) = 0;
    virtual void wait(JobHandle handle) = 0;
    virtual void wait_all() = 0;

    [[nodiscard]] virtual size_t worker_count() const noexcept = 0;
    [[nodiscard]] virtual bool is_complete(JobHandle handle) const noexcept = 0;
};

// Constructs the default job system for the current phase. P0 returns an
// inline-synchronous executor; P1 swaps in a work-stealing pool sized to
// std::thread::hardware_concurrency() - 1.
//
// Override the default with TIDE_JOBS_INLINE=1 in the environment to force
// the inline executor (handy for bisecting "is it parallelism or my logic"
// bugs).
[[nodiscard]] IJobSystem& default_job_system();

// Constructs an InlineJobSystem on the heap. Tests use this when they want
// to assert synchronous-execution semantics without depending on the
// global default_job_system() and its env-gated behaviour.
[[nodiscard]] std::unique_ptr<IJobSystem> make_inline_job_system();

// Convenience helpers — same submit() semantics, no dependency tracking.
inline JobHandle submit(IJobSystem& sys, JobFunction fn, const char* name = nullptr) {
    return sys.submit(JobDesc{.fn = std::move(fn), .deps = {}, .name = name});
}

} // namespace tide::jobs
