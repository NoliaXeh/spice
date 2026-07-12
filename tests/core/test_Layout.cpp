#include "doctest.h"

#include "spice/core/Layout.hpp"

using namespace spice::core;

namespace {
constexpr Rectangle screen { { 0, 0, 0 }, 80, 24 };
}

TEST_CASE("core::Layout starts empty") {
    Layout layout;
    CHECK(layout.empty());
    CHECK(layout.tiles(screen).empty());
    CHECK_FALSE(layout.contains(1));
}

TEST_CASE("core::Layout first pane fills the screen") {
    Layout layout;
    CHECK(layout.insert(1, 0, true));
    auto const tiles { layout.tiles(screen) };
    REQUIRE_EQ(tiles.size(), 1u);
    CHECK_EQ(tiles[0].first, 1u);
    CHECK_EQ(tiles[0].second, screen);
}

TEST_CASE("core::Layout horizontal split partitions the width exactly") {
    Layout layout;
    layout.insert(1, 0, true);
    layout.insert(2, 1, true);
    auto const tiles { layout.tiles(screen) };
    REQUIRE_EQ(tiles.size(), 2u);
    CHECK_EQ(tiles[0].second, Rectangle { { 0, 0, 0 }, 40, 24 });
    CHECK_EQ(tiles[1].second, Rectangle { { 0, 40, 0 }, 40, 24 });
}

TEST_CASE("core::Layout vertical split partitions the height exactly") {
    Layout layout;
    layout.insert(1, 0, true);
    layout.insert(2, 1, false);
    auto const tiles { layout.tiles(screen) };
    REQUIRE_EQ(tiles.size(), 2u);
    CHECK_EQ(tiles[0].second, Rectangle { { 0, 0, 0 }, 80, 12 });
    CHECK_EQ(tiles[1].second, Rectangle { { 12, 0, 0 }, 80, 12 });
}

TEST_CASE("core::Layout::remove() gives the space back to the sibling") {
    Layout layout;
    layout.insert(1, 0, true);
    layout.insert(2, 1, true);
    CHECK(layout.remove(2));
    auto const tiles { layout.tiles(screen) };
    REQUIRE_EQ(tiles.size(), 1u);
    CHECK_EQ(tiles[0].second, screen);

    CHECK(layout.remove(1));
    CHECK(layout.empty());
    CHECK_FALSE(layout.remove(1)); // already gone
}

TEST_CASE("core::Layout::neighbor() finds the pane in a direction") {
    Layout layout;
    layout.insert(1, 0, true);
    layout.insert(2, 1, true);  // 2 right of 1
    layout.insert(3, 2, false); // 3 below 2

    CHECK_EQ(layout.neighbor(screen, 1, Direction::right), 2u);
    CHECK_EQ(layout.neighbor(screen, 2, Direction::left), 1u);
    CHECK_EQ(layout.neighbor(screen, 2, Direction::down), 3u);
    CHECK_EQ(layout.neighbor(screen, 3, Direction::up), 2u);
    CHECK_FALSE(layout.neighbor(screen, 1, Direction::left).has_value());
    CHECK_FALSE(layout.neighbor(screen, 1, Direction::up).has_value());
}

TEST_CASE("core::Layout floats sit on top for hit testing") {
    Layout layout;
    layout.insert(1, 0, true);
    CHECK(layout.float_pane(2, { { 5, 10, 0 }, 20, 10 }));
    CHECK(layout.is_floating(2));
    CHECK_FALSE(layout.is_floating(1));

    CHECK_EQ(layout.pane_at(screen, { 6, 11, 0 }), 2u); // inside the float
    CHECK_EQ(layout.pane_at(screen, { 0, 0, 0 }), 1u);  // outside it
}

TEST_CASE("core::Layout newest float is topmost") {
    Layout layout;
    layout.float_pane(1, { { 0, 0, 0 }, 10, 10 });
    layout.float_pane(2, { { 0, 0, 0 }, 10, 10 }); // same spot, added later
    CHECK_EQ(layout.pane_at(screen, { 1, 1, 0 }), 2u);

    auto const floats { layout.floats() };
    REQUIRE_EQ(floats.size(), 2u);
    CHECK_EQ(floats.back().first, 2u); // bottom to top
}

TEST_CASE("core::Layout float and dock round-trip") {
    Layout layout;
    layout.insert(1, 0, true);
    layout.insert(2, 1, true);

    CHECK(layout.float_pane(2, { { 2, 2, 0 }, 10, 5 }));
    CHECK_EQ(layout.tiles(screen).size(), 1u); // 1 got the space back

    CHECK(layout.dock_pane(2, 1, false));
    CHECK_FALSE(layout.is_floating(2));
    CHECK_EQ(layout.tiles(screen).size(), 2u);
}

TEST_CASE("core::Layout a floating pane can be removed") {
    Layout layout;
    layout.float_pane(7, { { 0, 0, 0 }, 5, 5 });
    CHECK(layout.remove(7));
    CHECK(layout.empty());
}

TEST_CASE("core::Layout::insert() rejects duplicates and unknown targets") {
    Layout layout;
    layout.insert(1, 0, true);
    CHECK_FALSE(layout.insert(1, 1, true)); // already present
    CHECK_FALSE(layout.insert(2, 99, true)); // no such target
}
