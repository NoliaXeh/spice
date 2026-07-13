#include "spice/core/Pane.hpp"
#include "spice/core/Utf8.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace {

using namespace spice::core;

constexpr std::string_view corner_tl { "┌" }; // ┌
constexpr std::string_view corner_tr { "┐" }; // ┐
constexpr std::string_view corner_bl { "└" }; // └
constexpr std::string_view corner_br { "┘" }; // ┘
constexpr std::string_view edge_h { "─" };    // ─
constexpr std::string_view edge_v { "│" };    // │

auto paint_cell(
    Grid& grid, Position cell, std::string_view glyph, Color style, Color background
) -> void {
    grid.set_text(cell, glyph);
    grid.set_style(cell, style);
    grid.set_background(cell, background);
}

}

namespace spice::core {

Pane::Pane(PaneType type, std::shared_ptr<Buffer> buffer)
    : _type { type }
    , _buffer { std::move(buffer) }
{}

auto Pane::type() const -> PaneType {
    return _type;
}

auto Pane::buffer() const -> std::shared_ptr<Buffer> const& {
    return _buffer;
}

auto Pane::cursor() const -> Position {
    return _cursor;
}

auto Pane::set_cursor(Position position) -> void {
    _cursor = clamp(position);
}

auto Pane::scroll() const -> Position {
    return _scroll;
}

auto Pane::scroll_to_bottom(Rectangle area) -> void {
    auto const content { content_area(area) };
    uint32_t const lines { _buffer->line_count() };
    _scroll.line = lines > content.height ? lines - content.height : 0;
    _scroll.column = 0;
}

auto Pane::content_area(Rectangle area) -> Rectangle {
    if (area.width <= 2 || area.height <= 2) {
        return { area.position, 0, 0 };
    }
    return {
        { area.position.line + 1, area.position.column + 1, area.position.layer },
        area.width - 2,
        area.height - 2,
    };
}

auto Pane::clamp(Position position) const -> Position {
    uint32_t const last_line { _buffer->line_count() - 1 };
    uint32_t const line { position.line < last_line ? position.line : last_line };
    uint32_t const length { _buffer->line_length(line) };
    uint32_t const column { position.column < length ? position.column : length };
    return { line, column, 0 };
}

auto Pane::scroll_to_cursor(Rectangle content) -> void {
    if (content.width == 0 || content.height == 0) {
        return;
    }
    if (_cursor.line < _scroll.line) {
        _scroll.line = _cursor.line;
    } else if (_cursor.line >= _scroll.line + content.height) {
        _scroll.line = _cursor.line - content.height + 1;
    }
    if (_cursor.column < _scroll.column) {
        _scroll.column = _cursor.column;
    } else if (_cursor.column >= _scroll.column + content.width) {
        _scroll.column = _cursor.column - content.width + 1;
    }
}

auto Pane::draw(Grid& grid, Rectangle area, bool focused, Theme const& theme) -> void {
    if (area.width == 0 || area.height == 0) {
        return;
    }
    // the buffer may have shrunk under us (undo, edits from another pane
    // sharing it): pull the cursor back inside before using it to scroll
    _cursor = clamp(_cursor);

    Color const border {
        theme.color(focused ? Theme::Usage::border_focused : Theme::Usage::border)
    };
    Color const text { theme.color(Theme::Usage::text) };
    Color const background { theme.color(Theme::Usage::background) };

    uint32_t const top { area.position.line };
    uint32_t const bottom { area.position.line + area.height - 1 };
    uint32_t const left { area.position.column };
    uint32_t const right { area.position.column + area.width - 1 };

    // border with the buffer's name as title: ┌ name ────┐
    std::string_view const name { _buffer->name() };
    for (uint32_t column { left }; column <= right; ++column) {
        std::string_view top_glyph { edge_h };
        uint32_t const title_start { left + 2 };
        if (column >= title_start && column < title_start + name.size()
            && area.width > 4) {
            size_t const index { column - title_start };
            top_glyph = name.substr(index, 1);
        }
        paint_cell(grid, { top, column, 0 }, top_glyph, border, background);
        paint_cell(grid, { bottom, column, 0 }, edge_h, border, background);
    }
    for (uint32_t line { top }; line <= bottom; ++line) {
        paint_cell(grid, { line, left, 0 }, edge_v, border, background);
        paint_cell(grid, { line, right, 0 }, edge_v, border, background);
    }
    paint_cell(grid, { top, left, 0 }, corner_tl, border, background);
    paint_cell(grid, { top, right, 0 }, corner_tr, border, background);
    paint_cell(grid, { bottom, left, 0 }, corner_bl, border, background);
    paint_cell(grid, { bottom, right, 0 }, corner_br, border, background);

    // content; edit panes scroll to keep the cursor in view, other pane
    // types keep the scroll they were given (scrollback, plugin-driven)
    auto const content { content_area(area) };
    if (_type == PaneType::edit) {
        scroll_to_cursor(content);
    }

    for (uint32_t row { 0 }; row < content.height; ++row) {
        std::string_view const line { _buffer->line(_scroll.line + row) };
        size_t offset { utf8_offset(line, _scroll.column) };

        for (uint32_t column { 0 }; column < content.width; ++column) {
            Position const cell {
                content.position.line + row,
                content.position.column + column,
                0,
            };
            std::string_view glyph { " " };
            if (offset < line.size()) {
                glyph = line.substr(offset, utf8_length(line[offset]));
                offset += glyph.size();
            }
            paint_cell(grid, cell, glyph, text, background);
        }
    }
}

auto Pane::position_from_screen(Rectangle area, Position screen) const -> Position {
    auto const content { content_area(area) };
    uint32_t const line {
        screen.line > content.position.line ? screen.line - content.position.line : 0
    };
    uint32_t const column {
        screen.column > content.position.column ? screen.column - content.position.column : 0
    };
    return clamp({ _scroll.line + line, _scroll.column + column, 0 });
}

auto Pane::cursor_screen_position(Rectangle area) const -> Position {
    auto const content { content_area(area) };
    uint32_t const line { _cursor.line - _scroll.line };
    uint32_t const column { _cursor.column - _scroll.column };
    return {
        content.position.line + (line < content.height ? line : 0),
        content.position.column + (column < content.width ? column : 0),
        0,
    };
}

}
