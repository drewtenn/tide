#include "tide/input/Action.h"
#include "tide/input/Actions.h"

#include <doctest/doctest.h>

TEST_SUITE("input/Actions") {
    TEST_CASE("Action equality is by id, not by name") {
        using namespace tide::input;
        constexpr Action a{.name = "x", .id = 7};
        constexpr Action b{.name = "y", .id = 7};
        static_assert(a == b);
        CHECK(a == b);
    }

    TEST_CASE("Default-constructed Action is invalid (id == 0)") {
        using namespace tide::input;
        constexpr Action zero{};
        static_assert(!zero.valid());
        CHECK_FALSE(zero.valid());
    }

    TEST_CASE("Predefined actions have unique ids") {
        using namespace tide::input;
        static_assert(Actions::Quit.id != Actions::Confirm.id);
        static_assert(Actions::MoveForward.id != Actions::MoveBack.id);
        static_assert(Actions::Look.kind == ActionKind::Axis2D);
        CHECK(Actions::Quit.id == 1);
        CHECK(Actions::Look.kind == ActionKind::Axis2D);
    }

} // TEST_SUITE
