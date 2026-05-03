#include "tide/core/Handle.h"

#include <doctest/doctest.h>

#include <string>

namespace {
struct WidgetTag {};

using Widget = std::string;
using WidgetHandle = tide::Handle<WidgetTag>;
using WidgetPool = tide::HandlePool<Widget, WidgetTag>;
} // namespace

TEST_SUITE("core/Handle") {
    TEST_CASE("Default-constructed Handle is invalid") {
        WidgetHandle h;
        CHECK_FALSE(h.valid());
    }

    TEST_CASE("Allocate / get / release round trip") {
        WidgetPool pool;
        auto h = pool.allocate("hello");
        CHECK(h.valid());
        CHECK(pool.size() == 1);
        auto* w = pool.get(h);
        REQUIRE(w != nullptr);
        CHECK(*w == "hello");

        CHECK(pool.release(h));
        CHECK(pool.size() == 0);
        CHECK(pool.get(h) == nullptr);
    }

    TEST_CASE("ABA-safe reuse: stale handle is rejected after generation bump") {
        WidgetPool pool;
        auto h1 = pool.allocate("first");
        const auto idx = h1.index;
        const auto gen1 = h1.generation;

        REQUIRE(pool.release(h1));
        auto h2 = pool.allocate("second");

        // The slot was reused (free list LIFO), so index matches.
        CHECK(h2.index == idx);
        // Generation must have bumped — otherwise the stale handle would still resolve.
        CHECK(h2.generation != gen1);

        CHECK(pool.get(h1) == nullptr); // stale
        CHECK(pool.get(h2) != nullptr); // fresh
        CHECK(*pool.get(h2) == "second");
    }

    TEST_CASE("Releasing the same handle twice is a no-op on the second call") {
        WidgetPool pool;
        auto h = pool.allocate("x");
        CHECK(pool.release(h));
        CHECK_FALSE(pool.release(h));
    }

    TEST_CASE("Invalid handles never resolve") {
        WidgetPool pool;
        WidgetHandle invalid;
        CHECK(pool.get(invalid) == nullptr);
        CHECK_FALSE(pool.release(invalid));

        auto h = pool.allocate("real");
        WidgetHandle out_of_range{.index = h.index + 100, .generation = 1};
        CHECK(pool.get(out_of_range) == nullptr);
    }

    TEST_CASE("Many allocations grow the pool monotonically until releases create free slots") {
        WidgetPool pool;
        constexpr size_t N = 64;
        std::vector<WidgetHandle> handles;
        for (size_t i = 0; i < N; ++i) {
            handles.push_back(pool.allocate(std::to_string(i)));
        }
        CHECK(pool.size() == N);
        CHECK(pool.capacity() == N);

        for (auto h : handles)
            pool.release(h);
        CHECK(pool.size() == 0);
        CHECK(pool.capacity() == N); // capacity persists; slots are reused

        auto h = pool.allocate("reused");
        CHECK(pool.capacity() == N); // no growth — pulled from free list
        CHECK(*pool.get(h) == "reused");
    }

} // TEST_SUITE
