#include "doctest.h"

#include "spice/core/Grid.hpp"
#include "spice/core/Palette.hpp"
#include "spice/core/Theme.hpp"

using namespace spice::core;

namespace {

auto items() -> std::vector<Palette::Item> {
    return {
        { "session.close", "Close Spice" },
        { "pane.float", "Float pane" },
        { "pane.close", "Close current pane" },
    };
}

auto press(Palette& palette, Key key, std::string text = {}) -> Palette::Outcome {
    return palette.handle({ key, {}, std::move(text) });
}

}

TEST_CASE("core::Palette opens sorted by title with everything listed") {
    Palette palette;
    CHECK_FALSE(palette.is_open());
    palette.open(items());
    CHECK(palette.is_open());
    REQUIRE_EQ(palette.filtered().size(), 3u);
    CHECK_EQ(palette.filtered()[0].title, "Close Spice");
    CHECK_EQ(palette.filtered()[1].title, "Close current pane");
    CHECK_EQ(palette.filtered()[2].title, "Float pane");
    CHECK_EQ(palette.selected_name(), "session.close");
}

TEST_CASE("core::Palette typing filters case-insensitively") {
    Palette palette;
    palette.open(items());
    CHECK_EQ(press(palette, Key::character, "F"), Palette::Outcome::updated);
    CHECK_EQ(press(palette, Key::character, "l"), Palette::Outcome::updated);
    CHECK_EQ(press(palette, Key::character, "o"), Palette::Outcome::updated);
    CHECK_EQ(palette.query(), "Flo");
    REQUIRE_EQ(palette.filtered().size(), 1u);
    CHECK_EQ(palette.filtered()[0].name, "pane.float");
}

TEST_CASE("core::Palette backspace edits the filter") {
    Palette palette;
    palette.open(items());
    press(palette, Key::character, "z");
    CHECK(palette.filtered().empty());
    CHECK_EQ(press(palette, Key::backspace), Palette::Outcome::updated);
    CHECK_EQ(palette.query(), "");
    CHECK_EQ(palette.filtered().size(), 3u);
}

TEST_CASE("core::Palette selection moves and clamps") {
    Palette palette;
    palette.open(items());
    CHECK_EQ(palette.selected_index(), 0u);
    press(palette, Key::up); // clamped at the top
    CHECK_EQ(palette.selected_index(), 0u);
    press(palette, Key::down);
    press(palette, Key::down);
    press(palette, Key::down); // clamped at the bottom
    CHECK_EQ(palette.selected_index(), 2u);
    CHECK_EQ(palette.selected_name(), "pane.float");
}

TEST_CASE("core::Palette enter picks the selection and closes") {
    Palette palette;
    palette.open(items());
    press(palette, Key::character, "float");
    CHECK_EQ(press(palette, Key::enter), Palette::Outcome::picked);
    CHECK_EQ(palette.selected_name(), "pane.float");
    CHECK_FALSE(palette.is_open());
}

TEST_CASE("core::Palette enter with no match does nothing") {
    Palette palette;
    palette.open(items());
    press(palette, Key::character, "zzz");
    CHECK_EQ(press(palette, Key::enter), Palette::Outcome::ignored);
    CHECK(palette.is_open());
}

TEST_CASE("core::Palette escape closes without picking") {
    Palette palette;
    palette.open(items());
    CHECK_EQ(press(palette, Key::escape), Palette::Outcome::closed);
    CHECK_FALSE(palette.is_open());
}

TEST_CASE("core::Palette filter resets the selection") {
    Palette palette;
    palette.open(items());
    press(palette, Key::down);
    CHECK_EQ(palette.selected_index(), 1u);
    press(palette, Key::character, "c");
    CHECK_EQ(palette.selected_index(), 0u);
}

TEST_CASE("core::Palette::draw() paints title, query and highlighted selection") {
    Palette palette;
    palette.open(items());
    press(palette, Key::character, "f");

    Grid grid { 60, 20 };
    Theme const theme;
    Rectangle const screen { { 0, 0, 0 }, 60, 20 };
    palette.draw(grid, screen, theme);

    Rectangle const rect { Palette::area(screen) };
    // title on the top border
    CHECK_EQ(grid.char_at({ rect.position.line, rect.position.column + 2, 0 }), "C");
    // query line shows the prompt and filter
    CHECK_EQ(grid.char_at({ rect.position.line + 1, rect.position.column + 1, 0 }), ">");
    CHECK_EQ(grid.char_at({ rect.position.line + 1, rect.position.column + 3, 0 }), "f");
    // selected row is drawn with the selection background
    Color const selected_bg {
        grid.background_at({ rect.position.line + 2, rect.position.column + 2, 0 })
    };
    CHECK_EQ(selected_bg, theme.color(Theme::Usage::selection_background));
}

TEST_CASE("core::Palette input mode picks the typed text") {
    Palette palette;
    palette.open_input("Open file");
    CHECK(palette.is_open());
    CHECK(palette.is_input());

    press(palette, Key::character, "/");
    press(palette, Key::character, "a");
    press(palette, Key::character, "b");
    CHECK_EQ(press(palette, Key::enter), Palette::Outcome::picked);
    CHECK_EQ(palette.query(), "/ab");
    CHECK_FALSE(palette.is_open());
}

TEST_CASE("core::Palette picker mode recomputes items from the query") {
    Palette palette;
    palette.open_picker("Open file", [](std::string const& query) {
        std::vector<Palette::Item> items;
        if (query.empty()) {
            items.push_back({ "a", "a" });
            items.push_back({ "b", "b" });
        } else if (query == "x") {
            items.push_back({ "x-match", "x-match" });
        }
        return items;
    });
    CHECK(palette.is_picker());
    CHECK_FALSE(palette.is_input());
    CHECK_EQ(palette.filtered().size(), 2u);

    press(palette, Key::character, "x"); // the source, not a filter, decides
    REQUIRE_EQ(palette.filtered().size(), 1u);
    CHECK_EQ(palette.filtered()[0].name, "x-match");
    CHECK_EQ(press(palette, Key::enter), Palette::Outcome::picked);
    CHECK_EQ(palette.selected_name(), "x-match");
}

TEST_CASE("core::Palette picker with nothing listed picks the typed text") {
    Palette palette;
    palette.open_picker("Open file", [](std::string const&) {
        return std::vector<Palette::Item> {};
    });
    press(palette, Key::character, "n");
    CHECK_EQ(press(palette, Key::enter), Palette::Outcome::picked);
    CHECK_EQ(palette.selected_name(), ""); // caller falls back to query()
    CHECK_EQ(palette.query(), "n");
}

TEST_CASE("core::Palette::set_query() re-runs a picker's source") {
    Palette palette;
    palette.open_picker("Open file", [](std::string const& query) {
        return std::vector<Palette::Item> { { query + "!", query + "!" } };
    });
    palette.set_query("dir/");
    CHECK_EQ(palette.query(), "dir/");
    REQUIRE_EQ(palette.filtered().size(), 1u);
    CHECK_EQ(palette.filtered()[0].name, "dir/!");
}

TEST_CASE("core::Palette input mode escape cancels") {
    Palette palette;
    palette.open_input("Save as");
    press(palette, Key::character, "x");
    CHECK_EQ(press(palette, Key::escape), Palette::Outcome::closed);
    CHECK_FALSE(palette.is_open());
}

TEST_CASE("core::Palette input mode draws its title, command mode resets it") {
    Palette palette;
    Grid grid { 60, 20 };
    Theme const theme;
    Rectangle const screen { { 0, 0, 0 }, 60, 20 };
    Rectangle const rect { Palette::area(screen) };

    palette.open_input("Save as");
    palette.draw(grid, screen, theme);
    CHECK_EQ(grid.char_at({ rect.position.line, rect.position.column + 2, 0 }), "S");

    palette.open(items());
    CHECK_FALSE(palette.is_input());
    palette.draw(grid, screen, theme);
    CHECK_EQ(grid.char_at({ rect.position.line, rect.position.column + 2, 0 }), "C");
}

TEST_CASE("core::Palette::draw() shows a hint when nothing matches") {
    Palette palette;
    palette.open(items());
    press(palette, Key::character, "q");
    press(palette, Key::character, "q");

    Grid grid { 60, 20 };
    Theme const theme;
    Rectangle const screen { { 0, 0, 0 }, 60, 20 };
    palette.draw(grid, screen, theme);

    Rectangle const rect { Palette::area(screen) };
    CHECK_EQ(grid.char_at({ rect.position.line + 2, rect.position.column + 1, 0 }), "(");
}
