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

TEST_CASE("core::Pane draws border, title and content into a grid") {
    Grid grid { 12, 5 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("hello\nworld") };

    pane.draw(grid, { { 0, 0, 0 }, 12, 5 }, false, theme);

    CHECK_EQ(grid.char_at({ 0, 0, 0 }), "┌");
    CHECK_EQ(grid.char_at({ 0, 11, 0 }), "┐");
    CHECK_EQ(grid.char_at({ 4, 0, 0 }), "└");
    CHECK_EQ(grid.char_at({ 4, 11, 0 }), "┘");
    CHECK_EQ(grid.char_at({ 1, 0, 0 }), "│");

    // title on the top border, from column 2
    CHECK_EQ(grid.char_at({ 0, 2, 0 }), "b");
    CHECK_EQ(grid.char_at({ 0, 3, 0 }), "u");
    CHECK_EQ(grid.char_at({ 0, 4, 0 }), "f");

    // content inside the border
    CHECK_EQ(grid.char_at({ 1, 1, 0 }), "h");
    CHECK_EQ(grid.char_at({ 1, 5, 0 }), "o");
    CHECK_EQ(grid.char_at({ 2, 1, 0 }), "w");
}

TEST_CASE("core::Pane focused border uses the focused color") {
    Grid grid { 8, 4 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("x") };

    pane.draw(grid, { { 0, 0, 0 }, 8, 4 }, true, theme);
    Color const focused { grid.style_at({ 0, 0, 0 }) };
    CHECK_EQ(focused, theme.color(Theme::Usage::border_focused));

    pane.draw(grid, { { 0, 0, 0 }, 8, 4 }, false, theme);
    Color const blurred { grid.style_at({ 0, 0, 0 }) };
    CHECK_EQ(blurred, theme.color(Theme::Usage::border));
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
    CHECK_EQ(
        grid.background_at({ 1, 2, 0 }), // 'e'
        theme.color(Theme::Usage::selection_background)
    );
    CHECK_EQ(
        grid.background_at({ 1, 3, 0 }), // 'l'
        theme.color(Theme::Usage::selection_background)
    );
    CHECK_EQ(grid.background_at({ 1, 1, 0 }), theme.color(Theme::Usage::background)); // 'h'
    CHECK_EQ(grid.background_at({ 1, 4, 0 }), theme.color(Theme::Usage::background)); // second 'l'
}

TEST_CASE("core::Pane tiny areas draw nothing and stay safe") {
    Grid grid { 4, 4 };
    Theme const theme;
    Pane pane { PaneType::edit, make_buffer("x") };
    pane.draw(grid, { { 0, 0, 0 }, 1, 1 }, true, theme);
    pane.draw(grid, { { 0, 0, 0 }, 0, 0 }, true, theme);
    CHECK(Pane::content_area({ { 0, 0, 0 }, 2, 2 }).width == 0);
}
