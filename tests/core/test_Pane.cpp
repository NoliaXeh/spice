#include "doctest.h"

#include "spice/core/Grid.hpp"
#include "spice/core/Pane.hpp"
#include "spice/core/Theme.hpp"

#include <memory>

using namespace spice::core;

namespace {

auto make_buffer(std::string_view content) -> std::shared_ptr<Buffer> {
    return std::make_shared<Buffer>("buf", BufferCapability::editable, content);
}

}

TEST_CASE("core::Pane draws title bar, borders and content into a grid") {
    Grid grid { 20, 5 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("hello\nworld") };

    pane.draw(grid, { { 0, 0, 0 }, 20, 5 }, false, theme);

    // the title bar: " • buf" on a light background, dark text
    CHECK_EQ(grid.char_at({ 0, 0, 0 }), " ");
    CHECK_EQ(grid.char_at({ 0, 1, 0 }), "•"); // pane-type dot
    CHECK_EQ(grid.char_at({ 0, 3, 0 }), "b");
    CHECK_EQ(grid.char_at({ 0, 4, 0 }), "u");
    CHECK_EQ(grid.char_at({ 0, 5, 0 }), "f");
    CHECK_EQ(grid.background_at({ 0, 6, 0 }), theme.color(Theme::Usage::titlebar_background));
    CHECK_EQ(grid.style_at({ 0, 3, 0 }), theme.color(Theme::Usage::titlebar_text));
    CHECK_EQ(grid.style_at({ 0, 1, 0 }).g, colors::soft_green.g); // edit dot

    // side borders and the rounded bottom
    CHECK_EQ(grid.char_at({ 1, 0, 0 }), "│");
    CHECK_EQ(grid.char_at({ 1, 19, 0 }), "│");
    CHECK_EQ(grid.char_at({ 4, 0, 0 }), "╰");
    CHECK_EQ(grid.char_at({ 4, 19, 0 }), "╯");
    CHECK_EQ(grid.char_at({ 4, 5, 0 }), "─");

    // content inside the chrome: the gutter's line numbers, then the text
    CHECK_EQ(grid.char_at({ 1, 1, 0 }), "1");
    CHECK_EQ(grid.char_at({ 1, 3, 0 }), "h");
    CHECK_EQ(grid.char_at({ 1, 7, 0 }), "o");
    CHECK_EQ(grid.char_at({ 2, 1, 0 }), "2");
    CHECK_EQ(grid.char_at({ 2, 3, 0 }), "w");
}

TEST_CASE("core::Pane title bar buttons: F floats, x closes, both red") {
    Grid grid { 20, 5 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("x") };
    Rectangle const area { { 0, 0, 0 }, 20, 5 };

    // geometry: " F " at columns 12-14, " x " at 16-18
    CHECK_EQ(Pane::float_button(area), Rectangle { { 0, 12, 0 }, 3, 1 });
    CHECK_EQ(Pane::close_button(area), Rectangle { { 0, 16, 0 }, 3, 1 });

    pane.draw(grid, area, false, theme);
    CHECK_EQ(grid.char_at({ 0, 13, 0 }), "F");
    CHECK_EQ(grid.char_at({ 0, 17, 0 }), "x");
    Color const red { theme.color(Theme::Usage::titlebar_button_background) };
    CHECK_EQ(grid.background_at({ 0, 12, 0 }), red);
    CHECK_EQ(grid.background_at({ 0, 13, 0 }), red);
    CHECK_EQ(grid.background_at({ 0, 17, 0 }), red);

    // too narrow for buttons: bare bar, zero-sized rectangles
    Rectangle const narrow { { 0, 0, 0 }, 10, 5 };
    CHECK_EQ(Pane::float_button(narrow).width, 0u);
    CHECK_EQ(Pane::close_button(narrow).width, 0u);
}

TEST_CASE("core::Pane focus brightens the bar and colors the border") {
    Grid grid { 20, 4 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("x") };

    pane.draw(grid, { { 0, 0, 0 }, 20, 4 }, true, theme);
    CHECK_EQ(
        grid.background_at({ 0, 5, 0 }),
        theme.color(Theme::Usage::titlebar_background_focused)
    );
    bool const bold_title { grid.style_at({ 0, 3, 0 }).style.bold };
    CHECK(bold_title); // the focused title is bold
    CHECK_EQ(grid.style_at({ 1, 0, 0 }), theme.color(Theme::Usage::border_focused));

    pane.draw(grid, { { 0, 0, 0 }, 20, 4 }, false, theme);
    CHECK_EQ(
        grid.background_at({ 0, 5, 0 }), theme.color(Theme::Usage::titlebar_background)
    );
    CHECK_EQ(grid.style_at({ 1, 0, 0 }), theme.color(Theme::Usage::border));
}

TEST_CASE("core::Pane::set_cursor() clamps to the buffer") {
    Pane pane { PaneType::edit, make_buffer("ab\ncdef") };
    pane.set_cursor({ 9, 9, 0 });
    CHECK_EQ(pane.cursor(), Position { 1, 4, 0 }); // last line, end of line
    pane.set_cursor({ 0, 1, 0 });
    CHECK_EQ(pane.cursor(), Position { 0, 1, 0 });
}

TEST_CASE("core::Pane draw scrolls to keep the cursor visible") {
    Grid grid { 10, 4 }; // content is 8x2
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("l0\nl1\nl2\nl3\nl4") };

    pane.set_cursor({ 4, 0, 0 }); // on l4, below the 2-line view
    pane.draw(grid, { { 0, 0, 0 }, 10, 4 }, true, theme);
    CHECK_EQ(pane.scroll().line, 3u); // shows l3 and l4
    CHECK_EQ(grid.char_at({ 1, 2, 0 }), "3");
    CHECK_EQ(grid.char_at({ 2, 2, 0 }), "4");
}

TEST_CASE("core::Pane::scroll_to_bottom() shows the last lines") {
    Pane pane { PaneType::grid, make_buffer("a\nb\nc\nd\ne\nf") };
    pane.scroll_to_bottom({ { 0, 0, 0 }, 10, 4 }); // content height 2
    CHECK_EQ(pane.scroll().line, 4u); // e and f visible
}

TEST_CASE("core::Pane non-edit panes keep their scroll when drawn") {
    // regression: draw used to snap the scroll back to the (never moved)
    // cursor, undoing scroll_to_bottom on every frame
    Grid grid { 10, 4 };
    Theme const theme;
    Pane pane { PaneType::grid, make_buffer("a\nb\nc\nd\ne\nf") };
    Rectangle const area { { 0, 0, 0 }, 10, 4 };

    pane.scroll_to_bottom(area);
    pane.draw(grid, area, false, theme);
    CHECK_EQ(pane.scroll().line, 4u);
    CHECK_EQ(grid.char_at({ 1, 1, 0 }), "e");
    CHECK_EQ(grid.char_at({ 2, 1, 0 }), "f");
}

TEST_CASE("core::Pane::position_from_screen() maps clicks to the buffer") {
    Pane pane { PaneType::edit, make_buffer("hello\nworld") };
    Rectangle const area { { 2, 4, 0 }, 10, 4 };
    // content origin is (3,5); clicking there is buffer (0,0)
    CHECK_EQ(pane.position_from_screen(area, { 3, 5, 0 }), Position { 0, 0, 0 });
    CHECK_EQ(pane.position_from_screen(area, { 4, 7, 0 }), Position { 1, 2, 0 });
    // clicks past the text clamp to the line end
    CHECK_EQ(pane.position_from_screen(area, { 4, 30, 0 }), Position { 1, 5, 0 });
}

TEST_CASE("core::Pane::cursor_screen_position() is the content cell of the cursor") {
    Pane pane { PaneType::edit, make_buffer("hello") };
    pane.set_cursor({ 0, 3, 0 });
    Rectangle const area { { 2, 4, 0 }, 10, 4 };
    CHECK_EQ(pane.cursor_screen_position(area), Position { 3, 8, 0 });
}

TEST_CASE("core::Pane selection orders anchor and cursor") {
    Pane pane { PaneType::edit, make_buffer("hello\nworld") };
    CHECK_FALSE(pane.selection().has_value());

    pane.set_cursor({ 1, 2, 0 });
    pane.set_anchor({ 1, 2, 0 }); // anchor == cursor: still no selection
    CHECK(pane.has_anchor());
    CHECK_FALSE(pane.selection().has_value());

    pane.set_cursor({ 0, 1, 0 }); // cursor moved before the anchor
    auto const range { pane.selection() };
    REQUIRE(range.has_value());
    CHECK_EQ(range->first, Position { 0, 1, 0 });
    CHECK_EQ(range->second, Position { 1, 2, 0 });

    pane.clear_anchor();
    CHECK_FALSE(pane.selection().has_value());
}

TEST_CASE("core::Pane draws the selection with the selection colors") {
    Grid grid { 12, 4 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("hello") };
    pane.set_anchor({ 0, 1, 0 });
    pane.set_cursor({ 0, 3, 0 }); // "el" selected

    pane.draw(grid, { { 0, 0, 0 }, 12, 4 }, true, theme);
    // text sits right of the two-column gutter
    CHECK_EQ(
        grid.background_at({ 1, 4, 0 }), // 'e'
        theme.color(Theme::Usage::selection_background)
    );
    CHECK_EQ(
        grid.background_at({ 1, 5, 0 }), // 'l'
        theme.color(Theme::Usage::selection_background)
    );
    // unselected neighbours sit on the (focused) cursor's line highlight
    CHECK_EQ(grid.background_at({ 1, 3, 0 }), theme.color(Theme::Usage::cursor_line)); // 'h'
    CHECK_EQ(grid.background_at({ 1, 6, 0 }), theme.color(Theme::Usage::cursor_line)); // second 'l'
}

TEST_CASE("core::Pane read-only flag shows [ro] in the title") {
    Grid grid { 20, 4 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("x") };
    CHECK_FALSE(pane.read_only());

    pane.set_read_only(true);
    pane.draw(grid, { { 0, 0, 0 }, 20, 4 }, false, theme);
    // bar: " • buf [ro]" - the marker starts at column 7
    CHECK_EQ(grid.char_at({ 0, 7, 0 }), "[");
    CHECK_EQ(grid.char_at({ 0, 8, 0 }), "r");
    CHECK_EQ(grid.char_at({ 0, 9, 0 }), "o");
    CHECK_EQ(grid.char_at({ 0, 10, 0 }), "]");
}

TEST_CASE("core::Pane shows a scrollbar thumb once content overflows") {
    Grid grid { 20, 6 }; // content height 4
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11") };
    Rectangle const area { { 0, 0, 0 }, 20, 6 };

    pane.draw(grid, area, false, theme);
    CHECK_EQ(grid.char_at({ 1, 19, 0 }), "█"); // thumb at the top

    pane.set_cursor({ 11, 0, 0 }); // jump to the end: thumb at the bottom
    pane.draw(grid, area, true, theme);
    CHECK_EQ(grid.char_at({ 4, 19, 0 }), "█");
    CHECK_EQ(grid.char_at({ 1, 19, 0 }), "│"); // track above it

    Pane fits { PaneType::edit, make_buffer("a\nb") };
    fits.draw(grid, area, false, theme);
    CHECK_EQ(grid.char_at({ 1, 19, 0 }), "│"); // no overflow: no thumb
}

TEST_CASE("core::Pane highlights the cursor line when focused") {
    Grid grid { 20, 5 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("one\ntwo") };
    Rectangle const area { { 0, 0, 0 }, 20, 5 };
    pane.set_cursor({ 1, 0, 0 });

    pane.draw(grid, area, true, theme);
    CHECK_EQ(grid.background_at({ 2, 5, 0 }), theme.color(Theme::Usage::cursor_line));
    CHECK_EQ(grid.background_at({ 1, 5, 0 }), theme.color(Theme::Usage::background));

    pane.draw(grid, area, false, theme); // unfocused: no highlight
    CHECK_EQ(grid.background_at({ 2, 5, 0 }), theme.color(Theme::Usage::background));
}

TEST_CASE("core::Pane::resize_corner() is the bottom-right of the border") {
    Rectangle const area { { 2, 4, 0 }, 20, 6 };
    CHECK_EQ(Pane::resize_corner(area), Rectangle { { 7, 22, 0 }, 2, 1 });
    CHECK_EQ(Pane::resize_corner({ { 0, 0, 0 }, 1, 1 }).width, 0u); // too small
}

TEST_CASE("core::Pane tiny areas draw nothing and stay safe") {
    Grid grid { 4, 4 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("x") };
    pane.draw(grid, { { 0, 0, 0 }, 1, 1 }, true, theme);
    pane.draw(grid, { { 0, 0, 0 }, 0, 0 }, true, theme);
    CHECK(Pane::content_area({ { 0, 0, 0 }, 2, 2 }).width == 0);
}

TEST_CASE("core::Pane wide edit panes grow a line-number gutter") {
    Grid grid { 40, 10 };
    Theme theme;
    Pane pane { PaneType::edit, make_buffer("alpha\nbeta") };
    Rectangle const area { { 0, 0, 0 }, 30, 6 };

    pane.draw(grid, area, true, theme);
    // content starts at (1,1): "1", a space, then the text
    CHECK_EQ(grid.char_at({ 1, 1, 0 }), "1");
    CHECK_EQ(grid.char_at({ 1, 2, 0 }), " ");
    CHECK_EQ(grid.char_at({ 1, 3, 0 }), "a");
    CHECK_EQ(grid.char_at({ 2, 1, 0 }), "2");
    CHECK_EQ(grid.char_at({ 2, 3, 0 }), "b");

    // clicks and the cursor cell both live past the gutter
    CHECK_EQ(pane.position_from_screen(area, { 1, 3, 0 }), Position { 0, 0, 0 });
    pane.set_cursor({ 0, 0, 0 });
    CHECK_EQ(pane.cursor_screen_position(area), Position { 1, 3, 0 });
}

TEST_CASE("core::Pane narrow panes keep every column for text") {
    Grid grid { 20, 6 };
    Theme theme;
    Pane pane { PaneType::edit, make_buffer("narrow") };
    Rectangle const area { { 0, 0, 0 }, 10, 4 }; // content width 8: no room

    pane.draw(grid, area, false, theme);
    CHECK_EQ(grid.char_at({ 1, 1, 0 }), "n"); // text starts flush left
    CHECK_EQ(pane.position_from_screen(area, { 1, 1, 0 }), Position { 0, 0, 0 });
}
