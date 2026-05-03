// Phase 1 task 9 — work-stealing pool stress smoke.
//
// Asserts CORRECTNESS only. The TSAN CI lane (task 13) catches races
// directly; perf is informational. Each test case constructs its own
// WorkStealingJobSystem so concurrent test execution doesn't cross-pollute.

#include "../../../engine/jobs/src/WorkStealingJobSystem.h"
#include "tide/jobs/IJobSystem.h"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>

using namespace tide::jobs;

TEST_CASE("WorkStealingJobSystem: 10k tiny jobs all complete") {
    WorkStealingJobSystem sys{4};
    std::atomic<int> counter{0};
    constexpr int kJobs = 10'000;

    std::vector<JobHandle> handles;
    handles.reserve(kJobs);
    for (int i = 0; i < kJobs; ++i) {
        handles.push_back(sys.submit({
            .fn = [&counter] { counter.fetch_add(1, std::memory_order_relaxed); },
            .deps = {},
            .name = nullptr,
        }));
    }
    sys.wait_all();
    CHECK(counter.load() == kJobs);
    for (auto h : handles) CHECK(sys.is_complete(h));
}

TEST_CASE("WorkStealingJobSystem: chained deps run in topological order") {
    WorkStealingJobSystem sys{4};
    std::atomic<int> stage{0};

    JobHandle a = sys.submit({
        .fn = [&stage] {
            CHECK(stage.load() == 0);
            stage.store(1);
        },
    });
    JobHandle b = sys.submit({
        .fn = [&stage] {
            CHECK(stage.load() == 1);
            stage.store(2);
        },
        .deps = std::span<const JobHandle>(&a, 1),
    });
    JobHandle c = sys.submit({
        .fn = [&stage] {
            CHECK(stage.load() == 2);
            stage.store(3);
        },
        .deps = std::span<const JobHandle>(&b, 1),
    });
    sys.wait(c);
    CHECK(stage.load() == 3);
}

TEST_CASE("WorkStealingJobSystem: fan-in with 1000 leaf deps") {
    WorkStealingJobSystem sys{4};
    constexpr int kLeaves = 1000;
    std::atomic<int> leaf_count{0};

    std::vector<JobHandle> leaves;
    leaves.reserve(kLeaves);
    for (int i = 0; i < kLeaves; ++i) {
        leaves.push_back(sys.submit({
            .fn = [&leaf_count] { leaf_count.fetch_add(1, std::memory_order_relaxed); },
        }));
    }

    std::atomic<bool> join_ran{false};
    JobHandle join = sys.submit({
        .fn = [&] {
            CHECK(leaf_count.load() == kLeaves);
            join_ran.store(true);
        },
        .deps = std::span<const JobHandle>(leaves.data(), leaves.size()),
    });
    sys.wait(join);
    CHECK(join_ran.load());
}

TEST_CASE("WorkStealingJobSystem: submit from many threads doesn't deadlock or lose jobs") {
    WorkStealingJobSystem sys{4};
    constexpr int kSubmitters = 8;
    constexpr int kPerThread  = 500;
    std::atomic<int> counter{0};

    std::vector<std::thread> submitters;
    submitters.reserve(kSubmitters);
    for (int t = 0; t < kSubmitters; ++t) {
        submitters.emplace_back([&sys, &counter, kPerThread] {
            for (int i = 0; i < kPerThread; ++i) {
                (void)sys.submit({
                    .fn = [&counter] { counter.fetch_add(1, std::memory_order_relaxed); },
                });
            }
        });
    }
    for (auto& th : submitters) th.join();
    sys.wait_all();
    CHECK(counter.load() == kSubmitters * kPerThread);
}

TEST_CASE("WorkStealingJobSystem: parallel sum produces the correct value") {
    WorkStealingJobSystem sys{4};
    constexpr int kN = 1'000'000;
    std::vector<int> data(kN);
    std::iota(data.begin(), data.end(), 1);  // 1..N

    constexpr int kChunks = 16;
    const int per_chunk = kN / kChunks;
    std::array<int64_t, kChunks> partials{};

    std::vector<JobHandle> handles;
    handles.reserve(kChunks);
    for (int c = 0; c < kChunks; ++c) {
        const int begin = c * per_chunk;
        const int end   = (c == kChunks - 1) ? kN : (c + 1) * per_chunk;
        handles.push_back(sys.submit({
            .fn = [begin, end, c, &data, &partials] {
                int64_t s = 0;
                for (int i = begin; i < end; ++i) s += data[i];
                partials[c] = s;
            },
        }));
    }
    sys.wait_all();

    const int64_t got      = std::accumulate(partials.begin(), partials.end(), int64_t{0});
    const int64_t expected = static_cast<int64_t>(kN) * (kN + 1) / 2;
    CHECK(got == expected);
}

TEST_CASE("WorkStealingJobSystem: wait_all is reentrant-safe (idempotent on quiesced pool)") {
    WorkStealingJobSystem sys{2};
    std::atomic<int> hits{0};
    JobHandle h = sys.submit({.fn = [&hits] { hits.fetch_add(1); }});
    sys.wait_all();
    sys.wait_all();
    sys.wait_all();
    CHECK(hits.load() == 1);
    CHECK(sys.is_complete(h));
}
