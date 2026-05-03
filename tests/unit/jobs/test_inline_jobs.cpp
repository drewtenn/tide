#include "tide/jobs/IJobSystem.h"

#include <doctest/doctest.h>

#include <atomic>
#include <vector>

TEST_SUITE("jobs/InlineJobSystem") {
    TEST_CASE("submit() runs the function inline before returning") {
        // Use a fresh InlineJobSystem instance — default_job_system() is now
        // the work-stealing pool unless TIDE_JOBS_INLINE is set, and these
        // tests assert synchronous semantics that only the inline executor
        // provides.
        auto sys = tide::jobs::make_inline_job_system();
        int sentinel = 0;
        auto h = tide::jobs::submit(*sys, [&] { sentinel = 42; }, "test");
        CHECK(sentinel == 42);
        CHECK(sys->is_complete(h));
    }

    TEST_CASE("wait() on any handle is a no-op (inline synchronicity)") {
        auto sys = tide::jobs::make_inline_job_system();
        auto h = tide::jobs::submit(*sys, [] {}, nullptr);
        sys->wait(h);
        sys->wait_all();
        CHECK(sys->is_complete(h));
    }

    TEST_CASE("Many jobs run in the order they're submitted") {
        auto sys = tide::jobs::make_inline_job_system();
        std::vector<int> trace;
        for (int i = 0; i < 16; ++i) {
            tide::jobs::submit(*sys, [&trace, i] { trace.push_back(i); });
        }
        REQUIRE(trace.size() == 16);
        for (int i = 0; i < 16; ++i)
            CHECK(trace[i] == i);
    }

} // TEST_SUITE
