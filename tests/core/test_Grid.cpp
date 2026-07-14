#include "doctest.h"

#include "spice/core/Grid.hpp"
#include "spice/core/TermInfo.hpp"

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

TEST_CASE("core::Grid starts out with a default (zeroed) style and background") {
    Grid grid { 2, 2 };
    Color const style { grid.style_at({ 0, 0, 0 }) };
    Color const background { grid.background_at({ 0, 0, 0 }) };
    CHECK_EQ(style.r, 0);
    CHECK_EQ(style.g, 0);
    CHECK_EQ(style.b, 0);
    CHECK_EQ(background.r, 0);
    CHECK_EQ(background.g, 0);
    CHECK_EQ(background.b, 0);
}

TEST_CASE("core::Grid::set_style() and style_at() round-trip") {
    Grid grid { 3, 1 };
    Color const style {
        .r = 10, .g = 20, .b = 30,
        .style = { .bold = true, .italic = false, .underline = true,
                   .strikethrough = false, .blinking = false, .selected = false },
    };
    CHECK(grid.set_style({ 0, 1, 0 }, style));

    Color const got { grid.style_at({ 0, 1, 0 }) };
    CHECK_EQ(got.r, 10);
    CHECK_EQ(got.g, 20);
    CHECK_EQ(got.b, 30);

    bool const bold { got.style.bold };
    bool const italic { got.style.italic };
    bool const underline { got.style.underline };
    CHECK(bold);
    CHECK_FALSE(italic);
    CHECK(underline);

    // neighbouring cells untouched
    Color const neighbour { grid.style_at({ 0, 0, 0 }) };
    CHECK_EQ(neighbour.r, 0);
}

TEST_CASE("core::Grid::set_style() fails out of bounds") {
    Grid grid { 2, 2 };
    CHECK_FALSE(grid.set_style({ 5, 0, 0 }, {}));
    CHECK_FALSE(grid.set_style({ 0, 5, 0 }, {}));
}

TEST_CASE("core::Grid::set_background() and background_at() round-trip") {
    Grid grid { 3, 1 };
    Color const background { .r = 1, .g = 2, .b = 3, .style = {} };
    CHECK(grid.set_background({ 0, 1, 0 }, background));

    Color const got { grid.background_at({ 0, 1, 0 }) };
    CHECK_EQ(got.r, 1);
    CHECK_EQ(got.g, 2);
    CHECK_EQ(got.b, 3);

    // neighbouring cells and text style untouched
    Color const neighbour { grid.background_at({ 0, 0, 0 }) };
    CHECK_EQ(neighbour.r, 0);
}

TEST_CASE("core::Grid::set_background() fails out of bounds") {
    Grid grid { 2, 2 };
    CHECK_FALSE(grid.set_background({ 5, 0, 0 }, {}));
    CHECK_FALSE(grid.set_background({ 0, 5, 0 }, {}));
}

TEST_CASE("core::Grid::render() does not crash without a controlling terminal") {
    // Test runners have no tty, so TermInfo reports a 0x0 terminal here; this
    // only exercises the crop guard, not actual on-screen cropping.
    Grid grid { 3, 2 };
    TermInfo terminfo;
    grid.render(terminfo, { 0, 0, 0 });
}

TEST_CASE("core::drop_shadow() is currently disabled and leaves cells untouched") {
    Grid grid { 10, 6 };
    Color const bright { .r = 200, .g = 100, .b = 50, .style = {} };
    for (uint32_t line { 0 }; line < 6; ++line) {
        for (uint32_t column { 0 }; column < 10; ++column) {
            grid.set_background({ line, column, 0 }, bright);
        }
    }

    drop_shadow(grid, { { 1, 1, 0 }, 4, 3 });
    CHECK_EQ(grid.background_at({ 4, 3, 0 }).r, 200); // below: untouched
    CHECK_EQ(grid.background_at({ 2, 5, 0 }).r, 200); // beside: untouched
    drop_shadow(grid, { { 4, 6, 0 }, 8, 8 }); // and never crashes
}

TEST_CASE("core::Grid::render_cell() does not crash without a controlling terminal") {
    Grid grid { 3, 2 };
    TermInfo terminfo;
    grid.render_cell(terminfo, { 0, 0, 0 }, { 1, 1, 0 });
    grid.render_cell(terminfo, { 0, 0, 0 }, { 9, 9, 0 }); // out of grid: ignored
}

TEST_CASE("core::Grid::render_rect() does not crash without a controlling terminal") {
    Grid grid { 4, 3 };
    TermInfo terminfo;
    grid.render_rect(terminfo, { 0, 0, 0 }, { { 1, 1, 0 }, 2, 2 });
    grid.render_rect(terminfo, { 0, 0, 0 }, { { 1, 1, 0 }, 99, 99 }); // clipped to the grid
    grid.render_rect(terminfo, { 0, 0, 0 }, { { 9, 9, 0 }, 2, 2 });   // out of grid: ignored
}
