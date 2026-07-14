#include "spice/core/Palette.hpp"
#include "spice/core/Utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string_view>
#include <utility>

namespace {

using namespace spice::core;

constexpr std::string_view commands_title { "Commands" };
constexpr std::string_view prompt { "> " };
constexpr std::string_view no_match { "(no matching command)" };

constexpr uint32_t max_width { 50 };
constexpr uint32_t max_height { 14 };

auto lowered(std::string_view text) -> std::string {
    std::string out { text };
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

auto matches(std::string_view title_text, std::string const& query_lower) -> bool {
    return lowered(title_text).find(query_lower) != std::string::npos;
}

auto paint_cell(
    Grid& grid, Position cell, std::string_view glyph, Color style, Color background
) -> void {
    grid.set_text(cell, glyph);
    grid.set_style(cell, style);
    grid.set_background(cell, background);
}

//! Paints `text` (ASCII) into a row of cells, space-padded to `width`.
auto paint_row(
    Grid& grid, Position origin, uint32_t width,
    std::string_view text, Color style, Color background
) -> void {
    for (uint32_t column { 0 }; column < width; ++column) {
        std::string_view const glyph {
            column < text.size() ? text.substr(column, 1) : std::string_view(" ")
        };
        paint_cell(grid, { origin.line, origin.column + column, 0 }, glyph, style, background);
    }
}

}

namespace spice::core {

auto Palette::open(std::vector<Item> items) -> void {
    _items = std::move(items);
    std::ranges::sort(_items, {}, &Item::title);
    _title = commands_title;
    _input = false;
    _source = nullptr;
    _query.clear();
    _selected = 0;
    _scroll = 0;
    _open = true;
    refilter();
}

auto Palette::open_input(std::string title) -> void {
    _items.clear();
    _title = std::move(title);
    _input = true;
    _source = nullptr;
    _query.clear();
    _selected = 0;
    _scroll = 0;
    _open = true;
    refilter();
}

auto Palette::open_picker(
    std::string title, std::function<std::vector<Item>(std::string const&)> source
) -> void {
    _items.clear();
    _title = std::move(title);
    _input = false;
    _source = std::move(source);
    _query.clear();
    _selected = 0;
    _scroll = 0;
    _open = true;
    refilter();
}

auto Palette::is_input() const -> bool {
    return _input;
}

auto Palette::is_picker() const -> bool {
    return _source != nullptr;
}

auto Palette::set_query(std::string query) -> void {
    _query = std::move(query);
    refilter();
}

auto Palette::close() -> void {
    _open = false;
}

auto Palette::is_open() const -> bool {
    return _open;
}

auto Palette::refilter() -> void {
    if (_source) { // picker: the source computes the list for this query
        _filtered = _source(_query);
    } else {
        auto const query_lower { lowered(_query) };
        _filtered.clear();
        for (Item const& item : _items) {
            if (query_lower.empty() || matches(item.title, query_lower)) {
                _filtered.push_back(item);
            }
        }
    }
    _selected = 0;
    _scroll = 0;
}

auto Palette::handle(KeyEvent const& key) -> Outcome {
    if (!_open) {
        return Outcome::ignored;
    }

    switch (key.key) {
    case Key::character:
        if (key.mods.ctrl || key.mods.alt) {
            return Outcome::ignored;
        }
        _query += key.text;
        refilter();
        return Outcome::updated;

    case Key::backspace:
        if (!_query.empty()) {
            size_t const count { utf8_count(_query) };
            _query.resize(utf8_offset(_query, count - 1));
            refilter();
        }
        return Outcome::updated;

    case Key::up:
        if (_selected > 0) {
            --_selected;
        }
        return Outcome::updated;

    case Key::down:
        if (_selected + 1 < _filtered.size()) {
            ++_selected;
        }
        return Outcome::updated;

    case Key::enter:
        // a picker with nothing listed still picks: the typed text stands
        // for itself (selected_name() is empty, callers use query())
        if (!_input && !is_picker() && _filtered.empty()) {
            return Outcome::ignored;
        }
        _open = false;
        return Outcome::picked;

    case Key::escape:
        _open = false;
        return Outcome::closed;

    default:
        return Outcome::ignored;
    }
}

auto Palette::query() const -> std::string const& {
    return _query;
}

auto Palette::filtered() const -> std::vector<Item> const& {
    return _filtered;
}

auto Palette::selected_index() const -> uint32_t {
    return _selected;
}

auto Palette::selected_name() const -> std::string {
    if (_selected >= _filtered.size()) {
        return {};
    }
    return _filtered[_selected].name;
}

auto Palette::area(Rectangle screen) -> Rectangle {
    uint32_t const width { screen.width < max_width ? screen.width : max_width };
    uint32_t const height { screen.height < max_height ? screen.height : max_height };
    return {
        {
            screen.position.line + (screen.height - height) / 2,
            screen.position.column + (screen.width - width) / 2,
            0,
        },
        width,
        height,
    };
}

auto Palette::draw(Grid& grid, Rectangle screen, Theme const& theme) -> void {
    if (!_open) {
        return;
    }
    Rectangle const rect { area(screen) };
    if (rect.width <= 2 || rect.height <= 3) {
        return;
    }

    Color const border { theme.color(Theme::Usage::border_focused) };
    Color const text { theme.color(Theme::Usage::text) };
    Color const background { theme.color(Theme::Usage::background) };
    Color const info { theme.color(Theme::Usage::info) };
    Color const selection_text { theme.color(Theme::Usage::selection_text) };
    Color const selection_background { theme.color(Theme::Usage::selection_background) };

    uint32_t const top { rect.position.line };
    uint32_t const bottom { rect.position.line + rect.height - 1 };
    uint32_t const left { rect.position.column };
    uint32_t const right { rect.position.column + rect.width - 1 };

    std::string_view const title { _title };
    for (uint32_t column { left }; column <= right; ++column) {
        std::string_view top_glyph { "─" };
        uint32_t const title_start { left + 2 };
        if (column >= title_start && column < title_start + title.size()) {
            top_glyph = title.substr(column - title_start, 1);
        }
        paint_cell(grid, { top, column, 0 }, top_glyph, border, background);
        paint_cell(grid, { bottom, column, 0 }, "─", border, background);
    }
    for (uint32_t line { top }; line <= bottom; ++line) {
        paint_cell(grid, { line, left, 0 }, "│", border, background);
        paint_cell(grid, { line, right, 0 }, "│", border, background);
    }
    paint_cell(grid, { top, left, 0 }, "┌", border, background);
    paint_cell(grid, { top, right, 0 }, "┐", border, background);
    paint_cell(grid, { bottom, left, 0 }, "└", border, background);
    paint_cell(grid, { bottom, right, 0 }, "┘", border, background);

    uint32_t const content_width { rect.width - 2 };
    uint32_t const list_rows { rect.height - 3 }; // minus border and query line

    // query line: "> filter"
    paint_row(
        grid, { top + 1, left + 1, 0 }, content_width,
        std::string(prompt) + _query, text, background
    );

    // keep the selection visible
    if (_selected < _scroll) {
        _scroll = _selected;
    } else if (list_rows > 0 && _selected >= _scroll + list_rows) {
        _scroll = _selected - list_rows + 1;
    }

    for (uint32_t row { 0 }; row < list_rows; ++row) {
        Position const origin { top + 2 + row, left + 1, 0 };
        size_t const index { _scroll + row };

        if (!_input && _filtered.empty() && row == 0) {
            paint_row(
                grid, origin, content_width,
                is_picker() ? "(RETURN takes the typed text)" : no_match,
                info, background
            );
        } else if (index < _filtered.size()) {
            bool const selected { index == _selected };
            paint_row(
                grid, origin, content_width, " " + _filtered[index].title,
                selected ? selection_text : text,
                selected ? selection_background : background
            );
        } else {
            paint_row(grid, origin, content_width, "", text, background);
        }
    }
}

auto Palette::cursor_screen_position(Rectangle screen) const -> Position {
    Rectangle const rect { area(screen) };
    auto const column {
        static_cast<uint32_t>(rect.position.column + 1 + prompt.size() + utf8_count(_query))
    };
    uint32_t const limit { rect.position.column + rect.width - 2 };
    return { rect.position.line + 1, column < limit ? column : limit, 0 };
}

}
