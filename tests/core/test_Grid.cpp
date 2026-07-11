#include "doctest.h"

#include "spice/core/Grid.hpp"

using namespace spice::core;

TEST_CASE("core::Grid::width() and height()") {
    Grid grid { 5, 2 };
    CHECK_EQ(grid.width(), 5u);
    CHECK_EQ(grid.height(), 2u);
}

TEST_CASE("core::Grid starts out blank") {
    Grid grid { 3, 1 };
    CHECK_EQ(grid.char_at({ 0, 0, 0 }), " ");
    CHECK_EQ(grid.char_at({ 0, 1, 0 }), " ");
    CHECK_EQ(grid.char_at({ 0, 2, 0 }), " ");
}

TEST_CASE("core::Grid::char_at() returns empty out of bounds") {
    Grid grid { 3, 2 };
    CHECK(grid.char_at({ 2, 0, 0 }).empty()); // line out of bounds
    CHECK(grid.char_at({ 0, 3, 0 }).empty()); // column out of bounds
}

TEST_CASE("core::Grid::set_text() replaces a single cell") {
    Grid grid { 5, 1 };
    CHECK(grid.set_text({ 0, 2, 0 }, "x"));
    CHECK_EQ(grid.char_at({ 0, 2, 0 }), "x");
    CHECK_EQ(grid.char_at({ 0, 1, 0 }), " "); // neighbours untouched
    CHECK_EQ(grid.char_at({ 0, 3, 0 }), " ");
}

TEST_CASE("core::Grid::set_text() supports multi-byte utf-8") {
    Grid grid { 5, 1 };
    CHECK(grid.set_text({ 0, 2, 0 }, "\xe2\x82\xac")); // "€"
    CHECK_EQ(grid.char_at({ 0, 2, 0 }), "\xe2\x82\xac");
    CHECK_EQ(grid.char_at({ 0, 1, 0 }), " ");
    CHECK_EQ(grid.char_at({ 0, 3, 0 }), " ");
}

TEST_CASE("core::Grid::set_text() fails out of bounds") {
    Grid grid { 3, 2 };
    CHECK_FALSE(grid.set_text({ 2, 0, 0 }, "x"));
    CHECK_FALSE(grid.set_text({ 0, 3, 0 }, "x"));
}

TEST_CASE("core::Grid::set_text() rejects empty text") {
    Grid grid { 3, 1 };
    CHECK_FALSE(grid.set_text({ 0, 0, 0 }, ""));
}

TEST_CASE("core::Grid::line_at()") {
    Grid grid { 3, 2 };
    CHECK_EQ(grid.line_at(0), "   ");
    CHECK(grid.set_text({ 1, 1, 0 }, "y"));
    CHECK_EQ(grid.line_at(1), " y ");
}

TEST_CASE("core::Grid::line_at() returns empty out of bounds") {
    Grid grid { 3, 2 };
    CHECK(grid.line_at(2).empty());
}
